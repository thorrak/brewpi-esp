using namespace WaterTestCore;
void initializeHardware() {
  auto &beer = eepromManager.devices[0];
  beer.deviceFunction = DEVICE_BEER_TEMP;
  beer.deviceHardware = DEVICE_HARDWARE_ONEWIRE_TEMP;
  beer.hw.address[0] = 0x28;
  beer.hw.address[7] = 1;
  auto &bath = eepromManager.devices[1];
  bath.deviceFunction = DEVICE_CHAMBER_TEMP;
  bath.deviceHardware = DEVICE_HARDWARE_ONEWIRE_TEMP;
  bath.hw.address[0] = 0x28;
  bath.hw.address[7] = 2;
  auto &cool = eepromManager.devices[2];
  cool.deviceFunction = DEVICE_CHAMBER_COOL;
  cool.deviceHardware = DEVICE_HARDWARE_PIN;
  cool.hw.pinNr = 26;
  auto &heat = eepromManager.devices[3];
  heat.deviceFunction = DEVICE_CHAMBER_HEAT;
  heat.deviceHardware = DEVICE_HARDWARE_PIN;
  heat.hw.pinNr = 25;
  WaterTest::init();
}
void fresh(double t, int16_t raw = 320, bool valid = true, bool bathValid = true) {
  Native::clock = static_cast<uint64_t>(t * 1e6);
  WaterTest::onSample(WaterTest::addressOf(eepromManager.devices[0]), raw, valid, Native::clock - 750000,
                      Native::clock);
  WaterTest::onSample(WaterTest::addressOf(eepromManager.devices[1]), 112, bathValid, Native::clock - 750000,
                      Native::clock);
  WaterTest::tick();
}
void advance(double t, int16_t raw = 320) {
  while (Native::clock / 1e6 < t)
    fresh(std::min(t, Native::clock / 1e6 + 2), raw);
}
JsonDocument survey() {
  JsonDocument d;
  d["consent"] = true;
  d["water_confirmed"] = true;
  d["water_volume_l"] = 18.9;
  d["fermenter_model"] = "Native water test";
  d["fermenter_capacity_l"] = 26.5;
  d["cooling_type"] = "immersion_coil";
  d["probe_mounting"] = "thermowell";
  d["glycol_temperature_source"] = "chamber_probe";
  d["reported_chiller_setpoint_c"] = nullptr;
  return d;
}
void start() {
  fresh(std::max(2.0, Native::clock / 1e6 + 1));
  auto d = survey();
  std::string error;
  assert(WaterTest::requestStart(d.as<JsonVariantConst>(), error));
  assert(WaterTest::controlOwned());
  WaterTest::tick();
  assert(WaterTest::active());
  assert(!WaterTest::physicalPump());
  assert(WaterTest::manifest["test_id"].as<std::string>().size() == 36);
  assert(WaterTest::manifest["test_id"].as<std::string>().find('\0') == std::string::npos);
  assert(std::distance(std::filesystem::directory_iterator(Native::root),
                       std::filesystem::directory_iterator{}) == 1);
  assert(fs_exists(WaterTest::journalPath));
}
void uploadOnce() {
  Native::delays = 1;
  try {
    WaterTest::uploader(nullptr);
  } catch (const Native::Yield &) {
  }
}
void finishEligibleStopped() {
  advance(WaterTest::program.started + 310);
  assert(WaterTest::active() && !WaterTest::physicalPump());
  assert(WaterTest::program.completedPulses == 1 && WaterTest::program.totalPump == 10);
  std::string error;
  assert(WaterTest::requestStop(error));
  WaterTest::tick();
  assert(WaterTest::terminal["outcome"] == "stopped");
  assert(WaterTest::terminal["completed_pulses"] == 1);
  assert(WaterTest::uploadState == "pending");
}
void submit() {
  const auto requests = Native::payloads.size();
  for (unsigned n = 0; n < 600 && WaterTest::uploadState != "submitted"; ++n)
    uploadOnce();
  assert(WaterTest::uploadState == "submitted");
  assert(Native::payloads.size() > requests);
  assert(std::filesystem::is_empty(Native::root));
}
void submitAndResume() {
  finishEligibleStopped();
  submit();
  JsonDocument release;
  std::string error;
  assert(WaterTest::requestResume(release, error));
  WaterTest::tick();
  assert(!WaterTest::controlOwned());
}
void verifyNotSubmittedAndRestart(unsigned completedPulses = 0) {
  assert(!WaterTest::active() && WaterTest::controlOwned());
  assert(!WaterTest::physicalPump() && !WaterTest::physicalHeat());
  assert(WaterTest::program.completedPulses == completedPulses);
  assert(WaterTest::terminal["completed_pulses"] == completedPulses);
  assert(WaterTest::uploadState == "not_submitted");
  const auto requests = Native::payloads.size();
  for (unsigned n = 0; n < 3; ++n)
    uploadOnce();
  assert(Native::payloads.size() == requests);
  JsonDocument status;
  WaterTest::status(status);
  assert(status["upload_status"] == "not_submitted");
  assert(status["completed_pulses"] == completedPulses && status["can_start"] == false);
  const std::string previous = WaterTest::manifest["test_id"];
  auto request = survey();
  std::string error;
  assert(!WaterTest::requestStart(request, error));
  Native::fsyncDelayUs = 0;
  Native::fsyncUntilFail = -1;
  Native::truncateHook = {};
  JsonDocument release;
  const auto resumes = Native::resumes;
  assert(WaterTest::requestResume(release, error));
  WaterTest::tick();
  assert(!WaterTest::controlOwned() && Native::resumes == resumes + 1 && tempControl.cs.mode == 'b');
  fresh(Native::clock / 1e6 + 1);
  WaterTest::status(status);
  assert(status["upload_status"] == "not_submitted" && status["can_start"] == true);
  uploadOnce();
  assert(Native::payloads.size() == requests);
  assert(WaterTest::requestStart(request, error));
  WaterTest::tick();
  assert(WaterTest::active() && WaterTest::controlOwned());
  assert(WaterTest::manifest["test_id"] != previous);
  assert(WaterTest::program.completedPulses == 0 && WaterTest::terminal.isNull());
}
void writeFile(const char *path, const char *contents) {
  FILE *file = fs_open(path, "wb");
  assert(file);
  assert(fwrite(contents, 1, strlen(contents), file) == strlen(contents));
  fclose(file);
}
void verifyIdleAfterReboot() {
  assert(!WaterTest::active() && !WaterTest::controlOwned());
  assert(WaterTest::manifest.isNull() && WaterTest::terminal.isNull());
  assert(WaterTest::uploadState == "idle" && WaterTest::uploadError.empty());
  assert(WaterTest::reason.empty() && WaterTest::program.phase == Phase::Idle);
  assert(tempControl.cs.mode == 'b' && tempControl.cs.beerSetting == 10000);
  assert(tempControl.cs.fridgeSetting == 5000 && tempControl.cs.heatEstimator == 128 &&
         tempControl.cs.coolEstimator == 256 && Native::resumes == 0);
  uploadOnce();
  assert(Native::payloads.empty());
  JsonDocument status;
  WaterTest::status(status);
  assert(status["test_id"].isNull() && status["outcome"] == "");
  assert(status["phase"] == "idle" && status["upload_status"] == "idle");
}
void verifyJournal() {
  FILE *f = fs_open(WaterTest::journalPath, "rb");
  assert(f);
  Record r;
  unsigned expected = 0;
  while (fread(&r, sizeof(r), 1, f) == 1) {
    assert(valid(r));
    assert(r.boot == 0 && r.seq == expected++);
  }
  fclose(f);
}
int main(int argc, char **argv) {
  assert(argc == 3);
  Native::root = argv[2];
  std::filesystem::create_directories(Native::root);
  std::string scenario = argv[1];
  if (scenario == "cleanup_failure_after_reboot") {
    Native::removeFailurePath = WaterTest::journalPath;
    tempControl.cooler->setActive(true);
  } else if (scenario == "metadata_cleanup_failure_after_reboot") {
    Native::removeFailurePath = "/water-test-manifest.json";
    tempControl.heater->setActive(true);
  }
  initializeHardware();
  std::string error;
  if (scenario == "slow_sensor_fault" || scenario == "slow_temperature_limit") {
    if (scenario == "slow_sensor_fault")
      tempControl.glycolRuntime.cooling.minOn = 60;
    start();
    advance(302);
    if (scenario == "slow_sensor_fault") {
      fresh(332, -2048, false);
      assert(WaterTest::active() && WaterTest::physicalPump());
    }
    Native::fsyncDelayUs = 12000000;
    const uint64_t readAt = scenario == "slow_sensor_fault" ? 332100000 : 302100000;
    fresh(readAt / 1e6, scenario == "slow_temperature_limit" ? 272 : 320, scenario != "slow_sensor_fault");
    assert(!WaterTest::active() && !WaterTest::physicalPump());
    assert(tempControl.pump.edgeTimes.back() == readAt);
    assert(WaterTest::terminal["reason"] ==
           (scenario == "slow_temperature_limit" ? "temperature_limit" : "beer_sensor_stale"));
    FILE *journal = fs_open(WaterTest::journalPath, "rb");
    Record record{};
    bool foundEdge = false;
    while (fread(&record, sizeof(record), 1, journal) == 1)
      if (record.kind == 3 && record.role == 0 && (record.flags & 8) && !(record.flags & 2)) {
        assert(record.t_us == readAt);
        foundEdge = true;
      }
    fclose(journal);
    assert(foundEdge);
    verifyJournal();
    verifyNotSubmittedAndRestart();
  } else if (scenario == "slow_deadline") {
    start();
    advance(310);
    Native::clock = 311000000;
    Native::fsyncDelayUs = 3000000;
    for (unsigned n = 0; n < 6; ++n)
      WaterTest::onSample(WaterTest::beerAddress, 320, true, 310000000, 310500000 + n);
    WaterTest::tick();
    assert(!WaterTest::physicalPump());
    // A synchronous write cannot be interrupted. Recheck immediately afterward,
    // rather than allowing the remaining queue to extend the pulse further.
    assert(tempControl.pump.edgeTimes.back() == 314000000);
    verifyJournal();
  } else if (scenario == "slow_on_edge" || scenario == "slow_phase") {
    start();
    advance(300);
    Native::clock = 302000000;
    Native::fsyncDelayUs = scenario == "slow_on_edge" ? 12000000 : 6000000;
    WaterTest::tick();
    assert(!WaterTest::physicalPump());
    assert(tempControl.pump.edgeTimes.size() == 2);
    assert(tempControl.pump.edgeTimes.front() == 302000000);
    assert(tempControl.pump.edgeTimes.back() == 314000000);
    verifyJournal();
  } else if (scenario == "slow_storage_repair") {
    start();
    advance(302);
    Native::fsyncUntilFail = 0;
    Native::truncateHook = [] {
      assert(!WaterTest::physicalPump() && !WaterTest::physicalHeat());
      Native::clock += 12000000;
    };
    fresh(302.1);
    assert(!WaterTest::active());
    assert(tempControl.pump.edgeTimes.back() == 302100000);
    assert(WaterTest::terminal["reason"] == "recording_failure");
    verifyJournal();
  } else if (scenario == "queue_snapshot") {
    start();
    Native::clock = 3000000;
    uint64_t nextRead = Native::clock;
    Native::fsyncHook = [&] {
      WaterTest::onSample(WaterTest::beerAddress, 320, true, 2250000, ++nextRead);
    };
    WaterTest::onSample(WaterTest::beerAddress, 320, true, 2250000, Native::clock);
    WaterTest::onSample(WaterTest::glycolAddress, 112, true, 2250000, Native::clock);
    WaterTest::tick();
    assert(WaterTest::active());
    assert(uxQueueMessagesWaiting(WaterTest::samples) == 2);
    Native::fsyncHook = {};
    verifyJournal();
  } else if (scenario == "slow_queue_overflow" || scenario == "queued_unexpected_output") {
    start();
    advance(302);
    Native::clock = 302100000;
    Native::fsyncDelayUs = 12000000;
    const unsigned count = scenario == "slow_queue_overflow" ? 65 : 1;
    for (unsigned n = 0; n < count; ++n)
      WaterTest::onSample(WaterTest::beerAddress, 320, true, 301350000, Native::clock + n);
    if (scenario == "queued_unexpected_output")
      tempControl.heater->setActive(true);
    WaterTest::tick();
    assert(!WaterTest::physicalPump() && !WaterTest::physicalHeat());
    assert(tempControl.pump.edgeTimes.back() == 302100000);
    assert(WaterTest::terminal["reason"] ==
           (scenario == "slow_queue_overflow" ? "sample_queue_overflow" : "unexpected_output"));
    verifyJournal();
  } else if (scenario == "cleanup_failure") {
    start();
    submitAndResume();
    const std::string previous = WaterTest::manifest["test_id"];
    writeFile(WaterTest::journalPath, "stale recording");
    Native::removeFailurePath = WaterTest::journalPath;
    fresh(Native::clock / 1e6 + 1);
    auto d = survey();
    assert(WaterTest::requestStart(d, error));
    WaterTest::tick();
    assert(!WaterTest::active() && !WaterTest::controlOwned());
    assert(WaterTest::manifest["test_id"] == previous);
    assert(tempControl.cs.mode == 'b');
    assert(WaterTest::reason.find("Unable to remove") != std::string::npos);
    assert(fs_exists(WaterTest::journalPath));
    Native::removeFailurePath.clear();
    assert(WaterTest::requestStart(d, error));
    WaterTest::tick();
    assert(WaterTest::active());
    assert(WaterTest::manifest["test_id"] != previous);
    assert(!WaterTest::manifestUploaded && WaterTest::uploadedRecords == 0);
  } else if (scenario == "cleanup_failure_after_reboot" || scenario == "metadata_cleanup_failure_after_reboot") {
    verifyIdleAfterReboot();
    assert(fs_exists(Native::removeFailurePath.c_str()));
    assert(tempControl.cooler->isActive() || tempControl.heater->isActive());
    tempControl.cooler->setActive(false);
    tempControl.heater->setActive(false);
    fresh(2);
    auto d = survey();
    assert(WaterTest::requestStart(d, error));
    WaterTest::tick();
    assert(!WaterTest::active() && !WaterTest::controlOwned());
    assert(tempControl.cs.mode == 'b');
    assert(WaterTest::reason.find("Unable to remove") != std::string::npos);
    uploadOnce();
    assert(Native::payloads.empty());
    Native::removeFailurePath.clear();
    start();
  } else if (scenario == "discarded_after_reboot") {
    verifyIdleAfterReboot();
    assert(std::filesystem::is_empty(Native::root));
    start();
    verifyJournal();
  } else if (scenario == "corrupt_before_reboot") {
    writeFile(WaterTest::journalPath, "torn");
    for (auto path : {"/water-test-manifest.json", "/water-test-boots.json", "/water-test-finish.json",
                      "/water-test-ack.json", "/water-test-resumed.json", "/water-test-reserve.bin"}) {
      writeFile(path, "invalid");
      writeFile((std::string(path) + ".tmp").c_str(), "invalid");
    }
  } else if (scenario == "submitted_before_reboot") {
    start();
    submitAndResume();
  } else if (scenario == "normal_stop") {
    start();
    advance(302);
    assert(WaterTest::physicalPump());
    advance(307);
    assert(WaterTest::requestStop(error));
    WaterTest::tick();
    assert(!WaterTest::active() && !WaterTest::physicalPump());
    assert(WaterTest::terminal["outcome"] == "stopped");
    assert(WaterTest::terminal["reason"] == "user_stop");
    assert(WaterTest::controlOwned());
    verifyJournal();
    verifyNotSubmittedAndRestart();
  } else if (scenario == "baseline_stop") {
    start();
    fresh(3);
    assert(WaterTest::requestStop(error));
    WaterTest::tick();
    assert(WaterTest::terminal["outcome"] == "stopped");
    assert(tempControl.pump.edges.empty());
    verifyJournal();
    verifyNotSubmittedAndRestart();
  } else if (scenario == "full_pulse_stop") {
    start();
    finishEligibleStopped();
    assert(WaterTest::program.phase == Phase::Finished && Native::clock == 312000000);
    verifyJournal();
    submit();
  } else if (scenario == "resume_pending_upload") {
    start();
    finishEligibleStopped();
    JsonDocument resume;
    Native::fsyncUntilFail = 0;
    Native::openFailurePath = WaterTest::journalPath;
    Native::removeFailurePath = WaterTest::journalPath;
    assert(WaterTest::requestResume(resume, error));
    WaterTest::tick();
    assert(!WaterTest::controlOwned() && tempControl.cs.mode == 'b' && Native::resumes == 1);
    assert(WaterTest::uploadState == "pending");
    Native::fsyncUntilFail = -1;
    Native::openFailurePath.clear();
    Native::removeFailurePath.clear();
    for (unsigned n = 0; n < 100 && WaterTest::uploadState != "submitted"; ++n)
      uploadOnce();
    assert(WaterTest::uploadState == "submitted");
    assert(!WaterTest::controlOwned() && tempControl.cs.mode == 'b');
    assert(std::filesystem::is_empty(Native::root));
  } else if (scenario == "minimum_stop") {
    start();
    advance(302);
    assert(WaterTest::physicalPump());
    assert(WaterTest::requestStop(error));
    WaterTest::tick();
    assert(WaterTest::physicalPump());
    fresh(303);
    assert(WaterTest::physicalPump());
    fresh(304);
    assert(!WaterTest::physicalPump());
    assert(WaterTest::terminal["outcome"] == "stopped");
    assert(WaterTest::program.totalPump == 2);
    verifyJournal();
    verifyNotSubmittedAndRestart();
  } else if (scenario == "transient_sensor_errors") {
    start();
    advance(302);
    fresh(302.1, -2048, false, false);
    assert(WaterTest::active() && WaterTest::physicalPump());
    assert(WaterTest::program.lastSample == 302 && WaterTest::program.latestC == 20);
    JsonDocument status;
    WaterTest::status(status);
    assert(status["beer_c"] == 20 && status["glycol_c"] == 7);
    assert(status["preflight"]["beer_available"] == true);
    assert(status["preflight"]["chamber_available"] == true);
    FILE *journal = fs_open(WaterTest::journalPath, "rb");
    Record record{};
    unsigned badReadings = 0;
    while (fread(&record, sizeof(record), 1, journal) == 1)
      if (record.kind == 2 && record.read_us == 302100000) {
        assert(!(record.flags & 1) && (record.flags & 2));
        assert(record.raw == (record.role == 0 ? -2048 : 112));
        ++badReadings;
      }
    fclose(journal);
    assert(badReadings == 2);
    fresh(304, 319);
    assert(WaterTest::active() && WaterTest::physicalPump());
    assert(WaterTest::program.lastSample == 304 && WaterTest::program.latestC == 19.9375);
    WaterTest::status(status);
    assert(status["beer_c"] == 19.9375);
    verifyJournal();
  } else if (scenario == "transient_bad_start") {
    fresh(2);
    fresh(3, -2048, false, false);
    JsonDocument status;
    WaterTest::status(status);
    assert(status["can_start"] == true && status["beer_c"] == 20);
    assert(status["preflight"]["chamber_available"] == true);
    auto request = survey();
    assert(WaterTest::requestStart(request, error));
    WaterTest::tick();
    assert(WaterTest::active() && WaterTest::program.initialC == 20);
    assert(WaterTest::program.lastSample == 2);
    WaterTest::status(status);
    assert(status["beer_c"] == 20 && status["glycol_c"] == 7);
  } else if (scenario == "prolonged_invalid" || scenario == "silent_sensor") {
    tempControl.glycolRuntime.cooling.minOn = 60;
    start();
    advance(302);
    if (scenario == "prolonged_invalid") {
      for (double at = 304; at <= 332; at += 2) {
        fresh(at, -2048, false);
        assert(WaterTest::active() && WaterTest::physicalPump());
        assert(WaterTest::program.lastSample == 302);
      }
      fresh(332.1, -2048, false);
    } else {
      Native::clock = 332000000;
      WaterTest::tick();
      assert(WaterTest::active() && WaterTest::physicalPump());
      Native::clock = 332100000;
      WaterTest::tick();
    }
    assert(!WaterTest::active() && !WaterTest::physicalPump());
    assert(tempControl.pump.edgeTimes.back() == 332100000);
    assert(WaterTest::terminal["outcome"] == "failed");
    assert(WaterTest::terminal["reason"] == "beer_sensor_stale");
    verifyJournal();
    verifyNotSubmittedAndRestart();
  } else if (scenario == "minimum_water_limit") {
    tempControl.glycolRuntime.cooling.minOn = 60;
    start();
    advance(302);
    fresh(302.1, 64);
    assert(!WaterTest::physicalPump() && tempControl.pump.edgeTimes.back() == 302100000);
    assert(WaterTest::terminal["outcome"] == "stopped");
    assert(WaterTest::terminal["reason"] == "temperature_limit");
    verifyJournal();
    verifyNotSubmittedAndRestart();
  } else if (scenario == "bath_fault") {
    start();
    advance(302);
    fresh(302.1, 320, true, false);
    assert(WaterTest::active() && WaterTest::physicalPump());
    verifyJournal();
  } else if (scenario == "independent_hardware_availability") {
    JsonDocument status;
    auto check = [&](bool configured, bool beer, bool chamber, bool cooler) {
      WaterTest::status(status);
      assert(status["preflight"]["beer_configured"] == configured);
      assert(status["preflight"]["beer_available"] == beer);
      assert(status["preflight"]["chamber_available"] == chamber);
      assert(status["preflight"]["cooler_available"] == cooler);
    };
    check(true, false, false, true);
    fresh(2);
    check(true, true, true, true);
    const auto beer = eepromManager.devices[0];
    const auto bath = eepromManager.devices[1];
    const auto cool = eepromManager.devices[2];
    eepromManager.devices[0].deviceFunction = DEVICE_NONE;
    check(false, false, true, true);
    eepromManager.devices[0] = beer;
    eepromManager.devices[0].hw.address[0] = 0x10;
    check(false, false, true, true);
    eepromManager.devices[0] = beer;
    eepromManager.devices[0].deviceHardware = DEVICE_HARDWARE_PIN;
    check(false, false, true, true);
    eepromManager.devices[0] = beer;
    eepromManager.devices[0].hw.deactivate = true;
    check(false, false, true, true);
    eepromManager.devices[0] = beer;
    eepromManager.devices[2].deviceFunction = DEVICE_NONE;
    check(true, true, true, false);
    eepromManager.devices[0].deviceFunction = DEVICE_NONE;
    check(false, false, true, false);
    eepromManager.devices[0] = beer;
    eepromManager.devices[2] = cool;
    tempControl.cooler = nullptr;
    check(true, true, true, false);
    tempControl.cooler = &tempControl.pump;
    eepromManager.devices[2].deviceHardware = DEVICE_HARDWARE_ONEWIRE_TEMP;
    check(true, true, true, false);
    eepromManager.devices[2] = cool;
    extendedSettings.glycol = false;
    check(true, true, true, true);
    assert(status["preflight"]["ready"] == false);
    extendedSettings.glycol = true;
    eepromManager.devices[1].hw.address[0] = 0x10;
    check(true, true, false, true);
    eepromManager.devices[1] = bath;
    Native::clock += 31000000;
    check(true, false, false, true);
    assert(status["preflight"]["reason"] == "Waiting for a fresh valid beer probe reading.");
    fresh(Native::clock / 1e6, 320, false);
    check(true, false, true, true);
  } else if (scenario == "configured_glycol_probe") {
    fresh(2);
    auto request = survey();
    const auto bath = eepromManager.devices[1];
    eepromManager.devices[1].deviceFunction = DEVICE_NONE;
    assert(WaterTest::requestStart(request, error));
    WaterTest::tick();
    assert(!WaterTest::active() && !WaterTest::controlOwned());
    assert(WaterTest::reason == "Configure a DS18B20 glycol probe before starting.");
    eepromManager.devices[1] = bath;
    fresh(33, 320, true, false);
    assert(WaterTest::requestStart(request, error));
    WaterTest::tick();
    assert(!WaterTest::active() && !WaterTest::controlOwned());
    assert(WaterTest::reason == "Waiting for a fresh valid glycol bath probe reading.");
    fresh(34);
    assert(WaterTest::requestStart(request, error));
    WaterTest::tick();
    assert(WaterTest::active());
    JsonDocument resume;
    assert(!WaterTest::requestResume(resume, error));
    assert(WaterTest::requestStop(error));
    WaterTest::tick();
    assert(WaterTest::requestResume(resume, error));
    WaterTest::tick();
    assert(!WaterTest::controlOwned() && tempControl.cs.mode == 'b');
  } else if (scenario == "status_freshness") {
    start();
    JsonDocument visible;
    WaterTest::status(visible);
    assert(visible["beer_c"].is<double>() && visible["glycol_c"].is<double>());
    Native::clock += 31000000;
    WaterTest::status(visible);
    assert(visible["beer_c"].isNull() && visible["glycol_c"].isNull());
    fresh(Native::clock / 1e6);
    WaterTest::status(visible);
    assert(visible["beer_c"].is<double>() && visible["glycol_c"].is<double>());
  } else if (scenario == "fsync_failure") {
    start();
    advance(302);
    Native::fsyncUntilFail = 0;
    fresh(302.1);
    assert(!WaterTest::physicalPump());
    assert(WaterTest::terminal["reason"] == "recording_failure");
    assert(WaterTest::terminal["lost_ranges"].size() == 1);
    verifyJournal();
    verifyNotSubmittedAndRestart();
  } else if (scenario == "edge_fsync_failure") {
    start();
    advance(300);
    Native::clock = 302000000;
    Native::fsyncUntilFail = 0;
    WaterTest::tick();
    assert(!WaterTest::physicalPump());
    assert(WaterTest::terminal["reason"] == "recording_failure");
    verifyJournal();
    verifyNotSubmittedAndRestart();
  } else if (scenario == "start_failure") {
    fresh(2);
    auto d = survey();
    Native::openFailurePath = WaterTest::journalPath;
    assert(WaterTest::requestStart(d, error));
    WaterTest::tick();
    assert(!WaterTest::active() && !WaterTest::controlOwned() && WaterTest::manifest.isNull());
    assert(tempControl.cs.mode == 'b');
    Native::openFailurePath.clear();
    assert(WaterTest::requestStart(d, error));
    WaterTest::tick();
    assert(WaterTest::active());
  } else if (scenario == "queue_overflow") {
    start();
    advance(302);
    for (unsigned n = 0; n < 65; ++n)
      WaterTest::onSample(WaterTest::beerAddress, 320, true, Native::clock - 750000, Native::clock + n + 1);
    Native::clock += 100;
    WaterTest::tick();
    assert(!WaterTest::physicalPump());
    assert(WaterTest::terminal["reason"] == "sample_queue_overflow");
    verifyJournal();
  } else if (scenario == "unexpected_output") {
    start();
    tempControl.heater->setActive(true);
    WaterTest::tick();
    assert(!WaterTest::physicalHeat());
    assert(WaterTest::terminal["reason"] == "unexpected_output");
    verifyJournal();
  } else if (scenario == "active_before_reboot") {
    start();
    advance(302);
    assert(WaterTest::physicalPump());
    WaterTest::closeJournal();
    FILE *f = fs_open(WaterTest::journalPath, "ab");
    fwrite("torn", 1, 4, f);
    fclose(f);
  } else if (scenario == "upload_retry") {
    start();
    finishEligibleStopped();
    assert(fs_exists(WaterTest::journalPath));
    Native::nextHttpCode = 500;
    uploadOnce();
    assert(WaterTest::uploadState == "error" && fs_exists(WaterTest::journalPath));
    auto original = Native::payloads.back();
    Native::nextHttpCode = 201;
    uploadOnce();
    assert(Native::payloads.back() == original);
    assert(WaterTest::uploadState == "pending" && fs_exists(WaterTest::journalPath));
    Native::nextHttpCode = 500;
    uploadOnce();
    original = Native::payloads.back();
    Native::nextHttpCode = 201;
    uploadOnce();
    assert(Native::payloads.back() == original);
    for (unsigned n = 0; n < 100 && WaterTest::uploadState != "submitted"; ++n)
      uploadOnce();
    assert(WaterTest::uploadState == "submitted");
    assert(!fs_exists(WaterTest::journalPath));
    assert(std::filesystem::is_empty(Native::root));
  } else if (scenario == "stopped_before_reboot" || scenario == "pending_before_reboot" ||
             scenario == "partial_upload_before_reboot" || scenario == "resumed_pending_before_reboot") {
    start();
    finishEligibleStopped();
    if (scenario == "pending_before_reboot") {
      Native::nextHttpCode = 500;
      uploadOnce();
      assert(WaterTest::uploadState == "error");
    } else if (scenario == "partial_upload_before_reboot") {
      uploadOnce();
      uploadOnce();
      assert(WaterTest::manifestUploaded && WaterTest::uploadedRecords == 12);
    } else if (scenario == "resumed_pending_before_reboot") {
      JsonDocument resume;
      assert(WaterTest::requestResume(resume, error));
      WaterTest::tick();
      assert(tempControl.cs.mode == 'b' && !WaterTest::controlOwned());
    }
    assert(fs_exists(WaterTest::journalPath));
  } else if (scenario == "all_pulses") {
    start();
    advance(4002);
    assert(!WaterTest::active() && !WaterTest::physicalPump());
    assert(WaterTest::program.pulse == 3);
    assert(WaterTest::terminal["outcome"] == "inconclusive");
    assert(WaterTest::program.totalPump == 100);
    verifyJournal();
    verifyNotSubmittedAndRestart(3);
  } else if (scenario == "completed") {
    start();
    advance(302);
    advance(4002, 318);
    assert(!WaterTest::active() && !WaterTest::physicalPump());
    assert(WaterTest::terminal["outcome"] == "completed");
    assert(WaterTest::terminal["completed_pulses"] == 3);
    assert(WaterTest::uploadState == "pending");
    verifyJournal();
    submit();
  } else if (scenario == "failure_after_full_pulse" || scenario == "stale_stop_after_full_pulse") {
    start();
    advance(312);
    assert(WaterTest::active() && WaterTest::program.completedPulses == 1);
    if (scenario == "failure_after_full_pulse") {
      tempControl.heater->setActive(true);
    } else {
      Native::clock = 342100000;
      assert(WaterTest::requestStop(error));
    }
    WaterTest::tick();
    assert(WaterTest::terminal["outcome"] == "failed");
    assert(WaterTest::terminal["reason"] ==
           (scenario == "failure_after_full_pulse" ? "unexpected_output" : "beer_sensor_stale"));
    verifyJournal();
    verifyNotSubmittedAndRestart(1);
  } else
    assert(false);
  std::cout << "water_test_backend: " << scenario << " passed\n";
}
