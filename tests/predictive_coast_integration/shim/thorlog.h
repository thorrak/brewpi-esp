#pragma once
struct HostLog {
    template<typename... Args> void notice(const char*, Args...) {}
};
extern HostLog Log;
