/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "AdaptiveDoseController.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace AdaptiveCooling {
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
        c.deadband_c, c.initial_gain_c_per_on_s, c.minimum_gain_c_per_on_s,
        c.maximum_gain_c_per_on_s, c.dose_fraction, c.learning_fraction,
        c.observe_coast_s, c.max_observe_coast_s, c.initial_probe_s,
        c.far_probe_s, c.far_error_c, c.saturation_dose_s, c.unresolved_error_c,
        c.stop_horizon_s, c.settled_rate_c_per_s
    };
    for (double value : values) {
        if (!std::isfinite(value) || value <= 0.0) return false;
    }
    // Fixed-capacity firmware buffers support these windows at a 1 Hz cadence.
    return c.min_on_s >= 2.0 && c.min_off_s >= 2.0 &&
        c.dose_fraction <= 1.0 && c.learning_fraction <= 1.0 &&
        c.observe_coast_s <= c.max_observe_coast_s &&
        c.minimum_gain_c_per_on_s <= c.initial_gain_c_per_on_s &&
        c.initial_gain_c_per_on_s <= c.maximum_gain_c_per_on_s &&
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
    gain_ = config_.initial_gain_c_per_on_s;
    phase_ = Phase::Idle;
    start_c_ = start_s_ = off_s_ = pulse_budget_s_ = actual_on_s_ = 0.0;
    coast_min_c_ = infinity();
    clearMeasurements();
    emit(Phase::Idle, nanValue());
}

void Controller::resetTransient() {
    phase_ = Phase::Idle;
    actual_on_s_ = 0.0;
    start_c_ = nanValue();
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
    output_ = {phase, pump_on_, pump_on_ && !std::isfinite(pulse_budget_s_),
        temperature_c_, rate_c_per_s_, setpoint_c_, learning_updates_, gain_,
        pulse_budget_s_, endpoint, actual_on_s_};
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
    const double endpoint = temperature_c_ + std::min(0.0, rate_c_per_s_) * cfg.stop_horizon_s;
    if (phase_ == Phase::Coast) {
        coast_min_c_ = std::min(coast_min_c_, temperature_c_);
        const double coast_age = t - off_s_;
        const bool settled = std::abs(rate_c_per_s_) <= cfg.settled_rate_c_per_s;
        if (coast_age >= cfg.max_observe_coast_s || (coast_age >= cfg.observe_coast_s && settled)) {
            const double drop = std::max(0.0, start_c_ - coast_min_c_);
            if (actual_on_s_ >= cfg.min_on_s && drop >= 0.03125) {
                const double gain = std::min(cfg.maximum_gain_c_per_on_s,
                    std::max(cfg.minimum_gain_c_per_on_s, drop / actual_on_s_));
                const double alpha = learning_updates_ == 0 ? 1.0 : cfg.learning_fraction;
                gain_ += alpha * (gain - gain_);
                ++learning_updates_;
            } else if (actual_on_s_ >= cfg.min_on_s && error > cfg.unresolved_error_c) {
                gain_ = std::max(cfg.minimum_gain_c_per_on_s, gain_ * 0.5);
                ++learning_updates_;
            }
            phase_ = Phase::Idle;
        }
    }
    if (phase_ == Phase::Idle && error > cfg.deadband_c && endpoint > setpoint_c_) {
        const double pulse = learning_updates_ == 0
            ? (error > cfg.far_error_c ? cfg.far_probe_s : cfg.initial_probe_s)
            : cfg.dose_fraction * std::max(0.0, error) / gain_;
        pulse_budget_s_ = pulse >= cfg.saturation_dose_s
            ? infinity() : std::max(cfg.min_on_s, pulse);
        if (switchPump(t, true)) {
            phase_ = Phase::Cool;
            start_s_ = t;
            start_c_ = temperature_c_;
        }
    }
    if (phase_ == Phase::Cool) {
        const double elapsed = t - start_s_;
        if (elapsed >= pulse_budget_s_ || endpoint <= setpoint_c_ || error <= 0.0 ||
            raw_c_ <= setpoint_c_ - cfg.deadband_c) {
            if (switchPump(t, false)) {
                phase_ = Phase::Coast;
                off_s_ = t;
                actual_on_s_ = elapsed;
                coast_min_c_ = temperature_c_;
            }
        }
    }
    return emit(phase_, endpoint);
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

} // namespace AdaptiveCooling
