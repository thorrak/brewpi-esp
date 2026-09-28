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
constexpr size_t maximumDocumentBytes = 32768;
size_t freeBytes();
bool saveDocument(const char *path, const JsonDocument &document);
bool loadDocument(const char *path, JsonDocument &document);
bool loadDocumentPayload(const char *path, std::string &payload);
// With a null destination, verify the envelope and return its decoded size.
// Otherwise fill the caller's bounded buffer and verify the same format/CRC.
bool readDocumentPayload(const char *path, char *destination, size_t capacity, size_t &length);
bool reserveMetadata();
void releaseMetadataReserve();
bool removePreviousDataset();
} // namespace WaterTestStorage
