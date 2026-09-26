#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Platform-independent sequencing and journal primitives. Time is monotonic seconds;
// observations never move a relay deadline, and no network work runs here.
namespace WaterTestCore {
constexpr uint32_t baselineSeconds = 300;
constexpr uint32_t observationSeconds = 1200;
constexpr uint32_t maximumSeconds = 5400;
constexpr uint32_t maximumPulseSeconds = 60;
constexpr uint32_t maximumPumpSeconds = 180;
constexpr double freshnessSeconds = 10;
constexpr double maximumDropC = 3;
constexpr double minimumWaterC = 4;
enum class Phase : uint8_t { Idle, Baseline, Pulse, Observe, Stopping, Finished };
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
  UnexpectedOutput
};
inline const char *phaseName(Phase p) {
  const char *names[] = {"idle", "baseline", "pulse", "observe", "stopping", "finished"};
  return names[static_cast<unsigned>(p)];
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
                         "unexpected_output"};
  return names[static_cast<unsigned>(r)];
}
struct Program {
  Phase phase = Phase::Idle;
  End outcome = End::None;
  Reason reason = Reason::None;
  double started = 0, deadline = 0, switched = 0, pulseStarted = 0;
  double initialC = 0, latestC = 0, lastSample = 0, beforePulseC = 0, minimumC = 0;
  double totalPump = 0;
  uint32_t minimumOn = 2, minimumOff = 2;
  unsigned pulse = 0;
  bool pump = false, stopping = false;
  bool active() const { return phase != Phase::Idle && phase != Phase::Finished; }
  bool start(double now, double beerC, uint32_t on, uint32_t off) {
    *this = Program{};
    minimumOn = std::max<uint32_t>(2U, on);
    minimumOff = std::max<uint32_t>(2U, off);
    if (minimumOn > maximumPulseSeconds || minimumOff > observationSeconds)
      return false;
    phase = Phase::Baseline;
    started = switched = lastSample = now;
    initialC = latestC = beforePulseC = minimumC = beerC;
    deadline = now + baselineSeconds;
    reason = Reason::Start;
    return true;
  }
  void stop(double now) {
    if (!active())
      return;
    stopping = true;
    phase = Phase::Stopping;
    reason = Reason::UserStop;
    if (!pump || now - switched >= minimumOn)
      finish(now, End::Stopped, Reason::UserStop);
  }
  void finish(double now, End e, Reason r) {
    if (pump)
      totalPump += now - pulseStarted;
    pump = false;
    switched = now;
    phase = Phase::Finished;
    outcome = e;
    reason = r;
  }
  void sample(double read, double beerC, bool valid, double now) {
    if (!active())
      return;
    if (!valid || !std::isfinite(beerC)) {
      finish(now, End::Failed, Reason::SensorFault);
      return;
    }
    latestC = beerC;
    lastSample = read;
    minimumC = std::min(minimumC, beerC);
    if (beerC <= minimumWaterC || initialC - beerC >= maximumDropC)
      finish(now, End::Stopped, Reason::TemperatureLimit);
  }
  uint32_t choosePulse() const {
    // Whole-installation response selects only a bounded excitation; this is
    // not a cooling controller and never assumes heat propagation equals cooling.
    if (pulse == 0)
      return std::max<uint32_t>(minimumOn, 10U);
    const double drop = beforePulseC - minimumC;
    uint32_t duration = drop >= .5 ? 5 : drop < .125 ? (pulse == 1 ? 30 : 60) : (pulse == 1 ? 20 : 40);
    return std::max(minimumOn, duration);
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
    if (stopping) {
      if (!pump || now - switched >= minimumOn)
        finish(now, End::Stopped, Reason::UserStop);
      return;
    }
    if (now < deadline)
      return;
    if (phase == Phase::Pulse) {
      totalPump += now - pulseStarted;
      pump = false;
      switched = now;
      phase = Phase::Observe;
      reason = Reason::Observation;
      deadline = now + observationSeconds;
    } else if (phase == Phase::Baseline || phase == Phase::Observe) {
      if (pulse == 3) {
        bool response = initialC - latestC >= .125;
        finish(now, response ? End::Completed : End::Inconclusive,
               response ? Reason::ProgramComplete : Reason::NoResponse);
        return;
      }
      if (now - switched < minimumOff)
        return;
      uint32_t duration = choosePulse();
      if (totalPump + duration > maximumPumpSeconds) {
        finish(now, End::Inconclusive, Reason::PumpBudget);
        return;
      }
      reason = pulse ? Reason::AdditionalPulse : Reason::Pilot;
      ++pulse;
      pump = true;
      switched = pulseStarted = now;
      phase = Phase::Pulse;
      deadline = now + duration;
      beforePulseC = minimumC = latestC;
    }
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
  uint8_t kind; // 0 boot, 1 clock, 2 sample, 3 output, 4 phase, 5 fault, 6 gap
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
