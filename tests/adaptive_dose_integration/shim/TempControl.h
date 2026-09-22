#pragma once
#include "Brewpi.h"
#include "TempSensor.h"
#include "Actuator.h"
#include "EepromStructs.h"
#include "GlycolParams.h"
#include "AdaptiveDoseController.h"
#include "ControlTypes.h"

// Only TemperatureFormats uses this global facade, for the selected display
// units. The adapter swaps it for each handle while holding a Python lock.
struct HostTempControl { ControlConstants cc; };
extern HostTempControl tempControl;
struct HostExtendedSettings { bool glycol = true; };
extern HostExtendedSettings extendedSettings;
