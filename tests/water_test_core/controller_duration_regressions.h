#pragma once

#include "WaterTestCore.h"
#include <cassert>

// Current controller runs always keep their full duration, regardless of the
// number of natural responses or time spent near the target.
namespace WaterTestControllerDurationRegressions {
using namespace WaterTestCore;
inline void feed(Program &p, double until, double temperature) {
  for (double t = p.lastSample + 1; t <= until && p.active(); ++t) {
    p.sample(t, temperature, true, t);
    p.tick(t);
  }
}
inline void finishFinalObservation(Program &p) {
  assert(p.phase == Phase::ControllerFinalObserve && p.active() && !p.pump);
  p.confirmControllerFinalOff(p.lastSample);
  feed(p, p.controllerFinalObservationStarted + controllerFinalObservationSeconds, p.latestC);
  assert(!p.active() && p.controllerFinalObservationCompleted);
}
inline Program challenge() {
  Program p;
  assert(p.start(100, 20, 2, 2));
  p.sample(200, 20, true, 200);
  p.usefulResponse = p.responseSettled = true;
  p.observedCoast = 981;
  p.beginController(200);
  p.controllerRun = controllerRunCount; // Isolate the final run; sequencing below.
  assert(p.phase == Phase::Controller && !p.responseSettled);
  assert(p.deadline == 200 + controllerSeconds && p.targetC == 20 - 5.0 / 9.0);
  return p;
}
inline double requestedPulse(Program &p, double seconds = 2) {
  const double on = p.lastSample + 1;
  p.sample(on, p.targetC + .0625, true, on);
  p.setControllerPump(on, true);
  assert(p.pump);
  feed(p, on + seconds - 1, p.targetC);
  const double off = on + seconds;
  p.setControllerPump(off, false);
  p.sample(off, p.targetC, true, off);
  p.tick(off);
  assert(!p.pump);
  return off;
}
inline void holdingAndManyResponsesCannotShortenRun() {
  for (double temperature : {19.375, 19.4375, 19.5}) {
    Program p = challenge();
    const double deadline = p.deadline;
    for (unsigned cycle = 0; cycle < 5; ++cycle) {
      const double off = requestedPulse(p);
      feed(p, off + 450, temperature);
      assert(p.phase == Phase::Controller && p.active());
      assert(p.controllerCycles == cycle + 1);
      assert(!p.controllerRunDurationComplete && !p.responseSettled);
    }
    feed(p, deadline - 1, temperature);
    assert(p.phase == Phase::Controller && !p.controllerRunDurationComplete);
    feed(p, deadline, temperature);
    assert(p.phase == Phase::ControllerFinalObserve && p.controllerRunDurationComplete);
    assert(p.controllerRunEnded == deadline);
    assert(p.reason == Reason::ControllerDurationComplete);
    finishFinalObservation(p);
    assert(p.outcome == End::Completed && p.controllerRunEnded == deadline);
  }
}
inline void noCoolingDemandIsStillACompleteDuration() {
  Program p = challenge();
  feed(p, p.deadline, p.targetC);
  assert(p.controllerRunDurationComplete && p.controllerCycles == 0);
  assert(p.active() && p.phase == Phase::ControllerFinalObserve && !p.responseSettled);
  finishFinalObservation(p);
  assert(p.outcome == End::Completed && p.controllerCycles == 0);
}
inline void faultsAndSafetyCannotCompleteDuration() {
  for (const auto reason : {Reason::SensorStale, Reason::UserStop, Reason::PulseLimit,
                            Reason::PumpBudget, Reason::TemperatureLimit}) {
    Program p = challenge();
    p.setControllerPump(p.lastSample, true);
    if (reason == Reason::SensorStale)
      p.tick(p.lastSample + freshnessSeconds + .001);
    else if (reason == Reason::UserStop) {
      p.stop(p.lastSample + 1);
      assert(p.active() && p.pump); // Preserve relay minimum ON for user stop.
      p.sample(p.lastSample + 2, p.targetC, true, p.lastSample + 2);
      p.tick(p.lastSample);
    } else if (reason == Reason::PulseLimit)
      feed(p, p.lastSample + maximumPulseSeconds, p.targetC);
    else if (reason == Reason::PumpBudget) {
      p.totalPump = maximumPumpSeconds - 1;
      feed(p, p.lastSample + 1, p.targetC);
    } else
      p.sample(p.lastSample + 1, 20 - maximumDropC - .1, true, p.lastSample + 1);
    assert(!p.active() && !p.pump && p.reason == reason);
    assert(p.controllerRunEnded > p.controllerStarted && !p.controllerRunDurationComplete);
  }
}
inline void run() {
  holdingAndManyResponsesCannotShortenRun();
  noCoolingDemandIsStillACompleteDuration();
  faultsAndSafetyCannotCompleteDuration();
}
} // namespace WaterTestControllerDurationRegressions
