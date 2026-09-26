#pragma once
#include "Brewpi.h"
#include "TempSensor.h"
#include "Actuator.h"
#include "EepromStructs.h"
#include "GlycolParams.h"
#include "GlycolCoolingController.h"
#include "ControlTypes.h"

struct ControlContext;
struct HostDoor { bool sense() { return false; } };
struct HostAutoOff : ValueActuator { void update() {} };
extern ValueActuator cameraLightState;
extern MinTimes minTimes;

// The methods under test are copied from TempControl.cpp at build time. This
// facade supplies their enclosing fields without unrelated board services.
struct TempControl {
    ControlConstants cc;
    ControlSettings cs;
    ControlVariables cv{};
    GlycolRuntimeState glycolRuntime{};
    TempSensor* beerSensor = nullptr;
    TempSensor* fridgeSensor = nullptr;
    Actuator* heater = nullptr;
    Actuator* cooler = nullptr;
    Actuator* light = nullptr;
    Actuator* fan = nullptr;
    HostDoor doorValue;
    HostDoor* door = &doorValue;
    HostAutoOff cameraLight;
    bool doorOpen = false, doPosPeakDetect = false, doNegPeakDetect = false;
    uint8_t state = IDLE;
    uint16_t lastIdleTime = 0, lastHeatTime = 0, lastCoolTime = 0, waitTime = 0;
    GlycolConfig glycolConfig;
    unsigned settingsWrites = 0;
    bool storedWithOutputActive = false;
    void storeSettings() {
        ++settingsWrites;
        storedWithOutputActive = cooler->isActive() || heater->isActive() || light->isActive();
    }
    bool isDoorOpen() { return doorOpen; }
    bool stateIsHeating();
    bool stateIsCooling();
    void reset();
    void resetGlycolControl();
    void updateState();
    void updateOutputs();
    void setMode(char newMode, bool force = false);
    ControlContext makeControlContext();
    void getControlVariablesDoc(JsonDocument& doc);
    void getControlConstantsDoc(JsonDocument& doc);
};
extern TempControl tempControl;
struct HostExtendedSettings {
    bool glycol = true;
    GlycolCooling::Algorithm glycolCoolingAlgorithm = GlycolCooling::Algorithm::PredictiveCoast;
};
extern ValueActuator defaultActuator;
extern HostExtendedSettings extendedSettings;
