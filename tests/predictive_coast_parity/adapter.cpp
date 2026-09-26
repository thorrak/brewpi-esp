// Independent host test boundary. This file contains no controller decisions.
#include "PredictiveCoastController.h"
#include <cstddef>
#include <cstdint>
#include <new>

using PredictiveCooling::Config;
using PredictiveCooling::Controller;
using PredictiveCooling::Output;

extern "C" {
struct PredictiveResult {
    std::uint32_t phase;
    std::uint32_t pump_on;
    std::uint32_t full_cooling;
    std::uint32_t learning_updates;
    std::uint32_t response_updates;
    double temperature_c;
    double rate_c_per_s;
    double setpoint_c;
    double coast_s;
    double budget_gain_c_per_s;
    double pulse_budget_s;
    double predicted_endpoint_c;
    double actual_on_s;
};

// Order matches the frozen Python PredictiveCoastConfig dataclass fields.
void* predictive_create(const double* c, std::size_t n) {
    Config config;
    if (c != nullptr) {
        if (n != 19) return nullptr;
        config.min_on_s = c[0];
        config.min_off_s = c[1];
        config.rate_window_s = c[2];
        config.measurement_window_s = c[3];
        config.deadband_c = c[4];
        config.initial_coast_s = c[5];
        config.min_coast_estimate_s = c[6];
        config.max_coast_estimate_s = c[7];
        config.learning_fraction = c[8];
        config.rate_floor_c_per_s = c[9];
        config.observe_coast_s = c[10];
        config.max_observe_coast_s = c[11];
        config.near_target_c = c[12];
        config.startup_pulse_s = c[13];
        config.startup_budget_c_per_s = c[14];
        config.restart_margin_c = c[15];
        config.budget_learning_fraction = c[16];
        config.minimum_budget_gain_c_per_s = c[17];
        config.maximum_blind_budget_s = c[18];
    }
    return new (std::nothrow) Controller(config);
}
void predictive_destroy(void* p) { delete static_cast<Controller*>(p); }
void predictive_reset(void* p) { static_cast<Controller*>(p)->reset(); }
void predictive_reset_runtime(void* p, double t) {
    static_cast<Controller*>(p)->resetRuntime(t);
}
const char* predictive_phase_name(std::uint32_t phase) {
    return Controller::phaseName(static_cast<PredictiveCooling::Phase>(phase));
}
void predictive_step(void* p, double t, double sensor, double target,
                     std::uint8_t valid, PredictiveResult* result) {
    const Output o = static_cast<Controller*>(p)->step(t, sensor, target, valid != 0);
    *result = {static_cast<std::uint32_t>(o.phase), static_cast<std::uint32_t>(o.pump_on),
               static_cast<std::uint32_t>(o.full_cooling), o.learning_updates,
               o.response_updates, o.temperature_c, o.rate_c_per_s, o.setpoint_c,
               o.coast_s, o.budget_gain_c_per_s, o.pulse_budget_s,
               o.predicted_endpoint_c, o.actual_on_s};
}
}
