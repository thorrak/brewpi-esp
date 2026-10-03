// Native assertions sometimes need the exact durable bytes. Production uploads
// validate and stream these envelopes without materializing a complete payload.
namespace WaterTest {
bool loadDocumentPayload(const char *path, std::string &payload) {
  payload.clear();
  size_t length = 0;
  if (!WaterTestStorage::validateDocumentPayload(path, length))
    return false;
  payload.reserve(length);
  const auto append = [](void *context, const char *data, size_t size) {
    static_cast<std::string *>(context)->append(data, size);
    return true;
  };
  if (!WaterTestStorage::streamDocumentPayload(path, append, &payload, length)) {
    payload.clear();
    return false;
  }
  return payload.size() == length;
}
} // namespace WaterTest

using namespace WaterTestCore;
void initializeHardware() {
  assert(!WaterTest::program.hasHistory());
  Native::uploadStartHook = [] { assert(!WaterTest::program.hasHistory()); };
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
  assert(!WaterTest::program.hasHistory());
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
  assert(WaterTest::active() && WaterTest::program.hasHistory());
  assert(!WaterTest::physicalPump());
  assert(WaterTest::manifest["test_id"].as<std::string>().size() == 36);
  assert(WaterTest::manifest["test_id"].as<std::string>().find('\0') == std::string::npos);
  assert(WaterTest::manifest["installation"]["glycol_flow_source"] == "unknown");
  assert(WaterTest::manifest["installation"]["glycol_flow_value"].isNull());
  assert(WaterTest::manifest["installation"]["glycol_flow_unit"].isNull());
  assert(std::distance(std::filesystem::directory_iterator(Native::root),
                       std::filesystem::directory_iterator{}) == 3);
  assert(fs_exists(WaterTest::journalPath));
}
void finishBaseline() {
  while (WaterTest::program.phase == Phase::Baseline &&
         Native::clock / 1e6 <= WaterTest::program.started + baselineSeconds)
    fresh(Native::clock / 1e6 + 2);
  assert(WaterTest::active() && WaterTest::program.phase == Phase::Observe);
  assert(WaterTest::program.nextPulsePending && !WaterTest::physicalPump());
}
double beginFirstPulse() {
  if (WaterTest::program.phase == Phase::Baseline)
    finishBaseline();
  if (!WaterTest::program.pump)
    fresh(Native::clock / 1e6 + 2);
  assert(WaterTest::active() && WaterTest::physicalPump());
  assert(WaterTest::program.phase == Phase::Pulse && WaterTest::program.pulse == 1);
  return WaterTest::program.pulseStarted;
}
double completeFirstPulse() {
  beginFirstPulse();
  advance(WaterTest::program.deadline);
  assert(WaterTest::active() && WaterTest::program.phase == Phase::Observe && !WaterTest::physicalPump());
  assert(WaterTest::program.completedPulses == 1);
  return WaterTest::program.switched;
}
void uploadOnce() {
  Native::delays = 1;
  try {
    WaterTest::uploader(nullptr);
  } catch (const Native::Yield &) {
  }
}
void runScheduledUpload() {
  assert(Native::scheduledTask && WaterTest::uploaderTaskActive);
  const auto deleted = Native::taskDeletes;
  Native::delays = 10000;
  try {
    Native::scheduledTask(Native::scheduledTaskArgument);
    assert(false); // FreeRTOS task entry must delete itself, never return.
  } catch (const Native::TaskDeleted &) {
  }
  assert(Native::taskDeletes == deleted + 1 && Native::scheduledTask == nullptr);
  assert(!WaterTest::uploaderTaskActive && !WaterTest::uploaderBusy && Native::uploadWorkspace == nullptr);
}
void finishEligibleStopped() {
  completeFirstPulse();
  assert(WaterTest::active() && !WaterTest::physicalPump());
  assert(WaterTest::program.completedPulses == 1 && WaterTest::program.totalPump == 10);
  std::string error;
  assert(WaterTest::requestStop(error));
  WaterTest::tick();
  assert(WaterTest::terminal["outcome"] == "stopped");
  assert(WaterTest::terminal["completed_pulses"] == 1);
  assert(WaterTest::uploadState == "pending");
  assert(!WaterTest::program.hasHistory());
}
void submit() {
  assert(!WaterTest::program.hasHistory());
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
  assert(!WaterTest::program.hasHistory());
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
  assert(!WaterTest::program.hasHistory());
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
void verifyFlow(JsonVariantConst installation, const char *source, double value, const char *unit) {
  assert(installation["glycol_flow_source"] == source);
  assert(installation["glycol_flow_value"].is<double>());
  assert(installation["glycol_flow_value"].as<double>() == value);
  assert(installation["glycol_flow_unit"] == unit);
}
#include "controller_run_write_regressions.h"
#include "controller_final_observation_regressions.h"
#include "dual_controller_regressions.h"
#include "controller_observation_regressions.h"
#include "controller_checkpoint_precision_regressions.h"
#include "upload_streaming_regressions.h"
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
  } else if (scenario == "orphan_unreadable_after_reboot") {
    Native::openFailurePath = WaterTest::journalPath;
  }
  initializeHardware();
  std::string error;
  if (scenario == "upload_bounded_streaming" || scenario == "upload_metadata_crc" ||
      scenario == "upload_batch_crc" || scenario == "upload_partial_write_retry" ||
      scenario == "upload_lost_ack_retry" || scenario == "upload_incomplete_ack_retry" ||
      scenario == "upload_large_ack_retry" || scenario == "upload_workspace_allocation_failure") {
    verifyStreamingUpload(scenario);
  } else if (scenario == "history_allocation_failure" || scenario == "history_held_allocation_failure") {
    const bool held = scenario == "history_held_allocation_failure";
    WaterTest::startupHold = held;
    fresh(2);
    const auto pumpEdges = tempControl.pump.edges;
    const auto heatEdges = tempControl.heat.edges;
    Native::failHistoryAllocation = true;
    auto request = survey();
    assert(WaterTest::requestStart(request, error));
    WaterTest::tick();
    assert(Native::historyAllocations == 1);
    assert(Native::historyAllocationSize == historyCapacity * sizeof(Program::Reading));
    assert(!WaterTest::program.hasHistory() && !WaterTest::active());
    assert(WaterTest::controlOwned() == held && WaterTest::startupHold == held);
    assert(WaterTest::manifest.isNull() && !WaterTest::journal);
    assert(std::filesystem::is_empty(Native::root));
    assert(tempControl.cs.mode == 'b' && tempControl.cs.beerSetting == 10000 && tempControl.cs.fridgeSetting == 5000);
    assert(tempControl.pump.edges == pumpEdges && tempControl.heat.edges == heatEdges);
    assert(Native::taskCreates == 0 && Native::payloads.empty());
    assert(WaterTest::reason.find("Not enough free memory") != std::string::npos);
    Native::failHistoryAllocation = false;
    start();
    assert(Native::historyAllocations == 2 && WaterTest::program.hasHistory());
  } else if (scenario == "uploader_lifecycle") {
    assert(Native::taskCreates == 0 && !WaterTest::backgroundServicesReady);
    WaterTest::tick(); // The setup-time tick must not reserve an upload stack.
    assert(Native::taskCreates == 0);
    WaterTest::startBackgroundServices();
    WaterTest::startBackgroundServices();
    start();
    beginFirstPulse();
    assert(Native::taskCreates == 0 && WaterTest::active());
    finishEligibleStopped();
    assert(Native::taskCreates == 1 && WaterTest::uploaderTaskActive);
    for (unsigned n = 0; n < 20; ++n) WaterTest::tick();
    assert(Native::taskCreates == 1);
    Native::delayHook = [&](int) {
      const auto created = Native::taskCreates;
      WaterTest::tick(); // Main-loop service during every pacing delay.
      assert(Native::taskCreates == created);
    };
    runScheduledUpload();
    Native::delayHook = {};
    assert(WaterTest::uploadState == "submitted" && !fs_exists(WaterTest::journalPath));
    WaterTest::tick();
    assert(Native::taskCreates == 1);
    JsonDocument resume;
    assert(WaterTest::requestResume(resume, error));
    WaterTest::tick();
    start();
    assert(Native::taskCreates == 1 && WaterTest::active());
  } else if (scenario == "uploader_allocation_retry") {
    assert(Native::taskCreates == 0);
    start();
    finishEligibleStopped();
    WaterTest::tick();
    assert(Native::taskCreates == 0); // Pending work still waits for the actual loop.
    WaterTest::startBackgroundServices();
    assert(Native::taskCreates == 0);
    Native::failTaskCreation = true;
    WaterTest::tick();
    assert(Native::taskCreates == 1 && !WaterTest::uploaderTaskActive);
    assert(WaterTest::uploadState == "error" && fs_exists(WaterTest::journalPath));
    const auto firstRetry = WaterTest::nextUploaderAttemptUs;
    assert(firstRetry == Native::clock + 30000000);
    for (unsigned n = 0; n < 30; ++n) WaterTest::tick();
    Native::clock = firstRetry - 1;
    WaterTest::tick();
    assert(Native::taskCreates == 1);
    Native::clock = firstRetry;
    WaterTest::tick();
    assert(Native::taskCreates == 2 && !WaterTest::uploaderTaskActive);
    Native::failTaskCreation = false;
    Native::clock = WaterTest::nextUploaderAttemptUs;
    WaterTest::tick();
    assert(Native::taskCreates == 3 && WaterTest::uploaderTaskActive);
    runScheduledUpload();
    assert(WaterTest::uploadState == "submitted");
  } else if (scenario == "uploader_network_backoff") {
    WaterTest::startBackgroundServices();
    start();
    finishEligibleStopped();
    Native::nextHttpCode = 500;
    runScheduledUpload();
    assert(Native::taskCreates == 1 && Native::taskDeletes == 1);
    assert(WaterTest::uploadState == "error" && !WaterTest::manifestUploaded);
    assert(WaterTest::nextUploaderAttemptUs == Native::clock + 30000000);
    const std::string original = Native::payloads.front();
    Native::clock = WaterTest::nextUploaderAttemptUs - 1;
    WaterTest::tick();
    assert(Native::taskCreates == 1);
    Native::clock += 1;
    Native::nextHttpCode = 201;
    WaterTest::tick();
    assert(Native::taskCreates == 2);
    runScheduledUpload();
    assert(Native::payloads[1] == original && WaterTest::uploadState == "submitted");
    assert(Native::taskDeletes == 2 && WaterTest::nextUploaderAttemptUs == 0);
  } else if (scenario == "recovered_lazy_after_reboot") {
    assert(Native::taskCreates == 0 && WaterTest::terminalDurable);
    WaterTest::tick();
    assert(Native::taskCreates == 0 && WaterTest::controlOwned());
    WaterTest::startBackgroundServices();
    WaterTest::tick();
    assert(Native::taskCreates == 1);
    runScheduledUpload();
    assert(WaterTest::uploadState == "submitted" && WaterTest::controlOwned());
    assert(!WaterTest::physicalPump() && !WaterTest::physicalHeat());
  } else if (scenario == "flow_validation") {
    auto check = [&](JsonDocument &request, bool expected) {
      std::string failure;
      assert(WaterTest::requestStart(request, failure) == expected);
      if (expected) {
        JsonDocument queued;
        assert(deserializeJson(queued, WaterTest::queuedStart) == DeserializationError::Ok);
        assert(queued["glycol_flow_value"] == request["glycol_flow_value"]);
        assert(queued["glycol_flow_unit"] == request["glycol_flow_unit"]);
        WaterTest::queuedStart.clear();
        WaterTest::owned = false;
      } else {
        assert(!failure.empty() && WaterTest::queuedStart.empty() && !WaterTest::owned);
      }
    };
    auto request = survey();
    check(request, true); // Legacy clients omit all three fields.
    request["glycol_flow_source"] = "unknown";
    check(request, true);
    request["glycol_flow_value"] = nullptr;
    request["glycol_flow_unit"] = nullptr;
    check(request, true);
    request["glycol_flow_value"] = 12;
    check(request, false); // Stale values cannot survive a switch to unknown.
    request["glycol_flow_value"] = nullptr;
    request["glycol_flow_unit"] = "lpm";
    check(request, false);
    request.remove("glycol_flow_source");
    check(request, false); // A supplied unit requires an explicit source.
    request["glycol_flow_unit"] = nullptr;
    check(request, true); // Legacy omitted source with null companions is unknown.
    for (const char *source : {"pump_rating", "measured_at_fermenter"}) {
      for (const char *unit : {"us_gph", "us_gpm", "lph", "lpm"}) {
        request["glycol_flow_source"] = source;
        request["glycol_flow_value"] = 37.5;
        request["glycol_flow_unit"] = unit;
        check(request, true);
      }
    }
    for (const char *field : {"glycol_flow_source", "glycol_flow_value", "glycol_flow_unit"}) {
      const char *invalids[] = {"null", "true", "[]", "{}", "\"invalid\""};
      for (const char *invalid : invalids) {
        request["glycol_flow_source"] = "pump_rating";
        request["glycol_flow_value"] = 37.5;
        request["glycol_flow_unit"] = "lpm";
        JsonDocument wrong;
        assert(deserializeJson(wrong, invalid) == DeserializationError::Ok);
        request[field] = wrong.as<JsonVariantConst>();
        check(request, false);
      }
      request["glycol_flow_source"] = "pump_rating";
      request["glycol_flow_value"] = 37.5;
      request["glycol_flow_unit"] = "lpm";
      request.remove(field);
      check(request, false);
    }
    request["glycol_flow_source"] = "measured_at_fermenter";
    request["glycol_flow_unit"] = "lpm";
    for (double invalid : {0., -1., double(NAN), double(INFINITY)}) {
      request["glycol_flow_value"] = invalid;
      check(request, false);
    }
    request["glycol_flow_value"] = "12.5";
    check(request, false);
    JsonDocument parsed;
    assert(deserializeJson(parsed, "{\"flow\":1e999}") == DeserializationError::Ok);
    request["glycol_flow_value"] = parsed["flow"];
    check(request, false);
    for (const char *unit : {"us_gph", "lph", "lpm"}) {
      request["glycol_flow_value"] = 1e308;
      request["glycol_flow_unit"] = unit;
      check(request, true); // No arbitrary ceiling, or intermediate multiply overflow.
    }
    request["glycol_flow_unit"] = "us_gpm";
    check(request, false); // Converting this value to L/min overflows binary64.
    request["glycol_flow_value"] = 1e-200;
    check(request, true);
    request["glycol_flow_value"] = std::numeric_limits<double>::denorm_min();
    request["glycol_flow_unit"] = "lph";
    check(request, false); // Conversion must remain positive, including subnormal input.
  } else if (scenario == "flow_retention" || scenario == "flow_before_reboot") {
    fresh(2);
    auto request = survey();
    request["glycol_flow_source"] = "measured_at_fermenter";
    request["glycol_flow_value"] = 12.75;
    request["glycol_flow_unit"] = "us_gph";
    assert(WaterTest::requestStart(request, error));
    WaterTest::tick();
    assert(WaterTest::active());
    verifyFlow(WaterTest::manifest["installation"], "measured_at_fermenter", 12.75, "us_gph");
    JsonDocument stored;
    assert(WaterTest::loadDocument(WaterTest::manifestPath, stored));
    verifyFlow(stored["installation"], "measured_at_fermenter", 12.75, "us_gph");
    assert(WaterTest::requestStop(error));
    WaterTest::tick();
    if (scenario == "flow_before_reboot") return 0;
    submit();
    assert(deserializeJson(stored, Native::payloads.front()) == DeserializationError::Ok);
    verifyFlow(stored["installation"], "measured_at_fermenter", 12.75, "us_gph");
  } else if (scenario == "recovered_flow_after_reboot") {
    assert(WaterTest::terminalDurable && WaterTest::uploadState == "pending");
    verifyFlow(WaterTest::manifest["installation"], "measured_at_fermenter", 12.75, "us_gph");
    std::string original;
    assert(WaterTest::loadDocumentPayload(WaterTest::manifestPath, original));
    submit();
    assert(Native::payloads.front() == original);
    JsonDocument uploaded;
    assert(deserializeJson(uploaded, Native::payloads.front()) == DeserializationError::Ok);
    verifyFlow(uploaded["installation"], "measured_at_fermenter", 12.75, "us_gph");
  } else if (scenario == "slow_sensor_fault" || scenario == "slow_temperature_limit") {
    if (scenario == "slow_sensor_fault")
      tempControl.glycolRuntime.cooling.minOn = 60;
    start();
    const double pulseAt = beginFirstPulse();
    if (scenario == "slow_sensor_fault") {
      fresh(pulseAt + freshnessSeconds, -2048, false);
      assert(WaterTest::active() && WaterTest::physicalPump());
    }
    Native::fsyncDelayUs = 12000000;
    const double faultAt = pulseAt + (scenario == "slow_sensor_fault" ? freshnessSeconds : 0) + .1;
    const uint64_t readAt = uint64_t(std::llround(faultAt * 1e6));
    fresh(readAt / 1e6, scenario == "slow_temperature_limit" ? 262 : 320, scenario != "slow_sensor_fault");
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
    beginFirstPulse();
    const auto deadlineUs = uint64_t(std::llround(WaterTest::program.deadline * 1e6));
    advance(WaterTest::program.deadline - 2);
    Native::clock = deadlineUs - 1000000;
    Native::fsyncDelayUs = 3000000;
    WaterTest::lastLoggedRead[0] = 0;
    for (unsigned n = 0; n < 6; ++n)
      WaterTest::onSample(WaterTest::beerAddress, 320, true, deadlineUs - 2000000, deadlineUs - 1500000 + n);
    WaterTest::tick();
    assert(!WaterTest::physicalPump());
    // A synchronous write cannot be interrupted. Recheck immediately afterward,
    // rather than allowing the remaining queue to extend the pulse further.
    assert(tempControl.pump.edgeTimes.back() == deadlineUs + 2000000);
    verifyJournal();
  } else if (scenario == "slow_on_edge" || scenario == "slow_phase") {
    start();
    finishBaseline();
    Native::clock += 2000000;
    const auto pulseUs = Native::clock;
    Native::fsyncDelayUs = scenario == "slow_on_edge" ? 12000000 : 6000000;
    WaterTest::tick();
    assert(!WaterTest::physicalPump());
    assert(tempControl.pump.edgeTimes.size() == 2);
    assert(tempControl.pump.edgeTimes.front() == pulseUs);
    assert(tempControl.pump.edgeTimes.back() == pulseUs + 12000000);
    verifyJournal();
  } else if (scenario == "slow_storage_repair") {
    start();
    const double pulseAt = beginFirstPulse();
    Native::fsyncUntilFail = 0;
    WaterTest::lastLoggedRead[0] = WaterTest::lastLoggedRead[1] = 0;
    Native::truncateHook = [] {
      assert(!WaterTest::physicalPump() && !WaterTest::physicalHeat());
      Native::clock += 12000000;
    };
    fresh(pulseAt + .1);
    assert(!WaterTest::active());
    assert(tempControl.pump.edgeTimes.back() == uint64_t(std::llround((pulseAt + .1) * 1e6)));
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
    beginFirstPulse();
    Native::clock += 100000;
    const auto faultUs = Native::clock;
    Native::fsyncDelayUs = 12000000;
    const unsigned count = scenario == "slow_queue_overflow" ? 65 : 1;
    for (unsigned n = 0; n < count; ++n)
      WaterTest::onSample(WaterTest::beerAddress, 320, true, Native::clock - 750000, Native::clock + n);
    if (scenario == "queued_unexpected_output")
      tempControl.heater->setActive(true);
    WaterTest::tick();
    assert(!WaterTest::physicalPump() && !WaterTest::physicalHeat());
    assert(tempControl.pump.edgeTimes.back() == faultUs);
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
      assert(WaterTest::terminal["controllers"][0]["initialized"] == false);
      assert(WaterTest::terminal["controllers"][0]["learning_status"] == "not_started");
      assert(WaterTest::terminal["controllers"][0]["final_tuning"].isNull());
      assert(WaterTest::terminal["controllers"][0]["final_tuning_exact"].isNull());
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
  } else if (scenario == "orphan_retry_after_reboot") {
    assert(WaterTest::startupHold && !WaterTest::recoveryBlocked && !WaterTest::owned);
    assert(WaterTest::controlOwned() && !WaterTest::active());
    assert(fs_exists(WaterTest::journalPath) && fs_exists("/water-test-manifest.json.tmp"));
    assert(!fs_exists(WaterTest::manifestPath));
    // Settings load after recovery. Holding outputs must preserve those settings
    // for a new test's eventual prior_control snapshot.
    tempControl.cs.mode = 'b';
    tempControl.cooler->setActive(true);
    fresh(2);
    assert(!WaterTest::physicalPump() && !WaterTest::physicalHeat());
    assert(tempControl.cs.mode == 'b');
    JsonDocument state;
    WaterTest::status(state);
    assert(state["startup_interrupted"] == true && state["control_owned"] == true);
    assert(state["can_start"] == true && state["can_resume"] == false);
    assert(state["upload_status"] == "not_submitted" && WaterTest::manifest.isNull());
    auto d = survey();
    d["consent"] = false;
    assert(!WaterTest::requestStart(d, error));
    assert(fs_exists("/water-test-manifest.json.tmp"));
    d["consent"] = true;
    assert(WaterTest::requestStart(d, error));
    assert(WaterTest::requestStop(error));
    WaterTest::tick();
    assert(WaterTest::startupHold && WaterTest::controlOwned() && !WaterTest::owned);
    assert(fs_exists("/water-test-manifest.json.tmp"));
    Native::openFailurePath = WaterTest::journalPath;
    assert(WaterTest::requestStart(d, error));
    WaterTest::tick();
    assert(WaterTest::startupHold && WaterTest::controlOwned() && !WaterTest::active());
    assert(tempControl.cs.mode == 'b');
    Native::openFailurePath.clear();
    assert(WaterTest::requestStart(d, error));
    WaterTest::tick();
    assert(WaterTest::active() && !WaterTest::startupHold);
    assert(WaterTest::manifest["prior_control"]["mode"] == "b");
    assert(!fs_exists("/water-test-manifest.json.tmp"));
  } else if (scenario == "orphan_blocked_after_reboot" || scenario == "orphan_unreadable_after_reboot") {
    const auto files = std::distance(std::filesystem::directory_iterator(Native::root),
                                     std::filesystem::directory_iterator{});
    assert(WaterTest::recoveryBlocked && WaterTest::controlOwned() && !WaterTest::startupHold);
    assert(!WaterTest::active() && !WaterTest::physicalPump() && !WaterTest::physicalHeat());
    auto d = survey();
    assert(!WaterTest::requestStart(d, error));
    JsonDocument state;
    WaterTest::status(state);
    assert(state["can_start"] == false && state["can_resume"] == false);
    uploadOnce();
    assert(Native::payloads.empty());
    assert(files == std::distance(std::filesystem::directory_iterator(Native::root),
                                  std::filesystem::directory_iterator{}));
  } else if (scenario == "orphan_controller_only_before_reboot" ||
             scenario == "orphan_controller_tmp_only_before_reboot") {
    const std::string path = std::string(WaterTest::controllerOnePath) +
                            (scenario == "orphan_controller_tmp_only_before_reboot" ? ".tmp" : "");
    writeFile(path.c_str(), "saved controller results without campaign metadata");
  } else if (scenario.rfind("orphan_", 0) == 0) {
    writeFile(WaterTest::journalPath, scenario == "orphan_nonempty_before_reboot" ? "x" : "");
    writeFile("/water-test-manifest.json.tmp", "partial startup metadata");
    writeFile(WaterTest::reservePath, "reserved");
    if (scenario == "orphan_manifest_before_reboot") writeFile(WaterTest::manifestPath, "invalid");
    if (scenario == "orphan_finish_before_reboot") writeFile(WaterTest::finishPath, "invalid");
    if (scenario == "orphan_finish_tmp_before_reboot") writeFile("/water-test-finish.json.tmp", "invalid");
    if (scenario == "orphan_ack_tmp_before_reboot") writeFile("/water-test-ack.json.tmp", "invalid");
    if (scenario == "orphan_resumed_tmp_before_reboot") writeFile("/water-test-resumed.json.tmp", "invalid");
    if (scenario == "orphan_boots_tmp_before_reboot") writeFile("/water-test-boots.json.tmp", "invalid");
    if (scenario == "orphan_controller_one_tmp_before_reboot")
      writeFile((std::string(WaterTest::controllerOnePath) + ".tmp").c_str(), "invalid");
    if (scenario == "orphan_controller_two_tmp_before_reboot")
      writeFile((std::string(WaterTest::controllerTwoPath) + ".tmp").c_str(), "invalid");
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
    const double pulseAt = beginFirstPulse();
    advance(pulseAt + 5);
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
    assert(WaterTest::terminal["controllers"][0]["initialized"] == false);
    assert(WaterTest::terminal["controllers"][0]["learning_status"] == "not_started");
    assert(WaterTest::terminal["controllers"][0]["final_tuning"].isNull());
    verifyJournal();
    verifyRetainedAndRestart();
  } else if (scenario == "full_pulse_stop") {
    start();
    finishEligibleStopped();
    assert(WaterTest::program.phase == Phase::Finished && WaterTest::program.totalPump == 10);
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
    const double pulseAt = beginFirstPulse();
    assert(WaterTest::requestStop(error));
    WaterTest::tick();
    assert(WaterTest::physicalPump());
    fresh(pulseAt + 1);
    assert(WaterTest::physicalPump());
    fresh(pulseAt + 2);
    assert(!WaterTest::physicalPump());
    assert(WaterTest::terminal["outcome"] == "stopped");
    assert(WaterTest::program.totalPump == 2);
    verifyJournal();
    verifyRetainedAndRestart();
  } else if (scenario == "transient_sensor_errors") {
    start();
    const double pulseAt = beginFirstPulse();
    fresh(pulseAt + .1, -2048, false, false);
    assert(WaterTest::active() && WaterTest::physicalPump());
    assert(WaterTest::program.lastSample == pulseAt && WaterTest::program.latestC == 20);
    JsonDocument status;
    WaterTest::status(status);
    assert(status["beer_c"] == 20 && status["glycol_c"] == 7);
    assert(status["preflight"]["beer_available"] == true);
    assert(status["preflight"]["chamber_available"] == true);
    FILE *journal = fs_open(WaterTest::journalPath, "rb");
    Record record{};
    unsigned badReadings = 0;
    while (fread(&record, sizeof(record), 1, journal) == 1)
      if (record.kind == 2 && record.read_us == uint64_t(std::llround((pulseAt + .1) * 1e6))) {
        assert(!(record.flags & 1) && (record.flags & 2));
        assert(record.raw == (record.role == 0 ? -2048 : 112));
        ++badReadings;
      }
    fclose(journal);
    assert(badReadings == 2);
    fresh(pulseAt + 2, 319);
    assert(WaterTest::active() && WaterTest::physicalPump());
    assert(WaterTest::program.lastSample == pulseAt + 2 && WaterTest::program.latestC == 19.9375);
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
    const double pulseAt = beginFirstPulse();
    const double staleAt = pulseAt + freshnessSeconds;
    if (scenario == "prolonged_invalid") {
      for (double at = pulseAt + 2; at <= staleAt; at += 2) {
        fresh(at, -2048, false);
        assert(WaterTest::active() && WaterTest::physicalPump());
        assert(WaterTest::program.lastSample == pulseAt);
      }
      fresh(staleAt + .1, -2048, false);
    } else {
      Native::clock = uint64_t(std::llround(staleAt * 1e6));
      WaterTest::tick();
      assert(WaterTest::active() && WaterTest::physicalPump());
      Native::clock = uint64_t(std::llround((staleAt + .1) * 1e6));
      WaterTest::tick();
    }
    assert(!WaterTest::active() && !WaterTest::physicalPump());
    assert(tempControl.pump.edgeTimes.back() == uint64_t(std::llround((staleAt + .1) * 1e6)));
    assert(WaterTest::terminal["outcome"] == "failed");
    assert(WaterTest::terminal["reason"] == "beer_sensor_stale");
    verifyJournal();
    verifyRetainedAndRestart();
  } else if (scenario == "minimum_water_limit") {
    tempControl.glycolRuntime.cooling.minOn = 60;
    start();
    const double pulseAt = beginFirstPulse();
    fresh(pulseAt + .1, 64);
    assert(!WaterTest::physicalPump());
    assert(tempControl.pump.edgeTimes.back() == uint64_t(std::llround((pulseAt + .1) * 1e6)));
    assert(WaterTest::terminal["outcome"] == "stopped");
    assert(WaterTest::terminal["reason"] == "temperature_limit");
    verifyJournal();
    verifyRetainedAndRestart();
  } else if (scenario == "bath_fault") {
    start();
    const double pulseAt = beginFirstPulse();
    fresh(pulseAt + .1, 320, true, false);
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
    assert(!WaterTest::program.hasHistory());
    assert(WaterTest::program.head == 0 && WaterTest::program.count == 0);
    std::string finalMetadata;
    serializeJson(WaterTest::terminal, finalMetadata);
    const auto finalRecords = WaterTest::recordCount;
    const auto journalBytes = std::filesystem::file_size(Native::root + WaterTest::journalPath);
    assert(finalRecords > 0 && journalBytes == finalRecords * sizeof(Record));
    verifyJournal();
    uploadOnce();
    assert(Native::payloads.empty() && fs_exists(WaterTest::journalPath));
    assert(!WaterTest::program.hasHistory());
    Native::openFailurePath.clear();
    uploadOnce();
    assert(WaterTest::terminalDurable && Native::payloads.size()==1);
    std::string savedMetadata;
    assert(WaterTest::loadDocumentPayload(WaterTest::finishPath, savedMetadata));
    assert(savedMetadata == finalMetadata && WaterTest::recordCount == finalRecords);
    assert(std::filesystem::file_size(Native::root + WaterTest::journalPath) == journalBytes);
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
    const auto initial = WaterTest::manifest["test_program"]["controllers"][0];
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
    WaterTest::program.usefulResponse = true;
    WaterTest::program.beginController(Native::clock / 1e6);
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
    verifyControllerSnapshot(WaterTest::terminal["controllers"][0], dose);
    assert(WaterTest::loadDocument(WaterTest::finishPath, persisted));
    verifyControllerSnapshot(persisted["controllers"][0], dose);
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
    verifyControllerSnapshot(uploaded["controllers"][0], dose);
  } else if (scenario == "recovered_snapshot_after_reboot") {
    assert(WaterTest::terminalDurable && !WaterTest::testController);
    verifyControllerSnapshot(WaterTest::terminal["controllers"][0], false);
    submit();
    JsonDocument uploaded;
    assert(deserializeJson(uploaded,Native::payloads.back()) == DeserializationError::Ok);
    verifyControllerSnapshot(uploaded["controllers"][0], false);
  } else if (scenario == "controller_fast_loop_predictive" || scenario == "controller_fast_loop_dose") {
    const bool dose = scenario == "controller_fast_loop_dose";
    if (dose) extendedSettings.glycolCoolingAlgorithm = GlycolCooling::Algorithm::PulseDose;
    start();
    WaterTest::program.usefulResponse = true;
    WaterTest::program.beginController(Native::clock / 1e6);
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
    assert(WaterTest::terminal["controllers"][0]["initialized"] == true);
    assert(WaterTest::terminal["controllers"][0]["learning_status"] == "no_updates");
    assert(WaterTest::terminal["controllers"][0]["final_tuning"]["learning_updates"] == 0);
    verifyJournal();
    submit();
  } else if (scenario == "controller_continuous_pulse_limit") {
    start();
    WaterTest::program.usefulResponse = true;
    WaterTest::program.beginController(Native::clock / 1e6);
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
    assert(WaterTest::terminal["controller_completed"].isNull());
    assert(tempControl.pump.edgeTimes.back() == Native::clock);
    verifyJournal();
    submit();
  } else if (scenario == "controller_checkpoint_precision" || scenario == "checkpoint_precision_before_reboot") {
    controllerCheckpointPrecisionRegression(scenario == "checkpoint_precision_before_reboot");
  } else if (scenario == "recovered_checkpoint_precision_after_reboot") {
    assert(WaterTest::terminal["outcome"] == "interrupted");
    verifyCheckpointPrecisionUpload();
  } else if (scenario == "snapshot_exact_restoration") {
    snapshotExactRestorationRegression();
  } else if (scenario == "controller_observation_write_deadline") {
    controllerObservationWriteDeadlineRegression();
  } else if (scenario == "controller_declared_duration_recovery") {
    controllerDeclaredDurationRecoveryRegression();
  } else if (scenario == "controller_duration_boundary_before_reboot") {
    controllerDurationBoundaryBeforeReboot();
  } else if (scenario == "recovered_controller_duration_boundary_after_reboot") {
    recoveredControllerDurationBoundary();
  } else if (scenario == "controller_run_write_deadline") {
    controllerRunWriteDeadlineRegression();
  } else if (scenario.rfind("controller_final_", 0) == 0) {
    controllerFinalObservationRegression(scenario);
  } else if (scenario == "recovered_controller_final_tail_after_reboot") {
    recoveredControllerFinalObservationRegression();
  } else if (scenario == "dual_controllers_full_duration") {
    dualFullDurationRegression();
  } else if (scenario == "dual_settled_before_checkpoint_before_reboot") {
    legacyControllerEpisodesBeforeReboot(true);
  } else if (scenario == "recovered_dual_settled_before_checkpoint_after_reboot") {
    recoveredDualSettledBeforeCheckpointRegression();
  } else if (scenario == "dual_controller_stale_run") {
    dualStaleRunRegression();
  } else if (scenario == "dual_controllers_predictive_first" || scenario == "dual_controllers_dose_first" ||
             scenario == "dual_first_checkpoint_before_reboot" || scenario == "dual_second_running_before_reboot" ||
             scenario == "dual_controller_headroom_skip" || scenario == "dual_controller_checkpoint_failure" ||
             scenario == "dual_controller_corrupt_checkpoint") {
    dualControllerRegression(scenario);
  } else if (scenario == "recovered_dual_first_checkpoint_after_reboot" ||
             scenario == "recovered_dual_second_running_after_reboot") {
    recoveredDualControllerRegression(scenario);
  } else if (scenario == "controller_observations_predictive" || scenario == "controller_observations_dose" ||
             scenario == "controller_observation_write_failure" || scenario == "controller_observations_before_reboot" ||
             scenario == "controller_observation_stop" || scenario == "controller_observation_stale") {
    controllerObservationRegression(scenario);
  } else if (scenario == "recovered_controller_observations_after_reboot") {
    recoveredControllerObservationsRegression();
  } else if (scenario == "controller_episodes_before_reboot") {
    legacyControllerEpisodesBeforeReboot(false);
  } else if (scenario == "recovered_controller_episodes_after_reboot") {
    recoveredLegacyControllerEpisodes();
  } else if (scenario == "controller_interrupted_status") {
    start();
    WaterTest::program.controllerStarted = Native::clock/1e6;
    WaterTest::program.controllerCycles = 1;
    WaterTest::program.responseSettled = true;
    assert(WaterTest::requestStop(error));
    WaterTest::tick();
    assert(WaterTest::terminal["outcome"] == "stopped");
    assert(WaterTest::terminal["controller_completed"].isNull());
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
    const double pulseAt = beginFirstPulse();
    Native::fsyncUntilFail = 0;
    WaterTest::lastLoggedRead[0] = WaterTest::lastLoggedRead[1] = 0;
    fresh(pulseAt + .1);
    assert(!WaterTest::physicalPump());
    assert(WaterTest::terminal["reason"] == "recording_failure");
    assert(WaterTest::terminal["lost_ranges"].size() == 1);
    verifyJournal();
    verifyRetainedAndRestart();
  } else if (scenario == "edge_fsync_failure") {
    start();
    finishBaseline();
    Native::clock += 2000000;
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
    assert(!WaterTest::program.hasHistory());
    assert(tempControl.cs.mode == 'b');
    Native::openFailurePath.clear();
    assert(WaterTest::requestStart(d, error));
    WaterTest::tick();
    assert(WaterTest::active());
  } else if (scenario == "queue_overflow") {
    start();
    beginFirstPulse();
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
    beginFirstPulse();
    assert(WaterTest::physicalPump());
    WaterTest::closeJournal();
    FILE *f = fs_open(WaterTest::journalPath, "ab");
    fwrite("torn", 1, 4, f);
    fclose(f);
  } else if (scenario == "upload_pacing") {
    start();
    finishEligibleStopped();
    unsigned initialWaits = 0;
    size_t requests = 0;
    uint32_t acknowledged = 0;
    const auto deleted = Native::taskDeletes;
    Native::delays = 1000;
    Native::delayHook = [&](int milliseconds) {
      assert(Native::uploadWorkspace == nullptr && !WaterTest::uploaderBusy);
      if (!initialWaits) {
        assert(milliseconds == 1500 && Native::payloads.empty());
        ++initialWaits;
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
    } catch (const Native::TaskDeleted &) {
    }
    assert(initialWaits == 1 && Native::taskDeletes == deleted + 1);
    assert(WaterTest::uploadState == "submitted" && !fs_exists(WaterTest::journalPath));
    assert(acknowledged == WaterTest::recordCount);
    assert(requests == 1 + (WaterTest::recordCount + 11) / 12);
    assert(Native::payloads.size() == requests + 1);
  } else if (scenario == "upload_pacing_http_retry" || scenario == "upload_pacing_ack_retry" ||
             scenario == "upload_pacing_preparation_retry") {
    start();
    finishEligibleStopped();
    const bool preparationFailure = scenario == "upload_pacing_preparation_retry";
    unsigned step = 0;
    const auto deleted = Native::taskDeletes;
    Native::delays = 1000;
    Native::delayHook = [&](int milliseconds) {
      assert(Native::uploadWorkspace == nullptr && !WaterTest::uploaderBusy);
      if (step == 0) {
        assert(milliseconds == 1500 && Native::payloads.empty());
        step = 1;
      } else {
        assert(step == 1 && milliseconds == 50 && Native::payloads.size() == 1);
        assert(WaterTest::manifestUploaded && WaterTest::uploadedRecords == 0);
        Native::nextHttpCode = scenario == "upload_pacing_http_retry" ? 500 : 201;
        Native::invalidUploadAcknowledgement = scenario == "upload_pacing_ack_retry";
        Native::openFailurePath = preparationFailure ? WaterTest::journalPath : "";
        step = 2;
      }
    };
    try { WaterTest::uploader(nullptr); } catch (const Native::TaskDeleted &) {}
    assert(step == 2 && Native::taskDeletes == deleted + 1);
    assert(WaterTest::uploadState == "error" && WaterTest::uploadedRecords == 0);
    assert(Native::payloads.size() == (preparationFailure ? 1u : 2u));
    assert(fs_exists(WaterTest::journalPath));
    assert(WaterTest::nextUploaderAttemptUs == Native::clock + 30000000);
    Native::nextHttpCode = 201;
    Native::invalidUploadAcknowledgement = false;
    Native::openFailurePath.clear();
    Native::clock = WaterTest::nextUploaderAttemptUs;
    Native::delayHook = [&](int milliseconds) {
      assert(Native::uploadWorkspace == nullptr && !WaterTest::uploaderBusy);
      if (step == 2) {
        assert(milliseconds == 1500 && WaterTest::uploadedRecords == 0);
        step = 3;
      } else if (step == 3) {
        assert(milliseconds == 50 && WaterTest::uploadedRecords == 12);
        assert(Native::payloads.size() == (preparationFailure ? 2u : 3u));
        if (!preparationFailure) assert(Native::payloads[1] == Native::payloads[2]);
        step = 4;
      } else {
        assert(milliseconds == 50 && WaterTest::uploadState == "pending");
      }
    };
    try { WaterTest::uploader(nullptr); } catch (const Native::TaskDeleted &) {}
    assert(step == 4 && Native::taskDeletes == deleted + 2);
    assert(WaterTest::uploadState == "submitted" && WaterTest::uploadedRecords == WaterTest::recordCount);
    assert(!fs_exists(WaterTest::journalPath));
  } else if (scenario == "upload_retry") {
    start();
    finishEligibleStopped();
    assert(fs_exists(WaterTest::journalPath));
    const auto deleted = Native::taskDeletes;
    Native::delayHook = [&](int milliseconds) {
      assert(milliseconds != 30000); // Backoff releases the task stack now.
      assert(Native::uploadWorkspace == nullptr);
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
    assert(Native::taskDeletes == deleted + 3); // Two errors, then final completion.
  } else if (scenario == "upload_transport_diagnostics") {
    start();
    JsonDocument state;
    WaterTest::status(state);
    assert(state["upload_stage"] == "none");
    finishEligibleStopped();
    WaterTest::status(state);
    assert(state["upload_stage"] == "manifest");
    assert(state["upload_manifest_uploaded"] == false);
    assert(state["upload_records_uploaded"] == 0);
    assert(state["upload_records_total"] == WaterTest::recordCount);
    Native::nextHttpCode = 0;
    Native::nextHttpResult = ESP_ERR_HTTP_CONNECT;
    Native::nextSocketError = 24;
    uploadOnce();
    assert(WaterTest::uploadError.find("Manifest upload:") == 0);
    assert(WaterTest::uploadError.find("no server response") != std::string::npos);
    assert(WaterTest::uploadError.find("ESP_ERR_HTTP_CONNECT") != std::string::npos);
    assert(WaterTest::uploadError.find("socket error 24") != std::string::npos);
    assert(WaterTest::uploadError.find("status 0") == std::string::npos);
    assert(!WaterTest::manifestUploaded && fs_exists(WaterTest::journalPath));
    assert(Native::payloads.empty());
    Native::nextConnectionError = ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOST;
    uploadOnce();
    assert(WaterTest::uploadError.find("ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOST") != std::string::npos);
    Native::nextConnectionError = ESP_OK;
    Native::nextHttpCode = 201;
    Native::nextHttpResult = ESP_OK;
    Native::nextSocketError = 0;
    uploadOnce();
    assert(Native::payloads.size() == 1);
    WaterTest::status(state);
    assert(state["upload_stage"] == "batches");
    assert(state["upload_manifest_uploaded"] == true);
    for (unsigned n = 0; n < 100 && WaterTest::uploadedRecords < WaterTest::recordCount; ++n)
      uploadOnce();
    WaterTest::status(state);
    assert(state["upload_stage"] == "finish");
    assert(state["upload_records_uploaded"] == WaterTest::recordCount);
    Native::nextHttpCode = 400;
    Native::nextHttpBody = R"({"error":"controllers[1].target_c is required"})";
    uploadOnce();
    assert(WaterTest::uploadError == "Finish upload: HTTP upload pending (status 400). Server: controllers[1].target_c is required");
    assert(fs_exists(WaterTest::journalPath));
    const auto finish = Native::payloads.back();
    Native::nextHttpCode = 201;
    Native::nextHttpBody.clear();
    uploadOnce();
    assert(Native::payloads.back() == finish && WaterTest::uploadState == "submitted");
    assert(WaterTest::uploadError.empty() && !fs_exists(WaterTest::journalPath));
    WaterTest::status(state);
    assert(state["upload_stage"] == "complete");
  } else if (scenario == "upload_server_error_bounds") {
    start();
    finishEligibleStopped();
    Native::nextHttpCode = 400;
    Native::nextHttpBody = R"({"error":"invalid\r\nfield\tvalue\u0001"})";
    uploadOnce();
    assert(WaterTest::uploadError.find("Server: invalid  field value ") != std::string::npos);
    assert(WaterTest::uploadError.find('\n') == std::string::npos);
    Native::nextHttpBody = "{\"error\":\"" + std::string(600, 'x') + "\"}";
    uploadOnce();
    assert(WaterTest::uploadError.size() < 350);
    assert(WaterTest::uploadError.find(std::string(256, 'x') + "...") != std::string::npos);
    Native::nextHttpBody = "{\"error\":\"" + std::string(255, 'x') + "éé\"}";
    uploadOnce();
    assert(WaterTest::uploadError.find(std::string(255, 'x') + "...") != std::string::npos);
    Native::nextHttpBody = "<html>Private proxy internals</html>";
    uploadOnce();
    assert(WaterTest::uploadError.find("Private proxy") == std::string::npos);
    Native::nextHttpBody = R"({"detail":"Please retry later."})";
    uploadOnce();
    assert(WaterTest::uploadError.find("Server: Please retry later.") != std::string::npos);
    Native::nextHttpBody = std::string(8193, 'x');
    uploadOnce();
    assert(WaterTest::uploadError.find("Server reply exceeded 8192 bytes") != std::string::npos);
    assert(!WaterTest::manifestUploaded && fs_exists(WaterTest::journalPath));
    for (const auto &payload : Native::payloads)
      assert(payload == Native::payloads.front());
  } else if (scenario == "upload_request_setup_failure") {
    start();
    finishEligibleStopped();
    Native::nextHeaderResult = ESP_ERR_NO_MEM;
    uploadOnce();
    assert(Native::payloads.empty() && Native::httpCleanups == 1);
    assert(WaterTest::uploadError.find("HTTP request setup failed (ESP_ERR_NO_MEM)") != std::string::npos);
    Native::nextHeaderResult = ESP_OK;
    Native::nextHttpResult = ESP_ERR_NO_MEM;
    uploadOnce();
    assert(Native::payloads.empty() && Native::httpCleanups == 2);
    assert(!WaterTest::manifestUploaded && fs_exists(WaterTest::journalPath));
    Native::nextHttpResult = ESP_OK;
    uploadOnce();
    assert(WaterTest::manifestUploaded && Native::httpCleanups == 3);
  } else if (scenario == "upload_allocation_failure") {
    start();
    finishEligibleStopped();
    auto failAndRetry = [] {
      const size_t sent = Native::payloads.size();
      const uint32_t acknowledged = WaterTest::uploadedRecords;
      const bool manifestAcknowledged = WaterTest::manifestUploaded;
      Native::failHttpAllocation = true;
      uploadOnce();
      assert(WaterTest::uploadState == "error" && !WaterTest::uploaderBusy);
      assert(WaterTest::uploadedRecords == acknowledged);
      assert(WaterTest::manifestUploaded == manifestAcknowledged);
      assert(Native::payloads.size() == sent && Native::uploadWorkspace == nullptr);
      assert(fs_exists(WaterTest::journalPath));
      Native::failHttpAllocation = false;
      uploadOnce();
      assert(Native::payloads.size() == sent + 1 && Native::uploadWorkspace == nullptr);
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
  } else if (scenario == "observation_timeout") {
    start();
    const double offAt = completeFirstPulse();
    WaterTest::program.deadline = offAt + 240;
    while (WaterTest::active()) {
      const double at = Native::clock / 1e6 + 2;
      assert(at <= offAt + 240);
      fresh(at, int16_t(std::lround((20 - .002 * (at - offAt)) * 16)));
    }
    assert(!WaterTest::physicalPump() && !WaterTest::physicalHeat());
    assert(WaterTest::program.pulse == 1 && WaterTest::program.completedPulses == 1);
    assert(!WaterTest::program.responseSettled && WaterTest::program.controllerStarted == 0);
    assert(WaterTest::terminal["outcome"] == "inconclusive");
    assert(WaterTest::terminal["reason"] == "observation_timeout");
    assert(WaterTest::terminal["response_settled"] == false);
    assert(WaterTest::terminalDurable && WaterTest::uploadState == "pending");
    assert(!WaterTest::program.hasHistory());
    assert(fs_exists(WaterTest::journalPath) && fs_exists(WaterTest::finishPath));
    verifyJournal();
    submit();
    JsonDocument finish;
    assert(deserializeJson(finish, Native::payloads.back()) == DeserializationError::Ok);
    assert(finish["outcome"] == "inconclusive" && finish["reason"] == "observation_timeout");
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
    assert(!WaterTest::program.hasHistory());
    assert(WaterTest::program.head == 0 && WaterTest::program.count == 0);
    const auto finalRecords = WaterTest::recordCount;
    JsonDocument saved;
    assert(WaterTest::loadDocument(WaterTest::finishPath, saved));
    assert(saved["outcome"] == "completed");
    assert(saved["completed_pulses"] == WaterTest::terminal["completed_pulses"]);
    assert(saved["final_seq_by_boot"][WaterTest::recordingBoot] == finalRecords - 1);
    verifyJournal();
    submit();
    unsigned uploaded = 0;
    bool finishReceived = false;
    for (const auto &payload : Native::payloads) {
      JsonDocument request;
      assert(deserializeJson(request, payload) == DeserializationError::Ok);
      if (request["records"].is<JsonArray>()) {
        assert(request["first_seq"] == uploaded);
        for (const auto record : request["records"].as<JsonArrayConst>())
          assert(record["seq"] == uploaded++);
        assert(request["last_seq"] == uploaded - 1);
      }
      if (request["final_outputs"].is<JsonObject>()) {
        assert(request["outcome"] == "completed");
        assert(request["final_seq_by_boot"][WaterTest::recordingBoot] == finalRecords - 1);
        finishReceived = true;
      }
    }
    assert(uploaded == finalRecords && finishReceived);
  } else if (scenario == "failure_after_full_pulse" || scenario == "stale_stop_after_full_pulse") {
    start();
    completeFirstPulse();
    assert(WaterTest::active() && WaterTest::program.completedPulses == 1);
    if (scenario == "failure_after_full_pulse") {
      tempControl.heater->setActive(true);
    } else {
      Native::clock += uint64_t(std::llround((freshnessSeconds + .1) * 1e6));
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
  assert(WaterTest::program.hasHistory() == WaterTest::active());
  std::cout << "water_test_backend: " << scenario << " passed\n";
}
