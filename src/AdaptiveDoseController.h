/*
 * Adaptive pulse-dose cooling, ported from chillsim's frozen controller.
 * Copyright (C) 2026 BrewPi contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This core uses only time, beer temperature, setpoint and pump history.
 * All temperatures are Celsius, rates Celsius/second, and times seconds.
 * Feed cached raw sensor readings at 1 Hz; do not prefilter the input.
 */
#pragma once

#include "CoolingMeasurements.h"
#include <cstdint>

namespace AdaptiveCooling {

struct Config {
    double min_on_s = 2.0;
    double min_off_s = 2.0;
    double rate_window_s = 90.0;
    double measurement_window_s = 12.0;
    double deadband_c = 0.04;
    double initial_gain_c_per_on_s = 0.03;
    double minimum_gain_c_per_on_s = 0.00005;
    double maximum_gain_c_per_on_s = 0.5;
    double dose_fraction = 0.5;
    double learning_fraction = 0.5;
    double observe_coast_s = 450.0;
    double max_observe_coast_s = 2400.0;
    double initial_probe_s = 4.0;
    double far_probe_s = 30.0;
    double far_error_c = 2.0;
    double saturation_dose_s = 900.0;
    double unresolved_error_c = 0.125;
    double stop_horizon_s = 120.0;
    double settled_rate_c_per_s = 0.00005;
};

enum class Phase : uint8_t {
    Idle,
    Cool,
    Coast,
    DisabledOrSensorFault,
    SetpointChangeWait
};

struct Output {
    Phase phase;
    bool pump_on;
    bool full_cooling;
    double temperature_c;
    double rate_c_per_s;
    double setpoint_c;
    uint32_t learning_updates;
    double gain_c_per_on_s;
    // Infinity means a continuous dose; stop prediction still remains active.
    double pulse_budget_s;
    double predicted_endpoint_c;
    double actual_on_s;
};

class Controller {
public:
    explicit Controller(const Config& config = Config());

    // An identical tick returns the cached result; an invalid sample on any
    // tick (including a duplicate) overrides that cache and immediately stops.
    // A backwards/invalid clock also fails OFF. No clock wrap is inferred here:
    // the caller must provide a monotonically increasing 64-bit-derived clock.
    Output step(double time_s, double sensor_c, double setpoint_c,
                bool sensor_connected = true);

    // Abort the current response and clear measurements, retaining gain and
    // learning count. Repeated calls preserve the actual previous OFF edge.
    // Used for faults, mode changes, heater/switch gates and explicit disable.
    Output inhibit(double time_s);
    void resetRuntime(double time_s) { inhibit(time_s); }

    // Full reset is only for initialization/reconfiguration with outputs OFF.
    // Runtime inhibition must use inhibit() so relay timing is not forgotten.
    void reset();
    bool configurationValid() const { return config_valid_; }
    const Config& configuration() const { return config_; }
    const Output& output() const { return output_; }
    static const char* phaseName(Phase phase);

private:

    Config config_;
    bool config_valid_;
    bool pump_on_;
    bool restart_required_;
    double last_edge_s_;
    double last_time_s_;
    double setpoint_c_;
    GlycolCooling::Measurements measurements_;
    double temperature_c_, raw_c_, rate_c_per_s_;
    uint32_t learning_updates_;
    double gain_;
    Phase phase_;
    double start_c_, start_s_, off_s_, pulse_budget_s_, actual_on_s_, coast_min_c_;
    Output output_;

    static bool validate(const Config& config);
    void clearMeasurements();
    void resetTransient();
    bool observe(double time_s, double value_c);
    bool switchPump(double time_s, bool desired, bool fail_off = false);
    Output emit(Phase phase, double predicted_endpoint_c);
    Output control(double time_s);
};

} // namespace AdaptiveCooling
