#ifdef ENABLE_GLYCOL_LOGGING

#include "GlycolLog.h"
#include "ESPEepromAccess.h"  // For fs_open, fs_exists, fs_remove
#include "ntp.h"              // For getFormattedTime
#include "TempControl.h"      // For GlycolState enum
#include "Ticks.h"

#include <thorlog.h>
#include <cstdio>
#include <sys/stat.h>

// Global instance
GlycolLogger glycolLog;

GlycolLogger::GlycolLogger() {
    // Constructor - nothing to initialize
}

const char* GlycolLogger::stateToString(GlycolState state) {
    switch (state) {
        case GLYCOL_IDLE:             return "IDLE";
        case GLYCOL_COOLING:          return "COOLING";
        case GLYCOL_COASTING:         return "COASTING";
        case GLYCOL_EMERGENCY_COOLING: return "EMERGENCY";
        case GLYCOL_HEATING:          return "HEATING";
        default:                       return "UNKNOWN";
    }
}

void GlycolLogger::writeHeader() {
    FILE* file = fs_open(LOG_FILENAME, "w");
    if (!file) {
        logWarning("GlycolLog: Failed to create log file");
        return;
    }

    fprintf(file, "timestamp,millis,from_state,to_state,current_temp,setpoint,cooling_rate,cooling_duration_s,coast_estimate,learned_k,learned_C_off,learned_L,force_minimum,reason\n");
    fclose(file);
}

void GlycolLogger::rotateLogIfNeeded() {
    if (!fs_exists(LOG_FILENAME)) {
        return;
    }

    size_t fileSize = getLogSize();

    if (fileSize > MAX_LOG_SIZE) {
        if (fs_exists(ARCHIVED_LOG_FILENAME)) {
            fs_remove(ARCHIVED_LOG_FILENAME);
        }

        char currentPath[288];
        char archivedPath[288];
        snprintf(currentPath, sizeof(currentPath), "%s%s", FS_PREFIX, LOG_FILENAME);
        snprintf(archivedPath, sizeof(archivedPath), "%s%s", FS_PREFIX, ARCHIVED_LOG_FILENAME);
        if (rename(currentPath, archivedPath) != 0) {
            logWarning("GlycolLog: Failed to archive log during rotation");
        }

        writeHeader();
        logInfo("GlycolLog: Log rotated due to size limit");
    }
}

void GlycolLogger::logTransition(
    GlycolState from_state,
    GlycolState to_state,
    float current_temp,
    float setpoint,
    float cooling_rate,
    uint16_t cooling_duration_s,
    float coast_estimate,
    float learned_k,
    float learned_C_off,
    float learned_L,
    bool force_minimum,
    const char* reason
) {
    rotateLogIfNeeded();

    // Create file with header if it doesn't exist
    if (!fs_exists(LOG_FILENAME)) {
        writeHeader();
    }

    FILE* file = fs_open(LOG_FILENAME, "a");
    if (!file) {
        logWarning("GlycolLog: Failed to open log file for writing");
        return;
    }

    // Get timestamp
    char timestamp[32];
    if (!getFormattedTime(timestamp, sizeof(timestamp))) {
        strcpy(timestamp, "N/A");
    }

    // Write CSV line
    fprintf(file, "%s,%lu,%s,%s,%.3f,%.3f,%.4f,%u,%.3f,%.2f,%.3f,%.1f,%d,\"%s\"\n",
        timestamp,
        (unsigned long)ticks.millis(),
        stateToString(from_state),
        stateToString(to_state),
        current_temp,
        setpoint,
        cooling_rate,
        cooling_duration_s,
        coast_estimate,
        learned_k,
        learned_C_off,
        learned_L,
        force_minimum ? 1 : 0,
        reason
    );

    fclose(file);

    logDebug("GlycolLog: %s -> %s (%s)",
        stateToString(from_state),
        stateToString(to_state),
        reason
    );
}

void GlycolLogger::clearLog() {
    if (fs_exists(LOG_FILENAME)) {
        fs_remove(LOG_FILENAME);
    }
    if (fs_exists(ARCHIVED_LOG_FILENAME)) {
        fs_remove(ARCHIVED_LOG_FILENAME);
    }
    writeHeader();
    logInfo("GlycolLog: Log cleared");
}

void GlycolLogger::logReboot() {
    rotateLogIfNeeded();

    // Create file with header if it doesn't exist
    if (!fs_exists(LOG_FILENAME)) {
        writeHeader();
    }

    FILE* file = fs_open(LOG_FILENAME, "a");
    if (!file) {
        logWarning("GlycolLog: Failed to open log file for reboot entry");
        return;
    }

    // Get timestamp
    char timestamp[32];
    if (!getFormattedTime(timestamp, sizeof(timestamp))) {
        strcpy(timestamp, "N/A");
    }

    // Write reboot entry with zeros for data fields
    fprintf(file, "%s,%lu,REBOOT,REBOOT,0,0,0,0,0,0,0,0,0,\"Controller reboot\"\n",
        timestamp,
        (unsigned long)ticks.millis()
    );

    fclose(file);
    logInfo("GlycolLog: Logged reboot event");
}

size_t GlycolLogger::getLogSize() {
    char fullpath[288];
    snprintf(fullpath, sizeof(fullpath), "%s%s", FS_PREFIX, LOG_FILENAME);
    struct stat st;
    if (stat(fullpath, &st) == 0) {
        return st.st_size;
    }
    return 0;
}
#endif // ENABLE_GLYCOL_LOGGING
