#pragma once

#include "TempControl.h"

/**
 * Lightweight per-call view of TempControl state shared by the mode-specific
 * controllers. It owns nothing; it just bundles references and pointers so the
 * control algorithms can work without depending directly on the TempControl class.
 */
struct ControlContext {
    ControlConstants& cc;
    ControlSettings& cs;
    ControlVariables& cv;
    MinTimes& minTimes;
    TempSensor* beerSensor;
    TempSensor* fridgeSensor;
    Actuator* heater;
    Actuator* cooler;
    Actuator* light;
    uint8_t& state;
    uint16_t& lastIdleTime;
    uint16_t& lastHeatTime;
    uint16_t& lastCoolTime;
    uint16_t& waitTime;
};
