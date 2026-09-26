/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "PredictiveCoastController.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace PredictiveCooling {
namespace {
double nanValue() { return std::numeric_limits<double>::quiet_NaN(); }
double infinity() { return std::numeric_limits<double>::infinity(); }
bool validTemperature(double value) {
    return std::isfinite(value) && value >= -55.0 && value <= 125.0;
}
}

Controller::Controller(const Config& config)
    : config_(config), config_valid_(validate(config)) {
    reset();
}

bool Controller::validate(const Config& c) {
    const double values[] = {
        c.min_on_s, c.min_off_s, c.rate_window_s, c.measurement_window_s,
        c.deadband_c, c.initial_coast_s, c.min_coast_estimate_s,
        c.max_coast_estimate_s, c.learning_fraction, c.rate_floor_c_per_s,
        c.observe_coast_s, c.max_observe_coast_s, c.near_target_c,
        c.startup_pulse_s, c.startup_budget_c_per_s, c.restart_margin_c,
        c.budget_learning_fraction, c.minimum_budget_gain_c_per_s,
        c.maximum_blind_budget_s
    };
    for (double value : values) {
        if (!std::isfinite(value) || value <= 0.0) return false;
    }
    // Fixed-capacity firmware buffers support these windows at a 1 Hz cadence.
    return c.min_on_s >= 2.0 && c.min_off_s >= 2.0 &&
        c.learning_fraction <= 1.0 && c.budget_learning_fraction <= 1.0 &&
        c.observe_coast_s <= c.max_observe_coast_s &&
        c.min_coast_estimate_s <= c.initial_coast_s &&
        c.initial_coast_s <= c.max_coast_estimate_s &&
        c.startup_budget_c_per_s >= c.minimum_budget_gain_c_per_s &&
        c.rate_window_s <= 126.0 && c.measurement_window_s <= 30.0;
}

void Controller::reset() {
    pump_on_ = false;
    restart_required_ = false;
    last_edge_s_ = -infinity();
    last_time_s_ = nanValue();
    setpoint_c_ = nanValue();
    temperature_c_ = nanValue();
    raw_c_ = nanValue();
    learning_updates_ = 0;
    coast_s_ = config_.initial_coast_s;
    budget_gain_ = config_.startup_budget_c_per_s;
    response_updates_ = 0;
    phase_ = Phase::Idle;
    start_c_ = off_c_ = nanValue();
    off_rate_ = start_s_ = off_s_ = pulse_budget_s_ = actual_on_s_ = 0.0;
    clearMeasurements();
    emit(Phase::Idle, nanValue());
}

void Controller::resetTransient() {
    phase_ = Phase::Idle;
    start_c_ = off_c_ = nanValue();
}

void Controller::clearMeasurements() {
    measurements_.clear();
    rate_c_per_s_ = 0.0;
}

bool Controller::observe(double time_s, double value_c) {
    raw_c_ = value_c;
    return measurements_.observe(time_s, value_c, config_.rate_window_s,
        config_.measurement_window_s, temperature_c_, rate_c_per_s_);
}

bool Controller::switchPump(double time_s, bool desired, bool fail_off) {
    if (desired == pump_on_) return false;
    const double minimum = pump_on_ ? config_.min_on_s : config_.min_off_s;
    if (fail_off || time_s - last_edge_s_ >= minimum) {
        pump_on_ = desired;
        last_edge_s_ = time_s;
        return true;
    }
    return false;
}

Output Controller::emit(Phase phase, double endpoint) {
    output_ = {phase, pump_on_, pump_on_ && std::isinf(pulse_budget_s_),
        temperature_c_, rate_c_per_s_, setpoint_c_, learning_updates_, coast_s_,
        budget_gain_, response_updates_, pulse_budget_s_, endpoint, actual_on_s_};
    return output_;
}

Output Controller::inhibit(double time_s) {
    // Never regress the OFF edge when rejecting an invalid clock.
    if (!std::isfinite(time_s) || time_s < 0.0 ||
        (std::isfinite(last_time_s_) && time_s < last_time_s_)) {
        time_s = std::isfinite(last_time_s_) ? last_time_s_ : 0.0;
    }
    last_time_s_ = time_s;
    switchPump(time_s, false, true);
    clearMeasurements();
    resetTransient();
    setpoint_c_ = nanValue();
    restart_required_ = false;
    return emit(Phase::DisabledOrSensorFault, nanValue());
}

Output Controller::step(double time_s, double sensor_c, double setpoint_c,
                        bool sensor_connected) {
    if (!config_valid_ || !std::isfinite(time_s) || time_s < 0.0 ||
        (std::isfinite(last_time_s_) && time_s < last_time_s_) ||
        !sensor_connected || !validTemperature(sensor_c) || !validTemperature(setpoint_c)) {
        return inhibit(time_s);
    }
    if (time_s == last_time_s_) return output_;
    last_time_s_ = time_s;
    if (!std::isfinite(setpoint_c_) || std::abs(setpoint_c - setpoint_c_) > 1e-9) {
        resetTransient();
        setpoint_c_ = setpoint_c;
        restart_required_ = pump_on_;
    }
    if (!observe(time_s, sensor_c)) return inhibit(time_s);
    if (restart_required_) {
        switchPump(time_s, false);
        if (pump_on_) return emit(Phase::SetpointChangeWait, nanValue());
        restart_required_ = false;
    }
    return control(time_s);
}

Output Controller::control(double t) {
    const Config& cfg = config_;
    const double error = temperature_c_ - setpoint_c_;
    // Compute before coast learning, matching the frozen reference's ordering.
    const double predicted = temperature_c_ + std::min(rate_c_per_s_, 0.0) * coast_s_;
    if (phase_ == Phase::Coast) {
        const double coast_age = t - off_s_;
        const bool settled = std::abs(rate_c_per_s_) <= cfg.rate_floor_c_per_s;
        if (coast_age >= cfg.max_observe_coast_s || (coast_age >= cfg.observe_coast_s && settled)) {
            // Use the temperature at the END of observation, not its minimum.
            const double drop = std::max(0.0, off_c_ - temperature_c_);
            if (off_rate_ < -cfg.rate_floor_c_per_s && drop >= 0.04) {
                const double observed = std::min(cfg.max_coast_estimate_s,
                    std::max(cfg.min_coast_estimate_s, drop / -off_rate_));
                coast_s_ += cfg.learning_fraction * (observed - coast_s_);
                ++learning_updates_;
            }
            const double total_drop = std::max(0.0, start_c_ - temperature_c_);
            if (actual_on_s_ >= cfg.min_on_s) {
                double observed_gain = total_drop / actual_on_s_;
                observed_gain = std::max(cfg.minimum_budget_gain_c_per_s, observed_gain);
                budget_gain_ += cfg.budget_learning_fraction * (observed_gain - budget_gain_);
                ++response_updates_;
            }
            phase_ = Phase::Idle;
        }
    }
    if (phase_ == Phase::Idle && error > cfg.restart_margin_c && predicted > setpoint_c_) {
        start_s_ = t;
        start_c_ = temperature_c_;
        pulse_budget_s_ = error < cfg.near_target_c
            ? std::max(cfg.startup_pulse_s, 0.5 * error / budget_gain_) : infinity();
        if (pulse_budget_s_ > cfg.maximum_blind_budget_s) pulse_budget_s_ = infinity();
        if (switchPump(t, true)) phase_ = Phase::Cool;
    }
    if (phase_ == Phase::Cool) {
        const bool exhausted = t - start_s_ >= pulse_budget_s_;
        if (predicted <= setpoint_c_ || error <= 0.0 ||
            raw_c_ <= setpoint_c_ - cfg.deadband_c || exhausted) {
            if (switchPump(t, false)) {
                phase_ = Phase::Coast;
                off_c_ = temperature_c_;
                off_s_ = t;
                off_rate_ = rate_c_per_s_;
                actual_on_s_ = t - start_s_;
            }
        }
    }
    return emit(phase_, predicted);
}

const char* Controller::phaseName(Phase phase) {
    switch (phase) {
        case Phase::Idle: return "IDLE";
        case Phase::Cool: return "COOL";
        case Phase::Coast: return "COAST";
        case Phase::DisabledOrSensorFault: return "DISABLED_OR_SENSOR_FAULT";
        case Phase::SetpointChangeWait: return "SETPOINT_CHANGE_WAIT";
    }
    return "DISABLED_OR_SENSOR_FAULT";
}

} // namespace PredictiveCooling
