#pragma once
#include <cstddef>
#include <string>
#include <ArduinoJson.h>

namespace WaterTestStorage {
constexpr const char *journalPath = "/water-test-records.bin";
constexpr const char *manifestPath = "/water-test-manifest.json";
constexpr const char *finishPath = "/water-test-finish.json";
constexpr const char *receiptPath = "/water-test-ack.json";
constexpr const char *resumedPath = "/water-test-resumed.json";
constexpr const char *reservePath = "/water-test-reserve.bin";
constexpr size_t metadataReserveBytes = 16384;
size_t freeBytes();
bool saveDocument(const char *path, const JsonDocument &document);
bool loadDocument(const char *path, JsonDocument &document);
bool loadDocumentPayload(const char *path, std::string &payload);
bool reserveMetadata();
void releaseMetadataReserve();
bool removePreviousDataset();
} // namespace WaterTestStorage
