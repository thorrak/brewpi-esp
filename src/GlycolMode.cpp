#include "GlycolMode.h"

#include <math.h>

#include "GlycolLog.h"
#include "Ticks.h"

#if TEMP_CONTROL_STATIC
extern ValueActuator defaultActuator;
#endif

namespace {

bool stateIsHeating(const GlycolMode::Context& ctx) {
    return ctx.state == HEATING || ctx.state == HEATING_MIN_TIME;
}

bool heatingCapable(const GlycolMode::Context& ctx) {
    return (ctx.heater != &defaultActuator) ||
           (ctx.cc.lightAsHeater && (ctx.light != &defaultActuator));
}

void resetHeatingWindow(GlycolMode::Context& ctx) {
    ctx.runtime.heating_window_start_ms = 0;
    ctx.runtime.heating_window_on_time_s = 0;
}

void resetWaitTime(GlycolMode::Context& ctx) {
    ctx.waitTime = 0;
}

uint16_t timeSinceHeating(const GlycolMode::Context& ctx) {
    return ticks.timeSince(ctx.lastHeatTime);
}

uint16_t timeSinceCooling(const GlycolMode::Context& ctx) {
    return ticks.timeSince(ctx.lastCoolTime);
}

float estimateCoast(const GlycolMode::Context& ctx) {
    float rate = fabsf(ctx.runtime.current_cooling_rate);
    if (rate > ctx.config.min_rate_for_k_model) {
        return ctx.learned.k * rate;
    }
    return ctx.learned.C_off;
}

void addRateSample(GlycolMode::Context& ctx, float temp) {
    uint32_t now = millis();
    ctx.runtime.rate_buffer[ctx.runtime.rate_buffer_head].timestamp_ms = now;
    ctx.runtime.rate_buffer[ctx.runtime.rate_buffer_head].temp = temp;

    ctx.runtime.rate_buffer_head = (ctx.runtime.rate_buffer_head + 1) % RATE_BUFFER_SIZE;
    if (ctx.runtime.rate_buffer_count < RATE_BUFFER_SIZE) {
        ctx.runtime.rate_buffer_count++;
    }
}

float calculateRate(const GlycolMode::Context& ctx) {
    if (ctx.runtime.rate_buffer_count < 5) {
        return 0.0f;
    }

    uint8_t oldest_idx =
        (ctx.runtime.rate_buffer_head + RATE_BUFFER_SIZE - ctx.runtime.rate_buffer_count) % RATE_BUFFER_SIZE;
    uint8_t newest_idx = (ctx.runtime.rate_buffer_head + RATE_BUFFER_SIZE - 1) % RATE_BUFFER_SIZE;

    uint32_t time_span =
        ctx.runtime.rate_buffer[newest_idx].timestamp_ms -
        ctx.runtime.rate_buffer[oldest_idx].timestamp_ms;

    if (time_span > 180000) {
        return 0.0f;
    }

    float sum_t = 0;
    float sum_temp = 0;
    float sum_t2 = 0;
    float sum_t_temp = 0;
    float t0 = ctx.runtime.rate_buffer[oldest_idx].timestamp_ms;
    int n = ctx.runtime.rate_buffer_count;

    for (int i = 0; i < n; i++) {
        uint8_t idx = (oldest_idx + i) % RATE_BUFFER_SIZE;
        float t = (ctx.runtime.rate_buffer[idx].timestamp_ms - t0) / 60000.0f;
        float temp = ctx.runtime.rate_buffer[idx].temp;
        sum_t += t;
        sum_temp += temp;
        sum_t2 += t * t;
        sum_t_temp += t * temp;
    }

    float denominator = n * sum_t2 - sum_t * sum_t;
    if (fabsf(denominator) < 0.0001f) {
        return 0.0f;
    }

    return (n * sum_t_temp - sum_t * sum_temp) / denominator;
}

bool shouldStartCooling(const GlycolMode::Context& ctx) {
    if (ctx.cs.beerSetting == INVALID_TEMP) {
        return false;
    }

    float current_temp = tempToDouble(ctx.beerSensor->readFastFiltered(), 2);
    float setpoint = tempToDouble(ctx.cs.beerSetting, 2);
    float lookahead_s = ctx.learned.L + 30.0f;
    float lookahead_min = lookahead_s / 60.0f;
    float anticipated_peak = current_temp + (ctx.learned.drift_rate * lookahead_min);
    return anticipated_peak >= (setpoint + ctx.config.trigger_margin);
}

bool shouldStopCooling(const GlycolMode::Context& ctx) {
    if (ctx.cs.beerSetting == INVALID_TEMP) {
        return false;
    }

    float current_temp = tempToDouble(ctx.beerSensor->readFastFiltered(), 2);
    float setpoint = tempToDouble(ctx.cs.beerSetting, 2);
    float predicted_final = current_temp - estimateCoast(ctx);
    return predicted_final <= setpoint;
}

bool shouldStartHeating(const GlycolMode::Context& ctx) {
    if (ctx.cs.beerSetting == INVALID_TEMP) {
        return false;
    }
    if (!heatingCapable(ctx) || ctx.cc.pidMax_heat <= 0) {
        return false;
    }

    float current_temp = tempToDouble(ctx.beerSensor->readFastFiltered(), 2);
    float setpoint = tempToDouble(ctx.cs.beerSetting, 2);

    return ctx.runtime.heating_output > 0 &&
           current_temp <= (setpoint - ctx.config.trigger_margin);
}

bool shouldStopHeating(const GlycolMode::Context& ctx) {
    if (ctx.cs.beerSetting == INVALID_TEMP) {
        return true;
    }
    if (!heatingCapable(ctx) || ctx.runtime.heating_output <= 0) {
        return true;
    }

    float current_temp = tempToDouble(ctx.beerSensor->readFastFiltered(), 2);
    float setpoint = tempToDouble(ctx.cs.beerSetting, 2);
    return current_temp >= setpoint;
}

uint16_t heatingOnTime(const GlycolMode::Context& ctx) {
    uint16_t windowPeriod = ctx.minTimes.GLYCOL_WINDOW_PERIOD;
    if (windowPeriod == 0 || ctx.cc.pidMax_heat <= 0 || ctx.runtime.heating_output <= 0) {
        return 0;
    }

    uint32_t onTime = ((uint32_t) ctx.runtime.heating_output * windowPeriod) /
                      (uint32_t) ctx.cc.pidMax_heat;

    if (onTime > 0 && onTime < ctx.minTimes.GLYCOL_MIN_ON_TIME) {
        onTime = ctx.minTimes.GLYCOL_MIN_ON_TIME;
    }
    if (onTime > windowPeriod) {
        onTime = windowPeriod;
    }
    return onTime;
}

GlycolHeatingWindowState getHeatingWindowState(GlycolMode::Context& ctx, uint32_t now) {
    GlycolHeatingWindowState windowState{};
    windowState.period_s = ctx.minTimes.GLYCOL_WINDOW_PERIOD;
    if (windowState.period_s == 0) {
        windowState.period_s = 1;
    }

    uint32_t windowPeriodMs = (uint32_t) windowState.period_s * 1000UL;
    if (ctx.runtime.heating_window_start_ms == 0) {
        ctx.runtime.heating_window_start_ms = now;
    } else if ((now - ctx.runtime.heating_window_start_ms) >= windowPeriodMs) {
        uint32_t elapsedMs = now - ctx.runtime.heating_window_start_ms;
        ctx.runtime.heating_window_start_ms = now - (elapsedMs % windowPeriodMs);
    }

    ctx.runtime.heating_window_on_time_s = heatingOnTime(ctx);
    windowState.on_time_s = ctx.runtime.heating_window_on_time_s;
    windowState.elapsed_in_window_s = (now - ctx.runtime.heating_window_start_ms) / 1000UL;
    if (windowState.elapsed_in_window_s > windowState.period_s) {
        windowState.elapsed_in_window_s = windowState.period_s;
    }
    windowState.on_slice_active =
        windowState.on_time_s > 0 &&
        windowState.elapsed_in_window_s < windowState.on_time_s;
    return windowState;
}

GlycolHeatingGateResult getHeatingGate(const GlycolMode::Context& ctx, bool startingNewOnSlice) {
    GlycolHeatingGateResult gateResult{
        true,
        0,
        GLYCOL_HEATING_WAIT_NONE,
    };

    if (!startingNewOnSlice) {
        return gateResult;
    }

    uint16_t heatOffWait = 0;
    uint16_t sinceHeatingS = timeSinceHeating(ctx);
    if (sinceHeatingS < ctx.minTimes.MIN_HEAT_OFF_TIME) {
        heatOffWait = ctx.minTimes.MIN_HEAT_OFF_TIME - sinceHeatingS;
    }

    uint16_t switchWait = 0;
    uint16_t sinceCoolingS = timeSinceCooling(ctx);
    if (sinceCoolingS < ctx.minTimes.MIN_SWITCH_TIME) {
        switchWait = ctx.minTimes.MIN_SWITCH_TIME - sinceCoolingS;
    }

    gateResult.wait_time_s = max(heatOffWait, switchWait);
    if (gateResult.wait_time_s == 0) {
        return gateResult;
    }

    gateResult.allowed = false;
    gateResult.reason =
        (heatOffWait >= switchWait && heatOffWait > 0)
            ? GLYCOL_HEATING_WAIT_HEAT_OFF_DELAY
            : GLYCOL_HEATING_WAIT_SWITCH_DELAY;
    return gateResult;
}

void setHeatingWaitState(
    GlycolMode::Context& ctx,
    uint16_t waitTimeS,
    GlycolHeatingWaitReason reason,
    bool resetWindow
) {
    if (resetWindow) {
        resetHeatingWindow(ctx);
    }
    ctx.runtime.heating_wait_reason = reason;
    ctx.state = WAITING_TO_HEAT;
    ctx.waitTime = waitTimeS;
    ctx.lastIdleTime = ticks.seconds();
}

void setHeatingActiveState(GlycolMode::Context& ctx, uint16_t elapsedInWindowS) {
    ctx.runtime.heating_wait_reason = GLYCOL_HEATING_WAIT_NONE;
    ctx.state =
        (elapsedInWindowS < ctx.minTimes.GLYCOL_MIN_ON_TIME) ? HEATING_MIN_TIME : HEATING;
    ctx.lastHeatTime = ticks.seconds();
    resetWaitTime(ctx);
}

bool isEmergency(const GlycolMode::Context& ctx) {
    if (ctx.cs.beerSetting == INVALID_TEMP) {
        return false;
    }

    float current_temp = tempToDouble(ctx.beerSensor->readFastFiltered(), 2);
    float setpoint = tempToDouble(ctx.cs.beerSetting, 2);
    if (current_temp <= setpoint) {
        return false;
    }
    if (ctx.runtime.cooling_duration_s < ctx.config.emergency_detection_time_s) {
        return false;
    }

    float predicted_at_horizon =
        current_temp + (ctx.runtime.current_cooling_rate * ctx.config.emergency_horizon_min);
    return predicted_at_horizon > (setpoint + 0.1f);
}

bool canExitEmergency(const GlycolMode::Context& ctx) {
    if (ctx.cs.beerSetting == INVALID_TEMP) {
        return false;
    }

    float current_temp = tempToDouble(ctx.beerSensor->readFastFiltered(), 2);
    float setpoint = tempToDouble(ctx.cs.beerSetting, 2);

    uint32_t emergency_duration = (millis() - ctx.runtime.emergency_entry_time) / 1000;
    if (emergency_duration < ctx.config.min_emergency_dwell_time_s) {
        return false;
    }

    if (current_temp <= setpoint) {
        return true;
    }

    if (ctx.runtime.current_cooling_rate < -ctx.config.min_rate_for_k_model) {
        float temp_diff = current_temp - setpoint;
        float rate = fabsf(ctx.runtime.current_cooling_rate);
        if (rate > 0.001f) {
            float time_to_setpoint = temp_diff / rate;
            if (time_to_setpoint < ctx.config.emergency_horizon_min / 2.0f) {
                return true;
            }
        }
    }

    return false;
}

void transitionToIdle(GlycolMode::Context& ctx) {
#ifdef ENABLE_GLYCOL_LOGGING
    GlycolState prev_state = ctx.runtime.state;
#endif
    ctx.runtime.state = GLYCOL_IDLE;
    ctx.runtime.setpoint_changed_this_cycle = false;
    resetHeatingWindow(ctx);
    ctx.runtime.heating_wait_reason = GLYCOL_HEATING_WAIT_NONE;
    ctx.state = IDLE;
    ctx.lastIdleTime = ticks.seconds();
    resetWaitTime(ctx);

#ifdef ENABLE_GLYCOL_LOGGING
    float current_temp = tempToDouble(ctx.beerSensor->readFastFiltered(), 2);
    float setpoint = tempToDouble(ctx.cs.beerSetting, 2);
    glycolLog.logTransition(
        prev_state, GLYCOL_IDLE,
        current_temp, setpoint,
        ctx.runtime.current_cooling_rate,
        ctx.runtime.cooling_duration_s,
        estimateCoast(ctx),
        ctx.learned.k, ctx.learned.C_off, ctx.learned.L,
        ctx.runtime.force_minimum_cooling,
        "Transition to idle"
    );
#endif
}

void transitionToCooling(GlycolMode::Context& ctx) {
#ifdef ENABLE_GLYCOL_LOGGING
    GlycolState prev_state = ctx.runtime.state;
    float setpoint = tempToDouble(ctx.cs.beerSetting, 2);
#endif

    ctx.runtime.state = GLYCOL_COOLING;
    ctx.runtime.t_pump_on = millis();
    ctx.runtime.temp_at_pump_on = tempToDouble(ctx.beerSensor->readFastFiltered(), 2);
    ctx.runtime.cooling_confirmed = false;
    ctx.runtime.negative_rate_count = 0;
    ctx.runtime.setpoint_changed_this_cycle = false;
    ctx.runtime.cooling_duration_s = 0;
    resetHeatingWindow(ctx);
    ctx.runtime.heating_wait_reason = GLYCOL_HEATING_WAIT_NONE;
    ctx.runtime.rate_buffer_count = 0;
    ctx.runtime.rate_buffer_head = 0;

    ctx.state = COOLING;
    ctx.lastCoolTime = ticks.seconds();

#ifdef ENABLE_GLYCOL_LOGGING
    const char* reason = ctx.runtime.force_minimum_cooling
        ? "Starting cooling (forced minimum)"
        : "Starting cooling (prediction-based)";
    glycolLog.logTransition(
        prev_state, GLYCOL_COOLING,
        ctx.runtime.temp_at_pump_on, setpoint,
        ctx.runtime.current_cooling_rate,
        0,
        estimateCoast(ctx),
        ctx.learned.k, ctx.learned.C_off, ctx.learned.L,
        ctx.runtime.force_minimum_cooling,
        reason
    );
#endif
}

void transitionToCoasting(GlycolMode::Context& ctx) {
#ifdef ENABLE_GLYCOL_LOGGING
    GlycolState prev_state = ctx.runtime.state;
    float setpoint = tempToDouble(ctx.cs.beerSetting, 2);
#endif

    ctx.runtime.state = GLYCOL_COASTING;
    ctx.runtime.t_pump_off = millis();
    ctx.runtime.temp_at_pump_off = tempToDouble(ctx.beerSensor->readFastFiltered(), 2);
    ctx.runtime.min_temp_reached = ctx.runtime.temp_at_pump_off;
    ctx.runtime.cooling_rate_at_pump_off = ctx.runtime.current_cooling_rate;
    ctx.runtime.cooling_duration_s =
        (ctx.runtime.t_pump_off - ctx.runtime.t_pump_on) / 1000;

    bool longRun = ctx.runtime.cooling_duration_s > ctx.config.hot_glycol_threshold_s;
    if (longRun) {
        ctx.runtime.force_minimum_cooling = true;
    }

    ctx.state = IDLE;
    ctx.lastIdleTime = ticks.seconds();

#ifdef ENABLE_GLYCOL_LOGGING
    const char* reason = longRun
        ? "Stopping cooling (long run - forcing min next)"
        : "Stopping cooling (coast prediction)";
    glycolLog.logTransition(
        prev_state, GLYCOL_COASTING,
        ctx.runtime.temp_at_pump_off, setpoint,
        ctx.runtime.cooling_rate_at_pump_off,
        ctx.runtime.cooling_duration_s,
        estimateCoast(ctx),
        ctx.learned.k, ctx.learned.C_off, ctx.learned.L,
        ctx.runtime.force_minimum_cooling,
        reason
    );
#endif
}

void transitionToEmergency(GlycolMode::Context& ctx) {
#ifdef ENABLE_GLYCOL_LOGGING
    GlycolState prev_state = ctx.runtime.state;
    float current_temp = tempToDouble(ctx.beerSensor->readFastFiltered(), 2);
    float setpoint = tempToDouble(ctx.cs.beerSetting, 2);
#endif

    ctx.runtime.state = GLYCOL_EMERGENCY_COOLING;
    ctx.runtime.emergency_entry_time = millis();
    resetHeatingWindow(ctx);
    ctx.runtime.heating_wait_reason = GLYCOL_HEATING_WAIT_NONE;

    ctx.state = COOLING;
    ctx.lastCoolTime = ticks.seconds();

#ifdef ENABLE_GLYCOL_LOGGING
    glycolLog.logTransition(
        prev_state, GLYCOL_EMERGENCY_COOLING,
        current_temp, setpoint,
        ctx.runtime.current_cooling_rate,
        ctx.runtime.cooling_duration_s,
        estimateCoast(ctx),
        ctx.learned.k, ctx.learned.C_off, ctx.learned.L,
        ctx.runtime.force_minimum_cooling,
        "EMERGENCY - cannot keep up with cooling demand"
    );
#endif
}

void transitionToHeating(GlycolMode::Context& ctx) {
#ifdef ENABLE_GLYCOL_LOGGING
    GlycolState prev_state = ctx.runtime.state;
    float current_temp = tempToDouble(ctx.beerSensor->readFastFiltered(), 2);
    float setpoint = tempToDouble(ctx.cs.beerSetting, 2);
#endif

    ctx.runtime.state = GLYCOL_HEATING;
    resetHeatingWindow(ctx);
    ctx.runtime.heating_wait_reason = GLYCOL_HEATING_WAIT_NONE;
    ctx.runtime.setpoint_changed_this_cycle = false;

    ctx.state = WAITING_TO_HEAT;
    ctx.lastIdleTime = ticks.seconds();
    resetWaitTime(ctx);

#ifdef ENABLE_GLYCOL_LOGGING
    glycolLog.logTransition(
        prev_state, GLYCOL_HEATING,
        current_temp, setpoint,
        ctx.runtime.current_cooling_rate,
        0,
        estimateCoast(ctx),
        ctx.learned.k, ctx.learned.C_off, ctx.learned.L,
        ctx.runtime.force_minimum_cooling,
        "Starting heating (time-proportional PID)"
    );
#endif
}

bool updateLearning(GlycolMode::Context& ctx) {
    if (ctx.runtime.force_minimum_cooling) {
        ctx.runtime.force_minimum_cooling = false;
        logDebug("Glycol: Cleared force_minimum_cooling after cycle");
    }

    bool cycle_valid =
        ctx.runtime.cooling_duration_s >= ctx.config.min_training_duration_s &&
        (ctx.runtime.temp_at_pump_on - ctx.runtime.temp_at_pump_off) >= ctx.config.min_training_drop &&
        !ctx.runtime.setpoint_changed_this_cycle &&
        fabsf(ctx.runtime.cooling_rate_at_pump_off) > ctx.config.min_training_rate;

    if (!cycle_valid) {
        logDebug("Glycol: Cycle not valid for training");
        return false;
    }

    float actual_coast = ctx.runtime.temp_at_pump_off - ctx.runtime.min_temp_reached;
    float setpoint = tempToDouble(ctx.cs.beerSetting, 2);

    ctx.learned.C_off = 0.85f * ctx.learned.C_off + 0.15f * actual_coast;
    ctx.learned.C_off = constrain(ctx.learned.C_off, 0.05f, 2.0f);

    if (fabsf(ctx.runtime.cooling_rate_at_pump_off) > ctx.config.min_training_rate) {
        float observed_k = actual_coast / fabsf(ctx.runtime.cooling_rate_at_pump_off);
        observed_k = constrain(observed_k, 0.5f, 20.0f);

        float implied_coast_at_typical_rate = observed_k * 0.05f;
        float alpha;

        if (implied_coast_at_typical_rate > ctx.learned.C_off * 3.0f ||
            implied_coast_at_typical_rate < ctx.learned.C_off * 0.3f) {
            alpha = 0.05f;
        } else {
            float prediction_error = setpoint - ctx.runtime.min_temp_reached;

            if (fabsf(prediction_error) > 0.5f) {
                alpha = 0.5f;
            } else if (fabsf(prediction_error) > 0.2f) {
                alpha = 0.3f;
            } else {
                alpha = 0.15f;
            }
        }

        ctx.learned.k = (1.0f - alpha) * ctx.learned.k + alpha * observed_k;
        ctx.learned.k = constrain(ctx.learned.k, 1.0f, 15.0f);
    }

    logDebug("Glycol: Learning update - k=%.2f, C_off=%.3f", ctx.learned.k, ctx.learned.C_off);
    return true;
}

} // namespace

namespace GlycolMode {

void updatePID(Context& ctx, unsigned char& integralUpdateCounter) {
    ctx.cs.fridgeSetting = INVALID_TEMP;

    bool heatingRequested = heatingCapable(ctx) && (ctx.cv.beerDiff > 0) && (ctx.cc.pidMax_heat > 0);
    if (heatingRequested) {
        if (integralUpdateCounter++ == 60) {
            integralUpdateCounter = 0;

            temperature integratorUpdate = ctx.cv.beerDiff;
            bool coolingActive =
                ctx.runtime.state == GLYCOL_COOLING ||
                ctx.runtime.state == GLYCOL_COASTING ||
                ctx.runtime.state == GLYCOL_EMERGENCY_COOLING;

            if (coolingActive) {
                integratorUpdate = 0;
            } else if (abs(integratorUpdate) >= ctx.cc.iMaxError) {
                integratorUpdate = -(ctx.cv.diffIntegral >> 3);
            } else {
                long_temperature projectedIntegral = ctx.cv.diffIntegral + integratorUpdate;
                if (projectedIntegral < 0) {
                    projectedIntegral = 0;
                }

                temperature pTerm = multiplyFactorTemperatureDiff(ctx.cc.Kp_heat, ctx.cv.beerDiff);
                temperature iTerm = multiplyFactorTemperatureDiffLong(ctx.cc.Ki_heat, projectedIntegral);
                temperature dTerm = multiplyFactorTemperatureDiff(ctx.cc.Kd_heat, ctx.cv.beerSlope);
                long_temperature projectedOutput = (long_temperature) pTerm + iTerm + dTerm;

                if (projectedOutput >= ctx.cc.pidMax_heat) {
                    integratorUpdate = 0;
                }
            }

            ctx.cv.diffIntegral += integratorUpdate;
            if (ctx.cv.diffIntegral < 0) {
                ctx.cv.diffIntegral = 0;
            }
        }

        ctx.cv.p = multiplyFactorTemperatureDiff(ctx.cc.Kp_heat, ctx.cv.beerDiff);
        ctx.cv.i = multiplyFactorTemperatureDiffLong(ctx.cc.Ki_heat, ctx.cv.diffIntegral);
        ctx.cv.d = multiplyFactorTemperatureDiff(ctx.cc.Kd_heat, ctx.cv.beerSlope);

        long_temperature heatingOutput = (long_temperature) ctx.cv.p + ctx.cv.i + ctx.cv.d;
        if (heatingOutput < 0) {
            heatingOutput = 0;
        }
        ctx.runtime.heating_output = constrainTemp(heatingOutput, 0, ctx.cc.pidMax_heat);
        return;
    }

    ctx.cv.p = 0;
    ctx.cv.i = 0;
    ctx.cv.d = 0;
    ctx.cv.diffIntegral = 0;
    ctx.runtime.heating_output = 0;
}

bool updateState(Context& ctx) {
    bool learnedParamsChanged = false;

    if (ctx.cs.beerSetting == INVALID_TEMP) {
        transitionToIdle(ctx);
        return false;
    }

    float current_temp = tempToDouble(ctx.beerSensor->readFastFiltered(), 2);
    float setpoint = tempToDouble(ctx.cs.beerSetting, 2);
    addRateSample(ctx, current_temp);
    ctx.runtime.current_cooling_rate = calculateRate(ctx);

    if (current_temp < (setpoint - ctx.config.safety_margin_low)) {
        if (ctx.runtime.state == GLYCOL_COOLING ||
            ctx.runtime.state == GLYCOL_EMERGENCY_COOLING) {
            logDebug("Glycol: Safety limit - beer too cold");
            transitionToIdle(ctx);
            return false;
        }
    }

    switch (ctx.runtime.state) {
        case GLYCOL_IDLE: {
            if (shouldStartHeating(ctx)) {
                transitionToHeating(ctx);
                break;
            }

            if (ctx.runtime.current_cooling_rate > 0) {
                ctx.learned.drift_rate = 0.85f * ctx.learned.drift_rate +
                                         0.15f * ctx.runtime.current_cooling_rate;
                ctx.learned.drift_rate = constrain(ctx.learned.drift_rate, 0.0f, 0.5f);
            }

            uint16_t time_since_pump_off = (millis() - ctx.runtime.t_pump_off) / 1000;
            bool min_off_elapsed = (ctx.runtime.t_pump_off == 0) ||
                                   (time_since_pump_off >= ctx.config.min_off_time_s);

            if (min_off_elapsed && shouldStartCooling(ctx)) {
                transitionToCooling(ctx);
            } else {
                ctx.state = IDLE;
                ctx.lastIdleTime = ticks.seconds();
                resetWaitTime(ctx);
            }
            break;
        }

        case GLYCOL_COOLING: {
            ctx.runtime.cooling_duration_s = (millis() - ctx.runtime.t_pump_on) / 1000;

            if (!ctx.runtime.cooling_confirmed) {
                if (ctx.runtime.current_cooling_rate < -0.01f) {
                    ctx.runtime.negative_rate_count++;
                    if (ctx.runtime.negative_rate_count >= 3) {
                        float L_observed = (millis() - ctx.runtime.t_pump_on) / 1000.0f;
                        L_observed = constrain(L_observed, 5.0f, 300.0f);
                        ctx.learned.L = 0.8f * ctx.learned.L + 0.2f * L_observed;
                        ctx.runtime.cooling_confirmed = true;
                    }
                } else {
                    ctx.runtime.negative_rate_count = 0;
                }
            }

            uint32_t max_on_s = ctx.config.max_continuous_on_time_min * 60;
            if (ctx.runtime.cooling_duration_s > max_on_s) {
                logDebug("Glycol: Max continuous on time exceeded");
                transitionToCoasting(ctx);
                return false;
            }

            if (isEmergency(ctx)) {
                transitionToEmergency(ctx);
                return false;
            }

            if (ctx.runtime.cooling_duration_s < ctx.config.min_on_time_s) {
                ctx.state = COOLING_MIN_TIME;
                break;
            }
            ctx.state = COOLING;

            if (ctx.runtime.force_minimum_cooling) {
                logDebug("Glycol: Forced minimum cooling - stopping to re-learn");
                transitionToCoasting(ctx);
                break;
            }

            if (shouldStopCooling(ctx)) {
                transitionToCoasting(ctx);
            }
            break;
        }

        case GLYCOL_COASTING: {
            if (current_temp < ctx.runtime.min_temp_reached) {
                ctx.runtime.min_temp_reached = current_temp;
            }

            bool stabilized = (ctx.runtime.current_cooling_rate >= -0.005f);
            uint16_t time_since_pump_off = (millis() - ctx.runtime.t_pump_off) / 1000;
            uint16_t observation_time = max(ctx.config.min_off_time_s, (uint16_t) ctx.learned.L);
            bool min_off_elapsed = time_since_pump_off >= observation_time;

            if (stabilized && min_off_elapsed) {
                float actual_coast = ctx.runtime.temp_at_pump_off - ctx.runtime.min_temp_reached;
                bool coast_ineffective = actual_coast < 0.05f;
                bool still_above_setpoint = current_temp > setpoint;

                if (coast_ineffective && still_above_setpoint) {
                    logDebug("Glycol: Coast ineffective, extending cooling");
                    transitionToCooling(ctx);
                } else {
                    learnedParamsChanged = updateLearning(ctx);
                    transitionToIdle(ctx);
                }
            }
            break;
        }

        case GLYCOL_EMERGENCY_COOLING: {
            uint32_t emergency_duration = (millis() - ctx.runtime.emergency_entry_time) / 1000;
            uint32_t max_on_s = ctx.config.max_continuous_on_time_min * 60;

            if (emergency_duration > max_on_s) {
                logDebug("Glycol: Max on time in emergency, forcing off");
                transitionToCoasting(ctx);
                return false;
            }

            if (current_temp < (setpoint - ctx.config.safety_margin_low)) {
                logDebug("Glycol: Safety limit in emergency");
                transitionToIdle(ctx);
                return false;
            }

            if (canExitEmergency(ctx)) {
                logDebug("Glycol: Exiting emergency mode");
                transitionToCooling(ctx);
            }
            break;
        }

        case GLYCOL_HEATING: {
            if (shouldStopHeating(ctx)) {
                logDebug("Glycol: Heating satisfied");
                transitionToIdle(ctx);
                break;
            }

            uint32_t now = millis();
            GlycolHeatingWindowState windowState = getHeatingWindowState(ctx, now);
            if (windowState.on_time_s == 0) {
                transitionToIdle(ctx);
                break;
            }

            GlycolHeatingGateResult gateResult =
                getHeatingGate(ctx, windowState.on_slice_active && !stateIsHeating(ctx));
            if (!gateResult.allowed) {
                setHeatingWaitState(ctx, gateResult.wait_time_s, gateResult.reason, true);
                break;
            }

            if (windowState.on_slice_active) {
                setHeatingActiveState(ctx, windowState.elapsed_in_window_s);
            } else {
                setHeatingWaitState(
                    ctx,
                    windowState.period_s - windowState.elapsed_in_window_s,
                    GLYCOL_HEATING_WAIT_WINDOW_OFF,
                    false
                );
            }
            break;
        }
    }

    return learnedParamsChanged;
}

} // namespace GlycolMode
