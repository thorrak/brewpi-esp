#pragma once
#include "TemperatureFormats.h"
#include <ArduinoJson.h>
class JSONSaveable {
protected:
    static void writeJsonToFile(const char *filename, const JsonDocument& json_doc);
    static JsonDocument readJsonFromFile(const char*filename);

};


/**
 * \brief PID Control constants
 */
class ControlConstants : public JSONSaveable {
public:
    ControlConstants();

    temperature tempSettingMin; //<! Minimum valid control temperature
    temperature tempSettingMax; //<! Maximum valid control temperature
    temperature Kp;
    temperature Ki;
    temperature Kd;
    temperature iMaxError;
    temperature idleRangeHigh;
    temperature idleRangeLow;
    temperature heatingTargetUpper;
    temperature heatingTargetLower;
    temperature coolingTargetUpper;
    temperature coolingTargetLower;
    uint16_t maxHeatTimeForEstimate; //!< max time for heat estimate in seconds
    uint16_t maxCoolTimeForEstimate; //!< max time for heat estimate in seconds
    // for the filter coefficients the b value is stored. a is calculated from b.
    uint8_t fridgeFastFilter;	//!< for display, logging and on-off control
    uint8_t fridgeSlowFilter;	//!< for peak detection
    uint8_t fridgeSlopeFilter;	//!< not used in current control algorithm
    uint8_t beerFastFilter;	//!< for display and logging
    uint8_t beerSlowFilter;	//!< for on/off control algorithm
    uint8_t beerSlopeFilter;	//!< for PID calculation
    uint8_t lightAsHeater;		//!< Use the light to heat rather than the configured heater device
    uint8_t rotaryHalfSteps; //!< Define whether to use full or half steps for the rotary encoder
    temperature pidMax;
    temperature Kp_heat;    //!< Separate heating Kp for glycol mode
    temperature Ki_heat;    //!< Separate heating Ki for glycol mode
    temperature Kd_heat;    //!< Separate heating Kd for glycol mode
    temperature pidMax_heat; //!< Separate heating pidMax for glycol mode
    char tempFormat; //!< Temperature format (F/C)

    void toJson(JsonDocument &doc);
    void storeToFilesystem();
    void loadFromFilesystem();
    void setDefaults();

    /**
     * \brief Filename used when reading/writing data to flash
     */
    static constexpr auto filename = "/controlConstants.json";
private:
};

/** @} */

struct ControlSettings : public JSONSaveable {
public:
    ControlSettings();

    temperature beerSetting;
    temperature fridgeSetting;
    temperature heatEstimator; // updated automatically by self learning algorithm
    temperature coolEstimator; // updated automatically by self learning algorithm
    char mode;

    void toJson(JsonDocument &doc);
    void storeToFilesystem();
    void loadFromFilesystem();
    void setDefaults();

    /**
     * \brief Filename used when reading/writing data to flash
     */
    static constexpr auto filename = "/controlSettings.json";
};
