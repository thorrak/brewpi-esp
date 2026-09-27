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
  Native::clock = static_cast<uint64_t>(std::llround(t * 1e6));
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
                       std::filesystem::directory_iterator{}) == 3);
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
  advance(WaterTest::program.started + 72);
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
  assert(!fs_exists(WaterTest::journalPath));
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
void verifyRetainedAndRestart(unsigned completedPulses = 0) {
  assert(!WaterTest::active() && WaterTest::controlOwned());
  assert(!WaterTest::physicalPump() && !WaterTest::physicalHeat());
  assert(WaterTest::program.completedPulses == completedPulses);
  assert(WaterTest::terminal["completed_pulses"] == completedPulses);
  assert(WaterTest::uploadState == "pending");
  JsonDocument status;
  WaterTest::status(status);
  assert(status["can_start"] == false);
  const std::string previous = WaterTest::manifest["test_id"];
  auto request = survey();
  std::string error;
  assert(!WaterTest::requestStart(request, error));
  Native::fsyncDelayUs = 0;
  Native::fsyncUntilFail = -1;
  Native::truncateHook = {};
  submit();
  JsonDocument release;
  const auto resumes = Native::resumes;
  assert(WaterTest::requestResume(release, error));
  WaterTest::tick();
  assert(!WaterTest::controlOwned() && Native::resumes == resumes + 1 && tempControl.cs.mode == 'b');
  assert(std::filesystem::is_empty(Native::root));
  fresh(Native::clock / 1e6 + 1);
  WaterTest::status(status);
  assert(status["can_start"] == true);
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
void assertExact(JsonVariantConst value, double expected) {
  assert(value.is<const char *>());
  const double decoded = std::strtod(value.as<const char *>(), nullptr);
  assert(memcmp(&decoded, &expected, sizeof(expected)) == 0);
}
void verifyControllerSnapshot(JsonVariantConst metadata, bool dose) {
  assert(metadata["initialized"] == true && metadata["learning_status"] == "learned");
  assert(metadata["selection"] == (dose ? "pulse_dose" : "predictive_coast"));
  assert(metadata["numeric_encoding"] == "binary64-decimal-v1");
  assert(metadata["implementation_id"] == COOLING_IMPLEMENTATION_ID);
  assert(metadata["started_us"] == 2000000);
  assertExact(metadata["target_c_exact"], std::nextafter(19.75, 20.));
  const auto tuning = metadata["final_tuning"];
  const auto exact = metadata["final_tuning_exact"];
  if (dose) {
    assertExact(exact["gain_c_per_on_s"], .013123456789012345);
    assert(tuning["learning_updates"] == 9);
  } else {
    assertExact(exact["coast_s"], 300.12345678901234);
    assertExact(exact["budget_gain_c_per_s"], .013123456789012345);
    assert(tuning["learning_updates"] == 7 && tuning["response_updates"] == 11);
  }
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
    advance(64);
    if (scenario == "slow_sensor_fault") {
      fresh(94, -2048, false);
      assert(WaterTest::active() && WaterTest::physicalPump());
    }
    Native::fsyncDelayUs = 12000000;
    const uint64_t readAt = scenario == "slow_sensor_fault" ? 94100000 : 64100000;
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
    verifyRetainedAndRestart();
  } else if (scenario == "slow_deadline") {
    start();
    advance(72);
    Native::clock = 73000000;
    Native::fsyncDelayUs = 3000000;
    WaterTest::lastLoggedRead[0] = 0;
    for (unsigned n = 0; n < 6; ++n)
      WaterTest::onSample(WaterTest::beerAddress, 320, true, 72000000, 72500000 + n);
    WaterTest::tick();
    assert(!WaterTest::physicalPump());
    // A synchronous write cannot be interrupted. Recheck immediately afterward,
    // rather than allowing the remaining queue to extend the pulse further.
    assert(tempControl.pump.edgeTimes.back() == 76000000);
    verifyJournal();
  } else if (scenario == "slow_on_edge" || scenario == "slow_phase") {
    start();
    advance(62);
    Native::clock = 64000000;
    Native::fsyncDelayUs = scenario == "slow_on_edge" ? 12000000 : 6000000;
    WaterTest::tick();
    assert(!WaterTest::physicalPump());
    assert(tempControl.pump.edgeTimes.size() == 2);
    assert(tempControl.pump.edgeTimes.front() == 64000000);
    assert(tempControl.pump.edgeTimes.back() == 76000000);
    verifyJournal();
  } else if (scenario == "slow_storage_repair") {
    start();
    advance(64);
    Native::fsyncUntilFail = 0;
    WaterTest::lastLoggedRead[0] = WaterTest::lastLoggedRead[1] = 0;
    Native::truncateHook = [] {
      assert(!WaterTest::physicalPump() && !WaterTest::physicalHeat());
      Native::clock += 12000000;
    };
    fresh(64.1);
    assert(!WaterTest::active());
    assert(tempControl.pump.edgeTimes.back() == 64100000);
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
    advance(64);
    Native::clock = 64100000;
    Native::fsyncDelayUs = 12000000;
    const unsigned count = scenario == "slow_queue_overflow" ? 65 : 1;
    for (unsigned n = 0; n < count; ++n)
      WaterTest::onSample(WaterTest::beerAddress, 320, true, 63350000, Native::clock + n);
    if (scenario == "queued_unexpected_output")
      tempControl.heater->setActive(true);
    WaterTest::tick();
    assert(!WaterTest::physicalPump() && !WaterTest::physicalHeat());
    assert(tempControl.pump.edgeTimes.back() == 64100000);
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
  } else if (scenario == "recovered_active_after_reboot" || scenario == "recovered_ended_after_reboot" ||
             scenario == "recovered_resumed_after_reboot" || scenario == "recover_only_after_reboot") {
    assert(!WaterTest::active());
    const bool resumed = scenario == "recovered_resumed_after_reboot";
    assert(WaterTest::controlOwned() == !resumed);
    assert(!WaterTest::physicalPump() && !WaterTest::physicalHeat());
    assert(WaterTest::manifest["test_id"].is<const char *>());
    assert(WaterTest::terminalDurable && WaterTest::recordCount > 0);
    assert(std::string(WaterTest::recordingBoot) != WaterTest::currentBoot);
    assert(WaterTest::uploadState == "pending");
    if (scenario == "recovered_active_after_reboot" || scenario == "recover_only_after_reboot") {
      assert(WaterTest::terminal["outcome"] == "interrupted");
      assert(WaterTest::terminal["unobserved_shutdown"] == true);
      assert(WaterTest::terminal["controller"]["initialized"].isNull());
      assert(WaterTest::terminal["controller"]["learning_status"] == "unavailable_after_restart");
      assert(WaterTest::terminal["controller"]["final_tuning"].isNull());
      assert(WaterTest::terminal["controller"]["final_tuning_exact"].isNull());
      assert(WaterTest::terminal["t_us"].isNull());
      assert(WaterTest::terminal["lost_ranges"].size() == 1);
    } else {
      assert(WaterTest::terminal["outcome"] == "stopped");
    }
    if (scenario == "recover_only_after_reboot") {
      assert(WaterTest::terminal["outcome"]=="interrupted");
      return 0;
    }
    const std::string recordedBoot = WaterTest::recordingBoot;
    submit();
    for (const auto &payload : Native::payloads) {
      JsonDocument packet;
      assert(deserializeJson(packet, payload) == DeserializationError::Ok);
      if (packet["records"].is<JsonArray>())
        assert(packet["boot_id"] == recordedBoot);
    }
    if (!resumed) {
      JsonDocument release;
      assert(WaterTest::requestResume(release, error));
      WaterTest::tick();
    }
    assert(std::filesystem::is_empty(Native::root));
    start();
  } else if (scenario == "idle_after_reboot") {
    verifyIdleAfterReboot();
    assert(std::filesystem::is_empty(Native::root));
    start();
  } else if (scenario == "blocked_after_reboot") {
    assert(WaterTest::recoveryBlocked && WaterTest::controlOwned());
    assert(!WaterTest::active() && !WaterTest::physicalPump() && !WaterTest::physicalHeat());
    assert(fs_exists(WaterTest::journalPath) && fs_exists(WaterTest::manifestPath));
    auto d = survey();
    assert(!WaterTest::requestStart(d, error));
    uploadOnce();
    assert(Native::payloads.empty());
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
    advance(64);
    assert(WaterTest::physicalPump());
    advance(69);
    assert(WaterTest::requestStop(error));
    WaterTest::tick();
    assert(!WaterTest::active() && !WaterTest::physicalPump());
    assert(WaterTest::terminal["outcome"] == "stopped");
    assert(WaterTest::terminal["reason"] == "user_stop");
    assert(WaterTest::controlOwned());
    verifyJournal();
    verifyRetainedAndRestart();
  } else if (scenario == "baseline_stop") {
    start();
    fresh(3);
    assert(WaterTest::requestStop(error));
    WaterTest::tick();
    assert(WaterTest::terminal["outcome"] == "stopped");
    assert(tempControl.pump.edges.empty());
    assert(WaterTest::terminal["controller"]["initialized"] == false);
    assert(WaterTest::terminal["controller"]["learning_status"] == "not_started");
    assert(WaterTest::terminal["controller"]["final_tuning"].isNull());
    verifyJournal();
    verifyRetainedAndRestart();
  } else if (scenario == "full_pulse_stop") {
    start();
    finishEligibleStopped();
    assert(WaterTest::program.phase == Phase::Finished && Native::clock == 74000000);
    verifyJournal();
    submit();
  } else if (scenario == "resume_pending_upload") {
    start();
    finishEligibleStopped();
    JsonDocument resume;
    Native::fsyncUntilFail = 0;
    WaterTest::lastLoggedRead[0] = WaterTest::lastLoggedRead[1] = 0;
    Native::openFailurePath = WaterTest::journalPath;
    Native::removeFailurePath = WaterTest::journalPath;
    assert(!WaterTest::requestResume(resume, error));
    assert(WaterTest::controlOwned());
    Native::fsyncUntilFail = -1;
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
    assert(!fs_exists(WaterTest::journalPath));
  } else if (scenario == "minimum_stop") {
    start();
    advance(64);
    assert(WaterTest::physicalPump());
    assert(WaterTest::requestStop(error));
    WaterTest::tick();
    assert(WaterTest::physicalPump());
    fresh(65);
    assert(WaterTest::physicalPump());
    fresh(66);
    assert(!WaterTest::physicalPump());
    assert(WaterTest::terminal["outcome"] == "stopped");
    assert(WaterTest::program.totalPump == 2);
    verifyJournal();
    verifyRetainedAndRestart();
  } else if (scenario == "transient_sensor_errors") {
    start();
    advance(64);
    fresh(64.1, -2048, false, false);
    assert(WaterTest::active() && WaterTest::physicalPump());
    assert(WaterTest::program.lastSample == 64 && WaterTest::program.latestC == 20);
    JsonDocument status;
    WaterTest::status(status);
    assert(status["beer_c"] == 20 && status["glycol_c"] == 7);
    assert(status["preflight"]["beer_available"] == true);
    assert(status["preflight"]["chamber_available"] == true);
    FILE *journal = fs_open(WaterTest::journalPath, "rb");
    Record record{};
    unsigned badReadings = 0;
    while (fread(&record, sizeof(record), 1, journal) == 1)
      if (record.kind == 2 && record.read_us == 64100000) {
        assert(!(record.flags & 1) && (record.flags & 2));
        assert(record.raw == (record.role == 0 ? -2048 : 112));
        ++badReadings;
      }
    fclose(journal);
    assert(badReadings == 2);
    fresh(66, 319);
    assert(WaterTest::active() && WaterTest::physicalPump());
    assert(WaterTest::program.lastSample == 66 && WaterTest::program.latestC == 19.9375);
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
    advance(64);
    if (scenario == "prolonged_invalid") {
      for (double at = 66; at <= 94; at += 2) {
        fresh(at, -2048, false);
        assert(WaterTest::active() && WaterTest::physicalPump());
        assert(WaterTest::program.lastSample == 64);
      }
      fresh(94.1, -2048, false);
    } else {
      Native::clock = 94000000;
      WaterTest::tick();
      assert(WaterTest::active() && WaterTest::physicalPump());
      Native::clock = 94100000;
      WaterTest::tick();
    }
    assert(!WaterTest::active() && !WaterTest::physicalPump());
    assert(tempControl.pump.edgeTimes.back() == 94100000);
    assert(WaterTest::terminal["outcome"] == "failed");
    assert(WaterTest::terminal["reason"] == "beer_sensor_stale");
    verifyJournal();
    verifyRetainedAndRestart();
  } else if (scenario == "minimum_water_limit") {
    tempControl.glycolRuntime.cooling.minOn = 60;
    start();
    advance(64);
    fresh(64.1, 64);
    assert(!WaterTest::physicalPump() && tempControl.pump.edgeTimes.back() == 64100000);
    assert(WaterTest::terminal["outcome"] == "stopped");
    assert(WaterTest::terminal["reason"] == "temperature_limit");
    verifyJournal();
    verifyRetainedAndRestart();
  } else if (scenario == "bath_fault") {
    start();
    advance(64);
    fresh(64.1, 320, true, false);
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
  } else if (scenario == "offline_12h_budget") {
    start();
    Native::connected = false;
    WaterTest::denseUntilUs = UINT64_MAX;
    const uint64_t began = WaterTest::recordStartUs;
    // Isolate the storage worst case from the sequencer's early completion:
    // every fresh two-second reading drives Program::sample for twelve hours.
    for (uint64_t elapsed=2000000; elapsed<=43200000000ULL; elapsed+=2000000) {
      Native::clock = began + elapsed;
      WaterTest::processSample({WaterTest::beerAddress,Native::clock-750000,Native::clock,320,true});
      WaterTest::processSample({WaterTest::glycolAddress,Native::clock-750000,Native::clock,112,true});
      if (elapsed % 60000000 == 0)
        WaterTest::recordOutput(true,false,false,Reason::Observation);
      assert(WaterTest::active() && !WaterTest::recordingFailed);
      assert(WaterTest::program.lastSample == Native::clock/1e6);
    }
    assert(WaterTest::denseRecords == WaterTest::denseRecordBudget);
    assert(WaterTest::recordCount < WaterTest::maxRecords - 500);
    assert(WaterTest::recordCount*sizeof(Record) < 500000);
    WaterTest::program.finish(Native::clock/1e6,End::Inconclusive,Reason::RuntimeLimit);
    WaterTest::finishRun();
    assert(WaterTest::terminalDurable && WaterTest::terminal["lost_ranges"].isNull());
    verifyJournal();
    uploadOnce();
    assert(Native::payloads.empty());
    Native::connected = true;
    for (unsigned n=0; n<1200 && WaterTest::uploadState!="submitted"; ++n) uploadOnce();
    assert(WaterTest::uploadState == "submitted");
  } else if (scenario == "finish_persistence_failure") {
    start();
    Native::openFailurePath = std::string(WaterTest::finishPath) + ".tmp";
    assert(WaterTest::requestStop(error));
    WaterTest::tick();
    assert(!WaterTest::active() && !WaterTest::terminalDurable && WaterTest::uploadState=="error");
    uploadOnce();
    assert(Native::payloads.empty() && fs_exists(WaterTest::journalPath));
    Native::openFailurePath.clear();
    uploadOnce();
    assert(WaterTest::terminalDurable && Native::payloads.size()==1);
    submit();
  } else if (scenario == "receipt_persistence_failure") {
    start();
    finishEligibleStopped();
    for (unsigned n=0;n<100 && WaterTest::uploadedRecords<WaterTest::recordCount;++n) uploadOnce();
    Native::openFailurePath = std::string(WaterTest::receiptPath) + ".tmp";
    uploadOnce();
    assert(WaterTest::uploadState=="error" && fs_exists(WaterTest::journalPath));
    const auto original=Native::payloads.back();
    Native::openFailurePath.clear();
    uploadOnce();
    assert(WaterTest::uploadState=="submitted" && Native::payloads.back()==original);
  } else if (scenario == "controller_snapshot_predictive" || scenario == "controller_snapshot_dose" ||
             scenario == "controller_snapshot_before_reboot") {
    const bool dose = scenario == "controller_snapshot_dose";
    if (dose) extendedSettings.glycolCoolingAlgorithm = GlycolCooling::Algorithm::PulseDose;
    tempControl.glycolRuntime.cooling.minOn = 7;
    tempControl.glycolRuntime.cooling.minOff = 11;
    start();
    const auto initial = WaterTest::manifest["test_program"]["controller"];
    assert(initial["initialization"] == "fresh_defaults");
    assert(initial["configuration"].size() == 19 && initial["configuration_exact"].size() == 19);
    assert(initial["configuration"]["min_on_s"] == 7 && initial["configuration"]["min_off_s"] == 11);
    assertExact(initial["configuration_exact"]["min_on_s"], 7.);
    assertExact(initial["configuration_exact"]["min_off_s"], 11.);
    assert(initial["initial_tuning"]["learning_updates"] == 0);
    assert(!WaterTest::testController);
    JsonDocument persisted;
    assert(WaterTest::loadDocument(WaterTest::manifestPath, persisted));
    std::string before, after;
    serializeJson(WaterTest::manifest, before);
    assert(WaterTest::loadDocumentPayload(WaterTest::manifestPath, after));
    assert(before == after);
    const std::string originalManifest = before;
    WaterTest::manifest = persisted;
    extendedSettings.glycolCoolingAlgorithm = dose ? GlycolCooling::Algorithm::PredictiveCoast
                                                 : GlycolCooling::Algorithm::PulseDose;
    WaterTest::program.phase = Phase::Controller;
    WaterTest::program.role = Role::Controller;
    WaterTest::program.controllerStarted = Native::clock/1e6;
    WaterTest::program.targetC = std::nextafter(19.75, 20.);
    WaterTest::program.deadline = Native::clock/1e6 + controllerSeconds;
    WaterTest::tick();
    assert(WaterTest::testController && WaterTest::testController->minOnSeconds() == 7 &&
           WaterTest::testController->minOffSeconds() == 11);
    auto learned = WaterTest::testController->tuning();
    learned.predictive = {300.12345678901234, .013123456789012345, 7, 11};
    learned.pulse_dose = {.013123456789012345, 9};
    assert(WaterTest::testController->restoreTuning(learned));
    assert(WaterTest::requestStop(error));
    WaterTest::tick();
    verifyControllerSnapshot(WaterTest::terminal["controller"], dose);
    assert(WaterTest::loadDocument(WaterTest::finishPath, persisted));
    verifyControllerSnapshot(persisted["controller"], dose);
    before.clear(); after.clear();
    serializeJson(WaterTest::terminal,before);
    assert(WaterTest::loadDocumentPayload(WaterTest::finishPath,after));
    assert(before == after); // Immutable bytes survive a durable reload.
    const std::string originalFinish = before;
    WaterTest::terminal = persisted;
    if (scenario == "controller_snapshot_before_reboot") return 0;
    submit();
    assert(Native::payloads.front() == originalManifest);
    assert(Native::payloads.back() == originalFinish);
    JsonDocument uploaded;
    assert(deserializeJson(uploaded,Native::payloads.back()) == DeserializationError::Ok);
    verifyControllerSnapshot(uploaded["controller"], dose);
  } else if (scenario == "recovered_snapshot_after_reboot") {
    assert(WaterTest::terminalDurable && !WaterTest::testController);
    verifyControllerSnapshot(WaterTest::terminal["controller"], false);
    submit();
    JsonDocument uploaded;
    assert(deserializeJson(uploaded,Native::payloads.back()) == DeserializationError::Ok);
    verifyControllerSnapshot(uploaded["controller"], false);
  } else if (scenario == "controller_fast_loop_predictive" || scenario == "controller_fast_loop_dose") {
    const bool dose = scenario == "controller_fast_loop_dose";
    if (dose) extendedSettings.glycolCoolingAlgorithm = GlycolCooling::Algorithm::PulseDose;
    start();
    WaterTest::program.phase = Phase::Controller;
    WaterTest::program.role = Role::Controller;
    WaterTest::program.controllerStarted = Native::clock/1e6;
    WaterTest::program.targetC = 18;
    WaterTest::program.deadline = Native::clock/1e6 + controllerSeconds;
    const uint64_t began = Native::clock;
    double previousControllerStep = -1;
    unsigned controllerSteps = 0;
    // brewpiLoop may run hundreds of times per second while physical probe
    // conversions still arrive every two seconds. Never flood the core's ring.
    for (uint64_t elapsed=10000;elapsed<=40000000;elapsed+=10000) {
      Native::clock = began + elapsed;
      if (elapsed % 2000000 == 0)
        fresh(Native::clock/1e6);
      else
        WaterTest::tick();
      assert(WaterTest::active() && WaterTest::testController);
      if (WaterTest::lastControllerStep != previousControllerStep) {
        assert(previousControllerStep < 0 || WaterTest::lastControllerStep - previousControllerStep >= 1.);
        previousControllerStep = WaterTest::lastControllerStep;
        ++controllerSteps;
      }
      if (elapsed >= 5000000)
        assert(WaterTest::testController->output().phase != GlycolCooling::Phase::DisabledOrSensorFault);
    }
    assert(controllerSteps >= 39 && controllerSteps <= 40);
    assert(WaterTest::program.lastSample == Native::clock/1e6);
    assert(std::isfinite(WaterTest::testController->output().temperature_c));
    if (!dose) assert(WaterTest::physicalPump());
    // Freshness safety is checked at the fast loop cadence, independently of
    // the once-per-second controller step, including between scheduled steps.
    const uint64_t lastFresh = Native::clock;
    for (uint64_t elapsed=10000;elapsed<=30000000;elapsed+=10000) {
      Native::clock = lastFresh + elapsed;
      WaterTest::tick();
      assert(WaterTest::active());
    }
    Native::clock = lastFresh + 30010000;
    const uint64_t faultAt = Native::clock;
    WaterTest::tick();
    assert(!WaterTest::active() && !WaterTest::physicalPump() && !WaterTest::physicalHeat());
    assert(WaterTest::terminal["reason"] == "beer_sensor_stale");
    if (!dose) assert(tempControl.pump.edgeTimes.back() == faultAt);
    assert(WaterTest::terminal["controller"]["initialized"] == true);
    assert(WaterTest::terminal["controller"]["learning_status"] == "no_updates");
    assert(WaterTest::terminal["controller"]["final_tuning"]["learning_updates"] == 0);
    verifyJournal();
    submit();
  } else if (scenario == "controller_continuous_pulse_limit") {
    start();
    WaterTest::program.phase = Phase::Controller;
    WaterTest::program.role = Role::Controller;
    WaterTest::program.controllerStarted = WaterTest::program.pulseStarted = Native::clock/1e6;
    WaterTest::program.switched = Native::clock/1e6;
    WaterTest::program.pump = true;
    WaterTest::program.deadline = Native::clock/1e6 + controllerSeconds;
    tempControl.cooler->setActive(true);
    WaterTest::recordOutput(true,true,true,Reason::AdditionalPulse);
    Native::clock += uint64_t(maximumPulseSeconds)*1000000;
    WaterTest::program.lastSample = Native::clock/1e6;
    WaterTest::tick();
    assert(!WaterTest::active() && !WaterTest::physicalPump());
    assert(WaterTest::terminal["reason"] == "pulse_time_limit");
    assert(WaterTest::terminal["controller_completed"] == false);
    assert(tempControl.pump.edgeTimes.back() == Native::clock);
    verifyJournal();
    submit();
  } else if (scenario == "controller_interrupted_status") {
    start();
    WaterTest::program.controllerStarted = Native::clock/1e6;
    WaterTest::program.controllerCycles = 1;
    WaterTest::program.responseSettled = true;
    assert(WaterTest::requestStop(error));
    WaterTest::tick();
    assert(WaterTest::terminal["outcome"] == "stopped");
    assert(WaterTest::terminal["controller_completed"] == false);
    submit();
  } else if (scenario == "reserve_persistence_failure") {
    fresh(2);
    auto request=survey();
    Native::fsyncUntilFail=0;
    assert(WaterTest::requestStart(request,error));
    WaterTest::tick();
    assert(!WaterTest::active() && !WaterTest::controlOwned());
    assert(tempControl.cs.mode=='b' && tempControl.pump.edges.empty());
    assert(std::filesystem::is_empty(Native::root));
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
    advance(64);
    Native::fsyncUntilFail = 0;
    WaterTest::lastLoggedRead[0] = WaterTest::lastLoggedRead[1] = 0;
    fresh(64.1);
    assert(!WaterTest::physicalPump());
    assert(WaterTest::terminal["reason"] == "recording_failure");
    assert(WaterTest::terminal["lost_ranges"].size() == 1);
    verifyJournal();
    verifyRetainedAndRestart();
  } else if (scenario == "edge_fsync_failure") {
    start();
    advance(62);
    Native::clock = 64000000;
    Native::fsyncUntilFail = 0;
    WaterTest::lastLoggedRead[0] = WaterTest::lastLoggedRead[1] = 0;
    WaterTest::tick();
    assert(!WaterTest::physicalPump());
    assert(WaterTest::terminal["reason"] == "recording_failure");
    verifyJournal();
    verifyRetainedAndRestart();
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
    advance(64);
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
    advance(64);
    assert(WaterTest::physicalPump());
    WaterTest::closeJournal();
    FILE *f = fs_open(WaterTest::journalPath, "ab");
    fwrite("torn", 1, 4, f);
    fclose(f);
  } else if (scenario == "upload_pacing") {
    unsigned initialWaits = 0, completedWaits = 0;
    size_t requests = 0;
    uint32_t acknowledged = 0;
    Native::delays = 1000;
    Native::delayHook = [&](int milliseconds) {
      assert(Native::uploadBuffer == nullptr && !WaterTest::uploaderBusy);
      if (initialWaits < 2) {
        assert(milliseconds == 1500 && Native::payloads.empty());
        if (++initialWaits == 2) {
          start();
          finishEligibleStopped();
        }
        return;
      }
      if (WaterTest::uploadState == "submitted") {
        assert(milliseconds == 1500 && WaterTest::uploadedRecords == WaterTest::recordCount);
        assert(acknowledged == WaterTest::recordCount);
        assert(Native::payloads.size() == requests + 1);
        assert(!fs_exists(WaterTest::journalPath));
        if (++completedWaits == 2)
          throw Native::Yield{};
        return;
      }
      assert(milliseconds == 50 && WaterTest::uploadState == "pending");
      assert(Native::payloads.size() == requests + 1);
      JsonDocument sent;
      assert(deserializeJson(sent, Native::payloads.back()) == DeserializationError::Ok);
      if (requests == 0) {
        assert(WaterTest::manifestUploaded && !sent["records"].is<JsonArray>());
      } else {
        auto records = sent["records"].as<JsonArrayConst>();
        assert(records.size() > 0 && records.size() <= 12);
        assert(sent["first_seq"] == acknowledged);
        for (JsonObjectConst record : records)
          assert(record["seq"] == acknowledged++);
        assert(sent["last_seq"] == acknowledged - 1);
      }
      assert(WaterTest::uploadedRecords == acknowledged);
      ++requests;
    };
    try {
      WaterTest::uploader(nullptr);
    } catch (const Native::Yield &) {
    }
    assert(initialWaits == 2 && completedWaits == 2);
    assert(requests == 1 + (WaterTest::recordCount + 11) / 12);
  } else if (scenario == "upload_pacing_http_retry" || scenario == "upload_pacing_ack_retry" ||
             scenario == "upload_pacing_preparation_retry") {
    start();
    finishEligibleStopped();
    const bool preparationFailure = scenario == "upload_pacing_preparation_retry";
    unsigned step = 0, completedWaits = 0;
    Native::delays = 1000;
    Native::delayHook = [&](int milliseconds) {
      assert(Native::uploadBuffer == nullptr && !WaterTest::uploaderBusy);
      if (step == 0) {
        assert(milliseconds == 1500 && Native::payloads.empty());
        step = 1;
      } else if (step == 1) {
        assert(milliseconds == 50 && Native::payloads.size() == 1);
        assert(WaterTest::manifestUploaded && WaterTest::uploadedRecords == 0);
        Native::nextHttpCode = scenario == "upload_pacing_http_retry" ? 500 : 201;
        Native::invalidUploadAcknowledgement = scenario == "upload_pacing_ack_retry";
        Native::failUploadAllocation = preparationFailure;
        step = 2;
      } else if (step == 2) {
        assert(milliseconds == (preparationFailure ? 1500 : 30000));
        assert(Native::payloads.size() == (preparationFailure ? 1u : 2u));
        assert(WaterTest::uploadState == "error" && WaterTest::uploadedRecords == 0);
        assert(fs_exists(WaterTest::journalPath));
        Native::nextHttpCode = 201;
        Native::invalidUploadAcknowledgement = false;
        Native::failUploadAllocation = false;
        step = preparationFailure ? 4 : 3;
      } else if (step == 3) {
        assert(milliseconds == 1500 && Native::payloads.size() == 2);
        assert(WaterTest::uploadedRecords == 0 && WaterTest::uploadState == "error");
        step = 4;
      } else if (step == 4) {
        assert(milliseconds == 50 && WaterTest::uploadedRecords == 12);
        assert(Native::payloads.size() == (preparationFailure ? 2u : 3u));
        if (!preparationFailure)
          assert(Native::payloads[1] == Native::payloads[2]);
        step = 5;
      } else if (WaterTest::uploadState == "submitted") {
        assert(milliseconds == 1500 && WaterTest::uploadedRecords == WaterTest::recordCount);
        assert(!fs_exists(WaterTest::journalPath));
        if (++completedWaits == 2)
          throw Native::Yield{};
      } else {
        assert(milliseconds == 50 && WaterTest::uploadState == "pending");
      }
    };
    try {
      WaterTest::uploader(nullptr);
    } catch (const Native::Yield &) {
    }
    assert(step == 5 && completedWaits == 2);
  } else if (scenario == "upload_retry") {
    start();
    finishEligibleStopped();
    assert(fs_exists(WaterTest::journalPath));
    unsigned backoffs = 0;
    Native::delayHook = [&](int milliseconds) {
      if (milliseconds == 30000) {
        assert(Native::uploadBuffer == nullptr);
        ++backoffs;
      }
    };
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
    assert(!fs_exists(WaterTest::journalPath));
    assert(backoffs == 2);
  } else if (scenario == "upload_allocation_failure") {
    start();
    finishEligibleStopped();
    auto failAndRetry = [] {
      const size_t sent = Native::payloads.size();
      const uint32_t acknowledged = WaterTest::uploadedRecords;
      const bool manifestAcknowledged = WaterTest::manifestUploaded;
      Native::failUploadAllocation = true;
      uploadOnce();
      assert(WaterTest::uploadState == "error" && !WaterTest::uploaderBusy);
      assert(WaterTest::uploadedRecords == acknowledged);
      assert(WaterTest::manifestUploaded == manifestAcknowledged);
      assert(Native::payloads.size() == sent && Native::uploadBuffer == nullptr);
      assert(fs_exists(WaterTest::journalPath));
      Native::failUploadAllocation = false;
      uploadOnce();
      assert(Native::payloads.size() == sent + 1 && Native::uploadBuffer == nullptr);
    };
    failAndRetry();
    assert(WaterTest::manifestUploaded && WaterTest::uploadedRecords == 0);
    failAndRetry();
    assert(WaterTest::uploadedRecords == 12);
    for (unsigned n = 0; n < 100 && WaterTest::uploadedRecords < WaterTest::recordCount; ++n)
      uploadOnce();
    assert(WaterTest::uploadedRecords == WaterTest::recordCount);
    failAndRetry();
    assert(WaterTest::uploadState == "submitted" && !fs_exists(WaterTest::journalPath));
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
    advance(11000);
    assert(!WaterTest::active() && !WaterTest::physicalPump());
    assert(WaterTest::program.pulse == 6);
    assert(WaterTest::terminal["outcome"] == "inconclusive");
    assert(WaterTest::program.totalPump == 3010);
    verifyJournal();
    verifyRetainedAndRestart(6);
  } else if (scenario == "completed") {
    start();
    double water = 20;
    double tailUntil = 0;
    bool wasPump = false;
    while (WaterTest::active() && Native::clock < uint64_t(maximumSeconds)*1000000) {
      const double at = Native::clock/1e6 + 2;
      const bool pump = WaterTest::physicalPump();
      if (!pump && wasPump) tailUntil = at + 20;
      water -= pump ? .03 : at < tailUntil ? .015 : 0;
      wasPump = pump;
      fresh(at, static_cast<int16_t>(std::lround(water*16)));
    }
    assert(!WaterTest::active() && !WaterTest::physicalPump());
    assert(WaterTest::terminal["outcome"] == "completed");
    assert(WaterTest::terminal["completed_pulses"].as<unsigned>() >= 4);
    assert(WaterTest::terminal["useful_response"] == true);
    assert(WaterTest::uploadState == "pending");
    verifyJournal();
    submit();
  } else if (scenario == "failure_after_full_pulse" || scenario == "stale_stop_after_full_pulse") {
    start();
    advance(74);
    assert(WaterTest::active() && WaterTest::program.completedPulses == 1);
    if (scenario == "failure_after_full_pulse") {
      tempControl.heater->setActive(true);
    } else {
      Native::clock = 104100000;
      assert(WaterTest::requestStop(error));
    }
    WaterTest::tick();
    assert(WaterTest::terminal["outcome"] == "failed");
    assert(WaterTest::terminal["reason"] ==
           (scenario == "failure_after_full_pulse" ? "unexpected_output" : "beer_sensor_stale"));
    verifyJournal();
    verifyRetainedAndRestart(1);
  } else
    assert(false);
  std::cout << "water_test_backend: " << scenario << " passed\n";
}
