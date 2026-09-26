#pragma once

#ifdef ENABLE_GLYCOL_LOGGING

#include "GlycolCoolingAlgorithm.h"
#include "TemperatureFormats.h"
#include <cstddef>

struct GlycolRuntimeState;
enum GlycolState : uint8_t;

// Optional CSV diagnostics for the selectable controllers. Call only after
// applying relay commands: filesystem latency must never delay an OFF edge.
class GlycolLogger {
public:
    GlycolLogger();
    void logTransition(const GlycolRuntimeState& runtime,
                       temperature raw_temperature, temperature setpoint);
    void logReboot();
    void clearLog();
    static const char* getLogPath() { return LOG_FILENAME; }
    size_t getLogSize();

private:
    static constexpr const char* LOG_FILENAME = "/glycol_log.csv";
    static constexpr const char* ARCHIVED_LOG_FILENAME = "/glycol_log.archived.csv";
    static constexpr size_t MAX_LOG_SIZE = 30000;
    GlycolState last_state_;
    GlycolCooling::Algorithm last_algorithm_;
    bool schema_checked_ = false;

    bool writeHeader();
    bool archiveLog();
    bool prepareLog();
    const char* stateToString(GlycolState state);
};

extern GlycolLogger glycolLog;
#endif // ENABLE_GLYCOL_LOGGING
