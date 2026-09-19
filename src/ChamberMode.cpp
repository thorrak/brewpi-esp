#include "ChamberMode.h"

#include "Ticks.h"

#if TEMP_CONTROL_STATIC
extern ValueActuator defaultActuator;
#endif

namespace {

bool stateIsCooling(const ChamberMode::Context& ctx) {
    return ctx.state == COOLING || ctx.state == COOLING_MIN_TIME;
}

bool stateIsHeating(const ChamberMode::Context& ctx) {
    return ctx.state == HEATING || ctx.state == HEATING_MIN_TIME;
}

uint16_t timeSinceCooling(const ChamberMode::Context& ctx) {
    return ticks.timeSince(ctx.lastCoolTime);
}

uint16_t timeSinceHeating(const ChamberMode::Context& ctx) {
    return ticks.timeSince(ctx.lastHeatTime);
}

uint16_t timeSinceIdle(const ChamberMode::Context& ctx) {
    return ticks.timeSince(ctx.lastIdleTime);
}

void resetWaitTime(ChamberMode::Context& ctx) {
    ctx.waitTime = 0;
}

void updateWaitTime(ChamberMode::Context& ctx, uint16_t newTimeLimit, uint16_t newTimeSince) {
    if (newTimeSince < newTimeLimit) {
        uint16_t newWaitTime = newTimeLimit - newTimeSince;
        if (newWaitTime > ctx.waitTime) {
            ctx.waitTime = newWaitTime;
        }
    }
}

bool hasCooler(const ChamberMode::Context& ctx) {
    return ctx.cooler != &defaultActuator;
}

bool hasHeater(const ChamberMode::Context& ctx) {
    return ctx.heater != &defaultActuator ||
           (ctx.cc.lightAsHeater && (ctx.light != &defaultActuator));
}

void updateEstimatedPeak(
    ChamberMode::Context& ctx,
    uint16_t timeLimit,
    temperature estimator,
    uint16_t sinceIdleS
) {
    uint16_t activeTime = min(timeLimit, sinceIdleS);
    temperature estimatedOvershoot =
        ((long_temperature) estimator * activeTime) / 3600;
    if (stateIsCooling(ctx)) {
        estimatedOvershoot = -estimatedOvershoot;
    }
    ctx.cv.estimatedPeak = ctx.fridgeSensor->readFastFiltered() + estimatedOvershoot;
}

void increaseEstimator(temperature* estimator, temperature error) {
    temperature factor =
        614 + constrainTemp((temperature) abs(error) >> 5, 0, 154);
    *estimator = multiplyFactorTemperatureDiff(factor, *estimator);
    if (*estimator < 25) {
        *estimator = intToTempDiff(5) / 100;
    }
}

void decreaseEstimator(temperature* estimator, temperature error) {
    temperature factor =
        426 - constrainTemp((temperature) abs(error) >> 5, 0, 85);
    *estimator = multiplyFactorTemperatureDiff(factor, *estimator);
}

} // namespace

namespace ChamberMode {

void updatePID(Context& ctx, unsigned char& integralUpdateCounter) {
    temperature fridgeFastFiltered = ctx.fridgeSensor->readFastFiltered();

    if (integralUpdateCounter++ == 60) {
        integralUpdateCounter = 0;

        temperature integratorUpdate = ctx.cv.beerDiff;

        if (ctx.state != IDLE) {
            integratorUpdate = 0;
        } else if (abs(integratorUpdate) < ctx.cc.iMaxError) {
            bool updateSign = (integratorUpdate > 0);
            bool integratorSign = (ctx.cv.diffIntegral > 0);

            if (updateSign == integratorSign) {
                integratorUpdate =
                    (ctx.cs.fridgeSetting >= ctx.cc.tempSettingMax) ? 0 : integratorUpdate;
                integratorUpdate =
                    (ctx.cs.fridgeSetting <= ctx.cc.tempSettingMin) ? 0 : integratorUpdate;
                integratorUpdate =
                    ((ctx.cs.fridgeSetting - ctx.cs.beerSetting) >= ctx.cc.pidMax) ? 0 : integratorUpdate;
                integratorUpdate =
                    ((ctx.cs.beerSetting - ctx.cs.fridgeSetting) >= ctx.cc.pidMax) ? 0 : integratorUpdate;

                integratorUpdate =
                    (!updateSign && (fridgeFastFiltered > (ctx.cs.fridgeSetting + 1024))) ? 0 : integratorUpdate;
                integratorUpdate =
                    (updateSign && (fridgeFastFiltered < (ctx.cs.fridgeSetting - 1024))) ? 0 : integratorUpdate;
            } else {
                integratorUpdate = integratorUpdate * 2;
            }
        } else {
            integratorUpdate = -(ctx.cv.diffIntegral >> 3);
        }
        ctx.cv.diffIntegral = ctx.cv.diffIntegral + integratorUpdate;
    }

    ctx.cv.p = multiplyFactorTemperatureDiff(ctx.cc.Kp, ctx.cv.beerDiff);
    ctx.cv.i = multiplyFactorTemperatureDiffLong(ctx.cc.Ki, ctx.cv.diffIntegral);
    ctx.cv.d = multiplyFactorTemperatureDiff(ctx.cc.Kd, ctx.cv.beerSlope);
    long_temperature newFridgeSetting = ctx.cs.beerSetting;
    newFridgeSetting += ctx.cv.p;
    newFridgeSetting += ctx.cv.i;
    newFridgeSetting += ctx.cv.d;

    temperature lowerBound =
        (ctx.cs.beerSetting <= ctx.cc.tempSettingMin + ctx.cc.pidMax)
            ? ctx.cc.tempSettingMin
            : ctx.cs.beerSetting - ctx.cc.pidMax;
    temperature upperBound =
        (ctx.cs.beerSetting >= ctx.cc.tempSettingMax - ctx.cc.pidMax)
            ? ctx.cc.tempSettingMax
            : ctx.cs.beerSetting + ctx.cc.pidMax;

    ctx.cs.fridgeSetting =
        constrain(constrainTemp16(newFridgeSetting), lowerBound, upperBound);
}

void updateState(Context& ctx, bool stayIdle) {
    uint16_t sinceIdle = timeSinceIdle(ctx);
    uint16_t sinceCooling = timeSinceCooling(ctx);
    uint16_t sinceHeating = timeSinceHeating(ctx);
    temperature fridgeFast = ctx.fridgeSensor->readFastFiltered();
    temperature beerFast = ctx.beerSensor->readFastFiltered();
    ticks_seconds_t secs = ticks.seconds();

    switch (ctx.state) {
        case IDLE:
        case STATE_OFF:
        case WAITING_TO_COOL:
        case WAITING_TO_HEAT:
        case WAITING_FOR_PEAK_DETECT: {
            ctx.lastIdleTime = secs;
            if (stayIdle) {
                break;
            }

            resetWaitTime(ctx);
            if (fridgeFast > (ctx.cs.fridgeSetting + ctx.cc.idleRangeHigh)) {
                updateWaitTime(ctx, ctx.minTimes.MIN_SWITCH_TIME, sinceHeating);
                if (ctx.cs.mode == Modes::fridgeConstant) {
                    updateWaitTime(
                        ctx,
                        ctx.minTimes.MIN_COOL_OFF_TIME_FRIDGE_CONSTANT,
                        sinceCooling
                    );
                } else {
                    if (beerFast < (ctx.cs.beerSetting + 16)) {
                        ctx.state = IDLE;
                        break;
                    }
                    updateWaitTime(ctx, ctx.minTimes.MIN_COOL_OFF_TIME, sinceCooling);
                }

                if (hasCooler(ctx)) {
                    ctx.state = (ctx.waitTime > 0) ? WAITING_TO_COOL : COOLING;
                }
            } else if (fridgeFast < (ctx.cs.fridgeSetting + ctx.cc.idleRangeLow)) {
                updateWaitTime(ctx, ctx.minTimes.MIN_SWITCH_TIME, sinceCooling);
                updateWaitTime(ctx, ctx.minTimes.MIN_HEAT_OFF_TIME, sinceHeating);
                if (ctx.cs.mode != Modes::fridgeConstant) {
                    if (beerFast > (ctx.cs.beerSetting - 16)) {
                        ctx.state = IDLE;
                        break;
                    }
                }
                if (hasHeater(ctx)) {
                    ctx.state = (ctx.waitTime > 0) ? WAITING_TO_HEAT : HEATING;
                }
            } else {
                ctx.state = IDLE;
                break;
            }

            if (ctx.state == HEATING || ctx.state == COOLING) {
                if (ctx.doNegPeakDetect || ctx.doPosPeakDetect) {
                    ctx.state = WAITING_FOR_PEAK_DETECT;
                    break;
                }
            }
        } break;
        case COOLING:
        case COOLING_MIN_TIME: {
            ctx.doNegPeakDetect = true;
            ctx.lastCoolTime = secs;
            updateEstimatedPeak(
                ctx,
                ctx.cc.maxCoolTimeForEstimate,
                ctx.cs.coolEstimator,
                sinceIdle
            );
            ctx.state = COOLING;

            if (ctx.cv.estimatedPeak <= ctx.cs.fridgeSetting ||
                (ctx.cs.mode != Modes::fridgeConstant &&
                 beerFast < (ctx.cs.beerSetting - 16))) {
                if (sinceIdle > ctx.minTimes.MIN_COOL_ON_TIME) {
                    ctx.cv.negPeakEstimate = ctx.cv.estimatedPeak;
                    ctx.state = IDLE;
                    break;
                } else {
                    ctx.state = COOLING_MIN_TIME;
                    break;
                }
            }
        } break;
        case HEATING:
        case HEATING_MIN_TIME: {
            ctx.doPosPeakDetect = true;
            ctx.lastHeatTime = secs;
            updateEstimatedPeak(
                ctx,
                ctx.cc.maxHeatTimeForEstimate,
                ctx.cs.heatEstimator,
                sinceIdle
            );
            ctx.state = HEATING;

            if (ctx.cv.estimatedPeak >= ctx.cs.fridgeSetting ||
                (ctx.cs.mode != Modes::fridgeConstant &&
                 beerFast > (ctx.cs.beerSetting + 16))) {
                if (sinceIdle > ctx.minTimes.MIN_HEAT_ON_TIME) {
                    ctx.cv.posPeakEstimate = ctx.cv.estimatedPeak;
                    ctx.state = IDLE;
                    break;
                } else {
                    ctx.state = HEATING_MIN_TIME;
                    break;
                }
            }
        } break;
    }
}

bool detectPeaks(Context& ctx) {
    LOG_ID_TYPE detected = 0;
    bool estimatorChanged = false;
    temperature peak, estimate, error, oldEstimator, newEstimator;

    if (ctx.doPosPeakDetect && !stateIsHeating(ctx)) {
        peak = ctx.fridgeSensor->detectPosPeak();
        estimate = ctx.cv.posPeakEstimate;
        error = peak - estimate;
        oldEstimator = ctx.cs.heatEstimator;
        if (peak != INVALID_TEMP) {
            if (error > ctx.cc.heatingTargetUpper) {
                increaseEstimator(&(ctx.cs.heatEstimator), error);
                estimatorChanged = true;
            }
            if (error < ctx.cc.heatingTargetLower) {
                decreaseEstimator(&(ctx.cs.heatEstimator), error);
                estimatorChanged = true;
            }
            detected = INFO_POSITIVE_PEAK;
        } else if (timeSinceHeating(ctx) > ctx.minTimes.HEAT_PEAK_DETECT_TIME) {
            if (ctx.fridgeSensor->readFastFiltered() <
                (ctx.cv.posPeakEstimate + ctx.cc.heatingTargetLower)) {
                peak = ctx.fridgeSensor->readFastFiltered();
                decreaseEstimator(&(ctx.cs.heatEstimator), error);
                estimatorChanged = true;
                detected = INFO_POSITIVE_DRIFT;
            } else {
                ctx.doPosPeakDetect = false;
            }
        }
        if (detected) {
            newEstimator = ctx.cs.heatEstimator;
            ctx.cv.posPeak = peak;
            ctx.doPosPeakDetect = false;
        }
    } else if (ctx.doNegPeakDetect && !stateIsCooling(ctx)) {
        peak = ctx.fridgeSensor->detectNegPeak();
        estimate = ctx.cv.negPeakEstimate;
        error = peak - estimate;
        oldEstimator = ctx.cs.coolEstimator;
        if (peak != INVALID_TEMP) {
            if (error < ctx.cc.coolingTargetLower) {
                increaseEstimator(&(ctx.cs.coolEstimator), error);
                estimatorChanged = true;
            }
            if (error > ctx.cc.coolingTargetUpper) {
                decreaseEstimator(&(ctx.cs.coolEstimator), error);
                estimatorChanged = true;
            }
            detected = INFO_NEGATIVE_PEAK;
        } else if (timeSinceCooling(ctx) > ctx.minTimes.COOL_PEAK_DETECT_TIME) {
            if (ctx.fridgeSensor->readFastFiltered() >
                (ctx.cv.negPeakEstimate + ctx.cc.coolingTargetUpper)) {
                peak = ctx.fridgeSensor->readFastFiltered();
                decreaseEstimator(&(ctx.cs.coolEstimator), error);
                estimatorChanged = true;
                detected = INFO_NEGATIVE_DRIFT;
            } else {
                ctx.doNegPeakDetect = false;
            }
        }
        if (detected) {
            newEstimator = ctx.cs.coolEstimator;
            ctx.cv.negPeak = peak;
            ctx.doNegPeakDetect = false;
        }
    }

    if (detected) {
        logInfoTempTempFixedFixed(detected, peak, estimate, oldEstimator, newEstimator);
    }

    return estimatorChanged;
}

} // namespace ChamberMode
