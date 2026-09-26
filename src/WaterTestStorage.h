#pragma once
#include <ArduinoJson.h>
#include <cstddef>

// Durable metadata and file lifecycle; sequencing and actuators live in WaterTest.cpp.
namespace WaterTestStorage {
constexpr const char *manifestPath = "/water-test-manifest.json";
constexpr const char *journalPath = "/water-test-records.bin";
constexpr const char *finishPath = "/water-test-finish.json";
constexpr const char *ackPath = "/water-test-ack.json";
constexpr const char *bootsPath = "/water-test-boots.json";
constexpr const char *resumedPath = "/water-test-resumed.json";
constexpr const char *reservePath = "/water-test-reserve.bin";
bool readJson(const char *path, JsonDocument &doc);
bool atomicJson(const char *path, const JsonDocument &doc);
size_t freeBytes();
bool allocateReserve();
bool removePreviousDataset();
} // namespace WaterTestStorage
