#pragma once

// Include after the backend helpers and invoke in a fresh native scenario.
// This exercises the real selected controller, not a hand-written pump deadline.
void controllerRunWriteDeadlineRegression() {
  extendedSettings.glycolCoolingAlgorithm = GlycolCooling::Algorithm::PredictiveCoast;
  start();
  fresh(Native::clock / 1e6 + 2);
  WaterTest::program.usefulResponse = true;
  WaterTest::program.beginController(Native::clock / 1e6);
  assert(WaterTest::program.phase == Phase::Controller);
  assert(!WaterTest::testController && !WaterTest::physicalPump());

  bool stalledBoundary = false;
  unsigned writesAfterBoundary = 0;
  uint32_t boundarySequence = 0;
  uint64_t offExpectedUs = 0;
  Native::fsyncHook = [&] {
    FILE *file = fs_open(WaterTest::journalPath, "rb");
    assert(file);
    Record record{}, last{};
    bool found = false;
    while (fread(&record, sizeof(record), 1, file) == 1) {
      last = record;
      found = true;
    }
    fclose(file);
    assert(found);
    if (!stalledBoundary && last.kind == 9 && last.pulse == 0) {
      assert(last.role == 1);
      assert(WaterTest::testController && WaterTest::physicalPump());
      const auto &decision = WaterTest::testController->output();
      const double budget = decision.pulse_budget_s;
      assert(decision.pump_on && std::isfinite(budget) && budget >= 2 && budget < 20);
      boundarySequence = last.seq;
      // One synchronous journal write cannot be interrupted. As soon as it
      // returns, the selected algorithm's budget has already expired. This is
      // still well inside the sensor-freshness, stage, and hard pulse limits.
      Native::clock += uint64_t(std::llround(std::ceil(budget + 1) * 1e6));
      offExpectedUs = Native::clock;
      stalledBoundary = true;
      return;
    }
    if (stalledBoundary) {
      ++writesAfterBoundary;
      // OFF must be physically applied before any subsequent durable write,
      // including observation draining or a controller telemetry record.
      assert(!WaterTest::physicalPump());
      if (writesAfterBoundary == 1) {
        assert(last.seq == boundarySequence + 1);
        assert(last.kind == 3 && last.role == 0 && (last.flags & 8) && !(last.flags & 2));
        assert(last.t_us == offExpectedUs);
      }
    }
  };

  WaterTest::serviceProgram(true);
  Native::fsyncHook = {};
  assert(stalledBoundary && writesAfterBoundary >= 1);
  assert(WaterTest::active() && WaterTest::program.phase == Phase::Controller);
  assert(!WaterTest::physicalPump() && !WaterTest::testController->output().pump_on);
  assert(WaterTest::program.controllerCycles == 1);
  assert(tempControl.pump.edgeTimes.size() == 2);
  assert(tempControl.pump.edgeTimes.back() == offExpectedUs);
  assert(WaterTest::recordedControllerObservation == 0);
  assert(WaterTest::controllerObservations[0].completed() == 0);
  verifyJournal();
}
