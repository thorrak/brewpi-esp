#pragma once

void assertNoEpisodeFlags(JsonVariantConst value) {
  for (const auto key : {"controller_completed", "controller_timed_out", "controller_episodes_completed",
                         "controller_episodes_required", "controller_initial_settled",
                         "controller_stable_at_end", "controller_observation_complete"})
    assert(value[key].isNull());
}

void verifyObservationJournal(unsigned run, unsigned completed, unsigned interrupted) {
  unsigned ordinal = 0, done = 0, aborted = 0;
  for (const auto &row : finalObservationRecords()) {
    assert(row.kind != 8); // New campaigns never emit legacy episode records.
    if (row.kind != 10 || row.role != run)
      continue;
    assert(row.detail == ++ordinal && row.read_us <= row.t_us);
    assert(row.pulse >= 1 && row.pulse <= 4);
    if (row.pulse == 4) ++aborted;
    else ++done;
    JsonDocument decoded;
    WaterTestProtocol::recordToJson(decoded.to<JsonObject>(), row, WaterTest::recordingBoot, 0.);
    assert(decoded["controller_run"] == run && decoded["observation"] == ordinal);
    assert(decoded["event"] == "finished" && decoded["coast_s"].as<double>() >= 0.);
  }
  assert(done == completed && aborted == interrupted);
}

void controllerObservationRegression(const std::string &scenario) {
  const bool dose = scenario == "controller_observations_dose";
  if (dose) extendedSettings.glycolCoolingAlgorithm = GlycolCooling::Algorithm::PulseDose;
  start();
  auto plan = WaterTest::manifest["test_program"];
  assert(plan["controller_plan_version"] == 3 && plan["controller"].isNull());
  for (const auto controller : plan["controllers"].as<JsonArrayConst>()) {
    assert(controller["completion_policy"] == "fixed_duration_v1");
    assert(controller["observation_goal"] == 3 && controller["required_episodes"].isNull());
  }
  auto &p = WaterTest::program;
  p.usefulResponse = true;
  p.beginController(Native::clock / 1e6);
  WaterTest::applyProgram(Phase::Baseline);
  WaterTest::serviceProgram(true);
  fresh(Native::clock / 1e6 + 2);
  const double deadline = p.controllerRunDeadline, target = p.targetC;
  assert(WaterTest::testController && WaterTest::physicalPump());
  const bool failWrite = scenario == "controller_observation_write_failure";
  bool failedObservationWrite = false;
  if (failWrite) {
    Native::fsyncHook = [&] {
      const auto rows = finalObservationRecords();
      if (!failedObservationWrite && rows.back().kind == 10) {
        assert(rows.back().detail == 1 && rows.back().pulse == 1);
        failedObservationWrite = true;
        Native::fsyncUntilFail = 0;
      }
    };
  }
  // Flat measured temperature above target deliberately produces immediate
  // COAST -> COOL restarts. Each closure must survive the newly live response.
  for (unsigned n = 0; WaterTest::active() && WaterTest::controllerObservations[0].completed() < 4 && n < 1800; ++n)
    fresh(Native::clock / 1e6 + 2);
  Native::fsyncHook = {};
  if (failWrite) {
    assert(failedObservationWrite && !WaterTest::active());
    assert(WaterTest::terminal["reason"] == "recording_failure");
    assert(WaterTest::controllerObservations[0].completed() == 0);
    assert(WaterTest::terminal["controllers"][0]["observations"]["completed"] == 0);
    assert(!WaterTest::physicalPump());
    submit();
    return;
  }
  assert(WaterTest::active() && p.phase == Phase::Controller && p.controllerRun == 1);
  assert(p.targetC == target && p.controllerRunDeadline == deadline);
  assert(Native::clock / 1e6 < deadline && !p.controllerRunDurationComplete);
  assert(WaterTest::controllerObservations[0].completed() == 4);
  assert(WaterTest::controllerObservations[0].rateQualified == 4);
  assert(WaterTest::controllerObservations[0].interrupted == 0);
  assert(WaterTest::controllerObservations[1].completed() == 0);
  assert(WaterTest::physicalPump());
  const auto records = WaterTest::recordCount;
  for (unsigned n = 0; n < 3; ++n) {
    assert(!WaterTest::recordControllerObservation());
    WaterTest::serviceProgram(false);
  }
  assert(WaterTest::recordCount == records); // Persistent latest event is not duplicated.
  verifyObservationJournal(1, 4, 0);
  if (scenario == "controller_observations_before_reboot") {
    WaterTest::closeJournal();
    return;
  }
  if (scenario == "controller_observation_stale") {
    Native::clock += uint64_t(freshnessSeconds * 1e6) + 1;
    WaterTest::tick();
    assert(WaterTest::terminal["reason"] == "beer_sensor_stale");
  } else {
    std::string error;
    assert(WaterTest::requestStop(error));
    fresh(Native::clock / 1e6 + 2);
    assert(WaterTest::terminal["reason"] == "user_stop");
  }
  assert(!WaterTest::active() && !WaterTest::physicalPump());
  assertNoEpisodeFlags(WaterTest::terminal.as<JsonVariantConst>());
  const auto result = WaterTest::terminal["controllers"][0];
  assertNoEpisodeFlags(result);
  assert(result["run_duration_complete"] == false);
  assert(result["observations"]["completed"] == 4 && result["observations"]["rate_qualified"] == 4);
  assert(result["observations"]["interrupted"] == 1);
  const auto afterFinish = WaterTest::recordCount;
  WaterTest::closeControllerObservation();
  assert(WaterTest::recordCount == afterFinish);
  verifyObservationJournal(1, 4, 1);
  submit();
}

void recoveredControllerObservationsRegression() {
  assert(!WaterTest::active() && !WaterTest::physicalPump());
  assert(WaterTest::terminal["outcome"] == "interrupted");
  assertNoEpisodeFlags(WaterTest::terminal.as<JsonVariantConst>());
  const auto first = WaterTest::terminal["controllers"][0];
  assertNoEpisodeFlags(first);
  assert(first["status"] == "interrupted" && first["run_duration_complete"] == false);
  assert(first["observations"]["completed"] == 4 && first["observations"]["rate_qualified"] == 4);
  // A reboot cannot invent the pending response's shutdown timestamp/closure.
  assert(first["observations"]["interrupted"] == 0);
  assert(first["final_tuning"].isNull() && first["learning_status"] == "unavailable_after_restart");
  assert(WaterTest::terminal["controllers"][1]["status"] == "skipped");
  verifyObservationJournal(1, 4, 0);
  submit();
}

void controllerObservationWriteDeadlineRegression() {
  start();
  auto &p = WaterTest::program;
  p.usefulResponse = true;
  p.beginController(Native::clock / 1e6);
  WaterTest::applyProgram(Phase::Baseline);
  bool stalled = false;
  uint32_t observationSeq = 0;
  uint64_t offAt = 0;
  unsigned writesAfter = 0;
  Native::fsyncHook = [&] {
    const auto rows = finalObservationRecords();
    const auto &last = rows.back();
    if (!stalled && last.kind == 10) {
      assert(last.detail == 1 && last.pulse == 1);
      assert(WaterTest::physicalPump()); // The same step has started another pulse.
      const auto &decision = WaterTest::testController->output();
      assert(decision.pump_on && std::isfinite(decision.pulse_budget_s));
      assert(decision.pulse_budget_s >= 2 && decision.pulse_budget_s < 25);
      observationSeq = last.seq;
      Native::clock += uint64_t(std::ceil(decision.pulse_budget_s + 1) * 1e6);
      offAt = Native::clock;
      stalled = true;
      return;
    }
    if (stalled) {
      ++writesAfter;
      assert(!WaterTest::physicalPump());
      if (writesAfter == 1) {
        assert(last.seq == observationSeq + 1 && last.kind == 3 && last.role == 0);
        assert((last.flags & 8) && !(last.flags & 2) && last.t_us == offAt);
      }
    }
  };
  for (unsigned n = 0; !stalled && n < 500; ++n)
    fresh(Native::clock / 1e6 + 2);
  Native::fsyncHook = {};
  assert(stalled && writesAfter && WaterTest::active());
  assert(!WaterTest::physicalPump() && !WaterTest::testController->output().pump_on);
  assert(tempControl.pump.edgeTimes.back() == offAt);
  assert(WaterTest::controllerObservations[0].completed() == 1);
  assert(WaterTest::controllerObservations[0].interrupted == 0);
  verifyObservationJournal(1, 1, 0);
}

void controllerDurationBoundaryBeforeReboot() {
  bool interrupted = false;
  Native::fsyncHook = [] {
    if (!WaterTest::journal || WaterTest::program.controllerRun != 2) return;
    const auto rows = finalObservationRecords();
    if (rows.empty()) return;
    const auto &last = rows.back();
    if (last.kind == 9 && last.role == 2 && last.pulse == 1) {
      assert((last.flags & 128) && (last.flags & 1));
      assert(!WaterTest::physicalPump() && !WaterTest::controllerCheckpointed[1]);
      throw Native::Yield{};
    }
  };
  try {
    dualFullDurationRegression();
  } catch (const Native::Yield &) {
    interrupted = true;
  }
  Native::fsyncHook = {};
  assert(interrupted && !WaterTest::physicalPump());
  assert(WaterTest::controllerCheckpointed[0] && !WaterTest::controllerCheckpointed[1]);
  assert(!fs_exists(WaterTest::controllerTwoPath) && !fs_exists(WaterTest::finishPath));
  WaterTest::closeJournal();
}

void recoveredControllerDurationBoundary() {
  assert(!WaterTest::active() && WaterTest::terminal["outcome"] == "interrupted");
  const auto second = WaterTest::terminal["controllers"][1];
  assert(second["status"] == "completed" && second["reason"] == "duration_complete");
  assert(second["run_duration_complete"] == true);
  assert(second["ended_us"].is<uint64_t>() && second["target_c_exact"].is<const char *>());
  assert(second["observations"]["completed"].as<unsigned>() >= 1);
  assert(second["observations"]["interrupted"] == 0);
  assert(second["ended_us"].as<uint64_t>() - second["started_us"].as<uint64_t>() == 10800000000ULL);
  assert(second["learning_status"] == "unavailable_after_restart" && second["final_tuning"].isNull());
  assertNoEpisodeFlags(second);
  assert(WaterTest::terminal["controller_final_observation"]["completed"] == false);
  verifyObservationJournal(2, second["observations"]["completed"].as<unsigned>(), 0);
  submit();
  JsonDocument fixture;
  fixture["manifest"] = WaterTest::manifest;
  fixture["finish"] = WaterTest::terminal;
  auto records = fixture["records"].to<JsonArray>();
  for (const auto &payload : Native::payloads) {
    JsonDocument uploaded;
    assert(deserializeJson(uploaded, payload) == DeserializationError::Ok);
    for (JsonObjectConst row : uploaded["records"].as<JsonArrayConst>()) records.add(row);
  }
  std::string serialized;
  serializeJson(fixture, serialized);
  writeFile("/recovered-duration-boundary.json", serialized.c_str());
}

// Recovery belongs to the immutable recorded plan, including two-hour v3 runs
// created before the firmware's default increased to three hours.
void controllerDeclaredDurationRecoveryRegression() {
  start();
  assert(controllerSeconds == 10800);
  auto plans = WaterTest::manifest["test_program"]["controllers"].as<JsonArray>();
  plans[0]["max_duration_s"] = 7200;
  assert(plans[1]["max_duration_s"] == 10800);
  JsonDocument boundaries[controllerRunCount];
  unsigned settled[controllerRunCount]{};
  WaterTest::ObservationCounts observations[controllerRunCount]{};
  for (unsigned run = 0; run < controllerRunCount; ++run) {
    boundaries[run]["started_us"] = uint64_t(1000000);
    boundaries[run]["ended_us"] = uint64_t(7201000000);
    boundaries[run]["run_duration_complete"] = true;
  }
  WaterTest::recoverControllers(boundaries, settled, observations);
  const auto first = WaterTest::terminal["controllers"][0];
  const auto second = WaterTest::terminal["controllers"][1];
  assert(first["status"] == "completed" && first["reason"] == "duration_complete");
  assert(first["run_duration_complete"] == true && first["checkpoint_unavailable"] == true);
  assert(second["status"] == "interrupted" && second["reason"] == "reboot_interrupted");
  assert(second["run_duration_complete"] == false && second["checkpoint_unavailable"] == true);

  boundaries[1]["ended_us"] = uint64_t(10801000000);
  WaterTest::recoverControllers(boundaries, settled, observations);
  assert(WaterTest::terminal["controllers"][1]["status"] == "completed");
  assert(WaterTest::terminal["controllers"][1]["run_duration_complete"] == true);

  boundaries[1]["run_duration_complete"] = false;
  WaterTest::recoverControllers(boundaries, settled, observations);
  assert(WaterTest::terminal["controllers"][1]["status"] == "interrupted");
  assert(WaterTest::terminal["controllers"][1]["run_duration_complete"] == false);
}
