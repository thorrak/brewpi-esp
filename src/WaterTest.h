#pragma once

#include <ArduinoJson.h>
#include <cstdint>
#include <string>

namespace WaterTest {
// init may run before settings/device installation; never starts outputs.
void init();
void tick();
bool active();
// Includes queued start and completed test held OFF awaiting explicit resume.
bool controlOwned();
bool requestStart(JsonVariantConst body, std::string &error);
bool requestStop(std::string &error);
bool requestResume(JsonVariantConst body, std::string &error);
void status(JsonDocument &doc);
// Scanner calls once per physical read attempt (including failed conversion/read).
// Address uses the scanner's uint64_t byte order. Does no flash or network I/O.
void onSample(uint64_t address, int16_t rawSixteenths, bool valid, uint64_t conversionStartUs, uint64_t readUs);
} // namespace WaterTest
