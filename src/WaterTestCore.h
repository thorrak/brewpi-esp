#pragma once

#include "OneWireSensorPolicy.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <utility>

// Platform-independent sequencing and journal primitives. Time is monotonic seconds;
// adaptive decisions use fresh raw readings, and no network work runs here.
namespace WaterTestCore {
constexpr uint32_t baselineSeconds = 300; // upper bound, not a required wait
constexpr uint32_t minimumBaselineSeconds = 180;
constexpr uint32_t baselineWindowSeconds = 180;
constexpr uint32_t driftCorrectionSeconds = 300;
constexpr uint32_t observationSeconds = 21600;
constexpr uint32_t maximumSeconds = 43200;
constexpr uint32_t maximumPulseSeconds = 1800;
constexpr uint32_t maximumPumpSeconds = 7200;
constexpr uint32_t controllerSeconds = 10800;
constexpr unsigned controllerRunCount = 2;
constexpr uint32_t controllerTransitionSeconds = 1800;
constexpr uint32_t controllerFinalObservationSeconds = 90;
constexpr uint32_t controllerFinalObservationMaxSeconds = 180;
constexpr unsigned controllerFinalObservationRequiredSamples = 6;
constexpr unsigned controllerObservationGoal = 3;
constexpr double controllerTargetDropC = 5.0 / 9.0; // A temperature difference of 1 degree F.
constexpr double controllerHeadroomMarginC = .25;
constexpr unsigned historyCapacity = 2048;
constexpr double coolingTailRateTolerance = .000005;
constexpr double freshnessSeconds = OneWireSensorPolicy::connectedTimeoutUs / 1e6;
constexpr double diagnosticMaximumDropC = 3;
constexpr double maximumDropC = diagnosticMaximumDropC + controllerTargetDropC;
constexpr double minimumWaterC = 4;
// Append values: existing journal enum IDs must remain readable.
enum class Phase : uint8_t {
  Idle, Baseline, Pulse, Observe, Stopping, Finished, Controller, ControllerTransition, ControllerFinalObserve
};
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
  PulseLimit,
  ObservationTimeout,
  ControllerTransitionTimeout,
  ControllerHeadroom,
  ControllerFinalObservationTimeout,
  ControllerDurationComplete
};
enum class Role : uint8_t { Baseline, Calibration, Validation, Controller, Complete };
inline const char *phaseName(Phase p) {
  const char *names[] = {"idle", "baseline", "pulse", "observe", "stopping", "finished", "controller",
                         "controller_transition", "controller_final_observe"};
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
                         "pulse_time_limit",
                         "observation_timeout",
                         "controller_transition_timeout",
                         "controller_headroom",
                         "controller_final_observation_timeout",
                         "duration_complete"};
  return names[static_cast<unsigned>(r)];
}
struct Program {
  Program() = default;
  Program(const Program &) = delete;
  Program &operator=(const Program &) = delete;
  Program(Program &&) noexcept = default;
  Program &operator=(Program &&) noexcept = default;
  Phase phase = Phase::Idle;
  Role role = Role::Baseline;
  End outcome = End::None;
  Reason reason = Reason::None;
  double started = 0, deadline = 0, switched = 0, pulseStarted = 0;
  double initialC = 0, latestC = 0, lastSample = 0, beforePulseC = 0, minimumC = 0;
  double totalPump = 0, noiseC = .03125, baselineDrift = 0, baselineDriftUncertainty = 0;
  double lastPulseSeconds = 0, usefulPulseSeconds = 0, peakResponse = 0;
  double responseOnset = -1, observedDelay = 0, usefulResponseDrop = 0, observedCoast = 0;
  double targetC = 0, controllerStarted = 0, lastDecision = -1;
  double controllerRunStartC = 0, controllerRunDeadline = 0, controllerRunEnded = 0, controllerRunPumpStart = 0;
  double controllerFinalObservationStarted = 0, controllerFinalObservationEnded = 0,
         controllerFinalObservationLastRead = 0;
  uint32_t minimumOn = 2, minimumOff = 2;
  unsigned pulse = 0, completedPulses = 0, block = 0, escalation = 0;
  unsigned calibrationPulses = 0, validationPulses = 0, controllerCycles = 0;
  unsigned controllerRun = 0;
  unsigned controllerFinalObservationSamples = 0;
  bool pump = false, stopping = false, contrastStarted = false, responseSettled = false;
  bool usefulResponse = false, nextPulsePending = false;
  bool controllerTransitionReady = false, controllerRunDurationComplete = false;
  bool controllerFinalOffConfirmed = false, controllerFinalObservationCompleted = false;
  struct Reading {
    float elapsed = 0, c = 0;
  };
  struct History {
    Reading readings[historyCapacity]{};
  };
  struct HistoryDeleter {
    void operator()(History *history) const {
      if (history) {
        history->~History();
        std::free(history);
      }
    }
  };
  std::unique_ptr<History, HistoryDeleter> history;
  unsigned head = 0, count = 0;
  bool hasHistory() const { return bool(history); }
  bool allocateHistory() {
    if (history)
      return true;
    void *storage = std::malloc(sizeof(History));
    if (!storage)
      return false;
    history.reset(new (storage) History);
    head = count = 0;
    return true;
  }
  void releaseHistory() {
    history.reset();
    head = count = 0;
  }
  struct Stats {
    unsigned n = 0;
    double span = 0, mean = 0, slope = 0, noise = 0, slopeUncertainty = 0;
  };
  Stats stats(double begin, double end) const {
    Stats r;
    if (!history)
      return r;
    double sx = 0, sy = 0, sxx = 0, sxy = 0, syy = 0, first = end, last = begin;
    for (unsigned i = 0; i < count; ++i) {
      const auto &v = history->readings[i];
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
    // A trend smaller than one probe step across the window is not resolved,
    // even when repeated identical readings make the fitted noise very small.
    if (xx > 0 && r.span > 0)
      r.slopeUncertainty = std::max(3 * r.noise / std::sqrt(xx), .0625 / r.span);
    return r;
  }
  void remember(double read, double c) {
    if (!history)
      return;
    auto &readings = history->readings;
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
  bool responseEstimateCurrent(double read) const {
    return baselineDrift + baselineDriftUncertainty >= -coolingTailRateTolerance ||
           read - pulseStarted <= driftCorrectionSeconds;
  }
  bool start(double now, double beerC, uint32_t on, uint32_t off) {
    on = std::max<uint32_t>(2U, on);
    off = std::max<uint32_t>(2U, off);
    if (!std::isfinite(beerC) || on > maximumPulseSeconds || off > observationSeconds || !allocateHistory())
      return false;
    auto retainedHistory = std::move(history);
    *this = Program{};
    history = std::move(retainedHistory);
    minimumOn = on;
    minimumOff = off;
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
    if ((phase == Phase::Controller || phase == Phase::Stopping) && controllerRun && !controllerRunEnded)
      controllerRunEnded = now;
    if (controllerFinalObservationStarted && !controllerFinalObservationEnded)
      controllerFinalObservationEnded = now;
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
    if (read > lastSample) {
      remember(read, beerC);
      if (phase == Phase::ControllerFinalObserve && controllerFinalOffConfirmed && !pump &&
          read > controllerFinalObservationStarted) {
        ++controllerFinalObservationSamples;
        controllerFinalObservationLastRead = read;
      }
    }
    latestC = beerC;
    lastSample = read;
    minimumC = std::min(minimumC, beerC);
    if ((role == Role::Calibration || role == Role::Validation) && pulse && !nextPulsePending) {
      const double elapsed = read - pulseStarted;
      const bool backgroundCooling = baselineDrift + baselineDriftUncertainty < -coolingTailRateTolerance;
      // Only measured temperature drops earn response credit. Discount a
      // resolved background cooling trend while its estimate is recent; after
      // it expires, further cooling cannot be attributed to this pulse.
      if (responseEstimateCurrent(read)) {
        const double correction = backgroundCooling ? (baselineDrift - baselineDriftUncertainty) * elapsed : 0;
        peakResponse = std::max(peakResponse, beforePulseC - beerC + correction);
      }
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
    const double window = baselineWindowSeconds;
    const auto whole = stats(now - window, now), a = stats(now - window, now - window / 2),
               b = stats(now - window / 2, now);
    if (whole.n < 60 || whole.span < window - 5)
      return false;
    const bool consistent = a.n >= 20 && b.n >= 20 && a.span >= window / 2 - 5 && b.span >= window / 2 - 5 &&
                            std::abs(a.slope - b.slope) <= a.slopeUncertainty + b.slopeUncertainty && whole.noise <= .10;
    if (!consistent && now < deadline)
      return false;
    baselineDrift = whole.slope;
    baselineDriftUncertainty = whole.slopeUncertainty;
    noiseC = std::max(.03125, whole.noise);
    return true;
  }
  void refreshBaseline(double now) {
    const auto recent = stats(now - baselineWindowSeconds, now);
    if (recent.n < 60 || recent.span < baselineWindowSeconds - 5)
      return;
    baselineDrift = recent.slope;
    baselineDriftUncertainty = recent.slopeUncertainty;
    noiseC = std::max(.03125, recent.noise);
  }
  bool settled(double now) const {
    // A flat trace before any detected response never proves that cooling is finished.
    const double window = std::max(90., std::min(1800., observedDelay));
    if (now - switched < std::max({2 * window, 2 * observedDelay, observedCoast, double(minimumOff)}) ||
        responseOnset < 0)
      return false;
    return recoveredWindows(now, window);
  }
  bool recoveredWindows(double now, double window) const {
    const auto a = stats(now - 2 * window, now - window), b = stats(now - window, now),
               whole = stats(now - 2 * window, now);
    if (a.n < 20 || b.n < 20 || a.span < window - 5 || b.span < window - 5)
      return false;
    // Check the combined trend too: a downward probe step between two flat
    // windows is still cooling. Steady warming is a valid observed recovery.
    const auto recovered = [](double rate) { return rate >= -coolingTailRateTolerance; };
    const double tolerance = std::max(.0625, 2 * noiseC);
    const double warming = std::max(0., (a.slope + b.slope) / 2);
    return recovered(a.slope) && recovered(b.slope) && recovered(whole.slope) &&
           std::abs(a.slope - b.slope) * window <= tolerance &&
           std::abs(b.mean - a.mean - warming * window) <= tolerance;
  }
  uint32_t choosePulse() const {
    static const uint32_t candidates[] = {10, 30, 90, 270, 810, 1800};
    double selected = candidates[std::min(escalation, 5U)];
    if (role == Role::Validation || contrastStarted) {
      // The second controller's extra reserve must not increase excitation.
      const double remaining = std::max(0., latestC - std::max(minimumWaterC, initialC - diagnosticMaximumDropC));
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
    } else if (now - switched >= minimumOn) {
      endPulse(now);
      ++controllerCycles;
    }
  }
  bool controllerHasHeadroom(unsigned remainingRuns) const {
    return latestC - std::max(minimumWaterC, initialC - maximumDropC) >=
           remainingRuns * (controllerTargetDropC + controllerHeadroomMarginC);
  }
  void startControllerRun(double now, unsigned run) {
    // Called only at a guarded entry or after the wrapper has saved the prior
    // run. History, diagnostic results, relay timings and pump budget survive.
    controllerRun = run;
    controllerRunStartC = latestC;
    controllerRunEnded = 0;
    controllerRunPumpStart = totalPump;
    controllerRunDeadline = now + controllerSeconds;
    controllerTransitionReady = false;
    controllerRunDurationComplete = false;
    controllerCycles = 0;
    reason = Reason::Observation;
    role = Role::Controller;
    phase = Phase::Controller;
    ++block;
    controllerStarted = now;
    deadline = controllerRunDeadline;
    targetC = latestC - controllerTargetDropC;
    responseSettled = false;
    // Production observation telemetry belongs to the wrapper, not the sequencer.
  }
  void beginController(double now) {
    if (!usefulResponse) {
      finish(now, End::Inconclusive, Reason::NoResponse);
      return;
    }
    // Keep useful first-run evidence when only one full challenge still fits.
    // The additional reserve is protected by the diagnostic excitation budget;
    // actual remaining headroom is checked again before the second run.
    if (!controllerHasHeadroom(1)) {
      finish(now, End::Inconclusive, Reason::ControllerHeadroom);
      return;
    }
    startControllerRun(now, 1);
  }
  void endControllerRun(double now) {
    if (pump)
      ++controllerCycles; // The duration deadline can close an actual ON/OFF cycle.
    endPulse(now);
    controllerRunEnded = now;
    controllerRunDurationComplete = now >= controllerRunDeadline;
    if (controllerRun >= controllerRunCount) {
      // End the algorithm now. Its immutable result is saved before this
      // separate pump-OFF recording tail; it never adds controller observations.
      phase = Phase::ControllerFinalObserve;
      reason = controllerRunDurationComplete ? Reason::ControllerDurationComplete : Reason::Observation;
      controllerFinalObservationStarted = now;
      controllerFinalObservationEnded = controllerFinalObservationLastRead = 0;
      controllerFinalObservationSamples = 0;
      controllerFinalOffConfirmed = controllerFinalObservationCompleted = false;
      deadline = now + controllerFinalObservationMaxSeconds;
      return;
    }
    phase = Phase::ControllerTransition;
    reason = controllerRunDurationComplete ? Reason::ControllerDurationComplete : Reason::Observation;
    deadline = now + controllerTransitionSeconds;
    controllerTransitionReady = false;
  }
  void confirmControllerFinalOff(double now) {
    if (phase != Phase::ControllerFinalObserve || pump || controllerFinalOffConfirmed)
      return;
    // The hardware wrapper acknowledges the actual OFF application before any
    // durable writes. Queued acquisitions from before it cannot qualify.
    controllerFinalObservationStarted = std::max(controllerRunEnded, now);
    deadline = controllerFinalObservationStarted + controllerFinalObservationMaxSeconds;
    controllerFinalOffConfirmed = true;
  }
  bool controllerTransitionSettled(double now) const {
    const double window = std::max(90., std::min(1800., observedDelay));
    if (pump || now - controllerRunEnded < 2 * window ||
        now - switched < std::max({180., observedCoast, 2 * observedDelay, double(minimumOff)}))
      return false;
    return recoveredWindows(now, window);
  }
  bool advanceController(double now) {
    if (phase != Phase::ControllerTransition)
      return false;
    // The wrapper calls this only after durably checkpointing the ended run.
    // Recheck fresh evidence and all global safety guards after that write.
    tick(now);
    if (phase != Phase::ControllerTransition)
      return false;
    controllerTransitionReady = controllerTransitionSettled(now);
    if (!controllerTransitionReady)
      return false;
    if (!controllerHasHeadroom(controllerRunCount - controllerRun)) {
      finish(now, End::Inconclusive, Reason::ControllerHeadroom);
      return false;
    }
    startControllerRun(now, controllerRun + 1);
    return true;
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
    if (phase == Phase::Controller && now >= deadline) {
      // Controller time limits are output deadlines, not throttled statistics.
      endControllerRun(now);
      return;
    }
    if (phase == Phase::ControllerFinalObserve) {
      if (controllerFinalOffConfirmed && !pump &&
          now - controllerFinalObservationStarted >= controllerFinalObservationSeconds &&
          controllerFinalObservationLastRead - controllerFinalObservationStarted >= controllerFinalObservationSeconds &&
          controllerFinalObservationSamples >= controllerFinalObservationRequiredSamples) {
        controllerFinalObservationCompleted = true;
        finish(now, End::Completed, Reason::ProgramComplete);
      } else if (now >= deadline) {
        finish(now, End::Inconclusive, Reason::ControllerFinalObservationTimeout);
      }
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
    if (phase == Phase::ControllerTransition) {
      controllerTransitionReady = controllerTransitionSettled(now);
      if (!controllerTransitionReady && now >= deadline)
        finish(now, End::Inconclusive, Reason::ControllerTransitionTimeout);
      return;
    }
    // Controller phases are fixed-duration experiments. Temperature quality and
    // completed production observations are reported independently; neither can
    // shorten a run or impose a diagnostic cooling-tail gate on normal control.
    if (phase == Phase::Controller)
      return;
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
    const bool weakPilot = role == Role::Calibration && !contrastStarted && peakResponse < usefulThreshold();
    const auto recent = stats(now - 120, now);
    const bool coolingContinues = beforePulseC - latestC >= std::max(.0625, 2 * noiseC) && recent.span >= 110 &&
                                  recent.slope < -coolingTailRateTolerance;
    if (responseOnset < 0 && responseEstimateCurrent(lastSample) && coolingContinues &&
        peakResponse >= std::max(.0625, 2 * noiseC)) {
      responseOnset = std::max(0., lastSample - pulseStarted);
      observedDelay = std::max(observedDelay, responseOnset);
    }
    const bool coolingArrived = responseOnset >= 0 || coolingContinues;
    responseSettled = settled(now);
    const bool pilotWindow = weakPilot && !coolingArrived && now - switched >= std::max<uint32_t>(300U, minimumOff);
    if (!responseSettled && now >= deadline) {
      finish(now, End::Inconclusive, Reason::ObservationTimeout);
      return;
    }
    if (!responseSettled && !pilotWindow)
      return;
    if (responseSettled)
      observedCoast = std::max(observedCoast, now - switched);
    if (role == Role::Validation) {
      beginController(now);
      return;
    }
    if (peakResponse >= usefulThreshold()) {
      usefulResponse = true;
      if (!usefulPulseSeconds) {
        usefulPulseSeconds = lastPulseSeconds;
        usefulResponseDrop = peakResponse;
      }
    }
    if (contrastStarted) {
      if (responseSettled)
        refreshBaseline(now);
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
    if (responseSettled)
      refreshBaseline(now);
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
  uint8_t kind; // 0 boot, 1 clock, 2 sample, 3 output, 4 phase, 5 fault, 6 gap, 7 controller, 8 controller_episode (legacy), 9 controller_run, 10 controller_observation
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
