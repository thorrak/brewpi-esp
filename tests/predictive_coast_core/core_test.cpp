#include "PredictiveCoastController.h"

#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

using PredictiveCooling::Config;
using PredictiveCooling::Controller;
using PredictiveCooling::Phase;

namespace {
void close(double actual, double expected) {
    assert(std::abs(actual - expected) < 1e-10);
}

Config shortObservation() {
    Config config;
    config.measurement_window_s = 0.5;
    config.rate_window_s = 2.0;
    config.initial_coast_s = 4.0;
    config.min_coast_estimate_s = 1.0;
    config.max_coast_estimate_s = 100.0;
    config.learning_fraction = 0.5;
    config.observe_coast_s = 4.0;
    config.max_observe_coast_s = 4.0;
    return config;
}
}

int main() {
    {
        // Near target, use the small startup exposure and wait for its coast.
        Controller controller;
        assert(controller.configurationValid());
        for (int t = 0; t < 452; ++t) {
            const auto out = controller.step(t, 20.0625, 20.0);
            assert(out.pump_on == (t < 2));
            assert(out.phase == (t < 2 ? Phase::Cool : Phase::Coast));
            assert(!out.full_cooling);
            assert(out.pulse_budget_s == 2.0);
            assert(out.response_updates == 0);
        }
        const auto learned = controller.step(452, 20.0625, 20.0);
        assert(learned.pump_on);
        assert(learned.response_updates == 1);
        // No observed response reduces the budget gain, not the coast horizon.
        close(learned.budget_gain_c_per_s, 0.015025);
        assert(learned.learning_updates == 0);
        assert(learned.coast_s == 300.0);
        assert(learned.actual_on_s == 2.0);
    }
    {
        Controller far;
        const auto first = far.step(0.0, 21.0, 20.0);
        // At the 1 C boundary the reference permits immediate continuous ON.
        assert(first.pump_on && first.full_cooling);
        assert(std::isinf(first.pulse_budget_s));
        assert(first.response_updates == 0);
        assert(far.step(1.0, 19.0, 20.0).pump_on); // Minimum ON still applies.
        assert(!far.step(2.0, 19.0, 20.0).pump_on);
        Controller near;
        assert(!near.step(0.0, 20.9375, 20.0).full_cooling);
    }
    {
        Config config;
        config.startup_budget_c_per_s = 0.25 / 120.0;
        Controller at_limit(config);
        const auto bounded = at_limit.step(0.0, 20.5, 20.0);
        close(bounded.pulse_budget_s, 120.0);
        assert(!bounded.full_cooling);
        config.startup_budget_c_per_s *= 0.5;
        Controller beyond_limit(config);
        const auto continuous = beyond_limit.step(0.0, 20.5, 20.0);
        assert(continuous.full_cooling && std::isinf(continuous.pulse_budget_s));
    }
    {
        // A falling temperature can stop a far-target run before setpoint.
        Controller controller(shortObservation());
        assert(controller.step(0, 21.0, 20.0).pump_on);
        assert(controller.step(1, 20.9, 20.0).pump_on);
        const auto stopped = controller.step(2, 20.5, 20.0);
        assert(stopped.phase == Phase::Coast && !stopped.pump_on);
        close(stopped.predicted_endpoint_c, 19.5);
        for (int t = 3; t < 6; ++t) {
            assert(controller.step(t, 20.7 - t * 0.1, 20.0).response_updates == 0);
        }
        const auto learned = controller.step(6, 20.1, 20.0);
        assert(learned.learning_updates == 1 && learned.response_updates == 1);
        close(learned.coast_s, 2.8);
        close(learned.budget_gain_c_per_s, 0.24);
        // This tick's endpoint uses the previous coast estimate, before learning.
        close(learned.predicted_endpoint_c, 19.7);
        assert(!learned.pump_on);
        controller.inhibit(7);
        const auto restored = controller.step(9, 21.0, 20.0);
        close(restored.coast_s, 2.8);
        close(restored.budget_gain_c_per_s, 0.24);
        assert(restored.learning_updates == 1 && restored.response_updates == 1);
        assert(restored.actual_on_s == 2.0);
        controller.reset();
        close(controller.output().coast_s, 4.0);
        close(controller.output().budget_gain_c_per_s, 0.03);
        assert(controller.output().response_updates == 0);
    }
    {
        // Warming during coast must use the final reading, not its minimum.
        Controller controller(shortObservation());
        const double samples[] = {21.0, 20.9, 20.5, 20.1, 19.9, 20.0, 20.2};
        for (int t = 0; t <= 6; ++t) controller.step(t, samples[t], 20.0);
        assert(controller.output().response_updates == 1);
        close(controller.output().coast_s, 2.6);
        close(controller.output().budget_gain_c_per_s, 0.215);
    }
    {
        Config config = shortObservation();
        config.max_observe_coast_s = 10.0;
        Controller controller(config);
        controller.step(0, 21.0, 20.0);
        controller.step(1, 20.9, 20.0);
        controller.step(2, 20.5, 20.0);
        for (int t = 3; t < 6; ++t) {
            assert(controller.step(t, 20.4, 20.0).response_updates == 0);
        }
        const auto settled = controller.step(6, 20.4, 20.0);
        assert(settled.response_updates == 1);
        close(settled.coast_s, 2.5); // Observed horizon was clipped to 1 second.
    }
    {
        Controller controller;
        const auto first = controller.step(0, 21, 20);
        const auto duplicate = controller.step(0, 19, 20);
        assert(first.temperature_c == duplicate.temperature_c);
        assert(duplicate.pump_on);
        // A fault overrides duplicate-tick caching and the minimum ON interval.
        assert(!controller.step(0, 19, 20, false).pump_on);
        assert(!controller.step(1, 21, 20).pump_on);
        assert(controller.step(2, 21, 20).pump_on);
    }
    {
        Controller controller;
        assert(controller.step(0, 21, 20).pump_on);
        assert(!controller.inhibit(0.5).pump_on);
        assert(!controller.inhibit(1.0).pump_on);
        assert(!controller.step(2, 21, 20).pump_on);
        // The actual OFF edge remains 0.5, not the last inhibition call.
        assert(controller.step(2.5, 21, 20).pump_on);
    }
    {
        Config config;
        config.min_on_s = config.min_off_s = 10.0;
        Controller controller(config);
        assert(controller.step(0, 21, 20).pump_on);
        for (int t = 1; t < 10; ++t) {
            const auto changed = controller.step(t, 21, 19);
            assert(changed.pump_on && changed.phase == Phase::SetpointChangeWait);
        }
        for (int t = 10; t < 20; ++t) assert(!controller.step(t, 21, 19).pump_on);
        assert(controller.step(20, 21, 19).pump_on);
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
            assert(a.coast_s == b.coast_s);
            assert(a.budget_gain_c_per_s == b.budget_gain_c_per_s);
        }
        assert(!late.step(origin + 1, 21, 20).pump_on);
        assert(late.output().phase == Phase::DisabledOrSensorFault);
        assert(!late.step(origin + 8000, 21, 20).pump_on);
        assert(late.step(origin + 8001, 21, 20).pump_on);
    }
    {
        Controller controller;
        for (int t = 0; t <= 90; ++t) controller.step(t, 20.0 - 0.001 * t, -20.0);
        close(controller.output().rate_c_per_s, -0.001);
        close(controller.output().temperature_c, 19.916);
        assert(controller.step(7200, 18, -20).rate_c_per_s == 0.0);
        close(controller.output().temperature_c, 18.0);
        assert(!controller.step(7201, std::numeric_limits<double>::quiet_NaN(), -20).pump_on);
        assert(!controller.step(7202, 18, 126).pump_on);
    }
    {
        // A caller violating the documented 1 Hz capacity fails OFF safely.
        Controller controller;
        for (int t = 0; t < 32; ++t) assert(controller.step(t * 0.01, 21, 20).pump_on);
        assert(!controller.step(0.32, 21, 20).pump_on);
        assert(controller.output().phase == Phase::DisabledOrSensorFault);
    }
    {
        double Config::* fields[] = {
            &Config::min_on_s, &Config::min_off_s, &Config::rate_window_s,
            &Config::measurement_window_s, &Config::deadband_c,
            &Config::initial_coast_s, &Config::min_coast_estimate_s,
            &Config::max_coast_estimate_s, &Config::learning_fraction,
            &Config::rate_floor_c_per_s, &Config::observe_coast_s,
            &Config::max_observe_coast_s, &Config::near_target_c,
            &Config::startup_pulse_s, &Config::startup_budget_c_per_s,
            &Config::restart_margin_c, &Config::budget_learning_fraction,
            &Config::minimum_budget_gain_c_per_s, &Config::maximum_blind_budget_s
        };
        const double invalid[] = {0.0, -1.0, std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN()};
        for (auto field : fields) {
            for (double value : invalid) {
                Config config;
                config.*field = value;
                Controller controller(config);
                assert(!controller.configurationValid());
                assert(!controller.step(0, 21, 20).pump_on);
            }
        }
        Config config;
        config.min_on_s = 1.0;
        assert(!Controller(config).configurationValid());
        config = Config(); config.min_off_s = 1.0;
        assert(!Controller(config).configurationValid());
        config = Config(); config.learning_fraction = 1.01;
        assert(!Controller(config).configurationValid());
        config = Config(); config.budget_learning_fraction = 1.01;
        assert(!Controller(config).configurationValid());
        config = Config(); config.observe_coast_s = config.max_observe_coast_s + 1.0;
        assert(!Controller(config).configurationValid());
        config = Config(); config.initial_coast_s = config.min_coast_estimate_s - 1.0;
        assert(!Controller(config).configurationValid());
        config = Config(); config.initial_coast_s = config.max_coast_estimate_s + 1.0;
        assert(!Controller(config).configurationValid());
        config = Config(); config.startup_budget_c_per_s = config.minimum_budget_gain_c_per_s / 2.0;
        assert(!Controller(config).configurationValid());
        config = Config(); config.rate_window_s = 127.0;
        assert(!Controller(config).configurationValid());
        config = Config(); config.measurement_window_s = 31.0;
        assert(!Controller(config).configurationValid());
    }
    std::cout << "Predictive-coast core checks passed\n";
}
