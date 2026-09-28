int main(int argc, char **argv) {
  assert(argc == 3);
  Native::root = argv[2];
  std::filesystem::create_directories(Native::root);
  initializeHardware();
  const std::string scenario = argv[1];
  if (scenario == "snapshot_failure" || scenario == "held_retry_failure") {
    const bool retryHeld = scenario == "held_retry_failure";
    WaterTest::startupHold = retryHeld;
    fresh(2);
    ControllerMemoryTest::failAllocation = true;
    auto request = survey();
    std::string error;
    assert(WaterTest::requestStart(request.as<JsonVariantConst>(), error));
    WaterTest::tick();
    assert(ControllerMemoryTest::attempts == 1);
    assert(!WaterTest::active());
    assert(WaterTest::controlOwned() == retryHeld);
    assert(!WaterTest::physicalPump() && !WaterTest::physicalHeat());
    assert(WaterTest::manifest.isNull() && !WaterTest::journal);
    assert(std::filesystem::is_empty(Native::root));
    assert(WaterTest::reason.find("Not enough free memory") != std::string::npos);
    assert(tempControl.cs.mode == 'b');
    JsonDocument status;
    WaterTest::status(status);
    assert(status["active"] == false);
    assert(status["control_owned"].as<bool>() == retryHeld);
  } else {
    const bool dose = scenario == "release_dose";
    if (dose)
      extendedSettings.glycolCoolingAlgorithm = GlycolCooling::Algorithm::PulseDose;
    start();
    WaterTest::program.phase = Phase::Controller;
    WaterTest::program.role = Role::Controller;
    WaterTest::program.controllerStarted = Native::clock / 1e6;
    WaterTest::program.targetC = 19.75;
    WaterTest::program.deadline = Native::clock / 1e6 + controllerSeconds;
    if (scenario == "phase_failure") {
      const auto previousRecords = WaterTest::recordCount;
      ControllerMemoryTest::failAllocation = true;
      WaterTest::program.pump = true;
      WaterTest::program.pulseStarted = Native::clock / 1e6;
      tempControl.cooler->setActive(true);
      WaterTest::tick();
      assert(!WaterTest::active() && WaterTest::controlOwned());
      assert(!WaterTest::physicalPump() && !WaterTest::physicalHeat());
      assert(tempControl.cs.mode == Modes::off);
      assert(WaterTest::recordCount > previousRecords);
      assert(fs_exists(WaterTest::journalPath));
      assert(WaterTest::terminal["outcome"] == "failed");
      assert(WaterTest::terminal["controller"]["initialized"] == false);
      assert(WaterTest::reason.find("Not enough free memory") != std::string::npos);
      JsonDocument saved;
      assert(WaterTest::loadDocument(WaterTest::finishPath, saved));
      assert(saved["error_detail"] == WaterTest::terminal["error_detail"]);
      assert(saved["error_detail"].as<std::string>().find("retained") != std::string::npos);
      JsonDocument status;
      WaterTest::status(status);
      assert(status["active"] == false && status["control_owned"] == true);
      assert(status["can_start"] == false && status["can_resume"] == true);
    } else {
      WaterTest::testController = WaterTest::makeTestController(2, 2, WaterTest::testAlgorithm);
      assert(WaterTest::testController);
      auto tuning = WaterTest::testController->tuning();
      tuning.predictive.coast_s = 400;
      tuning.predictive.budget_gain_c_per_s = .0123456789012345;
      tuning.predictive.learning_updates = 2;
      tuning.pulse_dose.gain_c_per_on_s = .0145678901234567;
      tuning.pulse_dose.learning_updates = 3;
      assert(WaterTest::testController->restoreTuning(tuning));
      bool releasedBeforeFinishWrite = false;
      Native::fsyncHook = [&]() {
        if (WaterTest::terminal["controller"]["initialized"] == true) {
          assert(!WaterTest::testController);
          releasedBeforeFinishWrite = true;
        }
      };
      std::string error;
      assert(WaterTest::requestStop(error));
      WaterTest::tick();
      assert(releasedBeforeFinishWrite && !WaterTest::testController);
      assert(WaterTest::terminal["controller"]["initialized"] == true);
      assert(WaterTest::terminal["controller"]["learning_status"] == "learned");
      const auto exact = WaterTest::terminal["controller"]["final_tuning_exact"];
      if (dose)
        assertExact(exact["gain_c_per_on_s"], tuning.pulse_dose.gain_c_per_on_s);
      else {
        assertExact(exact["coast_s"], tuning.predictive.coast_s);
        assertExact(exact["budget_gain_c_per_s"], tuning.predictive.budget_gain_c_per_s);
      }
      assert(!WaterTest::active() && WaterTest::controlOwned());
      assert(!WaterTest::physicalPump() && !WaterTest::physicalHeat());
    }
  }
  std::cout << scenario << " passed\n";
}
