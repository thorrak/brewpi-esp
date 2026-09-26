/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "GlycolCoolingController.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace GlycolCooling {
namespace {
double nanValue() { return std::numeric_limits<double>::quiet_NaN(); }
bool validTemperature(double value) {
    return std::isfinite(value) && value >= -55.0 && value <= 125.0;
}
// Both frozen implementations use the same phase vocabulary. Assert the
// mapping at compile time so changing either core cannot silently misreport it.
#define CHECK_PHASE(name) \
    static_assert(static_cast<unsigned>(Phase::name) == static_cast<unsigned>(PredictiveCooling::Phase::name), "predictive phase mismatch"); \
    static_assert(static_cast<unsigned>(Phase::name) == static_cast<unsigned>(AdaptiveCooling::Phase::name), "pulse-dose phase mismatch")
CHECK_PHASE(Idle);
CHECK_PHASE(Cool);
CHECK_PHASE(Coast);
CHECK_PHASE(DisabledOrSensorFault);
CHECK_PHASE(SetpointChangeWait);
#undef CHECK_PHASE

}
Controller::Controller(Algorithm initial, const PredictiveCooling::Config& predictive,
                       const AdaptiveCooling::Config& dose)
    : predictive_(predictive), dose_(dose) { reset(initial); }

void Controller::reset(Algorithm initial) {
    predictive_.reset(); dose_.reset();
    active_ = requested_ = initial;
    last_time_s_ = nanValue();
    last_edge_s_ = -std::numeric_limits<double>::infinity();
    off_minimum_s_ = 0;
    handoff_wait_ = false;
    fresh_ = true;
    copyActiveOutput();
}

void Controller::copyActiveOutput() {
    if (active_ == Algorithm::PulseDose) {
        const auto& o = dose_.output();
        output_ = {static_cast<Phase>(o.phase), o.pump_on, o.full_cooling,
            o.temperature_c, o.rate_c_per_s, o.setpoint_c, o.learning_updates,
            nanValue(), nanValue(), 0, o.gain_c_per_on_s, o.pulse_budget_s,
            o.predicted_endpoint_c, o.actual_on_s};
    } else {
        const auto& o = predictive_.output();
        output_ = {static_cast<Phase>(o.phase), o.pump_on, o.full_cooling,
            o.temperature_c, o.rate_c_per_s, o.setpoint_c, o.learning_updates,
            o.coast_s, o.budget_gain_c_per_s, o.response_updates, nanValue(),
            o.pulse_budget_s, o.predicted_endpoint_c, o.actual_on_s};
    }
}

double Controller::minOnSeconds() const {
    return active_ == Algorithm::PulseDose ? dose_.configuration().min_on_s
                                         : predictive_.configuration().min_on_s;
}
double Controller::minOffSeconds() const {
    return active_ == Algorithm::PulseDose ? dose_.configuration().min_off_s
                                         : predictive_.configuration().min_off_s;
}
const char* Controller::algorithmVersion() const {
    return active_ == Algorithm::PulseDose ? "adaptive-pulse-dose-v1" : "predictive-coast-v1";
}
const char* Controller::phaseName(Phase phase) {
    switch (phase) {
        case Phase::Idle: return "IDLE";
        case Phase::Cool: return "COOL";
        case Phase::Coast: return "COAST";
        case Phase::SetpointChangeWait: return "SETPOINT_CHANGE_WAIT";
        case Phase::AlgorithmSwitchWait: return "ALGORITHM_SWITCH_WAIT";
        default: return "DISABLED_OR_SENSOR_FAULT";
    }
}
double Controller::validClock(double t) const {
    if (!std::isfinite(t) || t < 0 || (std::isfinite(last_time_s_) && t < last_time_s_))
        return std::isfinite(last_time_s_) ? last_time_s_ : 0;
    return t;
}
void Controller::trackEdge(bool was_on, double t) {
    if (was_on != output_.pump_on) {
        last_edge_s_ = t;
        if (!output_.pump_on) off_minimum_s_ = minOffSeconds();
    }
}
void Controller::inhibitCores(double t) {
    predictive_.inhibit(t); dose_.inhibit(t);
    fresh_ = false;
}
Output Controller::inhibit(double t) {
    t = validClock(t);
    bool was_on = output_.pump_on;
    inhibitCores(t);
    copyActiveOutput();
    trackEdge(was_on, t); // capture the outgoing algorithm's minimum OFF time
    if (active_ != requested_) {
        active_ = requested_;
        handoff_wait_ = true;
        copyActiveOutput();
    }
    last_time_s_ = t;
    return output_;
}
void Controller::externalOff(double t) {
    t = validClock(t);
    inhibitCores(t);
    copyActiveOutput();
    last_time_s_ = last_edge_s_ = t;
    off_minimum_s_ = minOffSeconds();
    handoff_wait_ = true;
}
Output Controller::step(double t, double sensor, double setpoint, bool connected) {
    // Faults override even a duplicate tick or a pending minimum-ON switch.
    if (validClock(t) != t || !connected || !validTemperature(sensor) ||
        !validTemperature(setpoint)) return inhibit(t);
    if (t == last_time_s_) return output_;
    last_time_s_ = t;

    if (active_ != requested_) {
        if (output_.pump_on && t - last_edge_s_ < minOnSeconds()) {
            output_.phase = Phase::AlgorithmSwitchWait;
            return output_; // hold the real ON slice; do not train an interrupted response
        }
        const bool was_on = output_.pump_on;
        if (!fresh_) inhibitCores(t);
        copyActiveOutput();
        trackEdge(was_on, t);
        active_ = requested_;
        copyActiveOutput();
        handoff_wait_ = !fresh_;
        // inhibit records this tick in both cores. Do not feed a duplicate tick
        // as a new observation. A fresh boot selection needs no such handoff.
        if (!fresh_) {
            output_.phase = Phase::AlgorithmSwitchWait;
            return output_;
        }
    }

    // This gate survives faults, mode changes, cancellation and repeated
    // requests. The inactive core's private OFF timestamp is never trusted.
    if (handoff_wait_ && !output_.pump_on && t - last_edge_s_ < std::max(off_minimum_s_, minOffSeconds())) {
        if (handoff_wait_) output_.phase = Phase::AlgorithmSwitchWait;
        return output_;
    }
    const bool was_on = output_.pump_on;
    if (active_ == Algorithm::PulseDose) dose_.step(t, sensor, setpoint, connected);
    else predictive_.step(t, sensor, setpoint, connected);
    copyActiveOutput();
    trackEdge(was_on, t);
    handoff_wait_ = false;
    fresh_ = false;
    return output_;
}
} // namespace GlycolCooling
