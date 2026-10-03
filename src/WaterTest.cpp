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
#include "WaterTestUpload.h"
#include "getGuid.h"
#include "ntp.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
#include <new>
#include <sys/time.h>
#include <unistd.h>

namespace WaterTest {
namespace {
using namespace WaterTestCore;
using namespace WaterTestStorage;
using WaterTestTransport::endpoint;
constexpr size_t maxRecords = 12500;
constexpr size_t requiredBytes = maxRecords * sizeof(Record) + 49152;
constexpr size_t batchSize = WaterTestUpload::batchSize;
// Only reboot recovery decodes the retired plan-v1/v2 episode completion rule.
constexpr unsigned legacyControllerRequiredEpisodes = 3;
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
// An interrupted save before acquisition started must not resume normal control
// on boot, but an explicit new Start may replace its empty recording.
std::atomic<bool> startupHold{false};
std::atomic<bool> uploaderBusy{false};
std::atomic<bool> backgroundServicesReady{false};
bool uploaderTaskActive = false; // protected by Guard, including task creation/exit
uint64_t nextUploaderAttemptUs = 0;
constexpr uint64_t uploaderRetryUs = 30000000;
Cache cache[Config::EepromFormat::MAX_DEVICES];
Program program;
Role recordedRole = Role::Complete;
struct TestControllerDeleter {
  void operator()(GlycolCooling::Controller *controller) const {
    if (controller) {
      controller->~Controller();
      std::free(controller);
    }
  }
};
using TestControllerPtr = std::unique_ptr<GlycolCooling::Controller, TestControllerDeleter>;
TestControllerPtr testController;
bool controllerAllocationFailed = false;
GlycolCooling::Algorithm testAlgorithm = GlycolCooling::Algorithm::PredictiveCoast;
uint64_t lastControllerRecordUs = 0;
double lastControllerStep = -1;
bool recordedControllerPump = false;
static_assert(static_cast<uint8_t>(GlycolCooling::ObservationReason::RateCondition) == 1 &&
              static_cast<uint8_t>(GlycolCooling::ObservationReason::CoastTimeLimit) == 2 &&
              static_cast<uint8_t>(GlycolCooling::ObservationReason::RateUnqualified) == 3 &&
              static_cast<uint8_t>(GlycolCooling::ObservationReason::Interrupted) == 4,
              "Observation reason IDs are part of the durable journal format");
struct ObservationCounts {
  unsigned rateQualified = 0, timeLimited = 0, rateUnqualified = 0, interrupted = 0;
  unsigned completed() const { return rateQualified + timeLimited + rateUnqualified; }
  bool record(uint8_t reason) {
    switch (reason) {
    case 1: ++rateQualified; return true;
    case 2: ++timeLimited; return true;
    case 3: ++rateUnqualified; return true;
    case 4: ++interrupted; return true;
    default: return false;
    }
  }
  void json(JsonObject out) const {
    out["goal"] = controllerObservationGoal;
    out["completed"] = completed();
    out["rate_qualified"] = rateQualified;
    out["time_limited"] = timeLimited;
    out["rate_unqualified"] = rateUnqualified;
    out["interrupted"] = interrupted;
  }
};
ObservationCounts controllerObservations[controllerRunCount];
uint32_t recordedControllerObservation = 0;
bool controllerCheckpointed[controllerRunCount] = {};
unsigned recordedControllerStarts = 0, recordedControllerFinishes = 0;
JsonDocument manifest, terminal;
bool fixedDurationPlan() { return manifest["test_program"]["controller_plan_version"] == 3; }
GlycolCooling::Algorithm algorithmForRun(unsigned run) {
  return run == 2 ? (testAlgorithm == GlycolCooling::Algorithm::PredictiveCoast
                         ? GlycolCooling::Algorithm::PulseDose : GlycolCooling::Algorithm::PredictiveCoast)
                  : testAlgorithm;
}
const char *controllerPath(unsigned run) { return run == 2 ? controllerTwoPath : controllerOnePath; }
JsonObjectConst initialController(unsigned run) {
  return manifest["test_program"]["controllers"][run - 1].as<JsonObjectConst>();
}
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
  if (program.phase == Phase::ControllerFinalObserve) {
    r.read_us = uint64_t(std::llround(program.controllerFinalObservationStarted * 1000000));
    r.t_us = std::max(r.t_us, r.read_us);
  }
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
  r.role = program.controllerRun;
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
bool recordControllerObservation() {
  if (!testController || !program.controllerRun)
    return false;
  const auto &event = testController->output().observation;
  if (!event.sequence || event.sequence <= recordedControllerObservation)
    return false;
  Record r{};
  r.kind = 10;
  r.role = program.controllerRun;
  r.code = static_cast<uint8_t>(algorithmForRun(program.controllerRun));
  r.pulse = static_cast<uint8_t>(event.reason);
  r.flags = event.rate_qualified ? 1 : 0;
  r.detail = event.sequence;
  r.read_us = uint64_t(std::llround(event.ended_s * 1000000));
  const float coast = event.off_s >= event.started_s && event.off_s <= event.ended_s
                          ? std::max(0., event.ended_s - event.off_s) : 0.;
  memcpy(&r.conversion_us, &coast, sizeof(coast));
  if (!append(r))
    return false;
  recordedControllerObservation = event.sequence;
  controllerObservations[program.controllerRun - 1].record(r.pulse);
  return true;
}
void closeControllerObservation() {
  if (!testController || !program.controllerRunEnded)
    return;
  // A step can close COAST and start a new pulse together. Save that completed
  // observation before inhibit replaces it with the new response's interruption.
  recordControllerObservation();
  testController->inhibit(program.controllerRunEnded);
  recordControllerObservation();
}
void recordControllerRun(bool finished) {
  Record r{};
  r.kind = 9;
  r.role = program.controllerRun;
  r.code = static_cast<uint8_t>(algorithmForRun(program.controllerRun));
  r.pulse = finished ? 1 : 0;
  r.flags = 128 | (program.controllerRunDurationComplete ? 1 : 0);
  r.read_us = uint64_t(std::llround((finished ? program.controllerRunEnded : program.controllerStarted) * 1000000));
  r.raw = std::lround(program.controllerRunStartC * 16);
  uint64_t targetBits;
  memcpy(&targetBits, &program.targetC, sizeof(targetBits));
  r.conversion_us = uint32_t(targetBits);
  r.detail = uint32_t(targetBits >> 32);
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
void controllerRunMetadata(JsonObject out, unsigned run) {
  const bool started = program.controllerRun == run && program.controllerStarted > 0;
  WaterTestControllerSnapshot::final(out, initialController(run), started ? testController.get() : nullptr,
                                    false, uint64_t(std::llround(program.controllerStarted * 1000000)),
                                    program.targetC, nowUs());
  out["test_id"] = manifest["test_id"];
  out["boot_id"] = recordingBoot;
  out["run"] = run;
  const bool complete = started && program.controllerRunDurationComplete;
  out["completion_policy"] = "fixed_duration_v1";
  out["run_duration_complete"] = complete;
  out["status"] = !started ? "skipped" : complete ? "completed"
                       : program.outcome == End::Stopped ? "stopped" : "failed";
  out["reason"] = complete ? "duration_complete" : reasonName(program.reason);
  (started ? controllerObservations[run - 1] : ObservationCounts{}).json(out["observations"].to<JsonObject>());
  if (started) {
    out["started_us"] = uint64_t(std::llround(program.controllerStarted * 1000000));
    out["ended_us"] = uint64_t(std::llround(program.controllerRunEnded * 1000000));
    out["start_c"] = program.controllerRunStartC;
    out["target_c"] = program.targetC;
    char exact[32];
    snprintf(exact, sizeof(exact), "%.17g", program.targetC);
    out["target_c_exact"] = exact;
    out["pump_cycles"] = program.controllerCycles;
    out["pump_on_s"] = program.totalPump - program.controllerRunPumpStart;
  }
}
bool loadControllerCheckpoint(unsigned run, JsonDocument &saved) {
  return loadDocument(controllerPath(run), saved) && saved["test_id"] == manifest["test_id"] &&
         saved["boot_id"] == recordingBoot && saved["run"] == run &&
         saved["selection"] == initialController(run)["selection"] && saved["ended_us"].is<uint64_t>() &&
         WaterTestControllerSnapshot::restoreExactNumbers(saved.as<JsonObject>());
}
bool checkpointController() {
  const unsigned run = program.controllerRun;
  if (!run || !program.controllerRunEnded || controllerCheckpointed[run - 1])
    return true;
  JsonDocument saved;
  controllerRunMetadata(saved.to<JsonObject>(), run);
  const bool ok = saveDocument(controllerPath(run), saved);
  controllerCheckpointed[run - 1] = ok;
  return ok;
}
void aggregateLegacyControllerResults() {
  bool complete = true, timedOut = false, initialSettled = true, observed = true;
  unsigned episodes = 0;
  for (JsonObjectConst result : terminal["controllers"].as<JsonArrayConst>()) {
    complete = complete && (result["controller_completed"] == true);
    timedOut = timedOut || (result["controller_timed_out"] == true);
    initialSettled = initialSettled && (result["controller_initial_settled"] == true);
    observed = observed && (result["controller_observation_complete"] == true);
    episodes += result["controller_episodes_completed"] | 0U;
  }
  terminal["controller_completed"] = complete;
  terminal["controller_timed_out"] = timedOut;
  terminal["controller_episodes_completed"] = episodes;
  terminal["controller_episodes_required"] = legacyControllerRequiredEpisodes * controllerRunCount;
  terminal["controller_initial_settled"] = initialSettled;
  terminal["controller_observation_complete"] = observed;
}
void finishControllers() {
  auto results = terminal["controllers"].to<JsonArray>();
  for (unsigned run = 1; run <= controllerRunCount; ++run) {
    JsonDocument saved;
    if (loadControllerCheckpoint(run, saved))
      results.add(saved.as<JsonObjectConst>());
    else if (run < program.controllerRun) {
      auto out = results.add<JsonObject>();
      WaterTestControllerSnapshot::final(out, initialController(run), nullptr, true, 0, 0, 0);
      out["run"] = run;
      out["status"] = "interrupted";
      out["reason"] = "checkpoint_unavailable";
      out["checkpoint_unavailable"] = true;
      out["completion_policy"] = "fixed_duration_v1";
      out["run_duration_complete"] = false;
      controllerObservations[run - 1].json(out["observations"].to<JsonObject>());
    } else
      controllerRunMetadata(results.add<JsonObject>(), run);
  }
}
void recoverControllers(const JsonDocument (&boundaries)[controllerRunCount],
                        const unsigned (&settled)[controllerRunCount],
                        const ObservationCounts (&observations)[controllerRunCount]) {
  auto results = terminal["controllers"].to<JsonArray>();
  for (unsigned run = 1; run <= controllerRunCount; ++run) {
    JsonDocument saved;
    if (loadControllerCheckpoint(run, saved)) {
      results.add(saved.as<JsonObjectConst>());
      continue;
    }
    auto out = results.add<JsonObject>();
    const auto recorded = boundaries[run - 1].as<JsonObjectConst>();
    const bool started = recorded["started_us"].is<uint64_t>();
    WaterTestControllerSnapshot::final(out, initialController(run), nullptr, started, 0, 0, 0);
    out["run"] = run;
    out["boot_id"] = recordingBoot;
    out["status"] = started ? "interrupted" : "skipped";
    out["reason"] = "reboot_interrupted";
    if (fixedDurationPlan()) {
      const uint64_t began = recorded["started_us"] | uint64_t(0);
      const uint64_t ended = recorded["ended_us"] | uint64_t(0);
      // A pending recording may have been made by firmware with a shorter plan.
      const uint32_t plannedSeconds = initialController(run)["max_duration_s"] | controllerSeconds;
      const bool complete = started && recorded["run_duration_complete"] == true && ended >= began &&
                            ended - began >= uint64_t(plannedSeconds) * 1000000;
      out["completion_policy"] = "fixed_duration_v1";
      out["run_duration_complete"] = complete;
      observations[run - 1].json(out["observations"].to<JsonObject>());
      if (complete) {
        out["status"] = "completed";
        out["reason"] = "duration_complete";
      }
    } else {
      out["controller_completed"] = false;
      out["controller_episodes_required"] = legacyControllerRequiredEpisodes;
      out["controller_episodes_completed"] = settled[run - 1];
      out["controller_initial_settled"] = settled[run - 1] > 0;
      out["controller_observation_complete"] = settled[run - 1] >= legacyControllerRequiredEpisodes;
    }
    // Exact target/boundaries survive in the journal. Unobserved stability,
    // deadline state and learned parameters do not; do not guess them.
    for (JsonPairConst pair : recorded)
      if (!fixedDurationPlan() || std::strcmp(pair.key().c_str(), "run_duration_complete") != 0)
        out[pair.key()] = pair.value();
    if (started || fs_exists(controllerPath(run)))
      out["checkpoint_unavailable"] = true;
  }
  if (!fixedDurationPlan())
    aggregateLegacyControllerResults();
}
void finishRun() {
  const uint64_t offUs = nowUs();
  const bool wasPump = physicalPump();
  const bool wasHeat = physicalHeat();
  forceOff();
  closeControllerObservation();
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
  terminal["useful_response"] = program.usefulResponse;
  terminal["baseline_noise_c"] = program.noiseC;
  terminal["baseline_drift_c_per_s"] = program.baselineDrift;
  terminal["baseline_drift_uncertainty_c_per_s"] = program.baselineDriftUncertainty;
  if (program.controllerFinalObservationStarted) {
    auto observation = terminal["controller_final_observation"].to<JsonObject>();
    observation["run"] = program.controllerRun;
    observation["started_us"] = uint64_t(std::llround(program.controllerFinalObservationStarted * 1000000));
    observation["ended_us"] = uint64_t(std::llround(program.controllerFinalObservationEnded * 1000000));
    observation["last_valid_read_us"] = uint64_t(std::llround(program.controllerFinalObservationLastRead * 1000000));
    observation["valid_beer_samples"] = program.controllerFinalObservationSamples;
    observation["completed"] = program.controllerFinalObservationCompleted;
    observation["reason"] = program.controllerFinalObservationCompleted ? "observation_complete" : reasonName(program.reason);
  }
  finishControllers();
  testController.reset();
  if (controllerAllocationFailed)
    terminal["error_detail"] = "Not enough free memory to start the cooling algorithm. Cooling has stopped; the "
                               "recorded test data is retained.";
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
  program.releaseHistory();
  uploadState = terminalDurable ? "pending" : "error";
  uploadError.clear();
  reason = terminal["error_detail"] | reasonName(program.reason);
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
TestControllerPtr makeTestController(uint32_t minimumOn, uint32_t minimumOff,
                                     GlycolCooling::Algorithm algorithm) {
  PredictiveCooling::Config predictive;
  AdaptiveCooling::Config dose;
  predictive.min_on_s = dose.min_on_s = minimumOn;
  predictive.min_off_s = dose.min_off_s = minimumOff;
  // This ESP toolchain implements nothrow new through throwing new; allocation
  // failure would still abort when C++ exceptions are disabled. The constructor
  // only initializes fixed storage, so check malloc before placement construction.
  void *storage = std::malloc(sizeof(GlycolCooling::Controller));
  if (!storage)
    return {};
  return TestControllerPtr(new (storage) GlycolCooling::Controller(algorithm, predictive, dose));
}
bool controllerPlan(JsonObject out, unsigned run) {
  // The preview uses the same constructor and effective relay settings as the
  // eventual test controller. It neither steps nor touches normal brewing tuning.
  auto defaults = makeTestController(minimumOn(), minimumOff(), algorithmForRun(run));
  if (!defaults)
    return false;
  WaterTestControllerSnapshot::initial(out, *defaults, COOLING_IMPLEMENTATION_ID);
  out["run"] = run;
  out["target_drop_c"] = controllerTargetDropC;
  out["minimum_headroom_c"] = controllerTargetDropC + controllerHeadroomMarginC;
  out["max_duration_s"] = controllerSeconds;
  out["completion_policy"] = "fixed_duration_v1";
  out["observation_goal"] = controllerObservationGoal;
  out["scope"] = "cooling_controller_core_with_test_safety_limits";
  return true;
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
  testController.reset();
  controllerAllocationFailed = false;
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
  install["glycol_flow_source"] = input["glycol_flow_source"] | "unknown";
  install["glycol_flow_value"] = input["glycol_flow_value"];
  install["glycol_flow_unit"] = input["glycol_flow_unit"];
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
  plan["decision_rules"]["version"] = 2;
  plan["decision_rules"]["useful_drop_c"] = .20;
  plan["decision_rules"]["useful_noise_multiplier"] = 4;
  plan["decision_rules"]["baseline_window_s"] = baselineWindowSeconds;
  plan["decision_rules"]["background_cooling_correction_s"] = driftCorrectionSeconds;
  plan["decision_rules"]["settling_window_min_s"] = 90;
  plan["decision_rules"]["settling_window_max_s"] = 1800;
  plan["decision_rules"]["cooling_tail_rate_tolerance_c_per_s"] = coolingTailRateTolerance;
  plan["decision_rules"]["weak_pilot_off_s"] = 300;
  plan["decision_rules"]["validation_gap_s"] = 30;
  plan["decision_rules"]["statistical_decision_interval_s"] = 1;
  plan["pulse_selection"] = "10/30/90/270/810/1800s caps; stop early at max(0.20C,4x baseline noise); response-sized "
                            "contrast then two reserved pulses; later doses capped by remaining temperature range";
  plan["observation_selection"] =
      "baseline180-300s; measured drops only, recent background cooling discounted; off>=180s, extended by response "
      "delay and measured coast, until flat or steady warming in two windows; refresh baseline after recovery; "
      "weak pilot may advance after300s without detected or ongoing cooling; unresolved off cap21600s ends inconclusive";
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
  // All survey values have been copied into the manifest. Release their parse
  // storage before allocating the controller used to capture its exact defaults.
  input.clear();
  plan["controller_plan_version"] = 3;
  plan["order"] = "selected_first";
  plan["controller_transition_max_s"] = controllerTransitionSeconds;
  auto finalObservation = plan["controller_final_observation"].to<JsonObject>();
  finalObservation["phase"] = "controller_final_observe";
  finalObservation["min_duration_s"] = controllerFinalObservationSeconds;
  finalObservation["max_duration_s"] = controllerFinalObservationMaxSeconds;
  finalObservation["min_valid_beer_samples"] = controllerFinalObservationRequiredSamples;
  finalObservation["record_valid_beer_samples"] = true;
  plan["diagnostic_max_drop_c"] = diagnosticMaximumDropC;
  auto controllers = plan["controllers"].to<JsonArray>();
  const bool controllerPlanReady = controllerPlan(controllers.add<JsonObject>(), 1) &&
                                   controllerPlan(controllers.add<JsonObject>(), 2);
  if (!controllerPlanReady) {
    manifest.clear();
    owned = false;
    uploadState = "idle";
    reason = "Not enough free memory to prepare the cooling test; the test was not started.";
    return;
  }
  if (!program.allocateHistory()) {
    manifest.clear();
    owned = false;
    uploadState = "idle";
    reason = "Not enough free memory to record the cooling test; the test was not started.";
    return;
  }
  journal = fs_open(journalPath, "wb");
  if (!journal) {
    program.releaseHistory();
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
    program.releaseHistory();
    manifest.clear();
    owned = false;
    uploadState = "idle";
    reason = "Unable to reserve and save the offline recording; the test was not started.";
    return;
  }
  if (!program.start(recordStartUs / 1e6, waterC, minimumOn(), minimumOff())) {
    closeJournal();
    removePreviousDataset();
    program.releaseHistory();
    manifest.clear();
    owned = false;
    uploadState = "idle";
    reason = "Unable to initialize the cooling test; the test was not started.";
    return;
  }
  forceOff();
  tempControl.cs.mode = Modes::off;
  lastControllerRecordUs = 0;
  lastControllerStep = -1;
  recordedControllerObservation = 0;
  std::fill_n(controllerObservations, controllerRunCount, ObservationCounts{});
  recordedControllerStarts = recordedControllerFinishes = 0;
  std::fill_n(controllerCheckpointed, controllerRunCount, false);
  recordedRole = Role::Complete;
  program.lastSample = cached(beerAddress)->lastGood.read / 1e6;
  reason.clear();
  uploadState = "pending";
  uploadError.clear();
  overflow = false;
  running = true;
  startupHold = false;
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
  if (program.controllerRun > recordedControllerStarts) {
    recordedControllerStarts = program.controllerRun;
    recordControllerRun(false);
    if (program.pump) {
      serviceProgram(false);
      if (!running)
        return;
    }
  }
  if (recordControllerObservation() && program.pump) {
    serviceProgram(false);
    if (!running)
      return;
  }
  if (program.controllerRun && program.controllerRunEnded &&
      program.controllerRun > recordedControllerFinishes) {
    closeControllerObservation();
    recordedControllerFinishes = program.controllerRun;
    recordControllerRun(true);
    // The pump has already been forced OFF. Save exact tuning before releasing
    // this controller or admitting a second run; a reboot cannot erase run 1.
    if (!checkpointController()) {
      program.finish(nowUs() / 1e6, End::Failed, Reason::StorageFailure);
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
  if (!physicalPump())
    program.confirmControllerFinalOff(nowUs() / 1e6);
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
  else if (allowNewPulse || program.pump || program.stopping || program.phase == Phase::ControllerFinalObserve ||
           now - program.started >= maximumSeconds)
    program.tick(now);
  // The same portable controller used in normal glycol mode gets fresh tuning.
  // Its output history is exactly the test pump history during this block.
  // Protective service between slow journal writes must also evaluate a live
  // controller's OFF decision. Only the ordinary pass can create a controller
  // or start a pulse while the pump is OFF.
  if ((allowNewPulse || (program.pump && testController)) &&
      phase == Phase::Controller && program.phase == Phase::Controller) {
    if (!testController) {
      testController = makeTestController(program.minimumOn, program.minimumOff, algorithmForRun(program.controllerRun));
      if (!testController) {
        controllerAllocationFailed = true;
        program.finish(now, End::Failed, Reason::StorageFailure);
        applyProgram(phase);
        return;
      }
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
  if (allowNewPulse && running && program.phase == Phase::ControllerTransition &&
      program.controllerTransitionReady && controllerCheckpointed[program.controllerRun - 1]) {
    if (program.advanceController(nowUs() / 1e6)) {
      testController.reset();
      lastControllerRecordUs = 0;
      lastControllerStep = -1;
      recordedControllerPump = false;
      recordedControllerObservation = 0;
    }
    applyProgram(Phase::ControllerTransition);
  }
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
  const bool finalObservation = beer && sample.valid && phase == Phase::ControllerFinalObserve &&
                                program.controllerFinalOffConfirmed && !physicalPump() &&
                                sample.read / 1e6 > program.controllerFinalObservationStarted &&
                                sample.read <= nowUs();
  // All fresh samples above drive decisions. Only the durable journal has a
  // declared lower cadence, with a bounded denser interval around pump edges.
  if (regular || dense || finalObservation || !program.active()) {
    append(r);
    lastLoggedRead[role] = sample.read;
    if (regular)
      nextSparseRead[role] = sample.read + sparseSampleUs;
    else if (dense)
      ++denseRecords;
  }
  persistProgram(phase, changed, r.t_us);
}

struct UploadWorkspaceDeleter {
  void operator()(char *buffer) const { heap_caps_free(buffer); }
};
using UploadWorkspace = std::unique_ptr<char[], UploadWorkspaceDeleter>;

struct StoredUpload {
  const char *path = nullptr;
  size_t length = 0;
  static bool write(void *context, WaterTestTransport::BodySink sink, void *sinkContext) {
    const auto &source = *static_cast<StoredUpload *>(context);
    return streamDocumentPayload(source.path, sink, sinkContext, source.length);
  }
};
struct BatchUpload {
  Record records[batchSize]{};
  char boot[37]{};
  WaterTestUpload::Batch batch{};
  void *workspace = nullptr;
  size_t length = 0;
  static bool write(void *context, WaterTestTransport::BodySink sink, void *sinkContext) {
    auto &source = *static_cast<BatchUpload *>(context);
    size_t written = 0;
    return WaterTestUpload::writeBatch(source.batch, source.workspace, WaterTestUpload::workspaceBytes,
                                       sink, sinkContext, written) && written == source.length;
  }
};
std::string uploadWorkspaceError() {
  // Capture the failed allocation's heap state, rather than relying on a later
  // /api/heap poll after temporary allocations and the worker stack disappear.
  const size_t free = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  char details[224];
  snprintf(details, sizeof(details),
           "Not enough memory for upload workspace (need %u bytes; free %u; largest block %u). "
           "Recording retained; will retry.",
           unsigned(WaterTestUpload::workspaceBytes), unsigned(free), unsigned(largest));
  return details;
}

bool uploadNeeded() {
  return !running && queuedStart.empty() && !terminal.isNull() && !recoveryBlocked && uploadState != "submitted";
}
// Return whether another attempt is needed after backoff. All request buffers
// and documents leave scope before the task deletes itself and frees its stack.
bool uploadPending() {
  uint32_t delayMs = 1500;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(delayMs));
    delayMs = 1500;
    std::string id, path, error;
    UploadWorkspace workspace;
    StoredUpload stored;
    BatchUpload batch;
    WaterTestTransport::BodySource body{};
    uint32_t next = 0, end = 0;
    uint32_t firstSequence = 0, lastSequence = 0;
    bool isManifest = false, isFinish = false;
    std::string batchId;
    JsonDocument response;
    {
      Guard lock;
      if (!uploadNeeded())
        return false;
      if (!terminalDurable) {
        terminalDurable = saveDocument(finishPath, terminal);
        if (!terminalDurable) {
          uploadState = "error";
          uploadError = "The recording is retained; finish details could not yet be saved.";
          return true;
        }
      }
      next = uploadedRecords;
      id = manifest["test_id"].as<std::string>();
      path = "/api/v1/water-tests/" + id;
      if (!manifestUploaded) {
        isManifest = true;
        stored.path = manifestPath;
      } else if (next < recordCount) {
        FILE *f = fs_open(journalPath, "rb");
        if (!f) {
          uploadState = "error";
          uploadError = "Recording file is unavailable.";
          return true;
        }
        const size_t count = std::min(batchSize, size_t(recordCount - next));
        bool validBatch = fseek(f, next * sizeof(Record), SEEK_SET) == 0;
        for (size_t i = 0; validBatch && i < count; ++i) {
          auto &record = batch.records[i];
          validBatch = fread(&record, sizeof(record), 1, f) == 1 && valid(record) &&
                       record.boot == 0 && record.seq == next + i;
        }
        fclose(f);
        // Never shorten a retry's payload while retaining its batch ID. The
        // server may already have accepted the complete batch and lost its ACK.
        if (!validBatch) {
          uploadState = "error";
          uploadError = "Recording checksum or sequence mismatch; retained for inspection.";
          return true;
        }
        end = next + count;
        firstSequence = batch.records[0].seq;
        lastSequence = batch.records[count - 1].seq;
        batchId = WaterTestProtocol::batchIdentifier(id, next);
        memcpy(batch.boot, recordingBoot, sizeof(batch.boot));
        batch.batch = {guid, id.c_str(), batchId.c_str(), batch.boot, batch.records, count,
                       manifest["sensors"]["beer"]["calibration_offset_c"] | 0.0,
                       manifest["sensors"]["glycol"]["calibration_offset_c"] | 0.0};
        workspace.reset(static_cast<char *>(heap_caps_malloc(WaterTestUpload::workspaceBytes, MALLOC_CAP_8BIT)));
        if (!workspace) {
          uploadState = "error";
          uploadError = uploadWorkspaceError();
          return true;
        }
        batch.workspace = workspace.get();
        if (!WaterTestUpload::writeBatch(batch.batch, batch.workspace, WaterTestUpload::workspaceBytes,
                                         nullptr, nullptr, batch.length)) {
          uploadState = "error";
          uploadError = "Recording could not be encoded within the bounded upload workspace; retained for inspection.";
          return true;
        }
        body = {batch.length, &batch, BatchUpload::write};
        path += "/batches";
      } else {
        isFinish = true;
        stored.path = finishPath;
        path += "/finish";
      }
      if (stored.path) {
        // Validate the saved envelope before HTTP headers can reach the portal.
        // The body itself is decoded directly from flash during transmission.
        if (!validateDocumentPayload(stored.path, stored.length)) {
          uploadState = "error";
          uploadError = "Saved upload metadata is unreadable or has a checksum mismatch; retained for inspection.";
          return true;
        }
        body = {stored.length, &stored, StoredUpload::write};
      }
      uploadState = "uploading";
      uploaderBusy = true;
    }
    bool ok = WaterTestTransport::send(path, !isManifest && !isFinish, body, response, error);
    workspace.reset();
    if (!ok)
      error = std::string(isManifest ? "Manifest upload: " : isFinish ? "Finish upload: " : "Record upload: ") + error;
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
      return true;
    if (isFinish)
      return false;
  }
}
// This worker never touches actuators. It starts only after acquisition closes,
// and releases its stack after delivery or a failed attempt rather than sleeping
// with 12 KiB reserved for the lifetime of the device.
void uploader(void *) {
  const bool retry = uploadPending();
  {
    Guard lock;
    nextUploaderAttemptUs = retry ? nowUs() + uploaderRetryUs : 0;
    uploaderBusy = false;
    uploaderTaskActive = false;
  }
  vTaskDelete(nullptr);
}
// Called with Guard held by the actual main loop, never by startup recovery.
void serviceUploader() {
  if (!backgroundServicesReady || uploaderTaskActive || !uploadNeeded() || nowUs() < nextUploaderAttemptUs)
    return;
  // Publish ownership before creation: the scheduler may run the task as soon
  // as xTaskCreate returns, including on the other core.
  uploaderTaskActive = true;
  if (xTaskCreate(uploader, "water-upload", 12288, nullptr, 1, nullptr) != pdPASS) {
    uploaderTaskActive = false;
    nextUploaderAttemptUs = nowUs() + uploaderRetryUs;
    uploadState = "error";
    uploadError = "Not enough memory to start the upload worker; the recording is retained and will be retried.";
  }
}
// Recovery closes an interrupted experiment; it never resumes a pulse or
// invents an OFF edge in the old boot's monotonic clock domain.
bool uncommittedEmptyStartup() {
  if (fs_exists(manifestPath))
    return false;
  // Finalization or legacy boot metadata indicates more than an abandoned
  // startup, even if those files are incomplete. Preserve that recovery block.
  for (const auto path : {finishPath, receiptPath, resumedPath, controllerOnePath, controllerTwoPath, "/water-test-boots.json"}) {
    if (fs_exists(path) || fs_exists((std::string(path) + ".tmp").c_str()))
      return false;
  }
  FILE *file = fs_open(journalPath, "rb");
  if (!file)
    return false;
  const bool empty = fgetc(file) == EOF && !ferror(file);
  const bool closed = fclose(file) == 0;
  return empty && closed;
}
void recoverDataset() {
  if (!fs_exists(manifestPath) && !fs_exists(journalPath) &&
      !fs_exists(controllerOnePath) && !fs_exists(controllerTwoPath) &&
      !fs_exists((std::string(controllerOnePath) + ".tmp").c_str()) &&
      !fs_exists((std::string(controllerTwoPath) + ".tmp").c_str()))
    return;
  if (uncommittedEmptyStartup()) {
    startupHold = true;
    forceOff();
    uploadState = "not_submitted";
    reason = "The controller restarted before the test began. No measurements were recorded. "
             "You can start a new test; outputs remain off.";
    return; // Keep all files until the user explicitly starts another test.
  }
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
  unsigned recoveredEpisodeStarts = 0, recoveredEpisodeSettles = 0;
  JsonDocument recoveredRuns[controllerRunCount];
  unsigned recoveredRunStarts[controllerRunCount] = {}, recoveredRunSettles[controllerRunCount] = {};
  ObservationCounts recoveredObservations[controllerRunCount];
  uint32_t recoveredObservationSequence[controllerRunCount] = {};
  uint64_t recoveredFinalObservationStarted = 0, recoveredFinalObservationLastRead = 0;
  unsigned recoveredFinalObservationSamples = 0;
  Record record{};
  while (fread(&record, sizeof(record), 1, file) == 1 && valid(record) && record.boot == 0 &&
         record.seq == recordCount && record.t_us >= lastRecordUs) {
    ++recordCount;
    lastRecordUs = record.t_us;
    if (record.kind == 4 && record.code == static_cast<uint8_t>(Phase::Observe))
      recoveredPulses = std::max(recoveredPulses, uint32_t(record.pulse));
    if (record.kind == 4 && record.code == static_cast<uint8_t>(Phase::ControllerFinalObserve))
      recoveredFinalObservationStarted = record.read_us;
    if (recoveredFinalObservationStarted && record.kind == 2 && record.role == 0 &&
        (record.flags & 1) && !(record.flags & 2) && record.read_us > recoveredFinalObservationStarted &&
        record.read_us > recoveredFinalObservationLastRead && record.read_us <= record.t_us) {
      ++recoveredFinalObservationSamples;
      recoveredFinalObservationLastRead = record.read_us;
    }
    if (record.kind == 9 && record.role >= 1 && record.role <= controllerRunCount) {
      auto &boundary = recoveredRuns[record.role - 1];
      if (record.pulse == 0) {
        JsonDocument decoded;
        WaterTestProtocol::recordToJson(decoded.to<JsonObject>(), record, recordingBoot, 0.);
        boundary["started_us"] = record.read_us;
        for (auto key : {"start_c", "target_c", "target_c_exact"})
          boundary[key] = decoded[key];
      } else if (record.pulse == 1) {
        boundary["ended_us"] = record.read_us;
        if (record.flags & 128)
          boundary["run_duration_complete"] = bool(record.flags & 1);
      }
    }
    if (record.kind == 10 && record.role >= 1 && record.role <= controllerRunCount &&
        record.detail == recoveredObservationSequence[record.role - 1] + 1 &&
        recoveredObservations[record.role - 1].record(record.pulse))
      recoveredObservationSequence[record.role - 1] = record.detail;
    if (record.kind == 8 && record.role >= 1 && record.role <= controllerRunCount &&
        record.pulse <= legacyControllerRequiredEpisodes) {
      auto &starts = recoveredRunStarts[record.role - 1];
      auto &settles = recoveredRunSettles[record.role - 1];
      if (record.code == 0 && starts == settles && record.pulse == starts + 1)
        ++starts;
      else if (record.code == 1 && starts == settles + 1 && record.pulse == starts)
        ++settles;
    }
    if (record.kind == 8 && record.pulse <= legacyControllerRequiredEpisodes) {
      if (record.code == 0 && recoveredEpisodeStarts == recoveredEpisodeSettles &&
          record.pulse == recoveredEpisodeStarts + 1)
        ++recoveredEpisodeStarts;
      else if (record.code == 1 && recoveredEpisodeStarts == recoveredEpisodeSettles + 1 &&
               record.pulse == recoveredEpisodeStarts)
        ++recoveredEpisodeSettles;
    }
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
    if (recoveredFinalObservationStarted) {
      auto observation = terminal["controller_final_observation"].to<JsonObject>();
      observation["run"] = controllerRunCount;
      observation["started_us"] = recoveredFinalObservationStarted;
      observation["ended_us"] = nullptr;
      observation["last_valid_read_us"] = recoveredFinalObservationLastRead;
      observation["valid_beer_samples"] = recoveredFinalObservationSamples;
      observation["completed"] = false;
      observation["reason"] = "reboot_interrupted";
    }
    if (manifest["test_program"]["controller"]["required_episodes"] == legacyControllerRequiredEpisodes) {
      terminal["controller_episodes_required"] = legacyControllerRequiredEpisodes;
      terminal["controller_episodes_completed"] = recoveredEpisodeSettles;
      terminal["controller_initial_settled"] = recoveredEpisodeSettles > 0;
      terminal["controller_observation_complete"] = recoveredEpisodeSettles >= legacyControllerRequiredEpisodes;
      terminal["controller_completed"] = false;
      // The durable episode history survives a reboot. Current stability and
      // whether a deadline was reached during the unobserved interval do not.
    }
    terminal["final_outputs"]["pump_on"] = false;
    terminal["final_outputs"]["heater_on"] = false;
    terminal["recovery_boot_id"] = currentBoot;
    terminal["last_recorded_us"] = lastRecordUs;
    terminal["unobserved_shutdown"] = true;
    if (manifest["test_program"]["controller_plan_version"] == 2 || fixedDurationPlan())
      recoverControllers(recoveredRuns, recoveredRunSettles, recoveredObservations);
    else
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
  reason = terminal["error_detail"] | (terminal["reason"] | "Recovered interrupted test.");
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
}
void startBackgroundServices() { backgroundServicesReady = true; }
bool active() { return running.load(); }
bool controlOwned() { return owned.load() || startupHold.load(); }
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
  if (recoveryBlocked || owned || running || uploaderBusy || uploaderTaskActive ||
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
  const auto flowSource = body["glycol_flow_source"];
  const auto flowValue = body["glycol_flow_value"];
  const auto flowUnit = body["glycol_flow_unit"];
  const bool legacyFlow = flowSource.isUnbound() && flowValue.isNull() && flowUnit.isNull();
  if (!legacyFlow) {
    if (!flowSource.is<const char *>() ||
        !oneOf(flowSource.as<const char *>(), {"unknown", "pump_rating", "measured_at_fermenter"})) {
      error = "Select whether glycol flow is unknown, a pump rating, or measured at the fermenter.";
      return false;
    }
    if (flowSource == "unknown") {
      if (!flowValue.isNull() || !flowUnit.isNull()) {
        error = "Unknown glycol flow must not include a rate or unit.";
        return false;
      }
    } else {
      if (!flowValue.is<double>() || !std::isfinite(flowValue.as<double>()) || flowValue.as<double>() <= 0 ||
          !flowUnit.is<const char *>() || !oneOf(flowUnit.as<const char *>(), {"us_gph", "us_gpm", "lph", "lpm"})) {
        error = "Enter a positive glycol flow rate and select its unit.";
        return false;
      }
      const double factor = flowUnit == "us_gph" ? 3.785411784 / 60.
                            : flowUnit == "us_gpm" ? 3.785411784
                            : flowUnit == "lph"    ? 1. / 60.
                                                  : 1.;
      const double litersPerMinute = flowValue.as<double>() * factor;
      if (!std::isfinite(litersPerMinute) || litersPerMinute <= 0) {
        error = "The glycol flow rate cannot be represented in liters per minute.";
        return false;
      }
    }
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
  } else if (owned || startupHold)
    forceOff();
  if (queuedResume) {
    queuedResume = false;
    tempControl.resumeAfterWaterTest(savedControl);
    owned = false;
    if (uploadState == "submitted")
      removePreviousDataset();
    reason = "Normal control resumed.";
  }
  serviceUploader();
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
  doc["control_owned"] = controlOwned();
  doc["startup_interrupted"] = startupHold.load();
  doc["phase"] = !queuedStart.empty() ? "preflight" : phaseName(program.phase);
  doc["pulse_number"] = program.pulse;
  doc["analysis_role"] = program.analysisRole();
  doc["block_id"] = program.block;
  doc["response_settled"] = program.responseSettled;
  if (program.phase == Phase::Controller || program.phase == Phase::ControllerTransition ||
      program.phase == Phase::ControllerFinalObserve) {
    doc["controller_run"] = program.controllerRun;
    doc["controller_run_count"] = controllerRunCount;
    doc["controller_algorithm"] = GlycolCooling::selectionName(algorithmForRun(program.controllerRun));
    doc["controller_target_c"] = program.targetC;
    doc["controller_completion_policy"] = "fixed_duration_v1";
    doc["controller_run_duration_complete"] = program.controllerRunDurationComplete;
    const double runNow = program.controllerRunEnded ? program.controllerRunEnded : nowUs() / 1e6;
    doc["controller_elapsed_s"] = std::max(0., runNow - program.controllerStarted);
    doc["controller_duration_s"] = controllerSeconds;
    controllerObservations[program.controllerRun - 1].json(doc["controller_observations"].to<JsonObject>());
    doc["controller_remaining_s"] = std::max(0., program.deadline - nowUs() / 1e6);
    if (program.phase == Phase::ControllerFinalObserve) {
      doc["controller_final_observation_remaining_s"] =
          std::max(0., program.controllerFinalObservationStarted + controllerFinalObservationSeconds - nowUs() / 1e6);
      doc["controller_final_observation_valid_samples"] = program.controllerFinalObservationSamples;
      doc["controller_final_observation_required_samples"] = controllerFinalObservationRequiredSamples;
    }
  }
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
  doc["upload_manifest_uploaded"] = manifestUploaded;
  doc["upload_records_uploaded"] = uploadedRecords;
  doc["upload_records_total"] = recordCount;
  doc["upload_stage"] = uploadState == "submitted" ? "complete"
                          : terminal.isNull() ? "none"
                          : !manifestUploaded ? "manifest"
                          : uploadedRecords < recordCount ? "batches" : "finish";
  doc["result_url"] = std::string(endpoint) + "/" + guid + "/";
  doc["moved_chamber_probe"] = movedProbe();
  doc["can_start"] = !recoveryBlocked && !owned && !running && !uploaderBusy && !uploaderTaskActive &&
                     (manifest.isNull() || uploadState == "submitted" || uploadState == "not_submitted") &&
                     check.empty();
  doc["can_resume"] = owned && !running && queuedStart.empty() &&
                      manifest["prior_control"]["mode"].is<const char *>();
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
