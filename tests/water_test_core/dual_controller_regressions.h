#pragma once

#include "controller_duration_regressions.h"
#include <cassert>
#include <cstring>

namespace WaterTestDualControllerRegressions {
using namespace WaterTestCore;
using WaterTestControllerDurationRegressions::feed;
using WaterTestControllerDurationRegressions::requestedPulse;
using WaterTestControllerDurationRegressions::finishFinalObservation;

inline Program challenge() {
  Program p = WaterTestControllerDurationRegressions::challenge();
  p.controllerRun = 1;
  return p;
}

inline void twoFullChallengesKeepFirstResultsUntilExplicitAdvance() {
  Program p = challenge();
  const auto *history = p.history.get();
  const double firstTarget = p.targetC;
  p.baselineDrift = .00001;
  p.calibrationPulses = 3;
  p.validationPulses = 2;
  requestedPulse(p);
  feed(p, p.deadline, firstTarget);
  const double ended = p.controllerRunEnded, firstPump = p.totalPump;
  assert(p.active() && !p.pump && p.phase == Phase::ControllerTransition);
  assert(p.controllerRunDurationComplete);
  assert(!p.advanceController(ended));
  p.setControllerPump(ended, true);
  assert(!p.pump);
  // No controller-specific tail gate exists inside a run, but the physical
  // recovery requirement between algorithms remains unchanged.
  feed(p, ended + 180, firstTarget);
  assert(p.controllerTransitionReady);
  const double secondStart = p.lastSample;
  assert(p.advanceController(secondStart));
  assert(p.controllerRun == 2 && p.phase == Phase::Controller);
  assert(!p.controllerRunDurationComplete && p.controllerRunEnded == 0);
  assert(p.controllerStarted == secondStart && p.controllerRunStartC == firstTarget);
  assert(p.targetC == firstTarget - controllerTargetDropC);
  assert(p.controllerRunDeadline == secondStart + controllerSeconds);
  assert(p.controllerCycles == 0 && p.totalPump == firstPump);
  assert(p.history.get() == history && p.calibrationPulses == 3 && p.validationPulses == 2);
  feed(p, p.deadline, p.targetC);
  const double secondEnded = p.controllerRunEnded;
  assert(p.controllerRunDurationComplete && p.controllerCycles == 0);
  finishFinalObservation(p);
  assert(!p.active() && p.outcome == End::Completed && p.controllerRun == 2);
  assert(p.controllerRunEnded == secondEnded && p.lastSample == secondEnded + controllerFinalObservationSeconds);
}

inline void deadlineClosesActualPumpBeforeSecondFreshChallenge() {
  Program p = challenge();
  feed(p, p.deadline - 120, p.targetC);
  p.setControllerPump(p.lastSample, true);
  const double firstDeadline = p.deadline;
  feed(p, firstDeadline, p.targetC);
  assert(p.phase == Phase::ControllerTransition && p.controllerRunDurationComplete && !p.pump);
  assert(p.controllerRunEnded == firstDeadline);
  assert(p.totalPump == 120 && p.controllerCycles == 1);
  feed(p, firstDeadline + 980, p.targetC);
  assert(!p.controllerTransitionReady);
  feed(p, firstDeadline + 981, p.targetC);
  assert(p.controllerTransitionReady && p.advanceController(p.lastSample));
  assert(p.controllerRun == 2 && !p.controllerRunDurationComplete && p.controllerCycles == 0);
  feed(p, p.deadline, p.targetC);
  const double secondEnded = p.controllerRunEnded;
  finishFinalObservation(p);
  assert(p.controllerRunDurationComplete && p.controllerRunEnded == secondEnded);
}

inline void transitionRejectsCoolingAndRequiresCoveredFreshWindows() {
  Program p = challenge();
  p.observedCoast = 0;
  p.responseOnset = -1; // Transition does not depend on diagnostic onset credit.
  p.endControllerRun(p.lastSample);
  const double ended = p.controllerRunEnded;
  for (double t = ended + 1; t <= ended + controllerTransitionSeconds && p.active(); ++t) {
    p.sample(t, 20 - (t - ended) * .0001, true, t);
    p.tick(t);
  }
  assert(!p.active() && p.outcome == End::Inconclusive);
  assert(p.reason == Reason::ControllerTransitionTimeout && p.controllerRun == 1);
  assert(p.controllerRunEnded == ended && !p.controllerTransitionReady);

  p = challenge();
  p.observedCoast = 0;
  p.endControllerRun(p.lastSample);
  const double begin = p.lastSample;
  feed(p, begin + 90, 20);
  feed(p, begin + 180, 19.9375);
  assert(p.active() && !p.controllerTransitionReady);
  // Both plateaus are flat individually; the combined downward step matters.
  assert(!p.advanceController(p.lastSample));
  feed(p, begin + 360, 19.9375);
  assert(p.controllerTransitionReady);
  p.sample(p.lastSample + 2, 19.875, true, p.lastSample + 2);
  assert(!p.advanceController(p.lastSample));
  assert(!p.controllerTransitionReady && p.controllerRun == 1);

  p = challenge();
  p.observedCoast = 0;
  p.endControllerRun(p.lastSample);
  // Fresh final data alone cannot replace two covered observation windows.
  p.sample(p.lastSample + 180, 20, true, p.lastSample + 180);
  p.tick(p.lastSample);
  assert(!p.controllerTransitionReady && !p.advanceController(p.lastSample));
}

inline void limitedHeadroomKeepsFirstEvidenceAndSkipsSecondExplicitly() {
  for (double initial : {6., 20.}) {
    Program p;
    assert(p.start(100, initial, 2, 2));
    const double floor = std::max(minimumWaterC, initial - maximumDropC);
    const double firstStartC = floor + controllerTargetDropC + controllerHeadroomMarginC + .01;
    p.sample(200, firstStartC, true, 200);
    p.usefulResponse = true;
    p.beginController(200);
    assert(p.controllerRun == 1 && p.phase == Phase::Controller);
    const double target = p.targetC;
    assert(target == firstStartC - controllerTargetDropC);
    p.sample(201, target, true, 201);
    p.endControllerRun(201);
    feed(p, 381, target);
    assert(p.controllerTransitionReady);
    assert(!p.advanceController(p.lastSample));
    assert(!p.active() && p.outcome == End::Inconclusive && p.reason == Reason::ControllerHeadroom);
    assert(p.controllerRun == 1 && p.controllerRunEnded == 201 && p.targetC == target);
  }
  Program p;
  assert(p.start(100, 20, 2, 2));
  p.usefulResponse = true;
  p.sample(200, 20 - maximumDropC + .75, true, 200);
  p.beginController(200);
  assert(!p.active() && p.reason == Reason::ControllerHeadroom && p.controllerRun == 0);
  // Added overall reserve must not increase the diagnostic contrast pulse.
  assert(maximumDropC == 3 + controllerTargetDropC && diagnosticMaximumDropC == 3);
  assert(p.start(300, 20, 2, 2));
  p.role = Role::Validation;
  p.latestC = 18;
  p.usefulPulseSeconds = 100;
  p.usefulResponseDrop = 1;
  assert(p.choosePulse() == 20);
}

inline void transitionAndSecondRunPreserveGlobalSafety() {
  for (const auto reason : {Reason::SensorStale, Reason::UserStop, Reason::StorageFailure,
                            Reason::TemperatureLimit, Reason::RuntimeLimit}) {
    Program p = challenge();
    p.endControllerRun(p.lastSample);
    const double ended = p.controllerRunEnded;
    if (reason == Reason::SensorStale)
      p.tick(ended + freshnessSeconds + .001);
    else if (reason == Reason::UserStop)
      p.stop(ended + 1);
    else if (reason == Reason::TemperatureLimit)
      p.sample(ended + 1, 20 - maximumDropC - .01, true, ended + 1);
    else if (reason == Reason::RuntimeLimit) {
      const double now = p.started + maximumSeconds;
      p.sample(now, 20, true, now);
      p.tick(now);
    } else
      p.finish(ended + 1, End::Failed, reason);
    assert(!p.active() && p.reason == reason && !p.pump);
    assert(p.controllerRun == 1 && p.controllerRunEnded == ended);
    assert(!p.advanceController(p.lastSample));
  }
  Program p = challenge();
  p.totalPump = maximumPumpSeconds - 1;
  p.endControllerRun(p.lastSample);
  feed(p, p.lastSample + 981, 20);
  assert(p.advanceController(p.lastSample));
  assert(p.controllerRun == 2 && p.totalPump == maximumPumpSeconds - 1);
  const double on = p.lastSample;
  p.setControllerPump(on, true);
  feed(p, on + 1, 20);
  assert(!p.active() && p.reason == Reason::PumpBudget && !p.pump);
  assert(p.controllerRun == 2 && p.controllerRunEnded == on + 1);
  assert(p.totalPump == maximumPumpSeconds && p.controllerRunPumpStart == maximumPumpSeconds - 1);
}

inline void run() {
  static_assert(static_cast<unsigned>(Phase::Controller) == 6);
  static_assert(static_cast<unsigned>(Phase::ControllerTransition) == 7);
  static_assert(static_cast<unsigned>(Reason::ObservationTimeout) == 18);
  assert(std::strcmp(phaseName(Phase::ControllerTransition), "controller_transition") == 0);
  assert(std::strcmp(reasonName(Reason::ControllerHeadroom), "controller_headroom") == 0);
  twoFullChallengesKeepFirstResultsUntilExplicitAdvance();
  deadlineClosesActualPumpBeforeSecondFreshChallenge();
  transitionRejectsCoolingAndRequiresCoveredFreshWindows();
  limitedHeadroomKeepsFirstEvidenceAndSkipsSecondExplicitly();
  transitionAndSecondRunPreserveGlobalSafety();
}
} // namespace WaterTestDualControllerRegressions
