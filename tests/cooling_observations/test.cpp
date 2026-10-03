#include "GlycolCoolingController.h"
#include <cassert>
#include <cmath>
#include <iostream>

using GlycolCooling::ObservationReason;

template<class Controller, class Config>
void responseEvents() {
    Controller c;
    double off = -1;
    bool wasOn = false;
    for (int t = 0; t < 600; ++t) {
        const auto o = c.step(t, t < 5 ? 20.5 : 20.0, 20.0);
        if (wasOn && !o.pump_on) off = t;
        wasOn = o.pump_on;
        if (o.observation.sequence) {
            assert(o.observation.sequence == 1 && o.observation.reason == ObservationReason::RateCondition);
            assert(o.observation.rate_qualified && !o.pump_on);
            assert(o.observation.started_s == 0 && o.observation.off_s == off);
            assert(o.observation.ended_s == t && t - off >= 450);
            assert(c.step(t, 20, 20).observation.sequence == 1); // Cached tick is not another event.
            assert(c.inhibit(t + 1).observation.sequence == 1); // No pending response to interrupt.
            break;
        }
    }
    assert(c.output().observation.sequence == 1);

    // The same step can finish an observation and immediately start another
    // pulse. Its completion must not disappear behind the new COOL state.
    c.reset();
    for (int t = 0; t < 600 && !c.output().observation.sequence; ++t)
        c.step(t, 20.5, 20.0);
    const auto next = c.output();
    assert(next.observation.sequence == 1 && next.pump_on);
    assert(next.observation.reason == ObservationReason::RateCondition && next.observation.rate_qualified);
    const auto aborted = c.inhibit(next.observation.ended_s);
    assert(!aborted.pump_on && aborted.observation.sequence == 2);
    assert(aborted.observation.reason == ObservationReason::Interrupted);
    assert(aborted.observation.off_s == aborted.observation.ended_s);
    assert(c.inhibit(next.observation.ended_s + 1).observation.sequence == 2);

    Config shortWindow;
    shortWindow.observe_coast_s = 10;
    shortWindow.max_observe_coast_s = 100;
    Controller immature(shortWindow);
    for (int t = 0; t < 80 && !immature.output().observation.sequence; ++t)
        immature.step(t, 20.5, 20.0);
    assert(immature.output().observation.sequence == 1);
    assert(immature.output().observation.reason == ObservationReason::RateUnqualified);
    assert(!immature.output().observation.rate_qualified);

    Config limited;
    limited.observe_coast_s = 10;
    limited.max_observe_coast_s = 20;
    Controller capped(limited);
    for (int t = 0; t <= 100; ++t) capped.step(t, 20.0 + t * 0.001, 25.0);
    for (int t = 101; t < 160 && !capped.output().observation.sequence; ++t)
        capped.step(t, 20.0 + t * 0.001, 20.0);
    const auto atCap = capped.output().observation;
    assert(atCap.sequence == 1 && atCap.reason == ObservationReason::CoastTimeLimit);
    assert(atCap.rate_qualified && atCap.ended_s - atCap.off_s == 20);

    // Interrupting a coast preserves its earlier physical OFF edge. A later
    // repeated inhibition must not invent another response event.
    c.reset();
    assert(c.step(0, 20.5, 20.0).pump_on);
    int coastStarted = 0;
    for (int t = 1; t <= 20; ++t) {
        if (!c.step(t, 20.5, 20.0).pump_on) {
            coastStarted = t;
            break;
        }
    }
    assert(coastStarted > 0 && c.output().observation.sequence == 0);
    const auto interruptedCoast = c.inhibit(coastStarted + 1).observation;
    assert(interruptedCoast.sequence == 1 && interruptedCoast.reason == ObservationReason::Interrupted);
    assert(interruptedCoast.started_s == 0 && interruptedCoast.off_s == coastStarted);
    assert(interruptedCoast.ended_s == coastStarted + 1);
    const auto repeated = c.inhibit(coastStarted + 2).observation;
    assert(repeated.sequence == 1 && repeated.off_s == coastStarted);
    assert(repeated.ended_s == interruptedCoast.ended_s);

    c.reset();
    c.step(0, 20.5, 20.0);
    assert(c.step(1, 20.5, 19.0).observation.reason == ObservationReason::Interrupted);
    assert(c.output().observation.sequence == 1);
    assert(c.step(1, 20.5, 19.0, false).observation.sequence == 1);
    c.reset();
    assert(c.step(0, 20, 20, false).observation.sequence == 0); // No invented cycle on startup fault.
}

void measurementQualification() {
    GlycolCooling::Measurements m;
    double temperature = 0, rate = 0;
    for (int t = 0; t <= 90; ++t) assert(m.observe(t, 20, 90, 12, temperature, rate));
    assert(m.rateQualified() && rate == 0);
    // A numerically zero rate with a hole in the window is not qualified.
    m.observe(100, 20, 90, 12, temperature, rate);
    assert(!m.rateQualified() && rate == 0);
    for (int t = 101; t <= 190; ++t) m.observe(t, 20, 90, 12, temperature, rate);
    assert(m.rateQualified());
    m.clear();
    m.observe(200, 20, 90, 12, temperature, rate);
    assert(!m.rateQualified() && rate == 0);
}

int main() {
    measurementQualification();
    responseEvents<AdaptiveCooling::Controller, AdaptiveCooling::Config>();
    responseEvents<PredictiveCooling::Controller, PredictiveCooling::Config>();
    for (auto algorithm : {GlycolCooling::Algorithm::PredictiveCoast, GlycolCooling::Algorithm::PulseDose}) {
        GlycolCooling::Controller c(algorithm);
        for (int t = 0; t < 600 && !c.output().observation.sequence; ++t) c.step(t, 20.5, 20);
        assert(c.output().observation.sequence == 1);
        assert(c.output().observation.reason == ObservationReason::RateCondition);
        assert(c.inhibit(600).observation.reason == ObservationReason::Interrupted);

        // External actuator ownership interrupts an active response and keeps
        // the selector's actual OFF edge as the minimum restart boundary.
        c.reset(algorithm);
        assert(c.step(0, 20.5, 20).pump_on);
        c.externalOff(1);
        assert(!c.output().pump_on && c.output().observation.sequence == 1);
        assert(c.output().observation.reason == ObservationReason::Interrupted);
        assert(c.output().observation.off_s == 1 && c.output().observation.ended_s == 1);
        assert(!c.step(2, 20.5, 20).pump_on);
        assert(c.step(3, 20.5, 20).pump_on);
        assert(c.output().observation.sequence == 1);
    }
    std::cout << "Production cooling observation telemetry regressions passed.\n";
}
