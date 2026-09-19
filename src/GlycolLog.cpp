#ifdef ENABLE_GLYCOL_LOGGING // Doing this using define gating instead of an extended setting for now

#include "GlycolLog.h"
#include "ESPEepromAccess.h"  // For FILESYSTEM
#include "ESP_BP_WiFi.h"
#include "ntp.h"              // For getFormattedTime
#include "TempControl.h"      // For GlycolState enum

#include <LittleFS.h>

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
    File file = FILESYSTEM.open(LOG_FILENAME, "w");
    if (!file) {
        Serial.println("GlycolLog: Failed to create log file");
        return;
    }

    file.println("timestamp,millis,from_state,to_state,current_temp,setpoint,cooling_rate,cooling_duration_s,coast_estimate,learned_k,learned_C_off,learned_L,force_minimum,reason");
    file.close();
}

void GlycolLogger::rotateLogIfNeeded() {
    if (!FILESYSTEM.exists(LOG_FILENAME)) {
        return;
    }

    File file = FILESYSTEM.open(LOG_FILENAME, "r");
    if (!file) {
        return;
    }

    size_t fileSize = file.size();
    file.close();

    if (fileSize > MAX_LOG_SIZE) {
        if (FILESYSTEM.exists(ARCHIVED_LOG_FILENAME)) {
            FILESYSTEM.remove(ARCHIVED_LOG_FILENAME);
        }

        if (!FILESYSTEM.rename(LOG_FILENAME, ARCHIVED_LOG_FILENAME)) {
            Serial.println("GlycolLog: Failed to archive log during rotation");
        }

        writeHeader();
        Serial.println("GlycolLog: Log rotated due to size limit");
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
    if (!FILESYSTEM.exists(LOG_FILENAME)) {
        writeHeader();
    }

    File file = FILESYSTEM.open(LOG_FILENAME, "a");
    if (!file) {
        Serial.println("GlycolLog: Failed to open log file for writing");
        return;
    }

    // Get timestamp
    char timestamp[32];
    if (!getFormattedTime(timestamp, sizeof(timestamp))) {
        // If NTP not synced, use "N/A"
        strcpy(timestamp, "N/A");
    }

    // Write CSV line
    // Format: timestamp,millis,from_state,to_state,current_temp,setpoint,cooling_rate,
    //         cooling_duration_s,coast_estimate,learned_k,learned_C_off,learned_L,force_minimum,reason
    file.printf("%s,%lu,%s,%s,%.3f,%.3f,%.4f,%u,%.3f,%.2f,%.3f,%.1f,%d,\"%s\"\n",
        timestamp,
        millis(),
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

    file.close();

    // Also log to serial for debugging
    Serial.printf("GlycolLog: %s -> %s (%s)\n",
        stateToString(from_state),
        stateToString(to_state),
        reason
    );
}

void GlycolLogger::clearLog() {
    if (FILESYSTEM.exists(LOG_FILENAME)) {
        FILESYSTEM.remove(LOG_FILENAME);
    }
    if (FILESYSTEM.exists(ARCHIVED_LOG_FILENAME)) {
        FILESYSTEM.remove(ARCHIVED_LOG_FILENAME);
    }
    writeHeader();
    Serial.println("GlycolLog: Log cleared");
}

void GlycolLogger::logReboot() {
    rotateLogIfNeeded();

    // Create file with header if it doesn't exist
    if (!FILESYSTEM.exists(LOG_FILENAME)) {
        writeHeader();
    }

    File file = FILESYSTEM.open(LOG_FILENAME, "a");
    if (!file) {
        Serial.println("GlycolLog: Failed to open log file for reboot entry");
        return;
    }

    // Get timestamp
    char timestamp[32];
    if (!getFormattedTime(timestamp, sizeof(timestamp))) {
        strcpy(timestamp, "N/A");
    }

    // Write reboot entry with zeros for data fields
    file.printf("%s,%lu,REBOOT,REBOOT,0,0,0,0,0,0,0,0,0,\"Controller reboot\"\n",
        timestamp,
        millis()
    );

    file.close();
    Serial.println("GlycolLog: Logged reboot event");
}

size_t GlycolLogger::getLogSize() {
    if (!FILESYSTEM.exists(LOG_FILENAME)) {
        return 0;
    }

    File file = FILESYSTEM.open(LOG_FILENAME, "r");
    if (!file) {
        return 0;
    }

    size_t size = file.size();
    file.close();
    return size;
}
#endif // ENABLE_GLYCOL_LOGGING
