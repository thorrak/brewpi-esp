#pragma once

#include "EepromStructs.h"
#include <ArduinoJson.h>

/**
 * Persisted glycol heating settings.
 */
struct GlycolConfig : public JSONSaveable {
    float trigger_margin; //!< Heating start margin below setpoint (display degrees)

    GlycolConfig();
    void setDefaults();
    void toJson(JsonDocument& doc);
    void storeToFilesystem();
    void loadFromFilesystem();

    static constexpr auto filename = "/glycolConfig.json";
};
