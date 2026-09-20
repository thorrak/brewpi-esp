#include "JsonKeys.h"
#include "GlycolParams.h"


// ----- GlycolLearnedParams -----
GlycolLearnedParams::GlycolLearnedParams() {
    setDefaults();
}

void GlycolLearnedParams::setDefaults() {
    k = 5.0f;           // 5 minutes equivalent coast time
    C_off = 0.5f;       // 0.5 degrees average coast (observed: 15s pump -> 0.5 deg drop)
    L = 300.0f;         // 300 seconds (5 min) dead time (observed: temp drop visible ~5 min after pump start)
    drift_rate = 0.02f; // 0.02 degrees/min drift
}

void GlycolLearnedParams::toJson(JsonDocument& doc) {
    doc[GlycolKeys::k] = k;
    doc[GlycolKeys::C_off] = C_off;
    doc[GlycolKeys::L] = L;
    doc[GlycolKeys::drift_rate] = drift_rate;
}

void GlycolLearnedParams::storeToFilesystem() {
    JsonDocument doc;
    toJson(doc);
    writeJsonToFile(GlycolLearnedParams::filename, doc);
}

void GlycolLearnedParams::loadFromFilesystem() {
    setDefaults();
    JsonDocument json_doc = readJsonFromFile(GlycolLearnedParams::filename);

    if (json_doc[GlycolKeys::k].is<float>()) k = json_doc[GlycolKeys::k];
    if (json_doc[GlycolKeys::C_off].is<float>()) C_off = json_doc[GlycolKeys::C_off];
    if (json_doc[GlycolKeys::L].is<float>()) L = json_doc[GlycolKeys::L];
    if (json_doc[GlycolKeys::drift_rate].is<float>()) drift_rate = json_doc[GlycolKeys::drift_rate];
}

// ----- GlycolConfig -----

GlycolConfig::GlycolConfig() {
    setDefaults();
}

void GlycolConfig::setDefaults() {
    // Timing
    min_on_time_s = 10;
    min_off_time_s = 10;
    max_continuous_on_time_min = 45;

    // Learning thresholds
    min_training_rate = 0.01f;       // degrees/min
    min_training_duration_s = 360;   // 6 minutes (must exceed dead time L of ~5 min)
    min_training_drop = 0.2f;        // degrees

    // Safety
    safety_margin_low = 0.5f;        // degrees below setpoint

    // Prediction
    min_rate_for_k_model = 0.02f;    // degrees/min
    trigger_margin = 0.1f;           // degrees

    // Emergency detection
    emergency_horizon_min = 20.0f;   // minutes
    emergency_detection_time_s = 90; // seconds
    min_emergency_dwell_time_s = 120;// 2 minutes

    // Hot glycol compensation
    hot_glycol_threshold_s = 300;    // 5 minutes - reservoir is warmed after long runs

}

void GlycolConfig::toJson(JsonDocument& doc) {
    doc[GlycolKeys::min_on_time_s] = min_on_time_s;
    doc[GlycolKeys::min_off_time_s] = min_off_time_s;
    doc[GlycolKeys::max_continuous_on_time_min] = max_continuous_on_time_min;
    doc[GlycolKeys::min_training_rate] = min_training_rate;
    doc[GlycolKeys::min_training_duration_s] = min_training_duration_s;
    doc[GlycolKeys::min_training_drop] = min_training_drop;
    doc[GlycolKeys::safety_margin_low] = safety_margin_low;
    doc[GlycolKeys::min_rate_for_k_model] = min_rate_for_k_model;
    doc[GlycolKeys::trigger_margin] = trigger_margin;
    doc[GlycolKeys::emergency_horizon_min] = emergency_horizon_min;
    doc[GlycolKeys::emergency_detection_time_s] = emergency_detection_time_s;
    doc[GlycolKeys::min_emergency_dwell_time_s] = min_emergency_dwell_time_s;
    doc[GlycolKeys::hot_glycol_threshold_s] = hot_glycol_threshold_s;
}

void GlycolConfig::storeToFilesystem() {
    JsonDocument doc;
    toJson(doc);
    writeJsonToFile(GlycolConfig::filename, doc);
}

void GlycolConfig::loadFromFilesystem() {
    setDefaults();
    JsonDocument json_doc = readJsonFromFile(GlycolConfig::filename);

    if (json_doc[GlycolKeys::min_on_time_s].is<uint16_t>()) min_on_time_s = json_doc[GlycolKeys::min_on_time_s];
    if (json_doc[GlycolKeys::min_off_time_s].is<uint16_t>()) min_off_time_s = json_doc[GlycolKeys::min_off_time_s];
    if (json_doc[GlycolKeys::max_continuous_on_time_min].is<uint16_t>()) max_continuous_on_time_min = json_doc[GlycolKeys::max_continuous_on_time_min];
    if (json_doc[GlycolKeys::min_training_rate].is<float>()) min_training_rate = json_doc[GlycolKeys::min_training_rate];
    if (json_doc[GlycolKeys::min_training_duration_s].is<uint16_t>()) min_training_duration_s = json_doc[GlycolKeys::min_training_duration_s];
    if (json_doc[GlycolKeys::min_training_drop].is<float>()) min_training_drop = json_doc[GlycolKeys::min_training_drop];
    if (json_doc[GlycolKeys::safety_margin_low].is<float>()) safety_margin_low = json_doc[GlycolKeys::safety_margin_low];
    if (json_doc[GlycolKeys::min_rate_for_k_model].is<float>()) min_rate_for_k_model = json_doc[GlycolKeys::min_rate_for_k_model];
    if (json_doc[GlycolKeys::trigger_margin].is<float>()) trigger_margin = json_doc[GlycolKeys::trigger_margin];
    if (json_doc[GlycolKeys::emergency_horizon_min].is<float>()) emergency_horizon_min = json_doc[GlycolKeys::emergency_horizon_min];
    if (json_doc[GlycolKeys::emergency_detection_time_s].is<uint16_t>()) emergency_detection_time_s = json_doc[GlycolKeys::emergency_detection_time_s];
    if (json_doc[GlycolKeys::min_emergency_dwell_time_s].is<uint16_t>()) min_emergency_dwell_time_s = json_doc[GlycolKeys::min_emergency_dwell_time_s];
    if (json_doc[GlycolKeys::hot_glycol_threshold_s].is<uint16_t>()) hot_glycol_threshold_s = json_doc[GlycolKeys::hot_glycol_threshold_s];
}
