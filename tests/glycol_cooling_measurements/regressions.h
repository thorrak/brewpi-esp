#pragma once

#include <cassert>
#include <cmath>
#include <limits>

// Both portable algorithms use these measurements. Keep coverage of real
// quantized startup, tick jitter, missing calls and sensor reconnection shared.
template <typename Controller>
void rateMaturityRegressions() {
    {
        // Run 59862fa6 began at 19 C, then one DS18B20 quantum lower. Three
        // cached samples previously projected the endpoint near 9.71 C.
        Controller controller;
        for (int t = 0; t <= 6; ++t) {
            const auto out = controller.step(t, t < 2 ? 19.0 : 18.9375, 18.6875);
            assert(out.rate_c_per_s == 0.0);
            assert(out.predicted_endpoint_c > 18.9);
            if (t == 2) {
                assert(out.pump_on);
                assert(std::abs(out.predicted_endpoint_c - 18.979166666666668) < 1e-12);
            }
        }
        assert(!controller.output().pump_on); // Blind startup budget still stops.
        assert(controller.output().actual_on_s <= 6.0);
    }
    const double cadences[] = {1.0, 1.00001, 1.03125, 2.0};
    for (double cadence : cadences) {
        Controller controller;
        for (int i = 0; i < 220; ++i) {
            const double t = cadence * i;
            const auto out = controller.step(t, 25.0 - 0.001 * t, 26.0);
            if (t < 90.0) assert(out.rate_c_per_s == 0.0);
            else assert(std::abs(out.rate_c_per_s + 0.001) < 1e-10);
        }
    }
    {
        Controller controller;
        for (int t = 0; t < 100; ++t) controller.step(t, 19.0, 20.0);
        for (int t = 1000; t < 1003; ++t) {
            assert(controller.step(t, t < 1002 ? 19.0 : 18.9375, 20.0).rate_c_per_s == 0.0);
        }
        for (int t = 1003; t <= 1090; ++t) {
            controller.step(t, 19.0 - 0.001 * (t - 1000), 20.0);
        }
        assert(controller.output().rate_c_per_s < -0.0009);
        assert(!controller.step(1091, std::numeric_limits<double>::quiet_NaN(), 20.0).pump_on);
        for (int t = 1092; t < 1182; ++t) {
            assert(controller.step(t, t < 1094 ? 19.0 : 18.9375, 20.0).rate_c_per_s == 0.0);
        }
    }
}
