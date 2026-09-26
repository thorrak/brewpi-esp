#include <algorithm>
#include "GlycolMode.h"

#include <math.h>

#include "Ticks.h"

#if TEMP_CONTROL_STATIC
extern ValueActuator defaultActuator;
#endif

namespace {

bool stateIsHeating(const GlycolMode::Context& ctx) {
    return ctx.state == HEATING || ctx.state == HEATING_MIN_TIME;
}

bool heatingCapable(const GlycolMode::Context& ctx) {
    return ctx.cc.lightAsHeater
        ? ctx.light != &defaultActuator
        : ctx.heater != &defaultActuator;
}

void resetHeatingWindow(GlycolMode::Context& ctx) {
    ctx.runtime.heating_window_active = false;
    ctx.runtime.heating_window_start_ms = 0;
    ctx.runtime.heating_window_on_time_s = 0;
}

void resetWaitTime(GlycolMode::Context& ctx) {
    ctx.waitTime = 0;
}

// Extend the wrapping MCU millisecond counter; elapsed comparisons remain valid
// across both the legacy 16-bit seconds wrap and the 49-day millis wrap.
double monotonicSeconds(GlycolMode::Context& ctx) {
    auto& runtime = ctx.runtime;
    uint32_t now = ticks.millis();
    if (!runtime.clock_initialized) {
        runtime.clock_initialized = true;
        runtime.clock_elapsed_ms = now;
    } else {
        runtime.clock_elapsed_ms += static_cast<uint32_t>(now - runtime.clock_last_ms);
    }
    runtime.clock_last_ms = now;
    return runtime.clock_elapsed_ms / 1000.0;
}

uint16_t elapsedSeconds(double now, double previous) {
    return static_cast<uint16_t>(std::min(65535.0, std::max(0.0, now - previous)));
}

uint16_t timeSinceHeating(const GlycolMode::Context& ctx) {
    return elapsedSeconds(ctx.runtime.clock_elapsed_ms / 1000.0,
                          ctx.runtime.last_heater_active_s);
}

uint16_t timeSinceCooling(const GlycolMode::Context& ctx) {
    return elapsedSeconds(ctx.runtime.clock_elapsed_ms / 1000.0,
                          ctx.runtime.last_pump_active_s);
}

bool stateIsCooling(const GlycolMode::Context& ctx) {
    return ctx.state == COOLING || ctx.state == COOLING_MIN_TIME;
}

double rawCelsius(temperature value) {
    // BrewPi absolute temperatures include the -48 C offset as well as Q9.
    return (static_cast<int32_t>(value) - C_OFFSET) / 512.0;
}

void trackActiveIntervalEnds(GlycolMode::Context& ctx, double now) {
    if (stateIsHeating(ctx)) {
        ctx.runtime.last_heater_active_s = now;
        ctx.lastHeatTime = ticks.seconds();
    }
    if (stateIsCooling(ctx)) {
        ctx.runtime.last_pump_active_s = now;
        ctx.lastCoolTime = ticks.seconds();
    }
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
    // Latch demand at the beginning of each window. Recomputing it during an
    // off slice can start a late pulse shorter than the minimum on time.
    if (!ctx.runtime.heating_window_active ||
        (now - ctx.runtime.heating_window_start_ms) >= windowPeriodMs) {
        ctx.runtime.heating_window_active = true;
        ctx.runtime.heating_window_start_ms = now;
        ctx.runtime.heating_window_on_time_s = heatingOnTime(ctx);
    }

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

    gateResult.wait_time_s = std::max(heatOffWait, switchWait);
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

void transitionToIdle(GlycolMode::Context& ctx) {
    ctx.runtime.state = GLYCOL_IDLE;
    resetHeatingWindow(ctx);
    ctx.runtime.heating_wait_reason = GLYCOL_HEATING_WAIT_NONE;
    ctx.state = IDLE;
    ctx.lastIdleTime = ticks.seconds();
    resetWaitTime(ctx);
}

void transitionToHeating(GlycolMode::Context& ctx) {
    // Discard any incomplete cooling observation. Heating must never be learned
    // as passive drift or as a weak cooling dose.
    ctx.runtime.cooling_output = ctx.runtime.cooling.inhibit(monotonicSeconds(ctx));
    ctx.runtime.cooling_step_initialized = false;
    ctx.runtime.state = GLYCOL_HEATING;
    resetHeatingWindow(ctx);
    ctx.runtime.heating_wait_reason = GLYCOL_HEATING_WAIT_NONE;
    ctx.state = WAITING_TO_HEAT;
    ctx.lastIdleTime = ticks.seconds();
    resetWaitTime(ctx);
}

void updateHeatingState(GlycolMode::Context& ctx) {
    if (shouldStopHeating(ctx)) {
        transitionToIdle(ctx);
        return;
    }
    GlycolHeatingWindowState windowState = getHeatingWindowState(ctx, ticks.millis());
    if (windowState.on_time_s == 0) {
        transitionToIdle(ctx);
        return;
    }
    GlycolHeatingGateResult gateResult =
        getHeatingGate(ctx, windowState.on_slice_active && !stateIsHeating(ctx));
    if (!gateResult.allowed) {
        setHeatingWaitState(ctx, gateResult.wait_time_s, gateResult.reason, true);
    } else if (windowState.on_slice_active) {
        setHeatingActiveState(ctx, windowState.elapsed_in_window_s);
    } else {
        setHeatingWaitState(ctx, windowState.period_s - windowState.elapsed_in_window_s,
                            GLYCOL_HEATING_WAIT_WINDOW_OFF, false);
    }
}

void applyCoolingOutput(GlycolMode::Context& ctx, bool wasCooling) {
    auto& runtime = ctx.runtime;
    const auto& output = runtime.cooling_output;
    runtime.heating_wait_reason = GLYCOL_HEATING_WAIT_NONE;
    resetHeatingWindow(ctx);
    resetWaitTime(ctx);

    if (output.pump_on) {
        if (!wasCooling) {
            runtime.pump_started_s = runtime.clock_elapsed_ms / 1000.0;
        }
        double activeSeconds = runtime.clock_elapsed_ms / 1000.0 - runtime.pump_started_s;
        runtime.state = output.full_cooling ? GLYCOL_EMERGENCY_COOLING : GLYCOL_COOLING;
        // Full cooling is a diagnostic label, not an overriding emergency mode.
        ctx.state = activeSeconds < runtime.cooling.minOnSeconds()
            ? COOLING_MIN_TIME : COOLING;
        runtime.last_pump_active_s = runtime.clock_elapsed_ms / 1000.0;
        ctx.lastCoolTime = ticks.seconds();
    } else {
        if (wasCooling) {
            runtime.t_pump_off = ticks.millis();
        }
        runtime.state = output.phase == GlycolCooling::Phase::Coast
            ? GLYCOL_COASTING : GLYCOL_IDLE;
        ctx.state = IDLE;
        ctx.lastIdleTime = ticks.seconds();
    }
}

} // namespace

namespace GlycolMode {

void updatePID(Context& ctx, unsigned char& integralUpdateCounter) {
    ctx.cs.fridgeSetting = INVALID_TEMP;
    monotonicSeconds(ctx);
    // UI setters can request an extra update between regular 1 Hz ticks.
    // They must not advance the heating integrator more quickly than real time.
    if (ctx.runtime.pid_step_initialized &&
        ctx.runtime.clock_elapsed_ms - ctx.runtime.pid_last_step_ms < 1000) {
        return;
    }
    ctx.runtime.pid_step_initialized = true;
    ctx.runtime.pid_last_step_ms = ctx.runtime.clock_elapsed_ms;

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

void suspend(Context& ctx) {
    ctx.runtime.cooling.request(ctx.requestedAlgorithm);
    double now = monotonicSeconds(ctx);
    bool wasCooling = stateIsCooling(ctx);
    trackActiveIntervalEnds(ctx, now);
    ctx.runtime.cooling_output = ctx.runtime.cooling.inhibit(now);
    ctx.runtime.cooling_step_initialized = false;
    if (wasCooling) ctx.runtime.t_pump_off = ticks.millis();
    ctx.runtime.heating_output = 0;
    transitionToIdle(ctx);
}

void updateState(Context& ctx) {
    ctx.runtime.cooling.request(ctx.requestedAlgorithm);
    double now = monotonicSeconds(ctx);
    bool wasCooling = stateIsCooling(ctx);
    trackActiveIntervalEnds(ctx, now);

    const temperature raw = ctx.beerSensor->readRawCached();
    if (ctx.cs.beerSetting == INVALID_TEMP || raw == INVALID_TEMP ||
        !ctx.beerSensor->isConnected()) {
        suspend(ctx);
        return;
    }

    if (ctx.runtime.state == GLYCOL_HEATING) {
        ctx.runtime.cooling_output = ctx.runtime.cooling.inhibit(now);
        ctx.runtime.cooling_step_initialized = false;
        updateHeatingState(ctx);
        return;
    }
    if (ctx.runtime.state == GLYCOL_IDLE && shouldStartHeating(ctx)) {
        transitionToHeating(ctx);
        return;
    }

    // Apply the heat->cool/boot guard BEFORE invoking the cooling core. A
    // blocked command must never become a fictitious pulse or learning sample.
    uint16_t sinceHeating = timeSinceHeating(ctx);
    if (sinceHeating < ctx.minTimes.MIN_SWITCH_TIME) {
        ctx.runtime.cooling_output = ctx.runtime.cooling.inhibit(now);
        ctx.runtime.cooling_step_initialized = false;
        ctx.runtime.state = GLYCOL_IDLE;
        if (raw > ctx.cs.beerSetting) {
            ctx.state = WAITING_TO_COOL;
            ctx.waitTime = ctx.minTimes.MIN_SWITCH_TIME - sinceHeating;
        } else {
            ctx.state = IDLE;
            resetWaitTime(ctx);
        }
        ctx.lastIdleTime = ticks.seconds();
        return;
    }

    // The raw sensor is already sampled/cached by updateTemperatures. Do not
    // perform another OneWire read or consume the old cascaded fast filter.
    // Repeated UI calls hold the last command until the next 1 Hz decision.
    if (ctx.runtime.cooling_step_initialized &&
        ctx.runtime.clock_elapsed_ms - ctx.runtime.cooling_last_step_ms < 1000) {
        return;
    }
    ctx.runtime.cooling_step_initialized = true;
    ctx.runtime.cooling_last_step_ms = ctx.runtime.clock_elapsed_ms;
    ctx.runtime.cooling_output = ctx.runtime.cooling.step(
        now, rawCelsius(raw), rawCelsius(ctx.cs.beerSetting));
    applyCoolingOutput(ctx, wasCooling);
}

} // namespace GlycolMode
