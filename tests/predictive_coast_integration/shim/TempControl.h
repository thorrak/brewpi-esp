#pragma once
#include "Brewpi.h"
#include "TempSensor.h"
#include "Actuator.h"
#include "EepromStructs.h"
#include "GlycolParams.h"
#include "GlycolCoolingController.h"
#include "ControlTypes.h"

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
