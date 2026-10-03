#pragma once
#include <cstddef>
#include <ArduinoJson.h>

namespace WaterTestStorage {
constexpr const char *journalPath = "/water-test-records.bin";
constexpr const char *manifestPath = "/water-test-manifest.json";
constexpr const char *finishPath = "/water-test-finish.json";
constexpr const char *receiptPath = "/water-test-ack.json";
constexpr const char *resumedPath = "/water-test-resumed.json";
constexpr const char *reservePath = "/water-test-reserve.bin";
constexpr const char *controllerOnePath = "/water-test-controller-1.json";
constexpr const char *controllerTwoPath = "/water-test-controller-2.json";
constexpr size_t metadataReserveBytes = 16384;
constexpr size_t maximumDocumentBytes = 32768;
size_t freeBytes();
bool saveDocument(const char *path, const JsonDocument &document);
bool loadDocument(const char *path, JsonDocument &document);
// Verify the persisted envelope and CRC, returning the decoded payload size.
bool validateDocumentPayload(const char *path, size_t &length);
// Validate size and CRC before delivering any decoded bytes, then stream the
// same open file in bounded chunks. The caller must also validate before opening
// its HTTP request; immutable metadata must remain unchanged during upload.
bool streamDocumentPayload(const char *path, bool (*sink)(void *, const char *, size_t),
                           void *context, size_t expectedLength);
bool reserveMetadata();
void releaseMetadataReserve();
bool removePreviousDataset();
} // namespace WaterTestStorage
