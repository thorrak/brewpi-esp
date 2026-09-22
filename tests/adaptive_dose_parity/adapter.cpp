// Independent host test boundary. This file contains no controller decisions.
#include "AdaptiveDoseController.h"
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>

using AdaptiveCooling::Config;
using AdaptiveCooling::Controller;
using AdaptiveCooling::Output;

extern "C" {
struct AdaptiveResult {
    std::uint32_t phase;
    std::uint32_t pump_on;
    std::uint32_t learning_updates;
    double temperature_c;
    double rate_c_per_s;
    double setpoint_c;
    double gain_c_per_on_s;
    double pulse_budget_s;
    double predicted_endpoint_c;
    double actual_on_s;
};

// Order is frozen in the independent Python harness CONFIG_FIELDS declaration.
void* adaptive_create(const double* c, std::size_t n) {
    Config config;
    if (c != nullptr) {
        if (n != 19) return nullptr;
        config.min_on_s = c[0];
        config.min_off_s = c[1];
        config.rate_window_s = c[2];
        config.measurement_window_s = c[3];
        config.deadband_c = c[4];
        config.initial_gain_c_per_on_s = c[5];
        config.minimum_gain_c_per_on_s = c[6];
        config.maximum_gain_c_per_on_s = c[7];
        config.dose_fraction = c[8];
        config.learning_fraction = c[9];
        config.observe_coast_s = c[10];
        config.max_observe_coast_s = c[11];
        config.initial_probe_s = c[12];
        config.far_probe_s = c[13];
        config.far_error_c = c[14];
        config.saturation_dose_s = c[15];
        config.unresolved_error_c = c[16];
        config.stop_horizon_s = c[17];
        config.settled_rate_c_per_s = c[18];
    }
    return new (std::nothrow) Controller(config);
}
void adaptive_destroy(void* p) { delete static_cast<Controller*>(p); }
void adaptive_reset(void* p) { static_cast<Controller*>(p)->reset(); }
void adaptive_reset_runtime(void* p, double t) {
    static_cast<Controller*>(p)->resetRuntime(t);
}
const char* adaptive_phase_name(std::uint32_t phase) {
    return Controller::phaseName(static_cast<AdaptiveCooling::Phase>(phase));
}
void adaptive_step(void* p, double t, double sensor, double target,
                   std::uint8_t valid, AdaptiveResult* result) {
    const Output o = static_cast<Controller*>(p)->step(t, sensor, target, valid != 0);
    *result = {static_cast<std::uint32_t>(o.phase), static_cast<std::uint32_t>(o.pump_on),
               o.learning_updates, o.temperature_c, o.rate_c_per_s, o.setpoint_c,
               o.gain_c_per_on_s, o.pulse_budget_s, o.predicted_endpoint_c, o.actual_on_s};
}
}
