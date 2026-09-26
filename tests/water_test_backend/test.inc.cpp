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
  d["bath_placement_confirmed"] = true;
  d["reported_chiller_setpoint_c"] = nullptr;
  return d;
}
void start() {
  fresh(2);
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
void submitAndResume() {
  fresh(3, 320, false);
  for (unsigned n = 0; n < 100 && WaterTest::uploadState != "submitted"; ++n)
    uploadOnce();
  assert(WaterTest::uploadState == "submitted");
  JsonDocument release;
  release["probe_returned"] = true;
  std::string error;
  assert(WaterTest::requestResume(release, error));
  WaterTest::tick();
  assert(!WaterTest::controlOwned());
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
    start();
    advance(302);
    Native::fsyncDelayUs = 12000000;
    const uint64_t readAt = 302100000;
    fresh(readAt / 1e6, scenario == "slow_temperature_limit" ? 272 : 320, scenario != "slow_sensor_fault");
    assert(!WaterTest::active() && !WaterTest::physicalPump());
    assert(tempControl.pump.edgeTimes.back() == readAt);
    assert(WaterTest::terminal["reason"] ==
           (scenario == "slow_temperature_limit" ? "temperature_limit" : "beer_sensor_fault"));
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
    fresh(4);
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
    JsonDocument resume;
    assert(!WaterTest::requestResume(resume, error));
    resume["probe_returned"] = true;
    assert(WaterTest::requestResume(resume, error));
    WaterTest::tick();
    assert(!WaterTest::controlOwned() && Native::resumes == 1 && tempControl.cs.mode == 'b');
  } else if (scenario == "resume_pending_upload") {
    start();
    fresh(3, 320, false);
    JsonDocument resume;
    resume["probe_returned"] = true;
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
  } else if (scenario == "sensor_fault") {
    start();
    advance(302);
    fresh(302.1, 320, false);
    assert(!WaterTest::physicalPump());
    assert(WaterTest::terminal["reason"] == "beer_sensor_fault");
    verifyJournal();
  } else if (scenario == "bath_fault") {
    start();
    advance(302);
    fresh(302.1, 320, true, false);
    assert(WaterTest::active() && WaterTest::physicalPump());
    verifyJournal();
  } else if (scenario == "status_freshness") {
    start();
    JsonDocument visible;
    WaterTest::status(visible);
    assert(visible["beer_c"].is<double>() && visible["glycol_c"].is<double>());
    Native::clock += 11000000;
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
    assert(WaterTest::uploadState == "pending");
    verifyJournal();
  } else if (scenario == "edge_fsync_failure") {
    start();
    advance(300);
    Native::clock = 302000000;
    Native::fsyncUntilFail = 0;
    WaterTest::tick();
    assert(!WaterTest::physicalPump());
    assert(WaterTest::terminal["reason"] == "recording_failure");
    assert(WaterTest::uploadState == "pending");
    verifyJournal();
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
    advance(302);
    fresh(302.1, 320, false);
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
    advance(307);
    assert(WaterTest::requestStop(error));
    WaterTest::tick();
    assert(!WaterTest::active());
    assert(WaterTest::terminal["outcome"] == "stopped");
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
      resume["probe_returned"] = true;
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
  } else
    assert(false);
  std::cout << "water_test_backend: " << scenario << " passed\n";
}
