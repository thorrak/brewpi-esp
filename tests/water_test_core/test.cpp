#include "WaterTestCore.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace WaterTestCore;
static void feed(Program &p, double until, double c = 20) {
  for (double t = p.lastSample + 1; t <= until && p.active(); t += 1) {
    p.sample(t, c, true, t);
    p.tick(t);
  }
}
static Program pilot(uint32_t on = 2, uint32_t off = 2) {
  Program p;
  assert(p.start(0, 20, on, off));
  feed(p, 60);
  assert(!p.pump && p.role == Role::Calibration);
  p.tick(61);
  assert(p.pump && p.pulse == 1);
  return p;
}
static void adaptive() {
  Program p = pilot();
  assert(p.deadline == 71);
  feed(p, 71);
  assert(!p.pump && p.completedPulses == 1);
  feed(p, 370);
  assert(!p.pump && !p.responseSettled);
  feed(p, 371);
  assert(p.pump && p.pulse == 2 && p.deadline == 401);
  // A useful sample shortens a long pulse without shortening relay minimums.
  feed(p, 374, 19.75);
  assert(!p.pump && p.lastPulseSeconds == 2);
  feed(p, 650, 19.75);
  assert(p.role == Role::Calibration && p.contrastStarted);
  // All ended recordings, even a baseline-only stop, can be submitted.
  Program stop;
  stop.start(0, 20, 2, 2);
  stop.stop(1);
  assert(stop.submissionEligible());
  assert(stop.completedPulses == 0);
  // No response must finish as inconclusive; it must not be called settled.
  p = pilot();
  feed(p, 20000);
  assert(!p.active() && p.reason == Reason::NoResponse);
  assert(p.submissionEligible() && !p.responseSettled && p.completedPulses == 6);
  assert(p.totalPump == 3010 && p.lastSample < 6000);
  // Stable drift is a usable baseline, even when it isn't flat.
  p.start(0, 20, 2, 2);
  for (int t = 1; t <= 61; t++) {
    p.sample(t, 20 + t * .001, true, t);
    p.tick(t);
  }
  assert(p.pump && std::abs(p.baselineDrift - .001) < 1e-8);
  // A low-resolution constant baseline moves on after one minute.
  assert(p.started == 0 && p.pulseStarted == 61);
}
static void boundaries() {
  Program p = pilot();
  p.role = Role::Calibration;
  p.contrastStarted = true;
  p.phase = Phase::Observe;
  p.pump = false;
  p.switched = 0;
  p.deadline = 62;
  p.usefulResponse = true;
  p.usefulPulseSeconds = 20;
  p.sample(62, 19.5, true, 62);
  p.tick(62);
  assert(!p.pump && p.role == Role::Validation && p.nextPulsePending);
  p.tick(63);
  assert(p.pump && p.validationPulses == 1 && p.deadline == 73);
  feed(p, 73, 19.5);
  feed(p, 102, 19.5);
  assert(!p.pump);
  feed(p, 103, 19.5);
  assert(p.pump && p.validationPulses == 2);
  feed(p, 113, 19.5);
  assert(!p.pump);
  p.deadline = 114;
  feed(p, 114, 19.5);
  assert(p.phase == Phase::Observe); // relay minimum OFF
  feed(p, 115, 19.5);
  assert(p.phase == Phase::Controller && !p.pump);
  p.setControllerPump(116, true);
  assert(p.pump);
  p.setControllerPump(117, false);
  assert(p.pump);
  p.setControllerPump(118, false);
  assert(!p.pump && p.controllerCycles == 1);
  feed(p, 500, p.targetC);
  assert(!p.active() && !p.controllerTimedOut && p.responseSettled);
}
static void safety() {
  Program p = pilot(30);
  assert(p.deadline == 91);
  p.stop(61.1);
  feed(p, 90);
  p.tick(90.9);
  assert(p.pump);
  p.sample(91, 20, true, 91);
  p.tick(91);
  assert(p.outcome == End::Stopped && !p.pump && p.totalPump == 30);
  p = pilot();
  p.sample(62, NAN, false, 62);
  assert(p.lastSample == 60);
  p.tick(90);
  assert(p.active());
  p.tick(90.001);
  assert(!p.active() && p.reason == Reason::SensorStale && p.submissionEligible());
  p = pilot();
  p.sample(62, 16.9, true, 62);
  assert(!p.pump && p.reason == Reason::TemperatureLimit);
  p = pilot();
  p.sample(62, INFINITY, true, 62);
  assert(p.latestC == 20);
  p.sample(maximumSeconds, 20, true, maximumSeconds);
  p.tick(maximumSeconds);
  assert(!p.active() && p.reason == Reason::RuntimeLimit);
  for (auto r : {Reason::StorageFailure, Reason::QueueOverflow, Reason::UnexpectedOutput}) {
    p = pilot();
    p.finish(62, End::Failed, r);
    assert(!p.pump && p.submissionEligible());
  }
  p = pilot();
  p.totalPump = maximumPumpSeconds - 1;
  feed(p, 62);
  assert(!p.pump && p.reason == Reason::PumpBudget);
  assert(!p.start(0, 20, maximumPulseSeconds + 1, 2));
  assert(!p.start(0, 20, 2, observationSeconds + 1));
  assert(!p.start(0, NAN, 2, 2));
}
static void controllerLimitsAndTail() {
  Program p = pilot();
  p.phase = Phase::Controller;
  p.role = Role::Controller;
  p.controllerStarted = 61;
  p.deadline = 61 + controllerSeconds;
  feed(p, 61 + maximumPulseSeconds - 1);
  assert(p.pump && p.active());
  feed(p, 61 + maximumPulseSeconds);
  assert(!p.pump && p.reason == Reason::PulseLimit && p.totalPump == maximumPulseSeconds);
  // The same duration in the excitation stage is an ordinary completed pulse.
  p = pilot();
  p.deadline = 61 + maximumPulseSeconds;
  feed(p, 61 + maximumPulseSeconds);
  assert(p.phase == Phase::Observe && p.reason == Reason::Observation);
  assert(p.completedPulses == 1 && p.totalPump == maximumPulseSeconds);
  // A short flat plateau cannot end validation before an already observed tail.
  p.start(0, 20, 2, 2);
  p.phase = Phase::Observe;
  p.role = Role::Validation;
  p.validationPulses = 2;
  p.switched = 11;
  p.pulseStarted = 1;
  p.deadline = 3611;
  p.observedCoast = 600;
  p.usefulResponse = true;
  feed(p, 610, 19.7);
  assert(p.phase == Phase::Observe && !p.responseSettled);
  feed(p, 611, 19.7);
  assert(p.phase == Phase::Controller && p.responseSettled);
  // The controller uses the same known tail floor before claiming stable control.
  p.start(0, 20, 2, 2);
  p.phase = Phase::Controller;
  p.role = Role::Controller;
  p.controllerStarted = 1;
  p.deadline = controllerSeconds;
  p.controllerCycles = 1;
  p.targetC = 20;
  p.observedCoast = 600;
  feed(p, 599);
  assert(p.active());
  feed(p, 600);
  assert(p.outcome == End::Completed && p.responseSettled);
}
static void fastLoopStatisticalCadence() {
  Program p;
  p.start(0, 20, 2, 2);
  double previousDecision = -1;
  unsigned decisions = 0;
  for (unsigned ticks = 1; ticks <= 6000; ++ticks) {
    const double now = ticks * .01;
    if (ticks % 200 == 0) p.sample(now, 20, true, now);
    p.tick(now);
    if (p.lastDecision != previousDecision) {
      assert(previousDecision < 0 || p.lastDecision - previousDecision >= 1.);
      previousDecision = p.lastDecision;
      ++decisions;
    }
  }
  assert(decisions >= 59 && decisions <= 60);
  // Freshness overrides a statistics decision made only ten milliseconds ago.
  p.lastSample = 30;
  p.lastDecision = 60;
  p.tick(60.01);
  assert(p.reason == Reason::SensorStale && !p.pump);
  // Pulse OFF deadlines also bypass that throttling guard.
  p = pilot();
  p.lastDecision = p.deadline - .01;
  p.sample(p.deadline, 20, true, p.deadline);
  p.tick(p.deadline);
  assert(p.phase == Phase::Observe && !p.pump);
}
int main() {
  adaptive();
  boundaries();
  safety();
  controllerLimitsAndTail();
  fastLoopStatisticalCadence();
  Record r{};
  r.kind = 2;
  r.raw = 320;
  seal(r);
  assert(valid(r));
  r.raw++;
  assert(!valid(r));
  assert(std::strcmp(phaseName(Phase::Controller), "controller") == 0);
  puts("adaptive water-test core checks passed");
}
