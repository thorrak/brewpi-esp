#include "WaterTestCore.h"
#include "recorded_trace.h"
#include "response_regressions.h"
#include "controller_duration_regressions.h"
#include "dual_controller_regressions.h"
#include "final_observation_regressions.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <type_traits>
using namespace WaterTestCore;
static_assert(!std::is_copy_constructible_v<Program> && !std::is_copy_assignable_v<Program>);
static_assert(std::is_nothrow_move_constructible_v<Program> && std::is_nothrow_move_assignable_v<Program>);
static_assert(sizeof(Program) < 1024);
static void historyLifetime() {
  Program p;
  assert(!p.hasHistory() && p.count == 0 && p.head == 0);
  p.remember(0, 20);
  assert(p.stats(0, 20).n == 0 && p.count == 0);
  assert(!p.start(0, NAN, 2, 2) && !p.hasHistory());
  assert(!p.start(0, 20, maximumPulseSeconds + 1, 2) && !p.hasHistory());
  assert(!p.start(0, 20, 2, observationSeconds + 1) && !p.hasHistory());
  assert(p.allocateHistory());
  const auto *history = p.history.get();
  assert(p.hasHistory() && p.phase == Phase::Idle);
  assert(p.start(100, 20, 2, 2) && p.history.get() == history);
  for (unsigned i = 1; i < 10; ++i)
    p.remember(100 + 2 * i, 20 + .25 * i);
  assert(p.allocateHistory() && p.history.get() == history && p.count == 10);
  auto stats = p.stats(100, 118);
  assert(stats.n == 10 && stats.span == 18);
  assert(stats.mean == 21.125 && stats.slope == .125 && stats.noise == 0);
  p.completedPulses = 2;
  p.usefulResponse = true;
  p.observedDelay = 15;
  p.controllerRun = 2;
  p.controllerRunStartC = 19;
  p.controllerRunEnded = 118;
  p.controllerTransitionReady = true;
  p.controllerRunDurationComplete = true;
  p.controllerFinalObservationStarted = 116;
  p.controllerFinalObservationEnded = p.controllerFinalObservationLastRead = 118;
  p.controllerFinalObservationSamples = 6;
  p.controllerFinalOffConfirmed = p.controllerFinalObservationCompleted = true;
  p.finish(118, End::Completed, Reason::ProgramComplete);
  assert(p.hasHistory());
  assert(p.start(200, 19, 3, 4) && p.history.get() == history);
  assert(p.count == 1 && p.head == 1 && p.started == 200 && p.initialC == 19);
  assert(p.completedPulses == 0 && !p.usefulResponse && p.observedDelay == 0);
  assert(p.controllerRun == 0 && p.controllerRunStartC == 0 && p.controllerRunEnded == 0);
  assert(!p.controllerTransitionReady && !p.controllerRunDurationComplete);
  assert(p.controllerFinalObservationStarted == 0 && p.controllerFinalObservationEnded == 0);
  assert(p.controllerFinalObservationLastRead == 0 && p.controllerFinalObservationSamples == 0);
  assert(!p.controllerFinalOffConfirmed && !p.controllerFinalObservationCompleted);
  assert(p.outcome == End::None && p.minimumOn == 3 && p.minimumOff == 4);
  // The ring retains the most recent readings and the same linear statistics.
  for (unsigned i = 1; i < historyCapacity + 5; ++i)
    p.remember(200 + 2 * i, 19 + .25 * i);
  stats = p.stats(200, 200 + 2 * (historyCapacity + 4));
  assert(p.count == historyCapacity && p.head == 5 && stats.n == historyCapacity);
  assert(stats.span == 2 * (historyCapacity - 1));
  assert(stats.mean == 19 + .25 * (5 + historyCapacity + 4) / 2);
  assert(stats.slope == .125 && stats.noise == 0);
  p.completedPulses = 3;
  p.totalPump = 42;
  p.observedDelay = 12;
  p.finish(5000, End::Completed, Reason::ProgramComplete);
  p.releaseHistory();
  assert(!p.hasHistory() && p.count == 0 && p.head == 0 && p.stats(200, 5000).n == 0);
  assert(p.phase == Phase::Finished && p.role == Role::Complete && p.submissionEligible());
  assert(p.outcome == End::Completed && p.reason == Reason::ProgramComplete);
  assert(p.completedPulses == 3 && p.totalPump == 42 && p.observedDelay == 12);
  assert(p.started == 200 && p.switched == 5000 && p.initialC == 19);
  p.releaseHistory();
  assert(p.start(6000, 21, 2, 2) && p.hasHistory());
  history = p.history.get();
  Program moved(std::move(p));
  assert(!p.hasHistory() && p.stats(6000, 6000).n == 0);
  assert(moved.history.get() == history && moved.count == 1 && moved.initialC == 21);
  assert(p.start(7000, 22, 2, 2));
  p = std::move(moved);
  assert(!moved.hasHistory() && moved.stats(6000, 6000).n == 0);
  assert(p.history.get() == history && p.count == 1 && p.initialC == 21);
}
static void feed(Program &p, double until, double c = 20) {
  for (double t = p.lastSample + 1; t <= until && p.active(); t += 1) {
    p.sample(t, c, true, t);
    p.tick(t);
  }
}
static Program pilot(uint32_t on = 2, uint32_t off = 2) {
  Program p;
  assert(p.start(0, 20, on, off));
  feed(p, minimumBaselineSeconds);
  assert(!p.pump && p.role == Role::Calibration);
  p.tick(minimumBaselineSeconds + 1);
  assert(p.pump && p.pulse == 1);
  return p;
}
static void adaptive() {
  Program p = pilot();
  const double firstOff = p.pulseStarted + 10;
  assert(p.deadline == firstOff);
  feed(p, firstOff);
  assert(!p.pump && p.completedPulses == 1);
  feed(p, firstOff + 299);
  assert(!p.pump && !p.responseSettled);
  feed(p, firstOff + 300);
  const double secondOn = p.pulseStarted;
  assert(p.pump && p.pulse == 2 && p.deadline == secondOn + 30);
  // A useful sample shortens a long pulse without shortening relay minimums.
  feed(p, secondOn + 3, 19.75);
  assert(!p.pump && p.lastPulseSeconds == 2);
  feed(p, secondOn + 280, 19.75);
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
  for (unsigned t = 1; t <= minimumBaselineSeconds + 1; t++) {
    p.sample(t, 20 + t * .001, true, t);
    p.tick(t);
  }
  assert(p.pump && std::abs(p.baselineDrift - .001) < 1e-8);
  assert(p.started == 0 && p.pulseStarted == minimumBaselineSeconds + 1);
}
static void boundaries() {
  Program p = pilot();
  feed(p, p.pulseStarted + 2, 19.5);
  assert(!p.pump && p.completedPulses == 1);
  const double pilotOff = p.switched;
  while (p.active() && !p.contrastStarted && p.lastSample < pilotOff + 1000)
    feed(p, p.lastSample + 1, 19.5);
  assert(p.pump && p.contrastStarted && p.usefulResponse);
  feed(p, p.deadline, 19.25);
  assert(!p.pump && p.completedPulses == 2);
  const double contrastOff = p.switched;
  while (p.active() && p.role == Role::Calibration && p.lastSample < contrastOff + 1000)
    feed(p, p.lastSample + 1, 19.25);
  assert(!p.pump && p.role == Role::Validation && p.nextPulsePending);
  assert(p.responseSettled);
  p.tick(p.lastSample + 1);
  assert(p.pump && p.validationPulses == 1 && p.deadline == p.pulseStarted + 5);
  feed(p, p.deadline, 19);
  const double firstValidationOff = p.switched;
  feed(p, firstValidationOff + 29, 19);
  assert(!p.pump);
  feed(p, firstValidationOff + 30, 19);
  assert(p.pump && p.validationPulses == 2);
  feed(p, p.deadline, 18.75);
  assert(!p.pump);
  const double secondValidationOff = p.switched;
  feed(p, secondValidationOff + 1, 18.75);
  assert(p.phase == Phase::Observe); // relay minimum OFF
  while (p.active() && p.phase == Phase::Observe && p.lastSample < secondValidationOff + 1000)
    feed(p, p.lastSample + 1, 18.75);
  assert(p.phase == Phase::Controller && !p.responseSettled && !p.pump);
  const double controllerStart = p.lastSample;
  assert(p.targetC == 18.75 - 5.0 / 9.0);
  assert(p.deadline == controllerStart + 10800);
  p.setControllerPump(controllerStart + 1, true);
  assert(p.pump);
  p.setControllerPump(controllerStart + 2, false);
  assert(p.pump);
  p.setControllerPump(controllerStart + 3, false);
  assert(!p.pump && p.controllerCycles == 1);
  p.sample(controllerStart + 3, p.targetC, true, controllerStart + 3);
  p.tick(controllerStart + 3);
  feed(p, controllerStart + 300, p.targetC);
  assert(p.active() && !p.controllerRunDurationComplete && !p.responseSettled);
}
static void controllerTargetHeadroom() {
  // Both independent temperature limits must leave room for the full 1 F
  // challenge plus the existing 0.25 C coast/overshoot margin.
  for (double initial : {6., 20.}) {
    const double floor = std::max(minimumWaterC, initial - maximumDropC);
    for (double remaining : {.5, .75, .8125}) {
      Program p;
      assert(p.start(100, initial, 2, 2));
      p.sample(200, floor + remaining, true, 200);
      p.usefulResponse = true;
      p.beginController(200);
      if (remaining < .8) {
        assert(!p.active() && p.controllerStarted == 0 && !p.pump);
        assert(p.reason == Reason::ControllerHeadroom);
      } else {
        assert(p.phase == Phase::Controller && p.targetC == floor + remaining - 5.0 / 9.0);
        assert(p.targetC - floor >= .25);
        // Exercise final-slot timeout here; dual sequencing has separate coverage.
        p.controllerRun = controllerRunCount;
        const double current = p.latestC;
        feed(p, p.deadline - 1, current);
        assert(p.active());
        feed(p, p.deadline, current);
        WaterTestControllerDurationRegressions::finishFinalObservation(p);
        assert(!p.active() && p.controllerRunDurationComplete && !p.responseSettled);
      }
    }
  }
}
static void safety() {
  Program p = pilot(30);
  const double minimumPulseEnd = p.pulseStarted + 30;
  assert(p.deadline == minimumPulseEnd);
  p.stop(p.pulseStarted + .1);
  feed(p, minimumPulseEnd - 1);
  p.tick(minimumPulseEnd - .1);
  assert(p.pump);
  p.sample(minimumPulseEnd, 20, true, minimumPulseEnd);
  p.tick(minimumPulseEnd);
  assert(p.outcome == End::Stopped && !p.pump && p.totalPump == 30);
  p = pilot();
  p.sample(p.pulseStarted + 1, NAN, false, p.pulseStarted + 1);
  assert(p.lastSample == minimumBaselineSeconds);
  const double staleAfter = p.lastSample + freshnessSeconds;
  p.tick(staleAfter);
  assert(p.active());
  p.tick(staleAfter + .001);
  assert(!p.active() && p.reason == Reason::SensorStale && p.submissionEligible());
  p = pilot();
  p.sample(p.pulseStarted + 1, 20 - maximumDropC - .1, true, p.pulseStarted + 1);
  assert(!p.pump && p.reason == Reason::TemperatureLimit);
  p = pilot();
  p.sample(p.pulseStarted + 1, INFINITY, true, p.pulseStarted + 1);
  assert(p.latestC == 20);
  p.sample(maximumSeconds, 20, true, maximumSeconds);
  p.tick(maximumSeconds);
  assert(!p.active() && p.reason == Reason::RuntimeLimit);
  for (auto r : {Reason::StorageFailure, Reason::QueueOverflow, Reason::UnexpectedOutput}) {
    p = pilot();
    p.finish(p.pulseStarted + 1, End::Failed, r);
    assert(!p.pump && p.submissionEligible());
  }
  p = pilot();
  p.totalPump = maximumPumpSeconds - 1;
  feed(p, p.pulseStarted + 1);
  assert(!p.pump && p.reason == Reason::PumpBudget);
  assert(!p.start(0, 20, maximumPulseSeconds + 1, 2));
  assert(!p.start(0, 20, 2, observationSeconds + 1));
  assert(!p.start(0, NAN, 2, 2));
}
static void controllerLimitsAndTail() {
  Program p = pilot();
  p.phase = Phase::Controller;
  p.role = Role::Controller;
  p.controllerStarted = p.pulseStarted;
  p.deadline = p.pulseStarted + controllerSeconds;
  const double maximumOff = p.pulseStarted + maximumPulseSeconds;
  feed(p, maximumOff - 1);
  assert(p.pump && p.active());
  feed(p, maximumOff);
  assert(!p.pump && p.reason == Reason::PulseLimit && p.totalPump == maximumPulseSeconds);
  // The same duration in the excitation stage is an ordinary completed pulse.
  p = pilot();
  p.deadline = p.pulseStarted + maximumPulseSeconds;
  feed(p, p.deadline);
  assert(p.phase == Phase::Observe && p.reason == Reason::Observation);
  assert(p.completedPulses == 1 && p.totalPump == maximumPulseSeconds);
  // A short flat plateau cannot end validation before an already observed tail.
  p = pilot();
  p.role = Role::Validation;
  p.pulse = 3;
  p.completedPulses = 2;
  p.validationPulses = 2;
  p.observedCoast = 600;
  p.usefulResponse = true;
  feed(p, p.deadline, 19.7);
  const double validationOff = p.switched;
  assert(p.phase == Phase::Observe && p.responseOnset >= 0);
  feed(p, validationOff + 599, 19.7);
  assert(p.phase == Phase::Observe && !p.responseSettled);
  feed(p, validationOff + 600, 19.7);
  assert(p.phase == Phase::Controller && !p.responseSettled);
  // A controller run stays active through the known tail and beyond it;
  // diagnostic settling no longer supplies a controller completion rule.
  const double controllerOn = p.lastSample + 1;
  p.setControllerPump(controllerOn, true);
  p.setControllerPump(controllerOn + p.minimumOn, false);
  const double controllerOff = p.switched;
  assert(p.controllerCycles == 1 && !p.pump);
  p.sample(controllerOff, p.targetC, true, controllerOff);
  p.tick(controllerOff);
  feed(p, controllerOff + 599, p.targetC);
  assert(p.active());
  feed(p, controllerOff + 600, p.targetC);
  assert(p.active() && !p.responseSettled);
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
  historyLifetime();
  adaptive();
  boundaries();
  controllerTargetHeadroom();
  safety();
  controllerLimitsAndTail();
  fastLoopStatisticalCadence();
  RecordedChillTrace::firstPulseRegression();
  responseRegressions();
  WaterTestControllerDurationRegressions::run();
  WaterTestDualControllerRegressions::run();
  WaterTestFinalObservationRegressions::run();
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
