#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include "EepromStructs.h"
#include "JsonKeys.h"
#include "MinTimesTypes.h"

extern std::string filesystem_root;
extern unsigned settings_writes;
extern unsigned min_times_writes;
FILE *fs_open(const char *filename, const char *mode);

inline size_t test_strlcpy(char *dest, const char *src, size_t size) {
    const size_t length = std::strlen(src);
    if (size) {
        const size_t count = length < size - 1 ? length : size - 1;
        std::memcpy(dest, src, count);
        dest[count] = '\0';
    }
    return length;
}
#define strlcpy test_strlcpy

struct TempControl {
    unsigned mode_changes = 0;
    char mode = Modes::beerConstant;
    void setMode(char setting, bool) { mode = setting; ++mode_changes; }
};
extern TempControl tempControl;
extern ExtendedSettings extendedSettings;
extern MinTimes minTimes;

struct Display {
    unsigned calls = 0;
    void init() { ++calls; }
    void printStationaryText() { ++calls; }
    void printState() { ++calls; }
    void printAll() { ++calls; }
};
extern Display display;

struct PiLink {
    std::string incoming;
    std::string outgoing;
    char prefix = 0;
    void print(const char *) {}
    void printNewLine() {}
    void receiveJsonMessage(JsonDocument &doc) { deserializeJson(doc, incoming); }
    void sendJsonMessage(char type, const JsonDocument &doc) {
        prefix = type;
        outgoing.clear();
        serializeJson(doc, outgoing);
    }
};
extern PiLink piLink;
struct Logger { void error(const char *) {} };
extern Logger Log;
struct CommandProcessor {
    void processExtendedSettingsJson();
    void sendExtendedSettings();
};
bool processExtendedSettingsJson(const JsonDocument &, bool);
void serveExtendedSettings(JsonDocument &);
