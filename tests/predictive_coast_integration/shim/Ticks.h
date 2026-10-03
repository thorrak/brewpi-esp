#pragma once
#include <cstdint>
class HostTicks {
public:
    uint64_t now_ms = 0;
    uint32_t millis() const { return uint32_t(now_ms); }
    uint16_t seconds() const { return uint16_t(now_ms / 1000); }
    uint16_t timeSince(uint16_t previous) const {
        return uint16_t(seconds() - previous);
    }
};
extern HostTicks ticks;
