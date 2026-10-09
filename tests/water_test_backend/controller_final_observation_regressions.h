#pragma once

// Exercise the production queue, relay application, durable journal, snapshot,
// and terminal paths rather than satisfying the tail through core-only reads.
void finishControllerFinalObservation() {
  auto &p = WaterTest::program;
  assert(p.phase == Phase::ControllerFinalObserve && WaterTest::active());
  const auto raw = int16_t(std::lround(p.latestC * 16));
  const double ended = p.controllerRunEnded;
  const double started = p.controllerFinalObservationStarted;
  const auto step = WaterTest::lastControllerStep;
  for (unsigned n = 0; WaterTest::active() && n <= controllerFinalObservationMaxSeconds; ++n)
    fresh(Native::clock / 1e6 + 2, raw);
  assert(!WaterTest::active() && p.controllerFinalObservationCompleted);
  assert(p.controllerRunEnded == ended && WaterTest::lastControllerStep == step);
  assert(p.controllerFinalObservationEnded - started >= controllerFinalObservationSeconds);
  assert(p.controllerFinalObservationSamples >= controllerFinalObservationRequiredSamples);
  assert(!WaterTest::physicalPump() && !WaterTest::physicalHeat());
}

std::vector<Record> finalObservationRecords() {
  std::vector<Record> result;
  FILE *file = fs_open(WaterTest::journalPath, "rb");
  assert(file);
  Record row;
  while (fread(&row, sizeof(row), 1, file) == 1) {
    assert(valid(row) && row.seq == result.size());
    result.push_back(row);
  }
  fclose(file);
  return result;
}

void startFinalObservationFixture(bool delayWrite = false, bool fractionalDeadline = false) {
  start();
  auto &p = WaterTest::program;
  p.usefulResponse = true;
  p.beginController(Native::clock / 1e6);
  WaterTest::applyProgram(Phase::Baseline);
  WaterTest::serviceProgram(true);
  assert(WaterTest::testController && p.controllerRun == 1);
  Native::clock += 2000000;
  p.sample(Native::clock / 1e6, p.targetC, true, Native::clock / 1e6);
  p.deadline = p.controllerRunDeadline = Native::clock / 1e6;
  p.tick(Native::clock / 1e6);
  WaterTest::applyProgram(Phase::Controller);
  assert(WaterTest::controllerCheckpointed[0]);
  const auto raw = int16_t(std::lround(p.latestC * 16));
  for (unsigned n = 0; p.controllerRun == 1 && WaterTest::active() && n < 1000; ++n)
    fresh(Native::clock / 1e6 + 2, raw);
  assert(p.controllerRun == 2 && p.phase == Phase::Controller);
  WaterTest::serviceProgram(true);
  assert(WaterTest::testController && WaterTest::physicalPump());
  Native::clock += 2000000;
  p.sample(Native::clock / 1e6, p.latestC, true, Native::clock / 1e6);
  const uint64_t offUs = Native::clock;
  bool delayed = false;
  if (delayWrite) {
    Native::fsyncHook = [&] {
      if (!delayed && p.phase == Phase::ControllerFinalObserve) {
        assert(!WaterTest::physicalPump());
        delayed = true;
        Native::clock += 20000000;
      }
    };
  }
  const auto lastStep = WaterTest::lastControllerStep;
  if (fractionalDeadline) {
    p.deadline = p.controllerRunDeadline = Native::clock / 1e6;
    p.lastDecision = p.deadline - .1;
    WaterTest::serviceProgram(true);
  } else {
    p.deadline = p.controllerRunDeadline = Native::clock / 1e6;
    p.tick(Native::clock / 1e6);
    WaterTest::applyProgram(Phase::Controller);
  }
  Native::fsyncHook = {};
  assert(!delayWrite || delayed);
  assert(WaterTest::active() && p.phase == Phase::ControllerFinalObserve);
  assert(!WaterTest::physicalPump() && tempControl.pump.edgeTimes.back() == offUs);
  assert(p.controllerFinalOffConfirmed && !p.controllerFinalObservationCompleted);
  assert(p.controllerFinalObservationStarted == offUs / 1e6);
  assert(p.controllerRunEnded == offUs / 1e6 && p.controllerRunDurationComplete);
  assert(p.controllerFinalObservationSamples == 0);
  assert(WaterTest::controllerCheckpointed[1] && WaterTest::recordedControllerFinishes == 2);
  assert(WaterTest::lastControllerStep == lastStep);
  assert(!fs_exists(WaterTest::finishPath));
}

void verifyDurableFinalObservation() {
  auto &p = WaterTest::program;
  const auto tail = WaterTest::terminal["controller_final_observation"];
  assert(tail["run"] == 2 && tail["completed"] == true);
  const uint64_t startUs = tail["started_us"], endUs = tail["ended_us"], lastUs = tail["last_valid_read_us"];
  assert(startUs == uint64_t(std::llround(p.controllerFinalObservationStarted * 1e6)));
  assert(endUs >= startUs + controllerFinalObservationSeconds * 1000000ULL);
  assert(lastUs >= startUs + controllerFinalObservationSeconds * 1000000ULL && lastUs <= endUs);
  unsigned count = 0;
  uint64_t lastRead = 0;
  bool seenRunFinish = false, seenTail = false;
  for (const auto &row : finalObservationRecords()) {
    if (row.kind == 9 && row.role == 2 && row.pulse == 1) {
      seenRunFinish = true;
      assert(row.read_us == uint64_t(std::llround(p.controllerRunEnded * 1e6)));
    }
    if (row.kind == 4 && row.code == uint8_t(Phase::ControllerFinalObserve))
      seenTail = true;
    if (row.kind == 2 && row.role == 0 && (row.flags & 1) && row.read_us > startUs) {
      assert(seenRunFinish && seenTail && !(row.flags & 2));
      assert(row.read_us > lastRead && row.read_us <= row.t_us);
      lastRead = row.read_us;
      ++count;
    }
    if (seenRunFinish)
      assert(row.kind != 7 && row.kind != 10 && !(row.kind == 3 && row.role == 0 && (row.flags & 2)));
  }
  assert(count >= controllerFinalObservationRequiredSamples);
  assert(count == tail["valid_beer_samples"].as<unsigned>() && count == p.controllerFinalObservationSamples);
  assert(lastRead == lastUs);
  assert(WaterTest::terminal["outcome"] == "completed");
  assert(WaterTest::terminal["controllers"][1]["status"] == "completed");
  assert(WaterTest::terminal["controllers"][1]["ended_us"].as<uint64_t>() <= startUs);
  assert(WaterTest::terminal["final_seq_by_boot"][WaterTest::recordingBoot] == WaterTest::recordCount - 1);
}

void controllerFinalObservationRegression(const std::string &scenario) {
  startFinalObservationFixture(scenario == "controller_final_slow_write",
                              scenario == "controller_final_fractional_deadline");
  auto &p = WaterTest::program;
  const auto raw = int16_t(std::lround(p.latestC * 16));
  const double began = p.controllerFinalObservationStarted;
  std::string checkpoint;
  assert(WaterTest::loadDocumentPayload(WaterTest::controllerTwoPath, checkpoint));
  // Tail samples must remain durable after all dense allowance is consumed and
  // when their acquisition time does not qualify for the ordinary sparse path.
  WaterTest::denseRecords = WaterTest::denseRecordBudget;
  WaterTest::nextSparseRead[0] = UINT64_MAX;
  if (scenario == "controller_final_filters") {
    Native::clock = uint64_t(std::llround((began + 1) * 1e6));
    const uint64_t oldRead = uint64_t(std::llround((began - .5) * 1e6));
    WaterTest::processSample({WaterTest::beerAddress, oldRead - 750000, oldRead, raw, true});
    assert(p.controllerFinalObservationSamples == 0);
    WaterTest::processSample({WaterTest::beerAddress, Native::clock - 750000, Native::clock, raw, false});
    assert(p.controllerFinalObservationSamples == 0);
    fresh(began + 2, raw);
    assert(p.controllerFinalObservationSamples == 1);
    const auto count = WaterTest::recordCount;
    WaterTest::processSample({WaterTest::beerAddress, Native::clock - 750000, Native::clock, raw, true});
    assert(p.controllerFinalObservationSamples == 1 && WaterTest::recordCount == count);
    fresh(began + 3, raw, false);
    assert(p.controllerFinalObservationSamples == 1);
  } else if (scenario == "controller_final_stop") {
    std::string error;
    assert(WaterTest::requestStop(error));
    WaterTest::tick();
    assert(!WaterTest::active() && WaterTest::terminal["reason"] == "user_stop");
  } else if (scenario == "controller_final_stale") {
    Native::clock += uint64_t(freshnessSeconds * 1e6) + 1;
    WaterTest::serviceProgram(false);
    assert(!WaterTest::active() && WaterTest::terminal["reason"] == "beer_sensor_stale");
  } else if (scenario == "controller_final_temperature") {
    fresh(began + 1, int16_t(minimumWaterC * 16));
    assert(!WaterTest::active() && WaterTest::terminal["reason"] == "temperature_limit");
  } else if (scenario == "controller_final_unexpected_output") {
    tempControl.pump.setActive(true);
    WaterTest::serviceProgram(false);
    assert(!WaterTest::active() && WaterTest::terminal["reason"] == "unexpected_output");
  } else if (scenario == "controller_final_write_failure") {
    for (unsigned n = 1; n < controllerFinalObservationRequiredSamples; ++n)
      fresh(began + n * 15, raw);
    assert(p.controllerFinalObservationSamples == controllerFinalObservationRequiredSamples - 1);
    Native::fsyncUntilFail = 0;
    fresh(began + controllerFinalObservationSeconds, raw);
    assert(!WaterTest::active() && WaterTest::terminal["reason"] == "recording_failure");
  } else if (scenario == "controller_final_tail_before_reboot") {
    fresh(began + 2, raw);
    assert(p.controllerFinalObservationSamples == 1 && !fs_exists(WaterTest::finishPath));
    WaterTest::closeJournal();
    return;
  }
  if (WaterTest::active()) {
    finishControllerFinalObservation();
    verifyDurableFinalObservation();
  } else {
    assert(!p.controllerFinalObservationCompleted);
    assert(WaterTest::terminal["controller_final_observation"]["completed"] == false);
    assert(!WaterTest::physicalPump() && !WaterTest::physicalHeat());
  }
  std::string retained;
  assert(WaterTest::loadDocumentPayload(WaterTest::controllerTwoPath, retained) && retained == checkpoint);
  assert(WaterTest::terminal["controllers"][1]["status"] == "completed");
  verifyJournal();
  submit();
}

void recoveredControllerFinalObservationRegression() {
  assert(!WaterTest::active() && WaterTest::terminal["outcome"] == "interrupted");
  assert(!WaterTest::physicalPump() && !WaterTest::physicalHeat());
  const auto second = WaterTest::terminal["controllers"][1];
  assert(second["run"] == 2 && second["status"] == "completed");
  assert(second["initialized"] == true && second["run_duration_complete"] == true);
  assert(second["ended_us"].is<uint64_t>() && second["final_tuning"].is<JsonObjectConst>());
  const auto observation = WaterTest::terminal["controller_final_observation"];
  assert(observation["completed"] == false && observation["ended_us"].isNull());
  assert(observation["valid_beer_samples"] == 1 && observation["reason"] == "reboot_interrupted");
  assert(observation["started_us"].as<uint64_t>() >= second["ended_us"].as<uint64_t>());
  assert(observation["last_valid_read_us"].as<uint64_t>() > observation["started_us"].as<uint64_t>());
  std::string checkpoint;
  assert(WaterTest::loadDocumentPayload(WaterTest::controllerTwoPath, checkpoint));
  JsonDocument saved;
  assert(deserializeJson(saved, checkpoint) == DeserializationError::Ok);
  assert(second["ended_us"] == saved["ended_us"] && second["target_c_exact"] == saved["target_c_exact"]);
  submit();
}
