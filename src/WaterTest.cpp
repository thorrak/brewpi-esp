#include "WaterTest.h"
#include "Brewpi.h"
#include "DeviceManager.h"
#include "ESPEepromAccess.h"
#include "EepromManager.h"
#include "GlycolCoolingController.h"
#include "TempControl.h"
#include "Version.h"
#include "WaterTestCore.h"
#include "WaterTestControllerSnapshot.h"
#include "WaterTestProtocol.h"
#include "WaterTestStorage.h"
#include "WaterTestTransport.h"
#include "getGuid.h"
#include "ntp.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <memory>
#include <sys/time.h>
#include <unistd.h>

namespace WaterTest {
namespace {
using namespace WaterTestCore;
using namespace WaterTestStorage;
using WaterTestTransport::endpoint;
constexpr size_t maxRecords = 12500;
constexpr size_t requiredBytes = maxRecords * sizeof(Record) + 32768;
constexpr size_t batchSize = 12;
constexpr size_t sampleQueueCapacity = 64;
constexpr uint64_t sparseSampleUs = 10000000;
constexpr uint64_t denseSampleUs = 2000000;
constexpr uint64_t edgeWindowUs = 120000000;
constexpr uint32_t denseRecordBudget = 2500;
struct Sample {
  uint64_t address, conversion, read;
  int16_t raw;
  bool valid;
};
struct Cache {
  Sample lastGood{};
  bool present = false;
};
SemaphoreHandle_t mutex = nullptr;
QueueHandle_t samples = nullptr;
std::atomic<bool> initialized{false}, owned{false}, running{false}, overflow{false};
std::atomic<bool> uploaderBusy{false};
Cache cache[Config::EepromFormat::MAX_DEVICES];
Program program;
Role recordedRole = Role::Complete;
std::unique_ptr<GlycolCooling::Controller> testController;
GlycolCooling::Algorithm testAlgorithm = GlycolCooling::Algorithm::PredictiveCoast;
uint64_t lastControllerRecordUs = 0;
double lastControllerStep = -1;
bool recordedControllerPump = false;
JsonDocument manifest, terminal;
std::string queuedStart, reason, uploadError, uploadState = "idle";
bool queuedStop = false, queuedResume = false, clockRecorded = false, recordingFailed = false;
bool lostRecord = false;
bool manifestUploaded = false, terminalDurable = false, recoveryBlocked = false;
uint32_t uploadedRecords = 0;
uint32_t seq = 0, recordCount = 0, lostSequence = 0;
uint64_t beerAddress = 0, glycolAddress = 0, lastBeerRead = 0, lastGlycolRead = 0, recordStartUs = 0;
double beerOffset = 0, glycolOffset = 0;
float beerC = NAN, glycolC = NAN;
char guid[17] = {}, currentBoot[37] = {};
ControlSettings savedControl;
FILE *journal = nullptr;
uint64_t lastRecordUs = 0;
uint64_t lastLoggedRead[2] = {}, nextSparseRead[2] = {}, denseUntilUs = 0;
uint32_t denseRecords = 0;
char recordingBoot[37] = {};

struct Guard {
  Guard() { xSemaphoreTakeRecursive(mutex, portMAX_DELAY); }
  ~Guard() { xSemaphoreGiveRecursive(mutex); }
};
uint64_t nowUs() { return static_cast<uint64_t>(esp_timer_get_time()); }
void uuid(char *out) {
  uint8_t bytes[16];
  esp_fill_random(bytes, sizeof(bytes));
  bytes[6] = (bytes[6] & 15) | 64;
  bytes[8] = (bytes[8] & 63) | 128;
  snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", bytes[0], bytes[1],
           bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
           bytes[12], bytes[13], bytes[14], bytes[15]);
}
void forceOff() {
  if (tempControl.cooler)
    tempControl.cooler->setActive(false);
  if (tempControl.heater)
    tempControl.heater->setActive(false);
  if (tempControl.light)
    tempControl.light->setActive(false);
  if (tempControl.fan)
    tempControl.fan->setActive(false);
}
bool physicalPump() { return tempControl.cooler && tempControl.cooler->isActive(); }
bool physicalHeat() {
  return (tempControl.heater && tempControl.heater->isActive()) ||
         (tempControl.cc.lightAsHeater && tempControl.light && tempControl.light->isActive());
}
uint64_t addressOf(const DeviceConfig &d) {
  uint64_t address = 0;
  memcpy(&address, d.hw.address, 8);
  return address;
}
std::string romOf(const DeviceConfig &d) {
  char rom[17];
  for (unsigned i = 0; i < 8; ++i)
    snprintf(rom + i * 2, 3, "%02X", d.hw.address[i]);
  return rom;
}
bool device(DeviceFunction function, DeviceConfig &result) {
  for (unsigned i = 0; i < Config::EepromFormat::MAX_DEVICES; ++i) {
    DeviceConfig d = eepromManager.fetchDevice(i);
    if (d.deviceFunction == function && !d.hw.deactivate && d.chamber == 1) {
      result = d;
      return true;
    }
  }
  return false;
}
Cache *cached(uint64_t address) {
  for (auto &c : cache)
    if (c.present && c.lastGood.address == address)
      return &c;
  return nullptr;
}
bool fresh(const DeviceConfig &d) {
  auto c = cached(addressOf(d));
  uint64_t now = nowUs();
  return c && c->lastGood.valid && now >= c->lastGood.read &&
         now - c->lastGood.read <= OneWireSensorPolicy::connectedTimeoutUs;
}
bool movedProbe() { return manifest["installation"]["glycol_temperature_source"] == "chamber_probe"; }
void recordFailure(uint32_t attempted) {
  forceOff();
  if (!lostRecord) {
    lostRecord = true;
    lostSequence = attempted;
  }
  recordingFailed = true;
}
bool append(Record r) {
  if (!journal || recordingFailed || recordCount >= maxRecords) {
    if (!recordingFailed)
      recordFailure(seq++);
    return false;
  }
  r.boot = 0;
  r.seq = seq++;
  r.t_us = std::max(r.t_us ? r.t_us : nowUs(), lastRecordUs);
  lastRecordUs = r.t_us;
  seal(r);
  bool ok = fwrite(&r, sizeof(r), 1, journal) == 1;
  if (ok)
    ok = fflush(journal) == 0;
  if (ok)
    ok = fsync(fileno(journal)) == 0;
  if (!ok) {
    forceOff();
    // The possibly torn record is excluded from the immutable upload prefix.
    ftruncate(fileno(journal), recordCount * sizeof(Record));
    clearerr(journal);
    recordFailure(r.seq);
    return false;
  }
  ++recordCount;
  return true;
}
void recordOutput(bool pump, bool requested, bool edge, Reason why, uint64_t appliedUs = 0) {
  if (edge)
    denseUntilUs = (appliedUs ? appliedUs : nowUs()) + edgeWindowUs;
  Record r{};
  r.t_us = appliedUs;
  r.kind = 3;
  r.role = pump ? 0 : 1;
  r.flags = (physicalPump() ? 2 : 0) | (requested ? 4 : 0) | (edge ? 8 : 0);
  r.code = static_cast<uint8_t>(why);
  append(r);
}
void recordPhase(uint64_t transitionUs = 0) {
  Record r{};
  r.kind = 4;
  r.t_us = transitionUs;
  r.code = static_cast<uint8_t>(program.phase);
  r.pulse = program.pulse;
  r.raw = program.block;
  r.flags = static_cast<uint8_t>(program.role);
  recordedRole = program.role;
  r.detail = program.deadline > nowUs() / 1e6 ? uint32_t(program.deadline - nowUs() / 1e6) : 0;
  append(r);
}
void recordController() {
  if (!testController || program.phase != Phase::Controller)
    return;
  const uint64_t now = nowUs();
  if (lastControllerRecordUs && now - lastControllerRecordUs < 60000000 && recordedControllerPump == program.pump)
    return;
  const auto &out = testController->output();
  Record r{};
  r.kind = 7;
  r.t_us = now;
  r.code = static_cast<uint8_t>(testController->selection());
  r.pulse = static_cast<uint8_t>(out.phase);
  r.raw = std::lround(program.targetC * 16);
  r.flags = program.pump ? 2 : 0;
  const float rate = out.rate_c_per_s, coast = out.coast_s;
  const float endpoint = out.predicted_endpoint_c;
  const float gain = testController->selection() == GlycolCooling::Algorithm::PredictiveCoast ? out.budget_gain_c_per_s
                                                                                              : out.gain_c_per_on_s;
  memcpy(&r.read_us, &rate, 4);
  memcpy(reinterpret_cast<char *>(&r.read_us) + 4, &coast, 4);
  memcpy(&r.conversion_us, &endpoint, 4);
  memcpy(&r.detail, &gain, 4);
  append(r);
  lastControllerRecordUs = now;
  recordedControllerPump = program.pump;
}
void recordClock() {
  if (clockRecorded || !isNtpSynced())
    return;
  timeval tv{};
  gettimeofday(&tv, nullptr);
  if (tv.tv_sec < 1577836800)
    return;
  Record r{};
  r.kind = 1;
  r.read_us = uint64_t(tv.tv_sec) * 1000000 + tv.tv_usec;
  // Assemble UTC and monotonic immediately together; append happens before flash.
  if (append(r))
    clockRecorded = true;
}
void recordBoot() {
  Record r{};
  r.kind = 0;
  r.code = static_cast<uint8_t>(Reason::Start);
  r.detail = esp_reset_reason();
  append(r);
  recordClock();
  recordOutput(true, false, false, Reason::Start);
  recordOutput(false, false, false, Reason::Start);
}
void common(JsonDocument &doc) {
  const std::string testId = manifest["test_id"].as<std::string>();
  doc["schema_version"] = 1;
  doc["device_guid"] = guid;
  doc["test_id"] = testId;
}
void closeJournal() {
  if (journal) {
    fflush(journal);
    fsync(fileno(journal));
    fclose(journal);
    journal = nullptr;
  }
}
void controllerTerminalMetadata(JsonObject out, bool interrupted = false) {
  WaterTestControllerSnapshot::final(out, manifest["test_program"]["controller"].as<JsonObjectConst>(),
                                    testController.get(), interrupted,
                                    uint64_t(std::llround(program.controllerStarted * 1000000)), program.targetC,
                                    nowUs());
}
void finishRun() {
  const uint64_t offUs = nowUs();
  const bool wasPump = physicalPump();
  const bool wasHeat = physicalHeat();
  forceOff();
  recordOutput(true, false, wasPump, program.reason, offUs);
  recordOutput(false, false, wasHeat, program.reason, offUs);
  if (program.reason == Reason::SensorFault || program.reason == Reason::SensorStale ||
      program.reason == Reason::StorageFailure || program.reason == Reason::QueueOverflow ||
      program.reason == Reason::UnexpectedOutput) {
    Record event{};
    event.kind = (program.reason == Reason::QueueOverflow || program.reason == Reason::UnexpectedOutput) ? 6 : 5;
    event.code = static_cast<uint8_t>(program.reason);
    append(event);
  }
  recordPhase();
  if (recordingFailed) {
    program.outcome = End::Failed;
    program.reason = Reason::StorageFailure;
  }
  terminal.clear();
  common(terminal);
  terminal["outcome"] = endName(program.outcome);
  terminal["completed_pulses"] = program.completedPulses;
  terminal["reason"] = reasonName(program.reason);
  terminal["final_phase"] = "finished";
  terminal["t_us"] = nowUs();
  terminal["boot_id"] = recordingBoot;
  terminal["final_outputs"]["pump_on"] = false;
  terminal["final_outputs"]["heater_on"] = false;
  terminal["elapsed_s"] = program.started > 0 ? uint32_t(nowUs() / 1e6 - program.started) : 0;
  terminal["response_settled"] = program.responseSettled;
  terminal["controller_completed"] = program.outcome == End::Completed && program.controllerStarted > 0 &&
                                     program.responseSettled && program.controllerCycles > 0 &&
                                     !program.controllerTimedOut;
  terminal["controller_timed_out"] = program.controllerTimedOut;
  terminal["useful_response"] = program.usefulResponse;
  terminal["baseline_noise_c"] = program.noiseC;
  terminal["baseline_drift_c_per_s"] = program.baselineDrift;
  controllerTerminalMetadata(terminal["controller"].to<JsonObject>());
  JsonObject bounds = terminal["final_seq_by_boot"].to<JsonObject>();
  closeJournal();
  bounds[recordingBoot] = recordCount ? int64_t(recordCount) - 1 : -1;
  if (lostRecord) {
    bounds[recordingBoot] = int64_t(seq) - 1;
    auto loss = terminal["lost_ranges"].to<JsonArray>().add<JsonObject>();
    loss["boot_id"] = recordingBoot;
    loss["first_seq"] = lostSequence;
    loss["last_seq"] = seq - 1;
  }
  releaseMetadataReserve();
  terminalDurable = saveDocument(finishPath, terminal);
  uploadState = terminalDurable ? "pending" : "error";
  uploadError.clear();
  reason = reasonName(program.reason);
  if (!terminalDurable)
    uploadError = "The recording is retained; saving its finish details will be retried.";
  running = false;
  queuedStop = false;
}
void settingsToManifest(JsonObject p) {
  p["mode"] = std::string(1, tempControl.cs.mode);
  p["beer_setting_raw"] = tempControl.cs.beerSetting;
  p["fridge_setting_raw"] = tempControl.cs.fridgeSetting;
  p["heat_estimator_raw"] = tempControl.cs.heatEstimator;
  p["cool_estimator_raw"] = tempControl.cs.coolEstimator;
  p["glycol"] = extendedSettings.glycol;
  p["cooling_algorithm"] = GlycolCooling::selectionName(extendedSettings.glycolCoolingAlgorithm);
}
void sensorManifest(JsonObject o, const DeviceConfig &d, const char *source, const char *placement) {
  o["rom"] = romOf(d);
  o["source_role"] = source;
  o["placement"] = placement;
  o["resolution_bits"] = 12;
  o["cadence_s"] = 2;
  o["calibration_offset_c"] = d.hw.calibration / 16.0;
  o["decision_filter"] = "none_calibrated_raw";
}
void outputManifest(JsonObject o, const DeviceConfig &d) {
  o["pin"] = d.hw.pinNr;
  o["inverted"] = d.hw.invert;
  o["hardware"] = "gpio";
}
uint32_t minimumOn() {
  return std::max<uint32_t>(2, extendedSettings.glycol
                                   ? static_cast<uint32_t>(std::ceil(tempControl.glycolRuntime.cooling.minOnSeconds()))
                                   : minTimes.MIN_COOL_ON_TIME);
}
uint32_t minimumOff() {
  return std::max<uint32_t>(2, extendedSettings.glycol
                                   ? static_cast<uint32_t>(std::ceil(tempControl.glycolRuntime.cooling.minOffSeconds()))
                                   : minTimes.MIN_COOL_OFF_TIME);
}
std::string preflight(bool useGlycol, DeviceConfig &beer, DeviceConfig &glycol, DeviceConfig &cool) {
  const bool hasBeer = device(DEVICE_BEER_TEMP, beer);
  const bool hasCooler = device(DEVICE_CHAMBER_COOL, cool);
  const bool hasGlycol = useGlycol && device(DEVICE_CHAMBER_TEMP, glycol);
  if (!extendedSettings.glycol)
    return "Enable glycol mode before running this glycol-pump water test.";
  if (!hasBeer || beer.deviceHardware != DEVICE_HARDWARE_ONEWIRE_TEMP || beer.hw.address[0] != 0x28)
    return "Configure a DS18B20 beer probe before starting.";
  if (!fresh(beer))
    return "Waiting for a fresh valid beer probe reading.";
  if (!hasCooler || cool.deviceHardware != DEVICE_HARDWARE_PIN || !tempControl.cooler)
    return "Configure a local GPIO cooling relay before starting.";
  if (physicalPump() || physicalHeat())
    return "Wait until normal heating and cooling outputs are OFF, then start the water test.";
  if (useGlycol) {
    if (!hasGlycol || glycol.deviceHardware != DEVICE_HARDWARE_ONEWIRE_TEMP || glycol.hw.address[0] != 0x28)
      return "Configure a DS18B20 glycol probe before starting.";
    if (addressOf(beer) == addressOf(glycol))
      return "Beer and glycol probes must be distinct.";
    if (!fresh(glycol))
      return "Waiting for a fresh valid glycol bath probe reading.";
  }
  if (minimumOn() > maximumPulseSeconds || minimumOff() > observationSeconds)
    return "Configured cooling relay minimum times exceed this test's conservative pulse limits.";
  float temperature = cached(addressOf(beer))->lastGood.raw / 16.0 + beer.hw.calibration / 16.0;
  if (temperature < 8 || temperature > 35)
    return "Start with water between 8 and 35 C (46.4 to 95 F).";
  if (freeBytes() < requiredBytes)
    return "Not enough free recording space. A full offline test needs " + std::to_string(requiredBytes) +
           " free bytes.";
  return "";
}
void recordingMetadata() {
  manifest["acquisition"]["recording_boot_id"] = recordingBoot;
  manifest["acquisition"]["started_us"] = recordStartUs;
  manifest["acquisition"]["recording_cadence_s"] = sparseSampleUs / 1000000;
  manifest["acquisition"]["dense_cadence_s"] = denseSampleUs / 1000000;
  manifest["acquisition"]["dense_window_s"] = edgeWindowUs / 1000000;
  manifest["acquisition"]["dense_extra_record_budget"] = denseRecordBudget;
  manifest["acquisition"]["max_records"] = maxRecords;
}
std::unique_ptr<GlycolCooling::Controller> makeTestController(uint32_t minimumOn, uint32_t minimumOff,
                                                             GlycolCooling::Algorithm algorithm) {
  PredictiveCooling::Config predictive;
  AdaptiveCooling::Config dose;
  predictive.min_on_s = dose.min_on_s = minimumOn;
  predictive.min_off_s = dose.min_off_s = minimumOff;
  return std::unique_ptr<GlycolCooling::Controller>(
      new GlycolCooling::Controller(algorithm, predictive, dose));
}
void controllerPlan(JsonObject out) {
  // The preview uses the same constructor and effective relay settings as the
  // eventual test controller. It neither steps nor touches normal brewing tuning.
  auto defaults = makeTestController(minimumOn(), minimumOff(), testAlgorithm);
  WaterTestControllerSnapshot::initial(out, *defaults, COOLING_IMPLEMENTATION_ID);
}
void startRun(const std::string &payload) {
  JsonDocument input;
  if (deserializeJson(input, payload) != DeserializationError::Ok) {
    reason = "Invalid start request.";
    owned = false;
    return;
  }
  if (!removePreviousDataset()) {
    reason = "Unable to remove the previous test's files; a new recording was not started.";
    owned = false;
    return;
  }
  bool bath = input["glycol_temperature_source"] == "chamber_probe";
  DeviceConfig beer{}, glycol{}, cool{};
  std::string failure = preflight(bath, beer, glycol, cool);
  if (!failure.empty()) {
    reason = failure;
    owned = false;
    return;
  }
  double bathC = bath ? cached(addressOf(glycol))->lastGood.raw / 16.0 + glycol.hw.calibration / 16.0
                      : input["reported_chiller_setpoint_c"].as<double>();
  double waterC = cached(addressOf(beer))->lastGood.raw / 16.0 + beer.hw.calibration / 16.0;
  if (input["glycol_temperature_source"] != "unknown" && waterC - bathC < 2) {
    reason = "Water must start at least 2 C (3.6 F) warmer than the declared glycol input.";
    owned = false;
    return;
  }
  manifest.clear();
  terminal.clear();
  recordingFailed = false;
  lostRecord = false;
  recordCount = seq = 0;
  manifestUploaded = false;
  terminalDurable = false;
  recoveryBlocked = false;
  uploadedRecords = 0;
  denseRecords = 0;
  lastLoggedRead[0] = lastLoggedRead[1] = 0;
  nextSparseRead[0] = nextSparseRead[1] = 0;
  memcpy(recordingBoot, currentBoot, sizeof(recordingBoot));
  clockRecorded = false;
  lastRecordUs = 0;
  char testId[37];
  testAlgorithm = extendedSettings.glycolCoolingAlgorithm;
  uuid(testId);
  manifest["test_id"] = testId;
  common(manifest);
  auto install = manifest["installation"].to<JsonObject>();
  for (auto key : {"fermenter_model", "fermenter_capacity_l", "water_volume_l", "cooling_type", "probe_mounting",
                   "glycol_temperature_source", "reported_chiller_setpoint_c"})
    install[key] = input[key];
  if (input["reported_input"].is<JsonObject>())
    for (auto key : {"volume_unit", "water_volume", "fermenter_capacity", "temperature_unit", "glycol_setpoint"})
      install["reported_input"][key] = input["reported_input"][key];
  manifest["consent"]["opt_in"] = true;
  manifest["consent"]["version"] = "water-test-consent-v1";
  manifest["consent"]["follow_up"] = false;
  manifest["consent"]["source"] = "device_web_ui";
  manifest["firmware"]["version"] = Config::Version::release;
  manifest["firmware"]["revision"] = FIRMWARE_REVISION;
  manifest["firmware"]["commit"] = Config::Version::git_sha;
  manifest["firmware"]["modified"] = Config::Version::git_dirty;
  manifest["firmware"]["board"] = CONTROLLER_TYPE;
  auto plan = manifest["test_program"].to<JsonObject>();
  plan["version"] = "adaptive-campaign-v1";
  plan["baseline_s"] = baselineSeconds;
  plan["minimum_baseline_s"] = minimumBaselineSeconds;
  plan["observation_s"] = observationSeconds;
  plan["max_duration_s"] = maximumSeconds;
  plan["max_pulse_s"] = maximumPulseSeconds;
  plan["max_pump_s"] = maximumPumpSeconds;
  plan["max_drop_c"] = maximumDropC;
  plan["minimum_water_c"] = minimumWaterC;
  plan["decision_rules"]["useful_drop_c"] = .20;
  plan["decision_rules"]["useful_noise_multiplier"] = 4;
  plan["decision_rules"]["settling_window_min_s"] = 90;
  plan["decision_rules"]["settling_window_max_s"] = 1800;
  plan["decision_rules"]["cooling_tail_rate_tolerance_c_per_s"] = coolingTailRateTolerance;
  plan["decision_rules"]["weak_pilot_off_s"] = 300;
  plan["decision_rules"]["validation_gap_s"] = 30;
  plan["decision_rules"]["statistical_decision_interval_s"] = 1;
  plan["pulse_selection"] = "10/30/90/270/810/1800s caps; stop early at max(0.20C,4x baseline noise); response-sized "
                            "contrast then two reserved pulses; later doses capped by remaining temperature range";
  plan["observation_selection"] =
      "baseline60-300s; off>=180s, extended by observed response delay, until response plateau; final check retains "
      "measured calibration coast; weak pilot may advance after300s only without credible ongoing cooling, without "
      "claiming settled; off cap21600s";
  controllerPlan(plan["controller"].to<JsonObject>());
  plan["controller"]["target_drop_c"] = .25;
  plan["controller"]["max_duration_s"] = controllerSeconds;
  plan["controller"]["scope"] = "cooling_controller_core_with_test_safety_limits";
  sensorManifest(manifest["sensors"]["beer"].to<JsonObject>(), beer, "beer", input["probe_mounting"] | "unknown");
  if (bath)
    sensorManifest(manifest["sensors"]["glycol"].to<JsonObject>(), glycol, "chamber", "glycol_bath");
  outputManifest(manifest["outputs"]["pump"].to<JsonObject>(), cool);
  DeviceConfig heat;
  if (device(DEVICE_CHAMBER_HEAT, heat))
    outputManifest(manifest["outputs"]["heater"].to<JsonObject>(), heat);
  manifest["outputs"]["initial"]["pump_on"] = false;
  manifest["outputs"]["initial"]["heater_on"] = false;
  manifest["outputs"]["normal_heat_uses_light"] = bool(tempControl.cc.lightAsHeater);
  manifest["outputs"]["minimum_on_s"] = minimumOn();
  manifest["outputs"]["minimum_off_s"] = minimumOff();
  settingsToManifest(manifest["prior_control"].to<JsonObject>());
  savedControl = tempControl.cs;
  manifest["acquisition"]["timestamp_unit"] = "microseconds";
  manifest["acquisition"]["sequence_start"] = 0;
  manifest["acquisition"]["freshness_limit_s"] = freshnessSeconds;
  manifest["acquisition"]["recording_format"] = "crc32-binary-v1";
  manifest["acquisition"]["clock_status_at_start"] = isNtpSynced() ? "synced" : "unknown";
  journal = fs_open(journalPath, "wb");
  if (!journal) {
    reason = "Unable to open the test recording file.";
    manifest.clear();
    uploadState = "idle";
    owned = false;
    return;
  }
  beerAddress = addressOf(beer);
  glycolAddress = bath ? addressOf(glycol) : 0;
  beerOffset = beer.hw.calibration / 16.0;
  glycolOffset = glycol.hw.calibration / 16.0;
  beerC = waterC;
  glycolC = bath ? bathC : NAN;
  lastBeerRead = lastGlycolRead = 0;
  recordStartUs = nowUs();
  denseUntilUs = recordStartUs + edgeWindowUs;
  recordingMetadata();
  if (!reserveMetadata() || !saveDocument(manifestPath, manifest)) {
    closeJournal();
    removePreviousDataset();
    manifest.clear();
    owned = false;
    uploadState = "idle";
    reason = "Unable to reserve and save the offline recording; the test was not started.";
    return;
  }
  forceOff();
  tempControl.cs.mode = Modes::off;
  program.start(recordStartUs / 1e6, waterC, minimumOn(), minimumOff());
  testController.reset();
  lastControllerRecordUs = 0;
  lastControllerStep = -1;
  recordedRole = Role::Complete;
  program.lastSample = cached(beerAddress)->lastGood.read / 1e6;
  reason.clear();
  uploadState = "pending";
  uploadError.clear();
  overflow = false;
  running = true;
  recordBoot();
  recordPhase();
  recordStartUs = lastRecordUs; // discard queued reads acquired before the initial record boundary
}
// Apply relay changes before any potentially blocking journal operation. The
// captured edge time is preserved even when the subsequent flash write is slow.
bool applyOutputs() {
  const bool changed = physicalPump() != program.pump;
  if (tempControl.cooler)
    tempControl.cooler->setActive(program.pump);
  if (tempControl.heater)
    tempControl.heater->setActive(false);
  if (tempControl.light)
    tempControl.light->setActive(false);
  if (tempControl.fan)
    tempControl.fan->setActive(false);
  return changed;
}
void serviceProgram(bool allowNewPulse);
void persistProgram(Phase oldPhase, bool pumpChanged, uint64_t appliedUs) {
  if (pumpChanged) {
    recordOutput(true, program.pump, true, program.reason, appliedUs);
    if (program.pump) {
      const auto phase = program.phase;
      // A slow ON-edge write may consume the entire pulse. This nested service
      // can only turn OFF; if it does, it persists that transition itself.
      serviceProgram(false);
      if (!running || phase != program.phase)
        return;
    }
  }
  if ((oldPhase != program.phase || recordedRole != program.role) && program.active()) {
    recordPhase(appliedUs);
    if (program.pump) {
      serviceProgram(false);
      if (!running)
        return;
    }
  }
  if (recordingFailed && program.active())
    program.finish(nowUs() / 1e6, End::Failed, Reason::StorageFailure);
  if (!program.active())
    finishRun();
}
void applyProgram(Phase oldPhase) {
  const uint64_t appliedUs = nowUs();
  const bool changed = applyOutputs();
  persistProgram(oldPhase, changed, appliedUs);
}
// New pulses wait until the current queue snapshot has been consumed. Protective
// OFF decisions, user stops, and pulse deadlines are serviced between writes.
void serviceProgram(bool allowNewPulse) {
  if (!running)
    return;
  const auto phase = program.phase;
  const double now = nowUs() / 1e6;
  const bool unexpected = physicalHeat() || physicalPump() != program.pump;
  if (queuedStop) {
    queuedStop = false;
    program.stop(now);
  }
  if (recordingFailed || overflow)
    program.finish(now, End::Failed, recordingFailed ? Reason::StorageFailure : Reason::QueueOverflow);
  else if (unexpected)
    program.finish(now, End::Failed, Reason::UnexpectedOutput);
  else if (allowNewPulse || program.pump || program.stopping || now - program.started >= maximumSeconds)
    program.tick(now);
  // The same portable controller used in normal glycol mode gets fresh tuning.
  // Its output history is exactly the test pump history during this block.
  if (allowNewPulse && phase == Phase::Controller && program.phase == Phase::Controller) {
    if (!testController) {
      testController = makeTestController(program.minimumOn, program.minimumOff, testAlgorithm);
      testController->externalOff(program.switched);
    }
    // Match normal GlycolMode's 1 Hz cadence. The outer loop runs much faster;
    // feeding every pass would overflow the controller's bounded filters.
    if (lastControllerStep < 0 || now - lastControllerStep >= 1.) {
      lastControllerStep = now;
      const auto decision = testController->step(now, program.latestC, program.targetC, true);
      program.setControllerPump(now, decision.pump_on);
    }
  }
  applyProgram(phase);
  if (allowNewPulse && running && program.phase == Phase::Controller)
    recordController();
}
Record sampleRecord(const Sample &sample, bool beer) {
  Record r{};
  r.t_us = nowUs();
  r.kind = 2;
  r.role = beer ? 0 : 1;
  r.raw = sample.raw;
  r.read_us = sample.read;
  r.conversion_us =
      sample.read >= sample.conversion ? uint32_t(std::min<uint64_t>(sample.read - sample.conversion, UINT32_MAX)) : 0;
  r.flags = (sample.valid ? 1 : 0) | (physicalPump() ? 2 : 0);
  return r;
}
void processSample(const Sample &sample) {
  Cache *slot = cached(sample.address);
  if (!slot)
    for (auto &c : cache)
      if (!c.present) {
        slot = &c;
        break;
      }
  if (slot) {
    slot->lastGood.address = sample.address;
    if (sample.valid && (!slot->lastGood.valid || sample.read > slot->lastGood.read))
      slot->lastGood = sample;
    slot->present = true;
  }
  if (!running || sample.read < recordStartUs)
    return;
  bool beer = sample.address == beerAddress;
  bool glycol = glycolAddress && sample.address == glycolAddress;
  if (!beer && !glycol)
    return;
  uint64_t &previous = beer ? lastBeerRead : lastGlycolRead;
  if (sample.read <= previous)
    return;
  previous = sample.read;
  const Record r = sampleRecord(sample, beer);
  const auto phase = program.phase;
  if (beer) {
    if (sample.valid)
      beerC = sample.raw / 16.0 + beerOffset;
    program.sample(sample.read / 1e6, beerC, sample.valid, nowUs() / 1e6);
  } else if (sample.valid)
    glycolC = sample.raw / 16.0 + glycolOffset;
  const bool changed = applyOutputs();
  if (changed)
    denseUntilUs = r.t_us + edgeWindowUs;
  const unsigned role = beer ? 0 : 1;
  const bool regular = !nextSparseRead[role] || sample.read >= nextSparseRead[role];
  const bool dense =
      denseRecords < denseRecordBudget &&
      (!sample.valid ||
       (sample.read <= denseUntilUs && (!lastLoggedRead[role] || sample.read - lastLoggedRead[role] >= denseSampleUs)));
  // All fresh samples above drive decisions. Only the durable journal has a
  // declared lower cadence, with a bounded denser interval around pump edges.
  if (regular || dense || !program.active()) {
    append(r);
    lastLoggedRead[role] = sample.read;
    if (regular)
      nextSparseRead[role] = sample.read + sparseSampleUs;
    else if (dense)
      ++denseRecords;
  }
  persistProgram(phase, changed, r.t_us);
}

struct UploadBufferDeleter {
  void operator()(char *buffer) const { heap_caps_free(buffer); }
};
using UploadBuffer = std::unique_ptr<char[], UploadBufferDeleter>;

UploadBuffer serializeUpload(const JsonDocument &doc, size_t &length) {
  if (doc.overflowed())
    return nullptr;
  length = measureJson(doc);
  UploadBuffer body(static_cast<char *>(heap_caps_malloc(length, MALLOC_CAP_8BIT)));
  if (body && serializeJson(doc, body.get(), length) != length)
    body.reset();
  return body;
}

UploadBuffer storedUpload(const char *path, size_t &length) {
  std::string payload;
  if (!loadDocumentPayload(path, payload))
    return nullptr;
  length = payload.size();
  UploadBuffer body(static_cast<char *>(heap_caps_malloc(length, MALLOC_CAP_8BIT)));
  if (body)
    memcpy(body.get(), payload.data(), length);
  return body;
}

// This worker never touches actuators. It starts only after acquisition is closed;
// upload/response delays cannot change an experiment's timing or sample cadence.
void uploader(void *) {
  uint32_t delayMs = 1500;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(delayMs));
    delayMs = 1500;
    std::string id, path, error;
    UploadBuffer body;
    size_t bodyLength = 0;
    uint32_t next = 0, end = 0;
    uint32_t firstSequence = 0, lastSequence = 0;
    bool isManifest = false, isFinish = false;
    std::string batchId;
    JsonDocument response;
    {
      Guard lock;
      if (running || !queuedStart.empty() || terminal.isNull() || recoveryBlocked || uploadState == "submitted")
        continue;
      if (!terminalDurable) {
        terminalDurable = saveDocument(finishPath, terminal);
        if (!terminalDurable) {
          uploadState = "error";
          uploadError = "The recording is retained; finish details could not yet be saved.";
          continue;
        }
      }
      next = uploadedRecords;
      id = manifest["test_id"].as<std::string>();
      path = "/api/v1/water-tests/" + id;
      if (!manifestUploaded) {
        isManifest = true;
        body = storedUpload(manifestPath, bodyLength);
      } else if (next < recordCount) {
        JsonDocument request;
        FILE *f = fs_open(journalPath, "rb");
        if (!f) {
          uploadState = "error";
          uploadError = "Recording file is unavailable.";
          continue;
        }
        fseek(f, next * sizeof(Record), SEEK_SET);
        Record r{};
        common(request);
        auto list = request["records"].to<JsonArray>();
        end = next;
        while (end < recordCount && end - next < batchSize && fread(&r, sizeof(r), 1, f) == 1) {
          if (!valid(r) || r.boot != 0)
            break;
          if (end == next) {
            firstSequence = r.seq;
            request["first_seq"] = firstSequence;
          }
          lastSequence = r.seq;
          request["last_seq"] = lastSequence;
          WaterTestProtocol::recordToJson(list.add<JsonObject>(), r, recordingBoot,
                                          manifest["sensors"][r.role ? "glycol" : "beer"]["calibration_offset_c"] |
                                              0.0);
          ++end;
        }
        fclose(f);
        if (end == next) {
          uploadState = "error";
          uploadError = "Recording checksum mismatch; retained for inspection.";
          continue;
        }
        batchId = WaterTestProtocol::batchIdentifier(id, next);
        request["batch_id"] = batchId;
        request["boot_id"] = recordingBoot;
        body = serializeUpload(request, bodyLength);
        path += "/batches";
      } else {
        isFinish = true;
        body = storedUpload(finishPath, bodyLength);
        path += "/finish";
      }
      if (!body) {
        uploadState = "error";
        uploadError = "Low memory.";
        continue;
      }
      uploadState = "uploading";
      uploaderBusy = true;
    }
    bool ok = WaterTestTransport::send(path, !isManifest && !isFinish, std::string_view(body.get(), bodyLength),
                                       response, error);
    body.reset();
    if (ok) {
      ok = WaterTestProtocol::acknowledged(response.as<JsonVariantConst>(), id, guid);
      if (!isManifest && !isFinish)
        ok = WaterTestProtocol::batchAcknowledged(response.as<JsonVariantConst>(), id, guid, batchId, firstSequence,
                                                  lastSequence);
      if (isFinish)
        ok = WaterTestProtocol::finishAcknowledged(response.as<JsonVariantConst>(), id, guid);
      if (!ok)
        error = "Server did not acknowledge this complete immutable request; retrying original data.";
    }
    response.clear();
    {
      Guard lock;
      if (ok) {
        if (isManifest)
          manifestUploaded = true;
        else if (!isFinish)
          uploadedRecords = end;
        if (isFinish) {
          JsonDocument receipt;
          receipt["test_id"] = manifest["test_id"];
          receipt["submitted"] = true;
          if (!saveDocument(receiptPath, receipt)) {
            ok = false;
            error = "Server accepted the test; saving its receipt will be retried before removing local data.";
          }
        }
        uploadError.clear();
        uploadState = isFinish && ok ? "submitted" : "pending";
        if (!isFinish)
          delayMs = 50;
        if (isFinish && ok) {
          fs_remove(journalPath);
          if (!owned)
            removePreviousDataset();
        }
      }
      if (!ok) {
        uploadState = "error";
        uploadError = error;
      }
      uploaderBusy = false;
    }
    if (!ok)
      vTaskDelay(pdMS_TO_TICKS(30000));
  }
}
// Recovery closes an interrupted experiment; it never resumes a pulse or
// invents an OFF edge in the old boot's monotonic clock domain.
void recoverDataset() {
  if (!fs_exists(manifestPath) && !fs_exists(journalPath))
    return;
  auto blocked = [](const char *message) {
    recoveryBlocked = true;
    owned = true;
    forceOff();
    uploadState = "error";
    uploadError = message;
    reason = "Saved recording needs recovery; outputs are held off.";
  };
  if (!loadDocument(manifestPath, manifest) || !manifest["test_id"].is<const char *>() ||
      manifest["device_guid"] != guid || !manifest["acquisition"]["recording_boot_id"].is<const char *>()) {
    manifest.clear();
    blocked("Saved test metadata is damaged. Its files have been retained for recovery.");
    return;
  }
  const std::string boot = manifest["acquisition"]["recording_boot_id"].as<std::string>();
  if (boot.size() != 36) {
    blocked("Saved recording boot identifier is invalid; its files have been retained.");
    return;
  }
  memcpy(recordingBoot, boot.c_str(), sizeof(recordingBoot));
  const char *mode = manifest["prior_control"]["mode"] | "o";
  savedControl.mode = mode[0];
  savedControl.beerSetting = manifest["prior_control"]["beer_setting_raw"] | 0;
  savedControl.fridgeSetting = manifest["prior_control"]["fridge_setting_raw"] | 0;
  savedControl.heatEstimator = manifest["prior_control"]["heat_estimator_raw"] | 0;
  savedControl.coolEstimator = manifest["prior_control"]["cool_estimator_raw"] | 0;
  JsonDocument resumed, receipt;
  const bool wasResumed =
      loadDocument(resumedPath, resumed) && resumed["test_id"] == manifest["test_id"] && resumed["resumed"] == true;
  const bool submitted =
      loadDocument(receiptPath, receipt) && receipt["test_id"] == manifest["test_id"] && receipt["submitted"] == true;
  owned = !wasResumed;
  if (owned)
    forceOff();
  terminalDurable = loadDocument(finishPath, terminal) && terminal["test_id"] == manifest["test_id"];
  if (fs_exists(finishPath) && !terminalDurable) {
    blocked("Saved finish details are damaged; original files have been retained for recovery.");
    return;
  }
  program.phase = Phase::Finished;
  if (submitted) {
    uploadState = "submitted";
    reason = "Previously recorded test was submitted.";
    fs_remove(journalPath);
    if (wasResumed)
      removePreviousDataset();
    return;
  }
  FILE *file = fs_open(journalPath, "rb");
  if (!file) {
    blocked("Saved recording is unavailable; its metadata has been retained.");
    return;
  }
  fseek(file, 0, SEEK_END);
  const long bytes = ftell(file);
  rewind(file);
  recordCount = seq = 0;
  lastRecordUs = 0;
  uint32_t recoveredPulses = 0;
  Record record{};
  while (fread(&record, sizeof(record), 1, file) == 1 && valid(record) && record.boot == 0 &&
         record.seq == recordCount && record.t_us >= lastRecordUs) {
    ++recordCount;
    lastRecordUs = record.t_us;
    if (record.kind == 4 && record.code == static_cast<uint8_t>(Phase::Observe))
      recoveredPulses = std::max(recoveredPulses, uint32_t(record.pulse));
  }
  fclose(file);
  const bool damaged = bytes < 0 || size_t(bytes) != recordCount * sizeof(Record);
  if (terminalDurable && damaged) {
    const auto loss = terminal["lost_ranges"][0];
    const uint32_t storedRecords = bytes > 0 ? (size_t(bytes) + sizeof(Record) - 1) / sizeof(Record) : 0;
    const bool knownTornTail = terminal["outcome"] == "interrupted" && terminal["lost_ranges"].size() == 1 &&
                               loss["boot_id"] == recordingBoot && loss["first_seq"].is<uint32_t>() &&
                               loss["first_seq"].as<uint32_t>() == recordCount && loss["last_seq"].is<uint32_t>() &&
                               loss["last_seq"].as<uint32_t>() + 1 == storedRecords;
    if (!knownTornTail) {
      blocked("Saved recording checksum or length changed after completion; retained for recovery.");
      return;
    }
  }
  seq = recordCount;
  if (!terminalDurable) {
    terminal.clear();
    common(terminal);
    terminal["outcome"] = "interrupted";
    terminal["reason"] = "reboot_interrupted";
    terminal["final_phase"] = "finished";
    terminal["completed_pulses"] = recoveredPulses;
    terminal["response_settled"] = false;
    terminal["final_outputs"]["pump_on"] = false;
    terminal["final_outputs"]["heater_on"] = false;
    terminal["recovery_boot_id"] = currentBoot;
    terminal["last_recorded_us"] = lastRecordUs;
    terminal["unobserved_shutdown"] = true;
    controllerTerminalMetadata(terminal["controller"].to<JsonObject>(), true);
    const uint64_t began = manifest["acquisition"]["started_us"] | uint64_t(0);
    terminal["elapsed_s"] = lastRecordUs >= began ? (lastRecordUs - began) / 1000000 : 0;
    terminal["final_seq_by_boot"][recordingBoot] = int64_t(recordCount) - 1;
    if (damaged) {
      const uint32_t declaredCount = bytes > 0 ? (size_t(bytes) + sizeof(Record) - 1) / sizeof(Record) : recordCount;
      terminal["final_seq_by_boot"][recordingBoot] = int64_t(declaredCount) - 1;
      auto loss = terminal["lost_ranges"].to<JsonArray>().add<JsonObject>();
      loss["boot_id"] = recordingBoot;
      loss["first_seq"] = recordCount;
      loss["last_seq"] = declaredCount - 1;
    }
    releaseMetadataReserve();
    terminalDurable = saveDocument(finishPath, terminal);
  }
  program.outcome = End::Failed;
  program.completedPulses = terminal["completed_pulses"] | 0U;
  reason = terminal["reason"] | "Recovered interrupted test.";
  uploadState = terminalDurable ? "pending" : "error";
  if (!terminalDurable)
    uploadError = "Recovered recording is retained; saving finish details will be retried.";
}
} // namespace

void init() {
  if (initialized)
    return;
  mutex = xSemaphoreCreateRecursiveMutex();
  samples = xQueueCreate(sampleQueueCapacity, sizeof(Sample));
  if (!mutex || !samples)
    return;
  getGuid(guid);
  uuid(currentBoot);
  initialized = true;
  recoverDataset();
  if (xTaskCreate(uploader, "water-upload", 12288, nullptr, 1, nullptr) != pdPASS) {
    uploadState = "error";
    uploadError = "Cannot start upload worker.";
  }
}
bool active() { return running.load(); }
bool controlOwned() { return owned.load(); }
void onSample(uint64_t address, int16_t raw, bool validReading, uint64_t conversion, uint64_t read) {
  if (!initialized)
    return;
  Sample sample{address, conversion, read, raw, validReading && raw >= -880 && raw <= 2000};
  if (xQueueSend(samples, &sample, 0) != pdPASS && running)
    overflow = true;
}
bool requestStart(JsonVariantConst body, std::string &error) {
  if (!initialized) {
    error = "Water test service is unavailable.";
    return false;
  }
  Guard lock;
  if (recoveryBlocked || owned || running || uploaderBusy ||
      (!manifest.isNull() && uploadState != "submitted" && uploadState != "not_submitted")) {
    error = owned || running ? "Finish or stop the current test and resume normal control before another test."
                             : "Wait for the current test's submission to finish before another test.";
    return false;
  }
  if (!body.is<JsonObjectConst>() || body["consent"] != true || body["water_confirmed"] != true) {
    error = "Confirm water-only preparation and consent to this test submission.";
    return false;
  }
  auto finiteNumber = [](JsonVariantConst v, double low, double high) {
    return v.is<double>() && std::isfinite(v.as<double>()) && v.as<double>() >= low && v.as<double>() <= high;
  };
  if (!finiteNumber(body["water_volume_l"], .01, 10000) ||
      (!body["fermenter_capacity_l"].isNull() && !finiteNumber(body["fermenter_capacity_l"], .01, 10000))) {
    error = "Enter valid water volume and fermenter capacity.";
    return false;
  }
  if (!body["fermenter_model"].is<const char *>() || strlen(body["fermenter_model"].as<const char *>()) > 256) {
    error = "Fermenter model must be at most 256 characters.";
    return false;
  }
  const char *cooling = body["cooling_type"] | "";
  const char *probe = body["probe_mounting"] | "";
  const char *source = body["glycol_temperature_source"] | "";
  auto oneOf = [](const char *value, std::initializer_list<const char *> choices) {
    for (auto choice : choices)
      if (strcmp(value, choice) == 0)
        return true;
    return false;
  };
  if (!oneOf(cooling, {"jacket", "immersion_coil", "other", "unknown"}) ||
      !oneOf(probe, {"thermowell", "immersed", "outside", "other", "unknown"}) ||
      !oneOf(source, {"chamber_probe", "reported_setpoint", "unknown"})) {
    error = "Select the cooling, probe, and glycol input configuration.";
    return false;
  }
  if (strcmp(source, "reported_setpoint") == 0 && !finiteNumber(body["reported_chiller_setpoint_c"], -60, 100)) {
    error = "Enter the reported glycol setpoint, or select unknown.";
    return false;
  }
  if (strcmp(source, "reported_setpoint") != 0 && !body["reported_chiller_setpoint_c"].isNull()) {
    error = "Only reported-setpoint mode accepts a static glycol temperature.";
    return false;
  }
  serializeJson(body, queuedStart);
  if (queuedStart.size() > 4096) {
    queuedStart.clear();
    error = "Survey is too large.";
    return false;
  }
  owned = true;
  reason.clear();
  return true;
}
bool requestStop(std::string &error) {
  if (!initialized) {
    error = "Water test service unavailable.";
    return false;
  }
  Guard lock;
  if (!running && queuedStart.empty()) {
    error = "No active water test.";
    return false;
  }
  queuedStop = true;
  return true;
}
bool requestResume(JsonVariantConst, std::string &error) {
  if (!initialized) {
    error = "Water test service unavailable.";
    return false;
  }
  Guard lock;
  if (!owned || running || !queuedStart.empty()) {
    error = "Stop the active test before resuming normal control.";
    return false;
  }
  if (!manifest["prior_control"]["mode"].is<const char *>()) {
    error = "Saved control metadata is unavailable; automatic resume is blocked.";
    return false;
  }
  JsonDocument resumed;
  resumed["test_id"] = manifest["test_id"];
  resumed["resumed"] = true;
  if (!saveDocument(resumedPath, resumed)) {
    error = "Unable to save the resume request; outputs remain off and the recording is retained.";
    return false;
  }
  queuedResume = true;
  return true;
}
void tick() {
  if (!initialized)
    return;
  Guard lock;
  serviceProgram(false);
  Sample sample;
  // Snapshot the queue so concurrent acquisition cannot indefinitely extend a
  // control tick. Any new arrivals remain available for the next tick.
  for (size_t pending = uxQueueMessagesWaiting(samples); pending; --pending) {
    if (xQueueReceive(samples, &sample, 0) != pdPASS)
      break;
    processSample(sample);
    serviceProgram(false);
  }
  if (!queuedStart.empty()) {
    std::string payload;
    payload.swap(queuedStart);
    if (queuedStop) {
      queuedStop = false;
      owned = false;
      reason = "Start cancelled.";
    } else
      startRun(payload);
  }
  if (owned)
    tempControl.cs.mode = Modes::off;
  serviceProgram(true);
  if (running) {
    recordClock();
    serviceProgram(false);
  } else if (owned)
    forceOff();
  if (queuedResume) {
    queuedResume = false;
    tempControl.resumeAfterWaterTest(savedControl);
    owned = false;
    if (uploadState == "submitted")
      removePreviousDataset();
    reason = "Normal control resumed.";
  }
}
void status(JsonDocument &doc) {
  doc.clear();
  if (!initialized) {
    doc["active"] = false;
    doc["control_owned"] = false;
    doc["reason"] = "Water test service unavailable.";
    doc["can_start"] = false;
    return;
  }
  Guard lock;
  DeviceConfig beer{}, glycol{}, cool{};
  std::string check = preflight(false, beer, glycol, cool);
  doc["device_guid"] = guid;
  doc["active"] = running || !queuedStart.empty();
  doc["control_owned"] = owned.load();
  doc["phase"] = !queuedStart.empty() ? "preflight" : phaseName(program.phase);
  doc["pulse_number"] = program.pulse;
  doc["analysis_role"] = program.analysisRole();
  doc["block_id"] = program.block;
  doc["response_settled"] = program.responseSettled;
  if (program.phase == Phase::Controller)
    doc["controller_target_c"] = program.targetC;
  doc["completed_pulses"] = program.completedPulses;
  doc["elapsed_s"] = running ? uint32_t(nowUs() / 1e6 - program.started) : (terminal["elapsed_s"] | 0U);
  doc["max_duration_s"] = maximumSeconds;
  doc["pump_on"] = physicalPump();
  doc["heater_on"] = physicalHeat();
  auto b = cached(addressOf(beer));
  if (b && fresh(beer))
    doc["beer_c"] = b->lastGood.raw / 16.0 + beer.hw.calibration / 16.0;
  else
    doc["beer_c"] = nullptr;
  DeviceConfig chamber;
  bool hasChamber = device(DEVICE_CHAMBER_TEMP, chamber) && chamber.deviceHardware == DEVICE_HARDWARE_ONEWIRE_TEMP;
  auto g = hasChamber ? cached(addressOf(chamber)) : nullptr;
  if (movedProbe() && g && fresh(chamber))
    doc["glycol_c"] = g->lastGood.raw / 16.0 + chamber.hw.calibration / 16.0;
  else
    doc["glycol_c"] = nullptr;
  doc["glycol_temperature_source"] = manifest["installation"]["glycol_temperature_source"];
  doc["reported_chiller_setpoint_c"] = manifest["installation"]["reported_chiller_setpoint_c"];
  doc["test_id"] = manifest["test_id"];
  doc["outcome"] = terminal["outcome"] | "";
  doc["reason"] = reason;
  doc["upload_status"] = uploadState;
  doc["upload_error"] = uploadError;
  doc["result_url"] = std::string(endpoint) + "/" + guid + "/";
  doc["moved_chamber_probe"] = movedProbe();
  doc["can_start"] = !recoveryBlocked && !owned && !running && !uploaderBusy &&
                     (manifest.isNull() || uploadState == "submitted" || uploadState == "not_submitted") &&
                     check.empty();
  doc["can_resume"] = owned && !running && queuedStart.empty();
  auto p = doc["preflight"].to<JsonObject>();
  p["ready"] = check.empty();
  p["reason"] = check;
  const bool beerConfigured = beer.deviceHardware == DEVICE_HARDWARE_ONEWIRE_TEMP && beer.hw.address[0] == 0x28;
  p["beer_configured"] = beerConfigured;
  p["beer_available"] = beerConfigured && fresh(beer);
  p["chamber_available"] = hasChamber && chamber.hw.address[0] == 0x28 && fresh(chamber);
  p["cooler_available"] = cool.deviceHardware == DEVICE_HARDWARE_PIN && tempControl.cooler;
  p["free_bytes"] = freeBytes();
  p["required_bytes"] = requiredBytes;
}
} // namespace WaterTest
