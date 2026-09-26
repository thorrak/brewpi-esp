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
void makeLegacyMetadata() {
  WaterTest::manifest["acquisition"].remove("local_metadata_version");
  assert(WaterTest::atomicJson(WaterTest::manifestPath, WaterTest::manifest));
  WaterTest::bootList.remove("test_id");
  WaterTest::bootList.remove("device_guid");
  WaterTest::bootList.remove("schema_version");
  assert(WaterTest::atomicJson(WaterTest::bootsPath, WaterTest::bootList));
  JsonDocument ack;
  if (WaterTest::readJson(WaterTest::ackPath, ack)) {
    ack.remove("test_id");
    ack.remove("device_guid");
    ack.remove("schema_version");
    assert(WaterTest::atomicJson(WaterTest::ackPath, ack));
  }
}
void verifyJournal() {
  FILE *f = fs_open(WaterTest::journalPath, "rb");
  assert(f);
  Record r;
  unsigned expected[8] = {};
  while (fread(&r, sizeof(r), 1, f) == 1) {
    assert(valid(r));
    assert(r.seq == expected[r.boot]++);
  }
  fclose(f);
}
int main(int argc, char **argv) {
  assert(argc == 3);
  Native::root = argv[2];
  std::filesystem::create_directories(Native::root);
  initializeHardware();
  std::string error;
  std::string scenario = argv[1];
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
  } else if (scenario == "cleanup_ack_failure" || scenario == "cleanup_resume_failure" ||
             scenario == "cleanup_manifest_failure") {
    start();
    submitAndResume();
    const std::string previous = WaterTest::manifest["test_id"];
    Native::removeFailurePath = scenario == "cleanup_ack_failure" ? WaterTest::ackPath :
                                scenario == "cleanup_resume_failure" ? WaterTest::resumedPath : WaterTest::manifestPath;
    fresh(4);
    auto d = survey();
    assert(WaterTest::requestStart(d, error));
    WaterTest::tick();
    assert(!WaterTest::active() && !WaterTest::controlOwned());
    assert(WaterTest::manifest["test_id"] == previous);
    assert(WaterTest::reason.find("Unable to remove") != std::string::npos);
    assert(fs_exists(Native::removeFailurePath.c_str()));
    Native::removeFailurePath.clear();
    assert(WaterTest::requestStart(d, error));
    WaterTest::tick();
    assert(WaterTest::active());
    assert(WaterTest::manifest["test_id"] != previous);
  } else if (scenario == "cleanup_ack_before_reboot" || scenario == "cleanup_resume_before_reboot" ||
             scenario == "cleanup_manifest_before_reboot") {
    start();
    submitAndResume();
    Native::removeFailurePath = scenario == "cleanup_ack_before_reboot" ? WaterTest::ackPath :
                                scenario == "cleanup_resume_before_reboot" ? WaterTest::resumedPath : WaterTest::manifestPath;
    fresh(4);
    auto d = survey();
    assert(WaterTest::requestStart(d, error));
    WaterTest::tick();
    assert(!WaterTest::active());
    assert(WaterTest::reason.find("Unable to remove") != std::string::npos);
  } else if (scenario == "cleanup_after_reboot") {
    assert(!WaterTest::controlOwned() && !WaterTest::active());
    assert(WaterTest::manifest.isNull() || WaterTest::uploadState == "submitted");
    start();
    assert(WaterTest::sameTest(WaterTest::bootList));
    assert(!fs_exists(WaterTest::ackPath) && !fs_exists(WaterTest::resumedPath));
  } else if (scenario == "foreign_markers_before_reboot" || scenario == "foreign_finish_before_reboot") {
    start();
    submitAndResume();
    JsonDocument oldAck, oldRelease, oldFinish;
    assert(WaterTest::readJson(WaterTest::ackPath, oldAck));
    assert(WaterTest::readJson(WaterTest::resumedPath, oldRelease));
    assert(WaterTest::readJson(WaterTest::finishPath, oldFinish));
    start();
    if (scenario == "foreign_markers_before_reboot") {
      fresh(3, 320, false);
      assert(WaterTest::atomicJson(WaterTest::ackPath, oldAck));
      assert(WaterTest::atomicJson(WaterTest::resumedPath, oldRelease));
    } else {
      WaterTest::closeJournal();
      assert(WaterTest::atomicJson(WaterTest::finishPath, oldFinish));
    }
  } else if (scenario == "foreign_markers_after_reboot") {
    assert(WaterTest::controlOwned() && !WaterTest::active());
    assert(WaterTest::uploadState == "pending");
    assert(fs_exists(WaterTest::journalPath));
    uploadOnce();
    JsonDocument first;
    assert(deserializeJson(first, Native::payloads.back()) == DeserializationError::Ok);
    assert(first["installation"].is<JsonObject>());
    assert(first["test_id"] == WaterTest::manifest["test_id"]);
    assert(WaterTest::controlOwned());
  } else if (scenario == "foreign_finish_after_reboot") {
    assert(WaterTest::controlOwned() && !WaterTest::active());
    assert(WaterTest::terminal["outcome"] == "interrupted");
    assert(WaterTest::terminal["test_id"] == WaterTest::manifest["test_id"]);
    assert(WaterTest::bootList["boots"].size() == 2);
    verifyJournal();
  } else if (scenario == "foreign_boots_before_reboot" || scenario == "untagged_boots_before_reboot") {
    start();
    fresh(3, 320, false);
    if (scenario == "foreign_boots_before_reboot")
      WaterTest::bootList["test_id"] = "a-different-test";
    else
      WaterTest::bootList.remove("test_id");
    assert(WaterTest::atomicJson(WaterTest::bootsPath, WaterTest::bootList));
  } else if (scenario == "invalid_boots_after_reboot") {
    assert(WaterTest::controlOwned() && !WaterTest::active());
    assert(WaterTest::uploadState == "error");
    uploadOnce();
    assert(Native::payloads.empty());
    assert(fs_exists(WaterTest::journalPath));
  } else if (scenario == "submitted_before_reboot" || scenario == "legacy_submitted_before_reboot") {
    start();
    submitAndResume();
    if (scenario == "legacy_submitted_before_reboot")
      makeLegacyMetadata();
  } else if (scenario == "submitted_after_reboot") {
    assert(!WaterTest::controlOwned() && !WaterTest::active());
    assert(WaterTest::uploadState == "submitted");
    uploadOnce();
    assert(Native::payloads.empty());
    assert(!fs_exists(WaterTest::journalPath));
  } else if (scenario == "legacy_submitted_after_reboot") {
    assert(!WaterTest::controlOwned() && !WaterTest::active());
    assert(WaterTest::uploadState == "pending");
    assert(!fs_exists(WaterTest::journalPath));
    uploadOnce();
    uploadOnce();
    assert(WaterTest::uploadState == "submitted");
    assert(Native::payloads.size() == 2);
  } else if (scenario == "legacy_active_before_reboot" || scenario == "legacy_pending_before_reboot") {
    start();
    advance(302);
    if (scenario == "legacy_pending_before_reboot") {
      fresh(302.1, 320, false);
      uploadOnce();
      uploadOnce();
    } else
      WaterTest::closeJournal();
    makeLegacyMetadata();
  } else if (scenario == "legacy_active_after_reboot" || scenario == "legacy_pending_after_reboot") {
    assert(WaterTest::controlOwned() && !WaterTest::active());
    assert(WaterTest::uploadState == "pending");
    assert(WaterTest::bootList["test_id"] == WaterTest::manifest["test_id"]);
    JsonDocument durableBoots;
    assert(WaterTest::readJson(WaterTest::bootsPath, durableBoots));
    assert(durableBoots["test_id"] == WaterTest::manifest["test_id"]);
    if (scenario == "legacy_active_after_reboot")
      assert(WaterTest::terminal["outcome"] == "interrupted");
    else {
      uploadOnce();
      JsonDocument first;
      assert(deserializeJson(first, Native::payloads.back()) == DeserializationError::Ok);
      assert(first["installation"].is<JsonObject>());
      uploadOnce();
      assert(deserializeJson(first, Native::payloads.back()) == DeserializationError::Ok);
      assert(first["first_seq"] == 0);
    }
    verifyJournal();
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
    assert(fs_exists(WaterTest::finishPath));
    verifyJournal();
  } else if (scenario == "edge_fsync_failure") {
    start();
    advance(300);
    Native::clock = 302000000;
    Native::fsyncUntilFail = 0;
    WaterTest::tick();
    assert(!WaterTest::physicalPump());
    assert(WaterTest::terminal["reason"] == "recording_failure");
    assert(fs_exists(WaterTest::finishPath));
    verifyJournal();
  } else if (scenario == "start_failure") {
    fresh(2);
    auto d = survey();
    Native::fsyncUntilFail = 0;
    assert(WaterTest::requestStart(d, error));
    WaterTest::tick();
    assert(!WaterTest::active() && !WaterTest::controlOwned() && WaterTest::manifest.isNull());
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
  } else if (scenario == "after_reboot") {
    assert(!WaterTest::active());
    assert(WaterTest::controlOwned());
    assert(!WaterTest::physicalPump() && !WaterTest::physicalHeat());
    assert(WaterTest::terminal["outcome"] == "interrupted");
    assert(WaterTest::bootList["boots"].size() == 2);
    assert(WaterTest::bootList["boots"][0] != WaterTest::bootList["boots"][1]);
    verifyJournal();
    fresh(3);
    assert(!WaterTest::active());
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
    assert(fs_exists(WaterTest::manifestPath) && fs_exists(WaterTest::finishPath));
  } else if (scenario == "partial_upload_before_reboot") {
    start();
    advance(302);
    fresh(302.1, 320, false);
    uploadOnce();
    uploadOnce();
    JsonDocument ack;
    assert(WaterTest::readJson(WaterTest::ackPath, ack));
    assert(ack["manifest"] == true && ack["next_record"] == 12);
    assert(fs_exists(WaterTest::journalPath));
  } else if (scenario == "after_partial_upload_reboot") {
    assert(!WaterTest::active());
    assert(WaterTest::uploadState == "pending");
    uploadOnce();
    assert(!Native::payloads.empty());
    JsonDocument request;
    assert(deserializeJson(request, Native::payloads[0]) == DeserializationError::Ok);
    assert(request["first_seq"] == 12);
    assert(request["batch_id"].is<const char *>());
    for (unsigned n = 0; n < 100 && WaterTest::uploadState != "submitted"; ++n)
      uploadOnce();
    assert(WaterTest::uploadState == "submitted");
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
