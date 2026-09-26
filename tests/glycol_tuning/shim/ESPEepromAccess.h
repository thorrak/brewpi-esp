#pragma once

#include <cerrno>
#include <cstdio>
#include <string>
#include <unistd.h>

#define FS_PREFIX "."

namespace TuningIO {
inline bool fail_open = false;
inline bool fail_sync = false;
inline bool fail_rename = false;
inline unsigned writes = 0;

inline void reset() {
    fail_open = fail_sync = fail_rename = false;
    writes = 0;
}
}

inline FILE* fs_open(const char* path, const char* mode) {
    if (mode[0] == 'w') {
        ++TuningIO::writes;
        if (TuningIO::fail_open) {
            errno = EIO;
            return nullptr;
        }
    }
    return std::fopen((std::string(FS_PREFIX) + path).c_str(), mode);
}

inline bool fs_remove(const char* path) {
    return std::remove((std::string(FS_PREFIX) + path).c_str()) == 0;
}

inline int tuning_test_fsync(int fd) {
    if (TuningIO::fail_sync) {
        errno = EIO;
        return -1;
    }
    return ::fsync(fd);
}

inline int tuning_test_rename(const char* source, const char* destination) {
    if (TuningIO::fail_rename) {
        errno = EIO;
        return -1;
    }
    return std::rename(source, destination);
}

#define fsync tuning_test_fsync
#define rename tuning_test_rename
