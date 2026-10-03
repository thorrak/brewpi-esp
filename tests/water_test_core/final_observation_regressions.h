#pragma once

#include "controller_duration_regressions.h"
#include <cassert>
#include <cstring>

namespace WaterTestFinalObservationRegressions {
using namespace WaterTestCore;
using WaterTestControllerDurationRegressions::challenge;
using WaterTestControllerDurationRegressions::feed;

inline Program durationEnded() {
  Program p = challenge();
  // Deliberately finish on a fractional deadline, less than one statistics
  // interval after the preceding decision. Output limits are never throttled.
  p.deadline = p.controllerRunDeadline = 202.25;
  p.setControllerPump(200, true);
  p.sample(202, p.targetC, true, 202);
  p.tick(202);
  p.tick(202.25);
  assert(p.active() && !p.pump && p.phase == Phase::ControllerFinalObserve);
  assert(p.controllerRunDurationComplete && p.controllerRunEnded == 202.25);
  assert(p.totalPump == 2.25 && p.controllerCycles == 1);
  assert(!p.controllerFinalOffConfirmed && p.controllerFinalObservationSamples == 0);
  return p;
}

inline void observationStartsAfterPhysicalOffAndKeepsRunFrozen() {
  Program p = durationEnded();
  const double ended = p.controllerRunEnded, target = p.targetC;
  p.sample(203, target, true, 203); // Before the physical relay acknowledgement.
  p.confirmControllerFinalOff(204);
  p.confirmControllerFinalOff(205); // A repeat cannot extend the observation.
  assert(p.controllerFinalObservationStarted == 204 && p.controllerFinalObservationSamples == 0);
  p.setControllerPump(205, true);
  assert(!p.pump);
  p.sample(204, target, true, 205); // Acquired at the OFF boundary, not after it.
  p.sample(205, target, false, 205);
  p.sample(206, NAN, true, 206);
  p.sample(208, target, true, 207); // A future timestamp is not a fresh sample.
  assert(p.controllerFinalObservationSamples == 0);
  p.sample(207, target, true, 207);
  p.sample(207, target, true, 208); // Duplicate acquisition.
  assert(p.controllerFinalObservationSamples == 1);
  feed(p, 293, target);
  assert(p.active() && !p.controllerFinalObservationCompleted);
  p.tick(294); // Wall-clock time alone cannot substitute for the final read.
  assert(p.active());
  p.sample(294, target, true, 294);
  p.tick(294);
  assert(!p.active() && p.outcome == End::Completed && p.controllerFinalObservationCompleted);
  assert(p.controllerFinalObservationEnded == 294 && p.controllerFinalObservationLastRead == 294);
  assert(p.controllerRunEnded == ended && p.controllerRunDeadline == ended && p.controllerRunDurationComplete);
  assert(p.controllerCycles == 1 && !p.responseSettled);
  assert(p.totalPump == 2.25 && p.targetC == target);
}

inline void sampleCountAndAcknowledgementAreRequired() {
  Program p = durationEnded();
  p.confirmControllerFinalOff(203);
  // Acquisition every 15 seconds meets freshness at the boundary while also
  // exercising exactly six distinct reads over the required observation.
  for (unsigned i = 1; i <= 6; ++i) {
    const double now = 203 + 15 * i;
    p.sample(now, p.targetC, true, now);
    p.tick(now);
    assert(p.controllerFinalObservationSamples == i);
    assert(p.active() == (i < 6));
  }
  assert(p.controllerFinalObservationCompleted);

  p = durationEnded();
  const double cap = p.deadline;
  feed(p, cap - .25, p.targetC);
  p.sample(cap, p.targetC, true, cap);
  p.tick(cap);
  assert(!p.active() && p.reason == Reason::ControllerFinalObservationTimeout);
  assert(p.outcome == End::Inconclusive && !p.controllerFinalObservationCompleted);
  assert(p.controllerFinalObservationSamples == 0 && !p.controllerFinalOffConfirmed);
}

inline void stopAndSafetyStillEndImmediately() {
  for (const auto reason : {Reason::SensorStale, Reason::UserStop, Reason::StorageFailure,
                            Reason::TemperatureLimit, Reason::RuntimeLimit}) {
    Program p = durationEnded();
    p.confirmControllerFinalOff(203);
    const double ended = p.controllerRunEnded;
    double now = 204;
    if (reason == Reason::SensorStale) {
      now = p.lastSample + freshnessSeconds + .001;
      p.tick(now);
    } else if (reason == Reason::UserStop) {
      p.stop(now);
    } else if (reason == Reason::TemperatureLimit) {
      p.sample(now, 20 - maximumDropC - .01, true, now);
    } else if (reason == Reason::RuntimeLimit) {
      now = p.started + maximumSeconds;
      p.sample(now, p.targetC, true, now);
      p.tick(now);
    } else {
      p.finish(now, End::Failed, reason);
    }
    assert(!p.active() && !p.pump && p.reason == reason);
    assert(!p.controllerFinalObservationCompleted && p.controllerFinalObservationEnded == now);
    assert(p.controllerRunEnded == ended && p.controllerRunDurationComplete && p.controllerCycles == 1);
  }
}

inline void run() {
  static_assert(static_cast<unsigned>(Phase::ControllerFinalObserve) == 8);
  assert(std::strcmp(phaseName(Phase::ControllerFinalObserve), "controller_final_observe") == 0);
  assert(std::strcmp(reasonName(Reason::ControllerFinalObservationTimeout),
                     "controller_final_observation_timeout") == 0);
  observationStartsAfterPhysicalOffAndKeepsRunFrozen();
  sampleCountAndAcknowledgementAreRequired();
  stopAndSafetyStillEndImmediately();
}
} // namespace WaterTestFinalObservationRegressions
