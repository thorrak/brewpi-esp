#pragma once

#include "OneWireSensorPolicy.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <new>

// Platform-independent sequencing and journal primitives. Time is monotonic seconds;
// adaptive decisions use fresh raw readings, and no network work runs here.
namespace WaterTestCore {
constexpr uint32_t baselineSeconds = 300; // upper bound, not a required wait
constexpr uint32_t minimumBaselineSeconds = 60;
constexpr uint32_t observationSeconds = 21600;
constexpr uint32_t maximumSeconds = 43200;
constexpr uint32_t maximumPulseSeconds = 1800;
constexpr uint32_t maximumPumpSeconds = 7200;
constexpr uint32_t controllerSeconds = 3600;
constexpr unsigned historyCapacity = 2048;
constexpr double coolingTailRateTolerance = .000005;
constexpr double freshnessSeconds = OneWireSensorPolicy::connectedTimeoutUs / 1e6;
constexpr double maximumDropC = 3;
constexpr double minimumWaterC = 4;
// Append values: existing journal enum IDs must remain readable.
enum class Phase : uint8_t { Idle, Baseline, Pulse, Observe, Stopping, Finished, Controller };
enum class End : uint8_t { None, Completed, Stopped, Inconclusive, Failed };
enum class Reason : uint8_t {
  None,
  Start,
  Pilot,
  AdditionalPulse,
  Observation,
  ProgramComplete,
  NoResponse,
  UserStop,
  SensorFault,
  SensorStale,
  TemperatureLimit,
  RuntimeLimit,
  StorageFailure,
  QueueOverflow,
  RelayLimit,
  PumpBudget,
  UnexpectedOutput,
  PulseLimit
};
enum class Role : uint8_t { Baseline, Calibration, Validation, Controller, Complete };
inline const char *phaseName(Phase p) {
  const char *names[] = {"idle", "baseline", "pulse", "observe", "stopping", "finished", "controller"};
  return names[static_cast<unsigned>(p)];
}
inline const char *roleName(Role r) {
  const char *names[] = {"baseline", "calibration", "validation", "controller", "complete"};
  return names[static_cast<unsigned>(r)];
}
inline const char *endName(End e) {
  const char *names[] = {"", "completed", "stopped", "inconclusive", "failed"};
  return names[static_cast<unsigned>(e)];
}
inline const char *reasonName(Reason r) {
  const char *names[] = {"",
                         "start",
                         "pilot_pulse",
                         "additional_pulse",
                         "observe_response",
                         "program_complete",
                         "no_measurable_response",
                         "user_stop",
                         "beer_sensor_fault",
                         "beer_sensor_stale",
                         "temperature_limit",
                         "runtime_limit",
                         "recording_failure",
                         "sample_queue_overflow",
                         "relay_minimum_limit",
                         "pump_time_limit",
                         "unexpected_output",
                         "pulse_time_limit"};
  return names[static_cast<unsigned>(r)];
}
struct Program {
  Phase phase = Phase::Idle;
  Role role = Role::Baseline;
  End outcome = End::None;
  Reason reason = Reason::None;
  double started = 0, deadline = 0, switched = 0, pulseStarted = 0;
  double initialC = 0, latestC = 0, lastSample = 0, beforePulseC = 0, minimumC = 0;
  double totalPump = 0, noiseC = .03125, baselineDrift = 0;
  double lastPulseSeconds = 0, usefulPulseSeconds = 0, peakResponse = 0;
  double responseOnset = -1, observedDelay = 0, usefulResponseDrop = 0, observedCoast = 0;
  double targetC = 0, controllerStarted = 0, stableSince = -1, lastDecision = -1;
  uint32_t minimumOn = 2, minimumOff = 2;
  unsigned pulse = 0, completedPulses = 0, block = 0, escalation = 0;
  unsigned calibrationPulses = 0, validationPulses = 0, controllerCycles = 0;
  bool pump = false, stopping = false, contrastStarted = false, responseSettled = false;
  bool usefulResponse = false, nextPulsePending = false, controllerTimedOut = false;
  struct Reading {
    float elapsed = 0, c = 0;
  };
  Reading readings[historyCapacity]{};
  unsigned head = 0, count = 0;
  struct Stats {
    unsigned n = 0;
    double span = 0, mean = 0, slope = 0, noise = 0;
  };
  Stats stats(double begin, double end) const {
    Stats r;
    double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0, first = end, last = begin;
    for (unsigned i = 0; i < count; ++i) {
      const auto &v = readings[i];
      const double time = started + v.elapsed;
      if (time < begin || time > end)
        continue;
      const double x = time - begin;
      ++r.n;
      sx += x;
      sy += v.c;
      sxx += x * x;
      sxy += x * v.c;
      syy += v.c * v.c;
      first = std::min(first, time);
      last = std::max(last, time);
    }
    if (r.n < 3)
      return r;
    r.span = last - first;
    r.mean = sy / r.n;
    const double xx = sxx - sx * sx / r.n, xy = sxy - sx * sy / r.n;
    r.slope = xx > 0 ? xy / xx : 0;
    r.noise = std::sqrt(std::max(0., (syy - sy * sy / r.n - r.slope * xy) / (r.n - 2)));
    return r;
  }
  void remember(double read, double c) {
    // Bound memory by the acquisition cadence, even if a host calls sample at 1 Hz.
    const double elapsed = read - started;
    if (count && elapsed - readings[(head + historyCapacity - 1) % historyCapacity].elapsed < 1.9)
      return;
    readings[head] = {static_cast<float>(elapsed), static_cast<float>(c)};
    head = (head + 1) % historyCapacity;
    count = std::min(historyCapacity, count + 1);
  }
  const char *analysisRole() const { return roleName(role); }
  bool active() const { return phase != Phase::Idle && phase != Phase::Finished; }
  bool submissionEligible() const { return phase == Phase::Finished && outcome != End::None; }
  double usefulThreshold() const { return std::max(.20, 4 * noiseC); }
  bool start(double now, double beerC, uint32_t on, uint32_t off) {
    // Reset in place: the history is larger than the MCU task stack.
    this->~Program();
    new (this) Program;
    minimumOn = std::max<uint32_t>(2U, on);
    minimumOff = std::max<uint32_t>(2U, off);
    if (!std::isfinite(beerC) || minimumOn > maximumPulseSeconds || minimumOff > observationSeconds)
      return false;
    phase = Phase::Baseline;
    started = switched = lastSample = now;
    initialC = latestC = beforePulseC = minimumC = beerC;
    deadline = now + baselineSeconds;
    reason = Reason::Start;
    remember(now, beerC);
    return true;
  }
  void endPulse(double now) {
    if (!pump)
      return;
    lastPulseSeconds = std::max(0., now - pulseStarted);
    totalPump += lastPulseSeconds;
    pump = false;
    switched = now;
  }
  void finish(double now, End e, Reason r) {
    endPulse(now);
    switched = now;
    phase = Phase::Finished;
    role = Role::Complete;
    outcome = e;
    reason = r;
  }
  void stop(double now) {
    if (!active())
      return;
    if (now - lastSample > freshnessSeconds) {
      finish(now, End::Failed, Reason::SensorStale);
      return;
    }
    if (now - started >= maximumSeconds) {
      finish(now, End::Inconclusive, Reason::RuntimeLimit);
      return;
    }
    stopping = true;
    phase = Phase::Stopping;
    reason = Reason::UserStop;
    if (!pump || now - switched >= minimumOn)
      finish(now, End::Stopped, Reason::UserStop);
  }
  void sample(double read, double beerC, bool valid, double now) {
    if (!active())
      return;
    if (!valid || !std::isfinite(beerC) || read > now || read < lastSample) {
      if (now - lastSample > freshnessSeconds)
        finish(now, End::Failed, Reason::SensorStale);
      return;
    }
    if (read > lastSample)
      remember(read, beerC);
    latestC = beerC;
    lastSample = read;
    minimumC = std::min(minimumC, beerC);
    if (role == Role::Calibration || role == Role::Validation) {
      // Remove measured pump-off drift from the excitation decision only.
      peakResponse = std::max(peakResponse, beforePulseC + baselineDrift * (read - pulseStarted) - beerC);
      if (responseOnset < 0 && peakResponse >= std::max(.125, 3 * noiseC)) {
        responseOnset = std::max(0., read - pulseStarted);
        observedDelay = std::max(observedDelay, responseOnset);
      }
    }
    if (beerC <= minimumWaterC || initialC - beerC >= maximumDropC)
      finish(now, End::Stopped, Reason::TemperatureLimit);
  }
  bool baselineReady(double now) {
    if (now - started < minimumBaselineSeconds)
      return false;
    const auto whole = stats(now - 60, now), a = stats(now - 60, now - 30), b = stats(now - 30, now);
    if (whole.n < 20 || whole.span < 55)
      return false;
    const bool consistent = a.n >= 8 && b.n >= 8 && std::abs(a.slope - b.slope) <= .0015 && whole.noise <= .10;
    if (!consistent && now < deadline)
      return false;
    baselineDrift = whole.slope;
    noiseC = std::max(.03125, whole.noise);
    return true;
  }
  bool settled(double now) const {
    // A flat trace before any detected response never proves that cooling is finished.
    const double window = std::max(90., std::min(1800., observedDelay));
    if (now - switched < std::max({2 * window, 2 * observedDelay, double(minimumOff)}) ||
        peakResponse < std::max(.125, 3 * noiseC))
      return false;
    const auto a = stats(now - 2 * window, now - window), b = stats(now - window, now);
    if (a.n < 20 || b.n < 20 || a.span < window - 5 || b.span < window - 5)
      return false;
    // A small steady cooling rate can still hide a large slow tail. Use a
    // tighter cooling threshold while allowing weak background warming.
    const auto nearBaseline = [&](double rate) {
      return rate - baselineDrift >= -coolingTailRateTolerance && rate - baselineDrift <= .0001;
    };
    return nearBaseline(a.slope) && nearBaseline(b.slope) &&
           std::abs(b.mean - a.mean - baselineDrift * window) <= std::max(.0625, 2 * noiseC);
  }
  uint32_t choosePulse() const {
    static const uint32_t candidates[] = {10, 30, 90, 270, 810, 1800};
    double selected = candidates[std::min(escalation, 5U)];
    if (role == Role::Validation || contrastStarted) {
      const double remaining = std::max(0., latestC - std::max(minimumWaterC, initialC - maximumDropC));
      // Reserve cooling range for the contrast, paired check and controller.
      // This is an excitation budget, not a claim of identified physical gain.
      const double budget = .20 * remaining * usefulPulseSeconds / std::max(.125, usefulResponseDrop);
      selected = std::max(5., std::min(usefulPulseSeconds * .5, budget));
    }
    return std::max(minimumOn, static_cast<uint32_t>(std::ceil(selected)));
  }
  void beginPulse(double now) {
    const auto duration = choosePulse();
    if (totalPump + duration > maximumPumpSeconds) {
      finish(now, End::Inconclusive, Reason::PumpBudget);
      return;
    }
    reason = pulse ? Reason::AdditionalPulse : Reason::Pilot;
    ++pulse;
    ++block;
    if (role == Role::Calibration)
      ++calibrationPulses;
    else
      ++validationPulses;
    pump = true;
    switched = pulseStarted = now;
    phase = Phase::Pulse;
    deadline = now + duration;
    beforePulseC = minimumC = latestC;
    peakResponse = 0;
    responseOnset = -1;
    responseSettled = false;
    nextPulsePending = false;
  }
  void observe(double now) {
    endPulse(now);
    ++completedPulses;
    phase = Phase::Observe;
    reason = Reason::Observation;
    // Weak pilots overlap if still uninformative after five minutes. They are
    // explicitly not called settled, and analysis retains their thermal history.
    deadline = now + observationSeconds;
  }
  void setControllerPump(double now, bool requested) {
    if (phase != Phase::Controller || !active() || requested == pump)
      return;
    if (requested) {
      if (now - switched < minimumOff)
        return;
      if (totalPump >= maximumPumpSeconds) {
        finish(now, End::Inconclusive, Reason::PumpBudget);
        return;
      }
      pump = true;
      switched = pulseStarted = now;
      responseSettled = false;
    } else if (now - switched >= minimumOn) {
      endPulse(now);
      ++controllerCycles;
    }
  }
  void beginController(double now) {
    if (!usefulResponse || latestC - std::max(minimumWaterC, initialC - maximumDropC) < .5) {
      finish(now, End::Completed, Reason::ProgramComplete);
      return;
    }
    role = Role::Controller;
    phase = Phase::Controller;
    ++block;
    controllerStarted = now;
    deadline = now + controllerSeconds;
    targetC = latestC - .25;
    stableSince = -1;
  }
  void tick(double now) {
    if (!active())
      return;
    if (now - lastSample > freshnessSeconds) {
      finish(now, End::Failed, Reason::SensorStale);
      return;
    }
    if (now - started >= maximumSeconds) {
      finish(now, End::Inconclusive, Reason::RuntimeLimit);
      return;
    }
    if (pump && totalPump + now - pulseStarted >= maximumPumpSeconds) {
      finish(now, End::Inconclusive, Reason::PumpBudget);
      return;
    }
    if (stopping) {
      if (!pump || now - switched >= minimumOn)
        finish(now, End::Stopped, Reason::UserStop);
      return;
    }
    if (phase == Phase::Controller && pump && now - pulseStarted >= maximumPulseSeconds) {
      finish(now, End::Stopped, Reason::PulseLimit);
      return;
    }
    if (phase == Phase::Pulse) {
      const bool enough = role == Role::Calibration && !contrastStarted && peakResponse >= usefulThreshold();
      if ((now >= deadline || enough) && now - switched >= minimumOn)
        observe(now);
      return;
    }
    // Keep expensive history scans at 1 Hz; safety and relay deadlines above
    // still run on every fast main-loop pass and between durable writes.
    if (lastDecision >= 0 && now - lastDecision < 1.)
      return;
    lastDecision = now;
    if (phase == Phase::Controller) {
      const auto recent = stats(now - 60, now);
      if (!pump && controllerCycles && recent.span >= 55 && std::abs(latestC - targetC) <= .08 &&
          std::abs(recent.slope) <= .0001) {
        if (stableSince < 0)
          stableSince = now;
        if (now - stableSince >= 180 && now - switched >= std::max({180., observedCoast, 2 * observedDelay})) {
          responseSettled = true;
          finish(now, End::Completed, Reason::ProgramComplete);
        }
      } else
        stableSince = -1;
      if (active() && now >= deadline) {
        controllerTimedOut = true;
        finish(now, End::Completed, Reason::ProgramComplete);
      }
      return;
    }
    if (phase == Phase::Baseline) {
      if (!baselineReady(now))
        return;
      role = Role::Calibration;
      phase = Phase::Observe;
      deadline = now;
      nextPulsePending = true;
      return; // persist the analysis boundary while outputs are still OFF
    }
    if (phase != Phase::Observe || now - switched < minimumOff)
      return;
    if (nextPulsePending) {
      beginPulse(now);
      return;
    }
    if (role == Role::Validation && validationPulses == 1) {
      if (now - switched >= std::max<uint32_t>(30U, minimumOff))
        beginPulse(now);
      return;
    }
    responseSettled = settled(now);
    if (role == Role::Validation && now - switched < observedCoast)
      responseSettled = false;
    const bool weakPilot = role == Role::Calibration && !contrastStarted && peakResponse < usefulThreshold();
    const auto recent = stats(now - 120, now);
    const bool coolingArrived =
        peakResponse >= std::max(.125, 3 * noiseC) ||
        (peakResponse >= std::max(.0625, 2 * noiseC) && recent.span >= 110 && recent.slope < baselineDrift - .00005);
    const bool pilotWindow = weakPilot && !coolingArrived && now - switched >= std::max<uint32_t>(300U, minimumOff);
    if (!responseSettled && now < deadline && !pilotWindow)
      return;
    if (role == Role::Validation) {
      beginController(now);
      return;
    }
    if (peakResponse >= usefulThreshold()) {
      if (responseSettled)
        observedCoast = std::max(observedCoast, now - switched);
      usefulResponse = true;
      if (!usefulPulseSeconds) {
        usefulPulseSeconds = lastPulseSeconds;
        usefulResponseDrop = peakResponse;
      }
    }
    if (contrastStarted) {
      role = Role::Validation;
      ++block;
      nextPulsePending = true;
      deadline = now;
      return; // held-out boundary precedes its first pump edge
    }
    if (usefulResponse)
      contrastStarted = true;
    else if (escalation < 5)
      ++escalation;
    else {
      finish(now, End::Inconclusive, Reason::NoResponse);
      return;
    }
    beginPulse(now);
  }
};
// Record checksums detect damaged data before upload.
#pragma pack(push, 1)
struct Record {
  uint64_t t_us;
  uint64_t read_us; // samples: acquisition; clocks: UTC anchor
  uint32_t seq;
  uint32_t conversion_us; // sample read minus conversion start
  int16_t raw;
  uint8_t kind; // 0 boot, 1 clock, 2 sample, 3 output, 4 phase, 5 fault, 6 gap, 7 controller
  uint8_t boot;
  uint8_t role;  // sample 0 beer/1 glycol, output 0 pump/1 heater
  uint8_t flags; // 1 valid, 2 pump, 4 requested, 8 edge
  uint8_t code;
  uint8_t pulse;
  uint32_t detail;
  uint32_t crc;
};
#pragma pack(pop)
static_assert(sizeof(Record) == 40, "Recording storage budget assumes 40-byte records");
inline uint32_t checksum(const void *data, size_t n) {
  auto bytes = static_cast<const uint8_t *>(data);
  uint32_t crc = 0xffffffffU;
  while (n--) {
    crc ^= *bytes++;
    for (unsigned b = 0; b < 8; ++b)
      crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
  }
  return ~crc;
}
inline void seal(Record &r) { r.crc = checksum(&r, offsetof(Record, crc)); }
inline bool valid(const Record &r) { return r.crc == checksum(&r, offsetof(Record, crc)); }
} // namespace WaterTestCore
