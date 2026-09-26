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
  if (scenario == "normal_stop") {
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
