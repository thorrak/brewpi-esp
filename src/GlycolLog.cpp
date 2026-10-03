#ifdef ENABLE_GLYCOL_LOGGING

#include "GlycolLog.h"
#include "ESPEepromAccess.h"
#include "ntp.h"
#include "TempControl.h"
#include "Ticks.h"

#include <thorlog.h>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sys/stat.h>

namespace {
constexpr char HEADER[] =
    "timestamp,millis,from_state,to_state,algorithm,temperature_c,setpoint_c,"
    "rate_c_per_s,actual_on_s,coast_s,budget_gain_c_per_s,gain_c_per_on_s,"
    "learning_updates,response_updates,pulse_budget_s,predicted_endpoint_c,phase\n";

double celsius(temperature value) {
    return value == INVALID_TEMP ? std::numeric_limits<double>::quiet_NaN()
        : (static_cast<int32_t>(value) - C_OFFSET) / 512.0;
}
}

GlycolLogger glycolLog;

GlycolLogger::GlycolLogger()
    : last_state_(GLYCOL_IDLE), last_algorithm_(GlycolCooling::Algorithm::PredictiveCoast) {}

const char* GlycolLogger::stateToString(GlycolState state) {
    switch (state) {
        case GLYCOL_IDLE: return "IDLE";
        case GLYCOL_COOLING: return "COOLING";
        case GLYCOL_COASTING: return "COASTING";
        case GLYCOL_FULL_COOLING: return "FULL_COOLING";
        case GLYCOL_HEATING: return "HEATING";
        default: return "UNKNOWN";
    }
}

bool GlycolLogger::writeHeader() {
    FILE* file = fs_open(LOG_FILENAME, "w");
    if (!file) return false;
    const bool written = fputs(HEADER, file) >= 0;
    const bool closed = fclose(file) == 0;
    return written && closed;
}

bool GlycolLogger::archiveLog() {
    if (fs_exists(ARCHIVED_LOG_FILENAME) && !fs_remove(ARCHIVED_LOG_FILENAME)) return false;
    char currentPath[288], archivedPath[288];
    snprintf(currentPath, sizeof(currentPath), "%s%s", FS_PREFIX, LOG_FILENAME);
    snprintf(archivedPath, sizeof(archivedPath), "%s%s", FS_PREFIX, ARCHIVED_LOG_FILENAME);
    // A failed rename leaves the current recording untouched.
    if (rename(currentPath, archivedPath) != 0) return false;
    return writeHeader();
}

bool GlycolLogger::prepareLog() {
    if (!fs_exists(LOG_FILENAME)) {
        schema_checked_ = writeHeader();
        return schema_checked_;
    }
    if (!schema_checked_) {
        // Preserve the old controller's differently shaped CSV as an archive;
        // never append rows with new meanings below the legacy column names.
        FILE* file = fs_open(LOG_FILENAME, "r");
        if (!file) return false;
        char header[sizeof(HEADER) + 1];
        const bool current = fgets(header, sizeof(header), file) && std::strcmp(header, HEADER) == 0;
        fclose(file);
        if (!current && !archiveLog()) return false;
        schema_checked_ = true;
    }
    return getLogSize() <= MAX_LOG_SIZE || archiveLog();
}

void GlycolLogger::logTransition(const GlycolRuntimeState& runtime,
                                temperature raw_temperature, temperature setpoint) {
    const auto algorithm = runtime.cooling.selection();
    if (runtime.state == last_state_ && algorithm == last_algorithm_) return;
    if (!prepareLog()) return;
    FILE* file = fs_open(LOG_FILENAME, "a");
    if (!file) return;

    char timestamp[32];
    if (!getFormattedTime(timestamp, sizeof(timestamp))) strcpy(timestamp, "N/A");
    const auto& output = runtime.cooling_output;
    const bool was_cooling = last_state_ == GLYCOL_COOLING || last_state_ == GLYCOL_FULL_COOLING;
    // Inhibition discards an incomplete core response. Retain the real active
    // interval in the transition record even when the core's duration resets.
    const double actual_on_s = output.pump_on || was_cooling
        ? runtime.last_pump_active_s - runtime.pump_started_s : output.actual_on_s;
    const int written = fprintf(file,
        "%s,%lu,%s,%s,%s,%.6f,%.6f,%.9f,%.3f,%.3f,%.9f,%.9f,%lu,%lu,%.3f,%.6f,%s\n",
        timestamp, (unsigned long)ticks.millis(), stateToString(last_state_),
        stateToString(runtime.state), runtime.cooling.algorithmVersion(),
        celsius(raw_temperature), celsius(setpoint), output.rate_c_per_s, actual_on_s,
        output.coast_s, output.budget_gain_c_per_s, output.gain_c_per_on_s,
        (unsigned long)output.learning_updates, (unsigned long)output.response_updates,
        output.pulse_budget_s, output.predicted_endpoint_c,
        runtime.state == GLYCOL_HEATING ? "HEATING" : GlycolCooling::Controller::phaseName(output.phase));
    const bool closed = fclose(file) == 0;
    if (written >= 0 && closed) {
        last_state_ = runtime.state;
        last_algorithm_ = algorithm;
    }
}

void GlycolLogger::clearLog() {
    if (fs_exists(LOG_FILENAME) && !fs_remove(LOG_FILENAME)) return;
    if (fs_exists(ARCHIVED_LOG_FILENAME) && !fs_remove(ARCHIVED_LOG_FILENAME)) return;
    schema_checked_ = writeHeader();
    Log.notice("GlycolLog: Log cleared");
}

void GlycolLogger::logReboot() {
    if (!prepareLog()) return;
    FILE* file = fs_open(LOG_FILENAME, "a");
    if (!file) return;
    char timestamp[32];
    if (!getFormattedTime(timestamp, sizeof(timestamp))) strcpy(timestamp, "N/A");
    fprintf(file, "%s,%lu,REBOOT,REBOOT,,nan,nan,0,0,nan,nan,nan,0,0,0,nan,REBOOT\n",
            timestamp, (unsigned long)ticks.millis());
    fclose(file);
    Log.notice("GlycolLog: Logged reboot event");
}

size_t GlycolLogger::getLogSize() {
    char fullpath[288];
    snprintf(fullpath, sizeof(fullpath), "%s%s", FS_PREFIX, LOG_FILENAME);
    struct stat st;
    return stat(fullpath, &st) == 0 ? st.st_size : 0;
}
#endif // ENABLE_GLYCOL_LOGGING
