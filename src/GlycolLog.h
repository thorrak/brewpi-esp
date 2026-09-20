#pragma once

#ifdef ENABLE_GLYCOL_LOGGING // Doing this using define gating instead of an extended setting for now

#include "GlycolParams.h"

// Forward declaration
enum GlycolState : uint8_t;

/**
 * \brief Glycol state transition logger
 *
 * Writes CSV entries for each glycol state transition with comprehensive
 * diagnostic information including timestamps, temperatures, rates, and
 * learned parameters.
 */
class GlycolLogger {
public:
    GlycolLogger();

    /**
     * \brief Log a state transition
     */
    void logTransition(
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
    );

    /**
     * \brief Log a reboot event
     */
    void logReboot();

    /**
     * \brief Clear the log file
     */
    void clearLog();

    /**
     * \brief Get the log file path
     */
    static const char* getLogPath() { return LOG_FILENAME; }

    /**
     * \brief Get the size of the log file in bytes
     */
    size_t getLogSize();

private:
    static constexpr const char* LOG_FILENAME = "/glycol_log.csv";
    static constexpr const char* ARCHIVED_LOG_FILENAME = "/glycol_log.archived.csv";
    static constexpr size_t MAX_LOG_SIZE = 30000;  // Keep both logs within the small filesystem budget

    void writeHeader();
    void rotateLogIfNeeded();
    const char* stateToString(GlycolState state);
};

// Global instance
extern GlycolLogger glycolLog;
#endif // ENABLE_GLYCOL_LOGGING
