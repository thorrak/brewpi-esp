#pragma once

// Production wrapper integration: actual controller allocation, durable run
// snapshots, record serialization, transition admission and reboot recovery.
void dualMeasured(double value) {
  auto &p = WaterTest::program;
  Native::clock += 2000000;
  const double now = Native::clock / 1e6;
  const auto phase = p.phase;
  p.sample(now, std::round(value * 16) / 16, true, now);
  p.tick(now);
  WaterTest::applyProgram(phase);
}

void dualWaitForTransition() {
  auto &p = WaterTest::program;
  const double temperature = p.latestC;
  for (unsigned n = 0; p.active() && !p.controllerTransitionReady && n < 1000; ++n)
    dualMeasured(temperature);
  assert(p.active() && p.phase == Phase::ControllerTransition && p.controllerTransitionReady);
}

void verifyFirstDualSnapshot(JsonVariantConst result) {
  assert(result["run"] == 1 && result["status"] == "completed");
  assert(result["run_duration_complete"] == true && result["reason"] == "duration_complete");
  assert(result["initialized"] == true && result["learning_status"] == "learned");
  assert(result["ended_us"].as<uint64_t>() > result["started_us"].as<uint64_t>());
  assert(result["start_c"].as<double>() > result["target_c"].as<double>());
  if (result["selection"] == "pulse_dose") {
    assert(result["final_tuning"]["learning_updates"] == 9);
    assertExact(result["final_tuning_exact"]["gain_c_per_on_s"], .013123456789012345);
  } else {
    assert(result["selection"] == "predictive_coast");
    assert(result["final_tuning"]["learning_updates"] == 7);
    assertExact(result["final_tuning_exact"]["coast_s"], 300.12345678901234);
  }
}

void dualControllerRegression(const std::string &scenario) {
  const bool doseFirst = scenario == "dual_controllers_dose_first";
  const bool headroomSkip = scenario == "dual_controller_headroom_skip";
  const bool checkpointFailure = scenario == "dual_controller_checkpoint_failure";
  const bool corruptCheckpoint = scenario == "dual_controller_corrupt_checkpoint";
  extendedSettings.glycolCoolingAlgorithm = doseFirst ? GlycolCooling::Algorithm::PulseDose
                                                   : GlycolCooling::Algorithm::PredictiveCoast;
  tempControl.glycolRuntime.cooling.minOn = 7;
  tempControl.glycolRuntime.cooling.minOff = 11;
  start();
  const auto planned = WaterTest::manifest["test_program"]["controllers"].as<JsonArrayConst>();
  assert(planned.size() == 2 && WaterTest::manifest["test_program"]["order"] == "selected_first");
  assert(planned[0]["selection"] == (doseFirst ? "pulse_dose" : "predictive_coast"));
  assert(planned[1]["selection"] == (doseFirst ? "predictive_coast" : "pulse_dose"));
  for (auto initial : planned) {
    assert(initial["initialization"] == "fresh_defaults");
    assert(initial["initial_tuning"]["learning_updates"] == 0);
    assert(initial["configuration"]["min_on_s"] == 7 && initial["configuration"]["min_off_s"] == 11);
  }
  // The recording's immutable order survives a later settings change.
  extendedSettings.glycolCoolingAlgorithm = doseFirst ? GlycolCooling::Algorithm::PredictiveCoast
                                                   : GlycolCooling::Algorithm::PulseDose;
  auto &p = WaterTest::program;
  if (headroomSkip)
    p.sample(Native::clock / 1e6, 17.3125, true, Native::clock / 1e6);
  p.usefulResponse = true;
  p.beginController(Native::clock / 1e6);
  WaterTest::applyProgram(Phase::Baseline);
  WaterTest::serviceProgram(true);
  assert(WaterTest::testController && p.controllerRun == 1);
  assert(WaterTest::testController->selection() == (doseFirst ? GlycolCooling::Algorithm::PulseDose
                                                           : GlycolCooling::Algorithm::PredictiveCoast));
  auto learned = WaterTest::testController->tuning();
  learned.predictive = {300.12345678901234, .013123456789012345, 7, 11};
  learned.pulse_dose = {.013123456789012345, 9};
  assert(WaterTest::testController->restoreTuning(learned));
  if (checkpointFailure)
    Native::openFailurePath = std::string(WaterTest::controllerOnePath) + ".tmp";
  Native::clock += 2000000;
  p.sample(Native::clock / 1e6, p.targetC, true, Native::clock / 1e6);
  p.deadline = p.controllerRunDeadline = Native::clock / 1e6;
  p.tick(Native::clock / 1e6);
  WaterTest::applyProgram(Phase::Controller);
  assert(!WaterTest::physicalPump());
  if (checkpointFailure) {
    assert(!WaterTest::active() && WaterTest::terminal["reason"] == "recording_failure");
    assert(p.controllerRun == 1 && !WaterTest::controllerCheckpointed[0]);
    assert(WaterTest::terminal["controllers"][1]["status"] == "skipped");
    assert(WaterTest::recordedControllerStarts == 1);
    Native::openFailurePath.clear();
    submit();
    return;
  }
  assert(WaterTest::active() && p.phase == Phase::ControllerTransition);
  assert(WaterTest::controllerCheckpointed[0] && WaterTest::recordedControllerFinishes == 1);
  JsonDocument first;
  assert(WaterTest::loadControllerCheckpoint(1, first));
  verifyFirstDualSnapshot(first.as<JsonVariantConst>());
  assertExact(first["target_c_exact"], p.targetC);
  std::string firstBytes;
  assert(WaterTest::loadDocumentPayload(WaterTest::controllerOnePath, firstBytes));
  if (scenario == "dual_first_checkpoint_before_reboot") {
    WaterTest::closeJournal();
    return;
  }
  dualWaitForTransition();
  const double nextStart = Native::clock / 1e6, nextTarget = p.latestC - controllerTargetDropC;
  WaterTest::serviceProgram(true);
  if (headroomSkip) {
    assert(!WaterTest::active() && WaterTest::terminal["reason"] == "controller_headroom");
    assert(p.controllerRun == 1);
    verifyFirstDualSnapshot(WaterTest::terminal["controllers"][0]);
    assert(WaterTest::terminal["controllers"][1]["status"] == "skipped");
    assert(WaterTest::terminal["controllers"][1]["reason"] == "controller_headroom");
    assert(WaterTest::terminal["controllers"][1]["started_us"].isNull());
    submit();
    return;
  }
  assert(p.controllerRun == 2 && p.controllerStarted == nextStart && p.targetC == nextTarget);
  assert(!WaterTest::testController && p.controllerCycles == 0);
  assert(!p.controllerRunDurationComplete && WaterTest::recordedControllerStarts == 2);
  WaterTest::serviceProgram(true);
  assert(WaterTest::testController && WaterTest::testController->selection() ==
         (doseFirst ? GlycolCooling::Algorithm::PredictiveCoast : GlycolCooling::Algorithm::PulseDose));
  assert(WaterTest::testController->minOnSeconds() == 7 && WaterTest::testController->minOffSeconds() == 11);
  const auto freshTuning = WaterTest::testController->tuning();
  assert(freshTuning.predictive.learning_updates == 0 && freshTuning.predictive.response_updates == 0);
  assert(freshTuning.pulse_dose.learning_updates == 0);
  std::string retained;
  assert(WaterTest::loadDocumentPayload(WaterTest::controllerOnePath, retained) && retained == firstBytes);
  if (scenario == "dual_second_running_before_reboot") {
    WaterTest::closeJournal();
    return;
  }
  if (corruptCheckpoint)
    writeFile(WaterTest::controllerOnePath, "corrupted checkpoint");
  Native::clock += 2000000;
  p.sample(Native::clock / 1e6, p.targetC, true, Native::clock / 1e6);
  p.deadline = p.controllerRunDeadline = Native::clock / 1e6;
  p.tick(Native::clock / 1e6);
  WaterTest::applyProgram(Phase::Controller);
  finishControllerFinalObservation();
  assert(!WaterTest::active() && WaterTest::terminal["controllers"].size() == 2);
  if (corruptCheckpoint) {
    const auto missing = WaterTest::terminal["controllers"][0];
    assert(missing["status"] == "interrupted" && missing["reason"] == "checkpoint_unavailable");
    assert(missing["checkpoint_unavailable"] == true && missing["run_duration_complete"] != true);
    assert(missing["initialized"].isNull() && missing["final_tuning"].isNull());
  } else
    verifyFirstDualSnapshot(WaterTest::terminal["controllers"][0]);
  assert(WaterTest::terminal["controllers"][1]["run"] == 2);
  assert(WaterTest::terminal["controllers"][1]["selection"] == planned[1]["selection"]);
  assert(WaterTest::terminal["controllers"][1]["initialized"] == true);
  assert(WaterTest::terminal["controllers"][1]["learning_status"] == "no_updates");
  assert(WaterTest::terminal["controllers"][1]["run_duration_complete"] == true);
  assertExact(WaterTest::terminal["controllers"][1]["target_c_exact"], nextTarget);
  assert(WaterTest::terminal["controller_timed_out"].isNull());
  assert(WaterTest::terminal["controller_episodes_required"].isNull());
  verifyJournal();
  submit();
  JsonDocument uploaded;
  assert(deserializeJson(uploaded, Native::payloads.back()) == DeserializationError::Ok);
  if (corruptCheckpoint)
    assert(uploaded["controllers"][0]["status"] == "interrupted");
  else
    verifyFirstDualSnapshot(uploaded["controllers"][0]);
  assert(uploaded["controllers"][1]["run"] == 2);
}

void recoveredDualControllerRegression(const std::string &scenario) {
  assert(!WaterTest::active() && WaterTest::terminal["outcome"] == "interrupted");
  assert(WaterTest::terminal["controllers"].size() == 2);
  verifyFirstDualSnapshot(WaterTest::terminal["controllers"][0]);
  const auto second = WaterTest::terminal["controllers"][1];
  const bool started = scenario == "recovered_dual_second_running_after_reboot";
  assert(second["status"] == (started ? "interrupted" : "skipped"));
  assert(second["selection"] == "pulse_dose" && second["run_duration_complete"] != true);
  assert(second["learning_status"] == (started ? "unavailable_after_restart" : "not_started"));
  assert(second["final_tuning"].isNull() && second["final_tuning_exact"].isNull());
  if (started) {
    assert(second["started_us"].is<uint64_t>() && second["target_c_exact"].is<const char *>());
    assert(second["controller_stable_at_end"].isNull() && second["controller_timed_out"].isNull());
  } else
    assert(second["started_us"].isNull());
  std::string persisted, inResult;
  assert(WaterTest::loadDocumentPayload(WaterTest::controllerOnePath, persisted));
  serializeJson(WaterTest::terminal["controllers"][0], inResult);
  assert(persisted == inResult);
  submit();
}

// Normal acquisition-driven runs must reach their own full three-hour limit.
// A flat target trace and any number of completed observations cannot end early.
void dualFullDurationRegression() {
  start();
  auto &p = WaterTest::program;
  p.usefulResponse = true;
  p.beginController(Native::clock / 1e6);
  WaterTest::applyProgram(Phase::Baseline);
  for (unsigned run = 1; run <= controllerRunCount; ++run) {
    assert(p.controllerRun == run && p.phase == Phase::Controller);
    const double started = p.controllerStarted, target = p.targetC;
    const auto raw = int16_t(std::lround(target * 16));
    WaterTest::serviceProgram(true);
    for (unsigned n = 0; p.phase == Phase::Controller && n < controllerSeconds; ++n) {
      fresh(Native::clock / 1e6 + 2, raw);
      if (Native::clock / 1e6 < started + controllerSeconds)
        assert(p.phase == Phase::Controller && p.controllerRun == run);
      assert(p.targetC == target);
    }
    assert(p.controllerRunEnded - started == controllerSeconds && p.controllerRunDurationComplete);
    JsonDocument checkpoint;
    assert(WaterTest::loadControllerCheckpoint(run, checkpoint));
    assert(checkpoint["status"] == "completed" && checkpoint["reason"] == "duration_complete");
    assert(checkpoint["run_duration_complete"] == true);
    assert(checkpoint["observations"]["goal"] == 3);
    if (run == 1) {
      assert(p.phase == Phase::ControllerTransition);
      dualWaitForTransition();
      WaterTest::serviceProgram(true);
      assert(p.controllerRun == 2);
      assert(WaterTest::recordedControllerObservation == 0);
      assert(WaterTest::controllerObservations[1].completed() == 0);
      assert(WaterTest::controllerObservations[0].completed() == checkpoint["observations"]["completed"].as<unsigned>());
    }
  }
  finishControllerFinalObservation();
  verifyDurableFinalObservation();
  assert(WaterTest::terminal["controller_completed"].isNull());
  assert(WaterTest::terminal["controller_timed_out"].isNull());
  for (JsonObjectConst result : WaterTest::terminal["controllers"].as<JsonArrayConst>()) {
    assert(result["status"] == "completed" && result["run_duration_complete"] == true);
    assert(result["controller_episodes_completed"].isNull());
  }
  submit();
}

// Explicit v2 fixture construction preserves old journal/recovery semantics;
// the v3 runtime must never synthesize these retired episode boundaries.
void legacyControllerEpisodesBeforeReboot(bool dual) {
  start();
  auto plan = WaterTest::manifest["test_program"].as<JsonObject>();
  plan["controller_plan_version"] = 2;
  for (JsonObject controller : plan["controllers"].as<JsonArray>()) {
    controller.remove("completion_policy");
    controller.remove("observation_goal");
    controller["max_duration_s"] = 7200; // Historical v2 plan retains its original limit.
    controller["required_episodes"] = 3;
    controller["maintenance_warming_c"] = .0625;
    controller["episode_plan"] = "initial_approach_then_two_natural_maintenance_cycles";
  }
  plan["controller"] = plan["controllers"][0];
  assert(WaterTest::saveDocument(WaterTest::manifestPath, WaterTest::manifest));
  auto &p = WaterTest::program;
  p.usefulResponse = true;
  auto boundary = [&](bool finished) {
    Record row{};
    row.kind = 9;
    row.role = p.controllerRun;
    row.code = static_cast<uint8_t>(WaterTest::algorithmForRun(p.controllerRun));
    row.pulse = finished ? 1 : 0;
    row.read_us = uint64_t(std::llround((finished ? p.controllerRunEnded : p.controllerStarted) * 1e6));
    row.raw = std::lround(p.controllerRunStartC * 16);
    uint64_t bits;
    memcpy(&bits, &p.targetC, sizeof(bits));
    row.conversion_us = uint32_t(bits);
    row.detail = uint32_t(bits >> 32);
    assert(WaterTest::append(row));
  };
  for (unsigned run = 1; run <= (dual ? 2U : 1U); ++run) {
    p.startControllerRun(Native::clock / 1e6, run);
    WaterTest::recordPhase();
    boundary(false);
    const double started = p.controllerStarted;
    for (unsigned episode = 1; episode <= (dual ? 3U : 1U); ++episode) {
      for (unsigned event = 0; event <= 1; ++event) {
        Native::clock += 2000000;
        Record row{};
        row.kind = 8;
        row.role = run;
        row.pulse = episode;
        row.code = event;
        row.detail = static_cast<uint32_t>(WaterTest::algorithmForRun(run));
        row.read_us = Native::clock;
        assert(WaterTest::append(row));
      }
    }
    if (dual && run == 1) {
      WaterTest::testController = WaterTest::makeTestController(p.minimumOn, p.minimumOff,
                                                              WaterTest::algorithmForRun(run));
      auto learned = WaterTest::testController->tuning();
      learned.predictive = {300.12345678901234, .013123456789012345, 7, 11};
      assert(WaterTest::testController->restoreTuning(learned));
      p.controllerRunEnded = Native::clock / 1e6;
      boundary(true);
      JsonDocument result;
      WaterTestControllerSnapshot::final(result.to<JsonObject>(), WaterTest::initialController(run),
          WaterTest::testController.get(), false, uint64_t(std::llround(started * 1e6)), p.targetC, Native::clock);
      result["test_id"] = WaterTest::manifest["test_id"];
      result["boot_id"] = WaterTest::recordingBoot;
      result["run"] = 1;
      result["started_us"] = uint64_t(std::llround(started * 1e6));
      result["ended_us"] = Native::clock;
      result["start_c"] = p.controllerRunStartC;
      result["status"] = "completed";
      result["reason"] = "controller_complete";
      result["controller_completed"] = true;
      result["controller_timed_out"] = false;
      result["controller_episodes_completed"] = 3;
      result["controller_episodes_required"] = 3;
      result["controller_initial_settled"] = true;
      result["controller_stable_at_end"] = true;
      result["controller_observation_complete"] = true;
      assert(WaterTest::saveDocument(WaterTest::controllerOnePath, result));
      Native::clock += 180000000;
    }
  }
  WaterTest::closeJournal();
}

void recoveredLegacyControllerEpisodes() {
  assert(!WaterTest::active() && WaterTest::terminal["outcome"] == "interrupted");
  const auto first = WaterTest::terminal["controllers"][0];
  assert(first["controller_episodes_completed"] == 1 && first["controller_episodes_required"] == 3);
  assert(first["controller_initial_settled"] == true && first["controller_observation_complete"] == false);
  assert(first["controller_completed"] == false && first["controller_stable_at_end"].isNull());
  verifyJournal();
  submit();
}

void recoveredDualSettledBeforeCheckpointRegression() {
  assert(!WaterTest::active() && WaterTest::terminal["outcome"] == "interrupted");
  const auto first = WaterTest::terminal["controllers"][0];
  const auto second = WaterTest::terminal["controllers"][1];
  assert(first["status"] == "completed" && first["controller_completed"] == true);
  assert(first["initialized"] == true && first["learning_status"] == "learned");
  assert(first["final_tuning"]["learning_updates"] == 7);
  assertExact(first["final_tuning_exact"]["coast_s"], 300.12345678901234);
  assert(second["status"] == "interrupted" && second["controller_completed"] == false);
  assert(second["learning_status"] == "unavailable_after_restart");
  assert(second["final_tuning"].isNull() && second["final_tuning_exact"].isNull());
  assert(second["controller_stable_at_end"].isNull() && second["controller_timed_out"].isNull());
  assert(second["started_us"].is<uint64_t>() && second["ended_us"].isNull());
  for (JsonObjectConst result : WaterTest::terminal["controllers"].as<JsonArrayConst>()) {
    assert(result["controller_episodes_completed"] == 3);
    assert(result["controller_initial_settled"] == true && result["controller_observation_complete"] == true);
  }
  assert(WaterTest::terminal["controller_episodes_completed"] == 6);
  assert(WaterTest::terminal["controller_episodes_required"] == 6);
  assert(WaterTest::terminal["controller_observation_complete"] == true);
  assert(WaterTest::terminal["controller_completed"] == false);
  assert(WaterTest::terminal["controller_timed_out"] == false);
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
  writeFile("/recovered-controller-observation.json", serialized.c_str());
}

void dualStaleRunRegression() {
  start();
  auto &p = WaterTest::program;
  p.usefulResponse = true;
  p.beginController(Native::clock / 1e6);
  WaterTest::applyProgram(Phase::Baseline);
  WaterTest::serviceProgram(true);
  fresh(Native::clock / 1e6 + 2, int16_t(std::lround(p.targetC * 16)));
  Native::clock += uint64_t(freshnessSeconds * 1e6) + 1000;
  WaterTest::tick();
  assert(!WaterTest::active() && WaterTest::terminal["reason"] == "beer_sensor_stale");
  assert(WaterTest::terminal["controllers"][0]["run_duration_complete"] != true);
  assert(WaterTest::terminal["controllers"][0]["controller_stable_at_end"].isNull());
  assert(WaterTest::terminal["controllers"][1]["status"] == "skipped");
  submit();
}
