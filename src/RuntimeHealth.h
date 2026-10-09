#pragma once

#include <cstdint>

namespace RuntimeHealth {
struct Snapshot {
    bool loopStarted;
    bool loopTaskFallback;
    uint32_t loopIterations;
    uint64_t lastLoopUs;
};

// A bounded, read-only snapshot. Reading health never starts tasks or touches I/O.
Snapshot snapshot();
}
