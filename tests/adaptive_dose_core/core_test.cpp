#include "AdaptiveDoseController.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

using AdaptiveCooling::Config;
using AdaptiveCooling::Controller;
using AdaptiveCooling::Phase;

int main() {
    {
        Controller controller;
        assert(controller.configurationValid());
        for (int t = 0; t <= 4; ++t) {
            const auto out = controller.step(t, 21.0, 20.0);
            assert(out.pump_on == (t < 4));
            assert(out.phase == (t < 4 ? Phase::Cool : Phase::Coast));
        }
        const auto completed = controller.output();
        assert(completed.actual_on_s == 4.0);
        assert(completed.pulse_budget_s == 4.0);
    }
    {
        Controller controller;
        const auto first = controller.step(0.0, 21.0, 20.0);
        const auto duplicate = controller.step(0.0, 19.0, 20.0);
        assert(first.temperature_c == duplicate.temperature_c);
        assert(duplicate.pump_on);
        // Even a duplicate tick must honor an invalid sensor immediately.
        assert(!controller.step(0.0, 19.0, 20.0, false).pump_on);
        assert(!controller.step(1.0, 21.0, 20.0).pump_on);
        assert(controller.step(2.0, 21.0, 20.0).pump_on);
    }
    {
        Controller controller;
        assert(controller.step(0.0, 21.0, 20.0).pump_on);
        assert(!controller.inhibit(0.5).pump_on);
        assert(!controller.inhibit(1.0).pump_on);
        assert(!controller.step(2.0, 21.0, 20.0).pump_on);
        // Repeated inhibition must not move the real OFF edge from 0.5 to 1.
        assert(controller.step(2.5, 21.0, 20.0).pump_on);
    }
    {
        Controller controller;
        assert(controller.step(0.0, 21.0, 20.0).pump_on);
        const auto changed = controller.step(1.0, 21.0, 19.0);
        assert(changed.phase == Phase::SetpointChangeWait);
        assert(changed.pump_on);
        assert(!controller.step(2.0, 21.0, 19.0).pump_on);
        assert(!controller.step(3.0, 21.0, 19.0).pump_on);
        assert(controller.step(4.0, 21.0, 19.0).pump_on);
    }
    {
        // An unresolved far-target probe permits sustained cooling only after
        // recording the complete first pulse and its coast observation.
        Controller controller;
        for (int t = 0; t < 480; ++t) {
            const auto out = controller.step(t, 30.0, 0.0);
            assert(out.pump_on == (t < 30));
            assert(!out.full_cooling);
        }
        const auto saturated = controller.step(480, 30.0, 0.0);
        assert(saturated.learning_updates == 1);
        assert(saturated.gain_c_per_on_s == 0.015);
        assert(saturated.full_cooling && saturated.pump_on);
        assert(std::isinf(saturated.pulse_budget_s));
        assert(controller.step(481, 30.0, 0.0).pump_on);
        // There is no mandatory emergency dwell to override this stop guard.
        assert(!controller.step(482, -0.125, 0.0).pump_on);
        controller.inhibit(483);
        const auto restored = controller.step(485, 30.0, 0.0);
        assert(restored.learning_updates == 1);
        assert(restored.gain_c_per_on_s == 0.015);
    }
    {
        Controller early;
        Controller late;
        const double origin = 4294967.375; // Beyond a 32-bit millisecond wrap.
        for (int t = 0; t < 8000; ++t) {
            const double sensor = 21.0 + (t % 11) / 16.0;
            const auto a = early.step(t + 0.375, sensor, 20.0);
            const auto b = late.step(origin + t, sensor, 20.0);
            assert(a.phase == b.phase && a.pump_on == b.pump_on);
            assert(a.temperature_c == b.temperature_c);
            assert(a.rate_c_per_s == b.rate_c_per_s);
            assert(a.gain_c_per_on_s == b.gain_c_per_on_s);
        }
        assert(!late.step(origin + 1, 21.0, 20.0).pump_on);
        assert(late.output().phase == Phase::DisabledOrSensorFault);
    }
    {
        Controller controller;
        for (int t = 0; t <= 90; ++t) controller.step(t, 20.0 - 0.001 * t, -20.0);
        assert(std::abs(controller.output().rate_c_per_s + 0.001) < 1e-12);
        assert(std::abs(controller.output().temperature_c - 19.916) < 1e-12);
        assert(controller.step(7200, 18.0, -20.0).rate_c_per_s == 0.0);
        assert(std::abs(controller.output().temperature_c - 18.0) < 1e-12);
        assert(!controller.step(7201, std::numeric_limits<double>::quiet_NaN(), -20.0).pump_on);
    }
    {
        Controller trained;
        for (int t = 0; t <= 454; ++t) trained.step(t, 21.0, 20.0);
        const auto tuning = trained.tuning();
        assert(tuning.learning_updates == 1 && tuning.gain_c_per_on_s == 0.015);
        assert(trained.output().pump_on);

        Controller restored;
        assert(restored.restoreTuning(tuning));
        assert(!restored.output().pump_on && restored.output().phase == Phase::Idle);
        assert(std::isnan(restored.output().temperature_c));
        assert(restored.output().actual_on_s == 0 && restored.output().pulse_budget_s == 0);
        assert(restored.tuning().gain_c_per_on_s == tuning.gain_c_per_on_s);
        assert(restored.tuning().learning_updates == tuning.learning_updates);
        const auto decision = restored.step(0, 21.0, 20.0);
        assert(decision.pump_on);
        assert(decision.pulse_budget_s == 0.5 / tuning.gain_c_per_on_s);
        assert(decision.pulse_budget_s != Controller().step(0, 21.0, 20.0).pulse_budget_s);

        const AdaptiveCooling::Tuning replacement = {0.04, 7};
        assert(restored.restoreTuning(replacement));
        assert(restored.output().pump_on && restored.output().phase == decision.phase);
        assert(restored.output().pulse_budget_s == decision.pulse_budget_s);
        assert(restored.step(0, 19, 20).temperature_c == decision.temperature_c);
        restored.inhibit(0.5);
        assert(restored.restoreTuning(tuning));
        assert(!restored.step(2, 21, 20).pump_on);
        assert(restored.step(2.5, 21, 20).pump_on);

        restored.reset();
        assert(restored.tuning().learning_updates == 0);
        assert(restored.tuning().gain_c_per_on_s == restored.configuration().initial_gain_c_per_on_s);
    }
    {
        Controller controller;
        const AdaptiveCooling::Tuning saved = {0.04, 7};
        assert(controller.restoreTuning(saved));
        const double invalid[] = {std::numeric_limits<double>::quiet_NaN(),
            std::numeric_limits<double>::infinity(), 0.0,
            controller.configuration().minimum_gain_c_per_on_s / 2,
            controller.configuration().maximum_gain_c_per_on_s * 2};
        for (double gain : invalid) {
            const AdaptiveCooling::Tuning bad = {gain, 99};
            assert(!controller.tuningValid(bad) && !controller.restoreTuning(bad));
            assert(controller.tuning().gain_c_per_on_s == saved.gain_c_per_on_s);
            assert(controller.tuning().learning_updates == saved.learning_updates);
        }
        assert(controller.restoreTuning({controller.configuration().minimum_gain_c_per_on_s, 0}));
        assert(controller.restoreTuning({controller.configuration().maximum_gain_c_per_on_s, 1}));
        Config invalid_config;
        invalid_config.min_on_s = 0;
        assert(!Controller(invalid_config).restoreTuning(saved));
    }
    {
        Config config;
        config.min_on_s = 1.0;
        Controller bad_minimum(config);
        assert(!bad_minimum.configurationValid());
        assert(!bad_minimum.step(0, 21, 20).pump_on);
        config = Config();
        config.observe_coast_s = config.max_observe_coast_s + 1.0;
        assert(!Controller(config).configurationValid());
        config = Config();
        config.rate_window_s = 127.0;
        assert(!Controller(config).configurationValid());
    }
    std::cout << "Adaptive-dose core checks passed\n";
}
