#include "GlycolTuning.h"
#include "ESPEepromAccess.h"
#include <ArduinoJson.h>
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <limits>
#include <unistd.h>

namespace {
constexpr unsigned schemaVersion = 1;
constexpr size_t maxFileBytes = 1024;
constexpr uint32_t saveIntervalMs = 30 * 60 * 1000;
constexpr uint32_t retryDelayMs = 30000;
constexpr char temporaryFile[] = "/glycolTuning.json.tmp";

double roundAtBoundary(double value, double boundary) {
    // Decimal parsing can place a saved boundary value a few ULPs outside it.
    return std::fabs(value - boundary) <= 4 * std::numeric_limits<double>::epsilon() * std::fabs(boundary)
        ? boundary : value;
}

bool sameLearnedValues(const GlycolCooling::Tuning& a, const GlycolCooling::Tuning& b) {
    return a.predictive.coast_s == b.predictive.coast_s &&
        a.predictive.budget_gain_c_per_s == b.predictive.budget_gain_c_per_s &&
        a.pulse_dose.gain_c_per_on_s == b.pulse_dose.gain_c_per_on_s;
}

bool readTuning(GlycolCooling::Controller& controller) {
    FILE* file = fs_open(GlycolTuningStore::filename, "rb");
    if (!file) return false;
    char buffer[maxFileBytes + 1];
    const size_t length = fread(buffer, 1, sizeof(buffer), file);
    const bool readOK = !ferror(file) && length > 0 && length <= maxFileBytes;
    const bool closeOK = fclose(file) == 0;
    if (!readOK || !closeOK) return false;

    JsonDocument doc;
    if (deserializeJson(doc, buffer, length)) return false;
    JsonObjectConst predictive = doc["predictive"].as<JsonObjectConst>();
    JsonObjectConst dose = doc["pulse_dose"].as<JsonObjectConst>();
    if (!doc["version"].is<unsigned>() || doc["version"].as<unsigned>() != schemaVersion ||
        strcmp(predictive["algorithm"] | "", GlycolCooling::algorithmVersion(GlycolCooling::Algorithm::PredictiveCoast)) != 0 ||
        strcmp(dose["algorithm"] | "", GlycolCooling::algorithmVersion(GlycolCooling::Algorithm::PulseDose)) != 0 ||
        !predictive["coast_s"].is<double>() || !predictive["budget_gain_c_per_s"].is<double>() ||
        !predictive["learning_updates"].is<uint32_t>() || !predictive["response_updates"].is<uint32_t>() ||
        !dose["gain_c_per_on_s"].is<double>() || !dose["learning_updates"].is<uint32_t>()) return false;

    GlycolCooling::Tuning tuning = {
        {predictive["coast_s"].as<double>(), predictive["budget_gain_c_per_s"].as<double>(),
         predictive["learning_updates"].as<uint32_t>(), predictive["response_updates"].as<uint32_t>()},
        {dose["gain_c_per_on_s"].as<double>(), dose["learning_updates"].as<uint32_t>()}
    };
    const auto& predictiveConfig = controller.predictiveConfiguration();
    const auto& doseConfig = controller.doseConfiguration();
    tuning.predictive.coast_s = roundAtBoundary(roundAtBoundary(tuning.predictive.coast_s,
        predictiveConfig.min_coast_estimate_s), predictiveConfig.max_coast_estimate_s);
    tuning.predictive.budget_gain_c_per_s = roundAtBoundary(tuning.predictive.budget_gain_c_per_s,
        predictiveConfig.minimum_budget_gain_c_per_s);
    tuning.pulse_dose.gain_c_per_on_s = roundAtBoundary(roundAtBoundary(tuning.pulse_dose.gain_c_per_on_s,
        doseConfig.minimum_gain_c_per_on_s), doseConfig.maximum_gain_c_per_on_s);
    return controller.restoreTuning(tuning);
}

bool writeTuning(const GlycolCooling::Tuning& tuning) {
    char buffer[maxFileBytes];
    // Keep double precision for the small response gains used by both cores.
    const int count = snprintf(buffer, sizeof(buffer),
        "{\"version\":%u,\"predictive\":{\"algorithm\":\"%s\",\"coast_s\":%.16e,"
        "\"budget_gain_c_per_s\":%.16e,\"learning_updates\":%" PRIu32 ",\"response_updates\":%" PRIu32 "},"
        "\"pulse_dose\":{\"algorithm\":\"%s\",\"gain_c_per_on_s\":%.16e,\"learning_updates\":%" PRIu32 "}}",
        schemaVersion, GlycolCooling::algorithmVersion(GlycolCooling::Algorithm::PredictiveCoast),
        tuning.predictive.coast_s, tuning.predictive.budget_gain_c_per_s,
        tuning.predictive.learning_updates, tuning.predictive.response_updates,
        GlycolCooling::algorithmVersion(GlycolCooling::Algorithm::PulseDose),
        tuning.pulse_dose.gain_c_per_on_s, tuning.pulse_dose.learning_updates);
    if (count <= 0 || static_cast<size_t>(count) >= sizeof(buffer)) return false;
    const size_t length = static_cast<size_t>(count);

    FILE* file = fs_open(temporaryFile, "wb");
    if (!file) return false;
    const bool written = fwrite(buffer, 1, length, file) == length &&
        fflush(file) == 0 && fsync(fileno(file)) == 0;
    const bool closed = fclose(file) == 0;
    if (!written || !closed ||
        rename(FS_PREFIX "/glycolTuning.json.tmp", FS_PREFIX "/glycolTuning.json") != 0) {
        fs_remove(temporaryFile);
        return false;
    }
    return true;
}
} // namespace

bool GlycolTuningStore::load(GlycolCooling::Controller& controller, uint32_t now_ms) {
    fs_remove(temporaryFile);
    const bool loaded = readTuning(controller);
    saved_ = controller.tuning();
    // A loaded snapshot starts a fresh interval on the current boot's clock.
    saved_at_ms_ = now_ms;
    save_delay_active_ = loaded;
    retry_pending_ = false;
    return loaded;
}

bool GlycolTuningStore::saveIfChanged(const GlycolCooling::Controller& controller, uint32_t now_ms) {
    if (save_delay_active_ && static_cast<uint32_t>(now_ms - saved_at_ms_) >= saveIntervalMs)
        save_delay_active_ = false;
    const auto tuning = controller.tuning();
    if (sameLearnedValues(tuning, saved_)) return true;
    if (save_delay_active_) return false;
    if (retry_pending_ && static_cast<uint32_t>(now_ms - failed_at_ms_) < retryDelayMs) return false;
    if (!writeTuning(tuning)) {
        failed_at_ms_ = now_ms;
        retry_pending_ = true;
        return false;
    }
    saved_ = tuning;
    saved_at_ms_ = now_ms;
    save_delay_active_ = true;
    retry_pending_ = false;
    return true;
}
