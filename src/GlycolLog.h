#pragma once

#ifdef ENABLE_GLYCOL_LOGGING // Doing this using define gating instead of an extended setting for now

#include <Arduino.h>
#include "GlycolParams.h"

/**
 * \file GlycolLog.h
 * \brief CSV logging for glycol state transitions
 *
 * Logs state transitions and diagnostic information to a CSV file
 * for later analysis via the web interface.
 */

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
     * \param from_state - Previous state
     * \param to_state - New state
     * \param current_temp - Current beer temperature
     * \param setpoint - Target temperature
     * \param cooling_rate - Current cooling rate (°/min)
     * \param cooling_duration_s - Duration of cooling cycle (seconds)
     * \param coast_estimate - Estimated coast amount
     * \param learned_k - Current learned k parameter
     * \param learned_C_off - Current learned C_off parameter
     * \param learned_L - Current learned dead time
     * \param force_minimum - Whether force_minimum_cooling is set
     * \param reason - Human-readable reason for transition
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
     * \return Path to the CSV log file
     */
    static const char* getLogPath() { return LOG_FILENAME; }

    /**
     * \brief Get the size of the log file in bytes
     * \return File size, or 0 if file doesn't exist
     */
    size_t getLogSize();

private:
    static constexpr const char* LOG_FILENAME = "/glycol_log.csv";
    static constexpr const char* ARCHIVED_LOG_FILENAME = "/glycol_log.archived.csv";
    static constexpr size_t MAX_LOG_SIZE = 40000;

    void writeHeader();
    void rotateLogIfNeeded();
    const char* stateToString(GlycolState state);
};

// Global instance
extern GlycolLogger glycolLog;
#endif // ENABLE_GLYCOL_LOGGING
