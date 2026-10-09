#pragma once

#include "WaterTestCore.h"

#include <cassert>
#include <cmath>

namespace WaterTestResponseRegressions {
using namespace WaterTestCore;

template <typename Temperature> void feedUntil(Program &p, double until, Temperature temperature) {
  for (double t = p.lastSample + 1; t <= until && p.active(); t += 1) {
    p.sample(t, temperature(t), true, t);
    p.tick(t);
  }
}

inline Program firstObservation(double initialC = 20) {
  Program p;
  assert(p.start(0, initialC, 2, 2));
  feedUntil(p, minimumBaselineSeconds + 12, [=](double) { return initialC; });
  assert(p.phase == Phase::Observe && p.pulse == 1 && p.completedPulses == 1);
  assert(std::abs(p.lastPulseSeconds - 10) < 1e-9);
  return p;
}

inline void staleWarmingDoesNotCreateCooling() {
  Program p = firstObservation();
  p.baselineDrift = .000263535;
  p.noiseC = .031790087;
  const double off = p.switched;
  feedUntil(p, off + 299, [](double) { return 20.; });
  assert(p.pulse == 1 && p.phase == Phase::Observe);
  assert(p.peakResponse == 0 && p.responseOnset < 0 && !p.usefulResponse);
  assert(!p.responseSettled);
  feedUntil(p, off + 300, [](double) { return 20.; });
  assert(p.pulse == 2 && p.pump && p.deadline - p.pulseStarted == 30);
  assert(!p.usefulResponse && !p.contrastStarted);
  assert(p.baselineDrift == .000263535);
}

inline void steadyWarmingAloneDoesNotCreateCooling() {
  Program p;
  assert(p.start(0, 20, 2, 2));
  for (double t = 1; t <= 1200 && p.active(); ++t) {
    p.sample(t, 20 + .001 * t, true, t);
    p.tick(t);
    assert(p.peakResponse == 0 && p.responseOnset < 0);
    assert(!p.usefulResponse && !p.contrastStarted);
  }
  assert(p.pulse >= 2 && p.validationPulses == 0);
}

inline void realResponseCanRecoverDuringSteadyWarming() {
  Program p;
  assert(p.start(0, 20, 2, 2));
  feedUntil(p, minimumBaselineSeconds + 12, [](double t) { return 20 + .001 * t; });
  assert(p.phase == Phase::Observe && p.pulse == 1);
  assert(std::abs(p.baselineDrift - .001) < .000001);
  const double off = p.switched;
  for (double t = p.lastSample + 1; t < off + 1200 && p.pulse == 1; ++t) {
    p.sample(t, 19.75 + .001 * t, true, t);
    p.tick(t);
  }
  assert(p.pulse == 2 && p.pump && p.contrastStarted && p.usefulResponse);
  assert(p.outcome == End::None && p.baselineDrift > 0);
  assert(p.usefulResponseDrop >= .20 && p.usefulResponseDrop <= .25);
}

inline void naturalCoolingDoesNotBecomePumpGain() {
  Program p;
  assert(p.start(0, 20, 2, 2));
  for (double t = 1; t <= 5000 && p.active(); ++t) {
    p.sample(t, 20 - .0005 * t, true, t);
    p.tick(t);
    assert(!p.usefulResponse && !p.contrastStarted);
    assert(p.peakResponse < .125 && p.responseOnset < 0);
  }
  assert(p.pulse > 0 && p.validationPulses == 0 && p.controllerStarted == 0);
}

inline void expiredBackgroundEstimateCannotCreateLateOnset() {
  Program p = firstObservation();
  p.baselineDrift = -.0005;
  p.baselineDriftUncertainty = .0001;
  p.peakResponse = .08;
  const double expires = p.pulseStarted + driftCorrectionSeconds;
  feedUntil(p, expires, [](double) { return 20.; });
  assert(p.responseOnset < 0 && p.pulse == 1);
  feedUntil(p, expires + 60, [=](double t) { return 19.9 - .0001 * (t - expires - 1); });
  assert(p.phase == Phase::Observe && p.pulse == 1 && !p.pump);
  assert(p.peakResponse == .08 && p.responseOnset < 0 && !p.responseSettled);
  assert(!p.usefulResponse && !p.contrastStarted);
}

inline void uncertainQuantizedBaselineCannotBoostResponse() {
  Program p;
  assert(p.start(0, 20, 2, 2));
  feedUntil(p, minimumBaselineSeconds, [](double t) {
    return t > minimumBaselineSeconds / 2 && static_cast<int>(t) % 4 < 2 ? 20.0625 : 20.;
  });
  assert(p.baselineDrift > 0 && p.baselineDrift <= p.baselineDriftUncertainty);
  for (double t = p.lastSample + 1; t < 1200 && p.active(); ++t) {
    p.sample(t, 20, true, t);
    p.tick(t);
    assert(p.peakResponse <= .0625 && !p.usefulResponse && !p.contrastStarted);
  }
  assert(p.pulse >= 2 && p.validationPulses == 0);
}

inline void weakResponseCanRecoverWithoutUsefulGain() {
  for (bool quantized : {false, true}) {
    Program p = firstObservation();
    p.baselineDrift = .000263535;
    const double off = p.switched;
    bool detectedWeakResponse = false;
    double largestResponse = 0;
    for (double t = p.lastSample + 1; t < off + 1200 && p.pulse == 1; ++t) {
      const double elapsed = t - off;
      const double drop = quantized ? (elapsed >= 100 ? .0625 : 0)
                                   : .09375 * std::min(1., elapsed / 100);
      p.sample(t, 20 - drop, true, t);
      largestResponse = std::max(largestResponse, p.peakResponse);
      p.tick(t);
      detectedWeakResponse = detectedWeakResponse || p.responseOnset >= 0;
    }
    assert(detectedWeakResponse && largestResponse >= .0625 && largestResponse < .125);
    assert(p.pulse == 2 && p.pump && p.deadline - p.pulseStarted == 30);
    assert(!p.usefulResponse && !p.contrastStarted && p.outcome == End::None);
  }
}

inline void smallResponseEscalatesAfterRecovery() {
  Program p = firstObservation(20.0625);
  p.baselineDrift = .000263535;
  p.noiseC = .031790087;
  const double off = p.switched;
  double largestResponse = 0;
  for (double t = p.lastSample + 1; t < off + 2400 && p.pulse == 1; ++t) {
    const double elapsed = t - off;
    const double temperature = elapsed < 400 ? 20.0625 - .1875 * elapsed / 400
                                             : 19.875 + .00002 * (elapsed - 400);
    p.sample(t, temperature, true, t);
    largestResponse = std::max(largestResponse, p.peakResponse);
    p.tick(t);
  }
  assert(largestResponse > .18 && largestResponse <= .1875 + 1e-9);
  assert(p.pulse == 2 && p.pump && p.deadline - p.pulseStarted == 30);
  assert(!p.usefulResponse && !p.contrastStarted);
  assert(std::abs(p.baselineDrift - .00002) < .000005);
}

inline void continuingTailDoesNotSettle() {
  Program p = firstObservation();
  p.baselineDrift = .000263535;
  const double off = p.switched;
  feedUntil(p, off + 3600, [=](double t) { return 19.75 - .00004 * (t - off); });
  assert(p.active() && p.phase == Phase::Observe && p.pulse == 1 && !p.pump);
  assert(!p.responseSettled && p.peakResponse >= .25);
  assert(p.baselineDrift == .000263535);
}

inline void quantizedContinuingTailDoesNotSettle() {
  Program p = firstObservation();
  const double off = p.switched;
  feedUntil(p, off + 1800, [=](double t) { return 19.75 - .0625 * std::floor((t - off) / 120); });
  assert(p.active() && p.phase == Phase::Observe && p.pulse == 1 && !p.pump);
  assert(!p.responseSettled && !p.usefulResponse && !p.contrastStarted);
}

inline void delayedCoolingMustArriveBeforeSettling() {
  Program p = firstObservation();
  const double off = p.switched;
  feedUntil(p, off + 200, [](double) { return 20.; });
  assert(p.pulse == 1 && p.responseOnset < 0 && !p.responseSettled);
  feedUntil(p, off + 300, [=](double t) { return 20 - .25 * (t - off - 200) / 100; });
  assert(p.pulse == 1 && p.observedDelay >= 225 && !p.responseSettled);
  feedUntil(p, off + 2 * p.observedDelay - 1, [](double) { return 19.75; });
  assert(p.pulse == 1 && !p.responseSettled);
  for (double t = p.lastSample + 1; t < off + 1800 && p.pulse == 1; ++t) {
    p.sample(t, 19.75, true, t);
    p.tick(t);
  }
  assert(p.pulse == 2 && p.contrastStarted && p.usefulResponse);
}

inline void knownCoastIsStillRespected() {
  Program p = firstObservation();
  const double off = p.switched;
  p.observedCoast = 900;
  feedUntil(p, off + 899, [](double) { return 19.75; });
  assert(p.pulse == 1 && p.phase == Phase::Observe && !p.responseSettled);
  feedUntil(p, off + 900, [](double) { return 19.75; });
  assert(p.pulse == 2 && p.usefulResponse && p.contrastStarted);
}

inline void unresolvedObservationEndsWithoutAnotherPulse() {
  Program p = firstObservation();
  const double off = p.switched;
  const double deadline = p.deadline;
  feedUntil(p, off + 1000, [=](double t) { return 19.75 - .00001 * (t - off); });
  assert(p.pulse == 1 && !p.responseSettled);
  p.sample(deadline, 19.5, true, deadline);
  p.tick(deadline);
  assert(p.phase == Phase::Finished && p.outcome == End::Inconclusive);
  assert(p.reason == Reason::ObservationTimeout && !p.pump && p.pulse == 1);
  assert(!p.usefulResponse && !p.contrastStarted && !p.responseSettled);
  assert(p.usefulPulseSeconds == 0 && p.usefulResponseDrop == 0);
}

inline void responsiveCampaignStillCompletes() {
  Program p;
  assert(p.start(0, 20, 2, 2));
  double temperature = 20, tailUntil = 0;
  bool previousPump = false, reachedValidation = false, reachedController = false;
  unsigned checkpointedRuns = 0;
  for (double t = 1; t < maximumSeconds && p.active(); ++t) {
    if (!p.pump && previousPump)
      tailUntil = t + 40;
    previousPump = p.pump;
    if (p.phase == Phase::Controller) {
      reachedController = true;
      // This mock reaches the target after one requested pulse and holds it.
      // A quiet installation must still receive the full scheduled run.
      temperature = p.targetC;
    } else if (p.phase != Phase::ControllerTransition && p.phase != Phase::ControllerFinalObserve) {
      reachedValidation = reachedValidation || p.role == Role::Validation;
      temperature -= p.pump ? .008 : t < tailUntil ? .002 : 0;
    }
    p.sample(t, temperature, true, t);
    if (p.phase == Phase::Controller) {
      if (!p.pump && !p.controllerCycles)
        p.setControllerPump(t, true);
      if (p.pump && t - p.switched >= p.minimumOn)
        p.setControllerPump(t, false);
    }
    p.tick(t);
    if (p.phase == Phase::ControllerFinalObserve)
      p.confirmControllerFinalOff(t);
    if (p.phase == Phase::ControllerTransition && p.controllerTransitionReady) {
      assert(p.controllerRun == 1 && p.controllerRunDurationComplete);
      ++checkpointedRuns; // Stand-in for the wrapper's durable run snapshot.
      assert(p.advanceController(t));
    }
  }
  assert(reachedValidation && reachedController);
  assert(p.phase == Phase::Finished && p.outcome == End::Completed && !p.pump);
  assert(p.validationPulses == 2 && p.controllerCycles == 1);
  assert(p.controllerRunDurationComplete);
  assert(p.controllerRun == 2 && checkpointedRuns == 1);
  assert(p.usefulResponse && !p.responseSettled);
}
} // namespace WaterTestResponseRegressions

inline void responseRegressions() {
  using namespace WaterTestResponseRegressions;
  staleWarmingDoesNotCreateCooling();
  steadyWarmingAloneDoesNotCreateCooling();
  realResponseCanRecoverDuringSteadyWarming();
  naturalCoolingDoesNotBecomePumpGain();
  expiredBackgroundEstimateCannotCreateLateOnset();
  uncertainQuantizedBaselineCannotBoostResponse();
  weakResponseCanRecoverWithoutUsefulGain();
  smallResponseEscalatesAfterRecovery();
  continuingTailDoesNotSettle();
  quantizedContinuingTailDoesNotSettle();
  delayedCoolingMustArriveBeforeSettling();
  knownCoastIsStillRespected();
  unresolvedObservationEndsWithoutAnotherPulse();
  responsiveCampaignStillCompletes();
}
