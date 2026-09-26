#include "WaterTest.h"
#include "Brewpi.h"
#include "DeviceManager.h"
#include "ESPEepromAccess.h"
#include "ESP_BP_WiFi.h"
#include "EepromManager.h"
#include "TempControl.h"
#include "Version.h"
#include "WaterTestCore.h"
#include "WaterTestProtocol.h"
#include "getGuid.h"
#include "ntp.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <esp_http_client.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sys/time.h>
#include <unistd.h>

namespace WaterTest {
namespace {
using namespace WaterTestCore;
constexpr const char *manifestPath = "/water-test-manifest.json";
constexpr const char *journalPath = "/water-test-records.bin";
constexpr const char *finishPath = "/water-test-finish.json";
constexpr const char *ackPath = "/water-test-ack.json";
constexpr const char *bootsPath = "/water-test-boots.json";
constexpr const char *resumedPath = "/water-test-resumed.json";
constexpr const char *reservePath = "/water-test-reserve.bin";
constexpr size_t maxRecords = 6000;
constexpr size_t requiredBytes = maxRecords * sizeof(Record) + 32768;
constexpr size_t batchSize = 12;
constexpr unsigned maxBoots = 8;
constexpr const char *endpoint = "http://chill.fermentrack.net";
struct Sample {
  uint64_t address, conversion, read;
  int16_t raw;
  bool valid;
};
struct Cache {
  Sample sample{};
  bool present = false;
};
SemaphoreHandle_t mutex = nullptr;
QueueHandle_t samples = nullptr;
std::atomic<bool> initialized{false}, owned{false}, running{false}, overflow{false};
std::atomic<bool> uploaderBusy{false};
Cache cache[Config::EepromFormat::MAX_DEVICES];
Program program;
JsonDocument manifest, terminal, bootList;
std::string queuedStart, reason, uploadError, uploadState = "idle";
bool queuedStop = false, queuedResume = false, clockRecorded = false, recordingFailed = false;
bool lostRecord = false;
uint8_t bootIndex = 0;
uint32_t seq = 0, recordCount = 0, lostSequence = 0;
uint64_t beerAddress = 0, glycolAddress = 0, lastBeerRead = 0, lastGlycolRead = 0, recordStartUs = 0;
double beerOffset = 0, glycolOffset = 0;
float beerC = NAN, glycolC = NAN;
char guid[17] = {}, currentBoot[37] = {};
ControlSettings savedControl;
FILE *journal = nullptr;
uint64_t lastRecordUs = 0;

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
bool readJson(const char *path, JsonDocument &doc) {
  FILE *f = fs_open(path, "rb");
  if (!f)
    return false;
  char buffer[512];
  std::string text;
  size_t n;
  while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) {
    text.append(buffer, n);
    if (text.size() > 16384) {
      fclose(f);
      return false;
    }
  }
  fclose(f);
  return deserializeJson(doc, text) == DeserializationError::Ok;
}
bool atomicJson(const char *path, const JsonDocument &doc) {
  std::string target = std::string(FS_PREFIX) + path, tmp = target + ".tmp", text;
  serializeJson(doc, text);
  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f)
    return false;
  bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
  ok = fflush(f) == 0 && ok;
  ok = fsync(fileno(f)) == 0 && ok;
  ok = fclose(f) == 0 && ok;
  if (ok)
    ok = rename(tmp.c_str(), target.c_str()) == 0;
  if (!ok)
    remove(tmp.c_str());
  return ok;
}
size_t freeBytes() {
  size_t total = 0, used = 0;
  return esp_littlefs_info("spiffs", &total, &used) == ESP_OK && total > used ? total - used : 0;
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
    if (c.present && c.sample.address == address)
      return &c;
  return nullptr;
}
bool fresh(const DeviceConfig &d) {
  auto c = cached(addressOf(d));
  uint64_t now = nowUs();
  return c && c->sample.valid && now >= c->sample.read && now - c->sample.read <= uint64_t(freshnessSeconds * 1000000);
}
bool movedProbe() { return manifest["installation"]["glycol_temperature_source"] == "chamber_probe"; }
void recordFailure(uint32_t attempted) {
  if (!lostRecord) {
    lostRecord = true;
    lostSequence = attempted;
  }
  recordingFailed = true;
  fs_remove(reservePath);
}
bool append(Record r) {
  if (!journal || recordingFailed || recordCount >= maxRecords) {
    if (!recordingFailed)
      recordFailure(seq++);
    return false;
  }
  r.boot = bootIndex;
  r.seq = seq++;
  r.t_us = std::max(nowUs(), lastRecordUs);
  lastRecordUs = r.t_us;
  seal(r);
  bool ok = fwrite(&r, sizeof(r), 1, journal) == 1;
  ok = fflush(journal) == 0 && ok;
  ok = fsync(fileno(journal)) == 0 && ok;
  if (!ok) {
    // The possibly torn record is excluded from the immutable upload prefix.
    ftruncate(fileno(journal), recordCount * sizeof(Record));
    clearerr(journal);
    recordFailure(r.seq);
    return false;
  }
  ++recordCount;
  return true;
}
void recordOutput(bool pump, bool requested, bool edge, Reason why) {
  Record r{};
  r.kind = 3;
  r.role = pump ? 0 : 1;
  r.flags = (physicalPump() ? 2 : 0) | (requested ? 4 : 0) | (edge ? 8 : 0);
  r.code = static_cast<uint8_t>(why);
  append(r);
}
void recordPhase() {
  Record r{};
  r.kind = 4;
  r.code = static_cast<uint8_t>(program.phase);
  r.pulse = program.pulse;
  r.detail = program.deadline > nowUs() / 1e6 ? uint32_t(program.deadline - nowUs() / 1e6) : 0;
  append(r);
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
void recordBoot(bool recovered) {
  Record r{};
  r.kind = 0;
  r.code = recovered ? static_cast<uint8_t>(Reason::Reboot) : static_cast<uint8_t>(Reason::Start);
  r.detail = esp_reset_reason();
  append(r);
  recordClock();
  recordOutput(true, false, false, recovered ? Reason::Reboot : Reason::Start);
  recordOutput(false, false, false, recovered ? Reason::Reboot : Reason::Start);
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
void finishRun() {
  const bool wasPump = physicalPump();
  forceOff();
  if (program.reason == Reason::SensorFault || program.reason == Reason::SensorStale ||
      program.reason == Reason::StorageFailure || program.reason == Reason::QueueOverflow ||
      program.reason == Reason::UnexpectedOutput) {
    Record event{};
    event.kind = (program.reason == Reason::QueueOverflow || program.reason == Reason::UnexpectedOutput) ? 6 : 5;
    event.code = static_cast<uint8_t>(program.reason);
    append(event);
  }
  recordOutput(true, false, wasPump, program.reason);
  recordOutput(false, false, false, program.reason);
  recordPhase();
  if (recordingFailed) {
    program.outcome = End::Failed;
    program.reason = Reason::StorageFailure;
  }
  terminal.clear();
  common(terminal);
  terminal["outcome"] = endName(program.outcome);
  terminal["reason"] = reasonName(program.reason);
  terminal["final_phase"] = "finished";
  terminal["t_us"] = nowUs();
  terminal["boot_id"] = bootList["boots"][bootIndex];
  terminal["final_outputs"]["pump_on"] = false;
  terminal["final_outputs"]["heater_on"] = false;
  terminal["elapsed_s"] = program.started > 0 ? uint32_t(nowUs() / 1e6 - program.started) : 0;
  terminal["response_settled"] = false; // Fixed observation windows do not prove equilibrium.
  JsonObject bounds = terminal["final_seq_by_boot"].to<JsonObject>();
  uint32_t counts[maxBoots] = {};
  closeJournal();
  FILE *f = fs_open(journalPath, "rb");
  Record r{};
  while (f && fread(&r, sizeof(r), 1, f) == 1)
    if (valid(r) && r.boot < maxBoots)
      counts[r.boot] = std::max(counts[r.boot], r.seq + 1);
  if (f)
    fclose(f);
  for (unsigned i = 0; i < bootList["boots"].size(); ++i) {
    const char *id = bootList["boots"][i];
    bounds[id] = counts[i] ? int64_t(counts[i]) - 1 : -1;
  }
  if (lostRecord) {
    const char *id = bootList["boots"][bootIndex];
    bounds[id] = int64_t(seq) - 1;
    auto loss = terminal["lost_ranges"].to<JsonArray>().add<JsonObject>();
    loss["boot_id"] = id;
    loss["first_seq"] = lostSequence;
    loss["last_seq"] = seq - 1;
  }
  fs_remove(reservePath);
  if (!atomicJson(finishPath, terminal)) {
    uploadError = "Cannot save test outcome; data retained for recovery after restart.";
    uploadState = "error";
  } else {
    uploadState = "pending";
    uploadError.clear();
  }
  reason = reasonName(program.reason);
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
void restoreSaved(JsonVariantConst p) {
  const char *mode = p["mode"] | "o";
  savedControl.mode = mode[0];
  savedControl.beerSetting = p["beer_setting_raw"].as<temperature>();
  savedControl.fridgeSetting = p["fridge_setting_raw"].as<temperature>();
  savedControl.heatEstimator = p["heat_estimator_raw"].as<temperature>();
  savedControl.coolEstimator = p["cool_estimator_raw"].as<temperature>();
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
  if (!extendedSettings.glycol)
    return "Enable glycol mode before running this glycol-pump water test.";
  if (!device(DEVICE_BEER_TEMP, beer) || beer.deviceHardware != DEVICE_HARDWARE_ONEWIRE_TEMP ||
      beer.hw.address[0] != 0x28)
    return "Configure a DS18B20 beer probe before starting.";
  if (!fresh(beer))
    return "Waiting for a fresh valid beer probe reading.";
  if (!device(DEVICE_CHAMBER_COOL, cool) || cool.deviceHardware != DEVICE_HARDWARE_PIN || !tempControl.cooler)
    return "Configure a local GPIO cooling relay before starting.";
  if (physicalPump() || physicalHeat())
    return "Wait until normal heating and cooling outputs are OFF, then start the water test.";
  if (useGlycol) {
    if (!device(DEVICE_CHAMBER_TEMP, glycol) || glycol.deviceHardware != DEVICE_HARDWARE_ONEWIRE_TEMP ||
        glycol.hw.address[0] != 0x28)
      return "Configure a DS18B20 chamber probe and place it in the glycol bath.";
    if (addressOf(beer) == addressOf(glycol))
      return "Beer and glycol probes must be distinct.";
    if (!fresh(glycol))
      return "Waiting for a fresh valid glycol bath probe reading.";
  }
  if (minimumOn() > maximumPulseSeconds || minimumOff() > observationSeconds)
    return "Configured cooling relay minimum times exceed this test's conservative pulse limits.";
  float temperature = cached(addressOf(beer))->sample.raw / 16.0 + beer.hw.calibration / 16.0;
  if (temperature < 8 || temperature > 35)
    return "Start with water between 8 and 35 C (46.4 to 95 F).";
  if (freeBytes() < requiredBytes)
    return "Not enough free recording space. A full offline test needs 272768 free bytes.";
  return "";
}
bool allocateReserve() {
  FILE *f = fs_open(reservePath, "wb");
  if (!f)
    return false;
  uint8_t zero[256] = {};
  bool ok = true;
  for (unsigned i = 0; i < 32; ++i)
    if (fwrite(zero, 1, sizeof(zero), f) != sizeof(zero)) {
      ok = false;
      break;
    }
  ok = fflush(f) == 0 && ok;
  ok = fsync(fileno(f)) == 0 && ok;
  fclose(f);
  return ok;
}
void startRun(const std::string &payload) {
  JsonDocument input;
  if (deserializeJson(input, payload) != DeserializationError::Ok) {
    reason = "Invalid start request.";
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
  double bathC = bath ? cached(addressOf(glycol))->sample.raw / 16.0 + glycol.hw.calibration / 16.0
                      : input["reported_chiller_setpoint_c"].as<double>();
  double waterC = cached(addressOf(beer))->sample.raw / 16.0 + beer.hw.calibration / 16.0;
  if (input["glycol_temperature_source"] != "unknown" && waterC - bathC < 2) {
    reason = "Water must start at least 2 C (3.6 F) warmer than the declared glycol input.";
    owned = false;
    return;
  }
  // Old datasets are replaceable only after every upload is acknowledged and
  // the participant has explicitly released control.
  for (auto path : {manifestPath, journalPath, finishPath, ackPath, bootsPath, resumedPath, reservePath})
    fs_remove(path);
  manifest.clear();
  terminal.clear();
  bootList.clear();
  recordingFailed = false;
  lostRecord = false;
  recordCount = seq = 0;
  bootIndex = 0;
  clockRecorded = false;
  lastRecordUs = 0;
  char testId[37];
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
  plan["version"] = "cooling-water-v1";
  plan["baseline_s"] = baselineSeconds;
  plan["observation_s"] = observationSeconds;
  plan["max_duration_s"] = maximumSeconds;
  plan["max_pulse_s"] = maximumPulseSeconds;
  plan["max_pump_s"] = maximumPumpSeconds;
  plan["max_drop_c"] = maximumDropC;
  plan["minimum_water_c"] = minimumWaterC;
  plan["pulse_selection"] = "10s pilot; response >=0.5C:5s; <0.125C:30/60s; otherwise20/40s; clamped to relay minimum";
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
  bootList["boots"].to<JsonArray>().add(currentBoot);
  if (!allocateReserve() || !atomicJson(bootsPath, bootList) || !atomicJson(manifestPath, manifest)) {
    reason = "Unable to reserve durable recording storage.";
    fs_remove(manifestPath);
    fs_remove(reservePath);
    manifest.clear();
    terminal.clear();
    bootList.clear();
    uploadState = "idle";
    owned = false;
    return;
  }
  journal = fs_open(journalPath, "wb");
  if (!journal) {
    program.start(nowUs() / 1e6, waterC, minimumOn(), minimumOff());
    program.finish(nowUs() / 1e6, End::Failed, Reason::StorageFailure);
    finishRun();
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
  forceOff();
  tempControl.cs.mode = Modes::off;
  program.start(recordStartUs / 1e6, waterC, minimumOn(), minimumOff());
  program.lastSample = cached(beerAddress)->sample.read / 1e6;
  reason.clear();
  uploadState = "pending";
  uploadError.clear();
  overflow = false;
  running = true;
  recordBoot(false);
  recordPhase();
  recordStartUs = lastRecordUs; // discard queued reads acquired before the initial record boundary
}
void applyProgram(Phase oldPhase, bool /*oldPump*/) {
  bool actual = physicalPump();
  if (actual != program.pump) {
    tempControl.cooler->setActive(program.pump);
    recordOutput(true, program.pump, true, program.reason);
  }
  if (tempControl.heater)
    tempControl.heater->setActive(false);
  if (tempControl.light)
    tempControl.light->setActive(false);
  if (tempControl.fan)
    tempControl.fan->setActive(false);
  if (oldPhase != program.phase && program.active())
    recordPhase();
  if (recordingFailed && program.active())
    program.finish(nowUs() / 1e6, End::Failed, Reason::StorageFailure);
  if (!program.active())
    finishRun();
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
    slot->sample = sample;
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
  Record r{};
  r.kind = 2;
  r.role = beer ? 0 : 1;
  r.raw = sample.raw;
  r.read_us = sample.read;
  r.conversion_us =
      sample.read >= sample.conversion ? uint32_t(std::min<uint64_t>(sample.read - sample.conversion, UINT32_MAX)) : 0;
  r.flags = (sample.valid ? 1 : 0) | (physicalPump() ? 2 : 0);
  append(r);
  if (recordingFailed) {
    program.finish(nowUs() / 1e6, End::Failed, Reason::StorageFailure);
    finishRun();
    return;
  }
  if (beer) {
    beerC = sample.valid ? sample.raw / 16.0 + beerOffset : NAN;
    auto p = program.phase;
    bool on = program.pump;
    program.sample(sample.read / 1e6, beerC, sample.valid, nowUs() / 1e6);
    applyProgram(p, on);
  } else
    glycolC = sample.valid ? sample.raw / 16.0 + glycolOffset : NAN;
}

struct Response {
  std::string body;
  bool tooLong = false;
};
esp_err_t httpEvent(esp_http_client_event_t *e) {
  if (e->event_id == HTTP_EVENT_ON_DATA) {
    auto r = static_cast<Response *>(e->user_data);
    if (r->body.size() + e->data_len > 8192) {
      r->tooLong = true;
      return ESP_FAIL;
    }
    r->body.append(static_cast<const char *>(e->data), e->data_len);
  }
  return ESP_OK;
}
bool send(const std::string &path, esp_http_client_method_t method, const std::string &body, JsonDocument &response,
          std::string &error) {
  if (!bp_wifi_is_connected()) {
    error = "WiFi disconnected; original data retained.";
    return false;
  }
  Response captured;
  std::string url = std::string(endpoint) + path;
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = method;
  config.timeout_ms = 6000;
  config.disable_auto_redirect = true;
  config.event_handler = httpEvent;
  config.user_data = &captured;
  auto client = esp_http_client_init(&config);
  if (!client) {
    error = "HTTP client allocation failed.";
    return false;
  }
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "User-Agent", "BrewPi-WaterTest/1");
  esp_http_client_set_post_field(client, body.data(), body.size());
  esp_err_t result = esp_http_client_perform(client);
  int code = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (result != ESP_OK || (code != 200 && code != 201) || captured.tooLong) {
    error = "HTTP upload pending (status " + std::to_string(code) + ").";
    if (code >= 300 && code < 400)
      error = "Server redirected HTTP; configure the collection API to accept HTTP without redirect.";
    return false;
  }
  if (deserializeJson(response, captured.body) != DeserializationError::Ok) {
    error = "Invalid server acknowledgement.";
    return false;
  }
  return true;
}
// This worker never touches actuators. It starts only after acquisition is closed;
// upload/response delays cannot change an experiment's timing or sample cadence.
void uploader(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    std::string id, body, path, error;
    uint32_t next = 0, end = 0;
    bool isManifest = false, isFinish = false;
    std::string batchId;
    JsonDocument ack, request, response;
    {
      Guard lock;
      if (running || !queuedStart.empty() || manifest.isNull() || !fs_exists(finishPath) || uploadState == "submitted")
        continue;
      readJson(ackPath, ack);
      next = ack["next_record"] | 0U;
      id = manifest["test_id"].as<std::string>();
      path = "/api/v1/water-tests/" + id;
      if (!ack["manifest"].as<bool>()) {
        isManifest = true;
        serializeJson(manifest, body);
      } else if (next < recordCount) {
        FILE *f = fs_open(journalPath, "rb");
        if (!f) {
          uploadState = "error";
          uploadError = "Recording file is unavailable.";
          continue;
        }
        fseek(f, next * sizeof(Record), SEEK_SET);
        Record r{};
        uint8_t b = 255;
        common(request);
        auto list = request["records"].to<JsonArray>();
        end = next;
        while (end < recordCount && end - next < batchSize && fread(&r, sizeof(r), 1, f) == 1) {
          if (!valid(r))
            break;
          if (b == 255)
            b = r.boot;
          else if (b != r.boot)
            break;
          if (end == next)
            request["first_seq"] = r.seq;
          request["last_seq"] = r.seq;
          WaterTestProtocol::recordToJson(list.add<JsonObject>(), r, bootList["boots"][r.boot],
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
        request["boot_id"] = bootList["boots"][b];
        serializeJson(request, body);
        path += "/batches";
      } else {
        isFinish = true;
        serializeJson(terminal, body);
        path += "/finish";
      }
      uploadState = "uploading";
      uploaderBusy = true;
    }
    bool ok = send(path, (!isManifest && !isFinish) ? HTTP_METHOD_POST : HTTP_METHOD_PUT, body, response, error);
    if (ok) {
      ok = WaterTestProtocol::acknowledged(response.as<JsonVariantConst>(), id, guid);
      if (!isManifest && !isFinish)
        ok = WaterTestProtocol::batchAcknowledged(response.as<JsonVariantConst>(), id, guid, batchId,
                                                  request["first_seq"].as<uint32_t>(),
                                                  request["last_seq"].as<uint32_t>());
      if (isFinish)
        ok = WaterTestProtocol::finishAcknowledged(response.as<JsonVariantConst>(), id, guid);
      if (!ok)
        error = "Server did not acknowledge this complete immutable request; retrying original data.";
    }
    {
      Guard lock;
      if (ok) {
        if (isManifest)
          ack["manifest"] = true;
        else if (isFinish)
          ack["finish"] = true;
        else
          ack["next_record"] = end;
        if (!atomicJson(ackPath, ack)) {
          ok = false;
          error = "Could not persist acknowledgement; retrying the same data.";
        }
      }
      if (ok) {
        uploadError.clear();
        uploadState = isFinish ? "submitted" : "pending";
        if (isFinish)
          fs_remove(journalPath);
      } else {
        uploadState = "error";
        uploadError = error;
      }
      uploaderBusy = false;
    }
    if (!ok)
      vTaskDelay(pdMS_TO_TICKS(30000));
  }
}
void recover() {
  if (!readJson(manifestPath, manifest)) {
    if (fs_exists(manifestPath)) {
      owned = true;
      reason = "Saved test metadata is damaged; control remains OFF.";
      uploadState = "error";
    }
    return;
  }
  bool resumed = fs_exists(resumedPath);
  owned = !resumed;
  restoreSaved(manifest["prior_control"]);
  if (!readJson(bootsPath, bootList) || !bootList["boots"].is<JsonArray>()) {
    reason = "Recording boot metadata is damaged; control remains OFF.";
    uploadState = "error";
    return;
  }
  FILE *f = fs_open(journalPath, "rb");
  Record r{};
  recordCount = 0;
  bool torn = false;
  while (f) {
    size_t n = fread(&r, 1, sizeof(r), f);
    if (!n)
      break;
    if (n != sizeof(r) || !valid(r) || r.boot >= bootList["boots"].size()) {
      torn = true;
      break;
    }
    ++recordCount;
  }
  if (f)
    fclose(f);
  if (readJson(finishPath, terminal)) {
    program.phase = Phase::Finished;
    reason = terminal["reason"] | "";
    JsonDocument ack;
    readJson(ackPath, ack);
    uploadState = ack["finish"].as<bool>() ? "submitted" : "pending";
    return;
  }
  forceOff();
  owned = true;
  program.phase = Phase::Finished;
  program.outcome = End::Interrupted;
  program.reason = Reason::Reboot;
  if (bootList["boots"].size() >= maxBoots) {
    reason = "Repeated interrupted recovery exceeded boot journal limit; data retained.";
    uploadState = "error";
    return;
  }
  bootIndex = bootList["boots"].size();
  bootList["boots"].add(currentBoot);
  if (!atomicJson(bootsPath, bootList)) {
    reason = "Cannot persist recovery; control remains OFF.";
    uploadState = "error";
    return;
  }
  journal = fs_open(journalPath, "r+b");
  if (journal) {
    ftruncate(fileno(journal), recordCount * sizeof(Record));
    fseek(journal, 0, SEEK_END);
  } else
    journal = fs_open(journalPath, "wb");
  seq = 0;
  lastRecordUs = 0;
  recordBoot(true);
  Record gap{};
  gap.kind = 6;
  gap.code = static_cast<uint8_t>(Reason::RecordingGap);
  gap.detail = torn ? 1 : 0;
  append(gap);
  finishRun();
}
} // namespace

void init() {
  if (initialized)
    return;
  mutex = xSemaphoreCreateRecursiveMutex();
  samples = xQueueCreate(64, sizeof(Sample));
  if (!mutex || !samples)
    return;
  getGuid(guid);
  uuid(currentBoot);
  initialized = true;
  recover();
  if (xTaskCreate(uploader, "water-upload", 12288, nullptr, 1, nullptr) != pdPASS) {
    uploadState = "error";
    uploadError = "Cannot start upload worker. Data will remain on device.";
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
  if (owned || running || uploaderBusy || (!manifest.isNull() && uploadState != "submitted")) {
    error = "Finish submission and explicitly resume normal control before another test.";
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
  if (strcmp(source, "chamber_probe") == 0 && body["bath_placement_confirmed"] != true) {
    error = "Confirm the chamber probe is physically immersed in the glycol bath.";
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
bool requestResume(JsonVariantConst body, std::string &error) {
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
  if (movedProbe() && body["probe_returned"] != true) {
    error = "Return the chamber probe to its normal position and confirm before resuming.";
    return false;
  }
  queuedResume = true;
  return true;
}
void tick() {
  if (!initialized)
    return;
  Guard lock;
  Sample sample;
  while (xQueueReceive(samples, &sample, 0) == pdPASS)
    processSample(sample);
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
  if (running) {
    auto phase = program.phase;
    bool pump = program.pump;
    bool unexpected = physicalHeat() || physicalPump() != program.pump;
    if (queuedStop) {
      queuedStop = false;
      program.stop(nowUs() / 1e6);
    }
    if (recordingFailed || overflow)
      program.finish(nowUs() / 1e6, End::Failed, recordingFailed ? Reason::StorageFailure : Reason::QueueOverflow);
    else if (unexpected)
      program.finish(nowUs() / 1e6, End::Failed, Reason::UnexpectedOutput);
    else
      program.tick(nowUs() / 1e6);
    applyProgram(phase, pump);
    if (running)
      recordClock();
  } else if (owned)
    forceOff();
  if (queuedResume) {
    queuedResume = false;
    JsonDocument resumed;
    resumed["test_id"] = manifest["test_id"];
    resumed["resumed"] = true;
    if (atomicJson(resumedPath, resumed)) {
      tempControl.resumeAfterWaterTest(savedControl);
      owned = false;
      reason = "Normal control resumed.";
    } else
      reason = "Could not persist control release; control remains OFF.";
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
  doc["elapsed_s"] = running ? uint32_t(nowUs() / 1e6 - program.started) : (terminal["elapsed_s"] | 0U);
  doc["max_duration_s"] = maximumSeconds;
  doc["pump_on"] = physicalPump();
  doc["heater_on"] = physicalHeat();
  auto b = cached(addressOf(beer));
  if (b && fresh(beer))
    doc["beer_c"] = b->sample.raw / 16.0 + beer.hw.calibration / 16.0;
  else
    doc["beer_c"] = nullptr;
  DeviceConfig chamber;
  bool hasChamber = device(DEVICE_CHAMBER_TEMP, chamber) && chamber.deviceHardware == DEVICE_HARDWARE_ONEWIRE_TEMP;
  auto g = hasChamber ? cached(addressOf(chamber)) : nullptr;
  if (movedProbe() && g && fresh(chamber))
    doc["glycol_c"] = g->sample.raw / 16.0 + chamber.hw.calibration / 16.0;
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
  doc["can_start"] =
      !owned && !running && !uploaderBusy && (manifest.isNull() || uploadState == "submitted") && check.empty();
  doc["can_resume"] = owned && !running && queuedStart.empty();
  auto p = doc["preflight"].to<JsonObject>();
  p["ready"] = check.empty();
  p["reason"] = check;
  p["beer_available"] = beer.deviceHardware == DEVICE_HARDWARE_ONEWIRE_TEMP && fresh(beer);
  p["chamber_available"] = hasChamber && fresh(chamber);
  p["cooler_available"] = cool.deviceHardware == DEVICE_HARDWARE_PIN;
  p["free_bytes"] = freeBytes();
  p["required_bytes"] = requiredBytes;
}
} // namespace WaterTest
