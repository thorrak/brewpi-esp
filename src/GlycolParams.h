#pragma once

#include "EepromStructs.h"
#include <ArduinoJson.h>


/**
 * Legacy predictive-controller parameters (retained on disk for rollback).
 * Adaptive pulse-dose cooling does not consume or update these fields.
 * Learned parameters for the former glycol mode (persisted to filesystem)
 * These adapt to the specific system characteristics over time
 */
struct GlycolLearnedParams : public JSONSaveable {
    float k;            //!< Coast factor in minutes (estimated_coast = k * |cooling_rate|)
    float C_off;        //!< Average coast drop in degrees (rate-independent fallback)
    float L;            //!< Dead time in seconds (pump ON to observable cooling)
    float drift_rate;   //!< Temperature drift rate in degrees/min when idle (warming)

    GlycolLearnedParams();
    void setDefaults();
    void toJson(JsonDocument& doc);
    void storeToFilesystem();
    void loadFromFilesystem();

    static constexpr auto filename = "/glycolLearned.json";
};

/**
 * Legacy predictive cooling settings retained for configuration compatibility.
 * Adaptive cooling uses the explicit SI-unit defaults in AdaptiveCooling::Config;
 * these cooling fields are ignored. trigger_margin still controls heating start.
 * Configuration parameters for legacy glycol mode (persisted to filesystem)
 */
struct GlycolConfig : public JSONSaveable {
    // Timing
    uint16_t min_on_time_s;           //!< Minimum pump on time (pump protection)
    uint16_t min_off_time_s;          //!< Minimum pump off time (pump protection)
    uint16_t max_continuous_on_time_min;  //!< Safety: max continuous pump run time

    // Learning thresholds
    float min_training_rate;          //!< Minimum |rate| to trust for k training (deg/min)
    uint16_t min_training_duration_s; //!< Minimum cycle length to learn from
    float min_training_drop;          //!< Minimum temp drop to learn from (deg)

    // Safety
    float safety_margin_low;          //!< Hard limit below setpoint (deg)

    // Prediction
    float min_rate_for_k_model;       //!< Below this rate, use C_off instead (deg/min)
    float trigger_margin;             //!< Margin above setpoint to trigger cooling (deg)

    // Emergency detection
    float emergency_horizon_min;      //!< Minutes to look ahead for "can't catch up" detection
    uint16_t emergency_detection_time_s;  //!< Time before declaring emergency
    uint16_t min_emergency_dwell_time_s;  //!< Minimum time in emergency before exiting

    // Hot glycol compensation
    uint16_t hot_glycol_threshold_s;  //!< Pump run time above which reservoir is warmed by beer


    GlycolConfig();
    void setDefaults();
    void toJson(JsonDocument& doc);
    void storeToFilesystem();
    void loadFromFilesystem();

    static constexpr auto filename = "/glycolConfig.json";
};
