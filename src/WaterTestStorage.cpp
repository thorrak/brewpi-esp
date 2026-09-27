#include "WaterTestStorage.h"
#include "ESPEepromAccess.h"
#include <string>
#include <unistd.h>
#include "WaterTestCore.h"

namespace WaterTestStorage {
size_t freeBytes() {
  size_t total = 0, used = 0;
  return esp_littlefs_info("spiffs", &total, &used) == ESP_OK && total > used ? total - used : 0;
}
// Persist the exact JSON bytes inside a checksummed envelope. Rename commits the
// complete replacement; a torn .tmp file never replaces the last good document.
bool saveDocument(const char *path, const JsonDocument &document) {
  std::string payload;
  if (document.overflowed() || !serializeJson(document, payload))
    return false;
  JsonDocument envelope;
  envelope["payload"] = payload;
  envelope["crc32"] = WaterTestCore::checksum(payload.data(), payload.size());
  std::string bytes;
  if (envelope.overflowed() || !serializeJson(envelope, bytes))
    return false;
  const std::string temporary = std::string(path) + ".tmp";
  FILE *file = fs_open(temporary.c_str(), "wb");
  if (!file)
    return false;
  bool ok = fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
  if (ok)
    ok = fflush(file) == 0 && fsync(fileno(file)) == 0;
  if (fclose(file) != 0)
    ok = false;
  if (ok) {
    const std::string from = std::string(FS_PREFIX) + temporary;
    const std::string to = std::string(FS_PREFIX) + path;
    ok = ::rename(from.c_str(), to.c_str()) == 0;
  }
  if (!ok)
    fs_remove(temporary.c_str());
  return ok;
}
bool loadDocumentPayload(const char *path, std::string &payload) {
  FILE *file = fs_open(path, "rb");
  if (!file)
    return false;
  std::string bytes;
  char buffer[512];
  size_t count;
  while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0 && bytes.size() <= 32768)
    bytes.append(buffer, count);
  const bool ok = !ferror(file) && bytes.size() <= 32768;
  fclose(file);
  JsonDocument envelope;
  if (!ok || deserializeJson(envelope, bytes) != DeserializationError::Ok ||
      !envelope["payload"].is<const char *>() || !envelope["crc32"].is<uint32_t>())
    return false;
  payload = envelope["payload"].as<std::string>();
  if (WaterTestCore::checksum(payload.data(), payload.size()) != envelope["crc32"].as<uint32_t>())
    return false;
  return true;
}
bool loadDocument(const char *path, JsonDocument &document) {
  std::string payload;
  if (!loadDocumentPayload(path, payload))
    return false;
  document.clear();
  return deserializeJson(document, payload) == DeserializationError::Ok;
}
bool reserveMetadata() {
  FILE *file = fs_open(reservePath, "wb");
  if (!file)
    return false;
  const char zeros[256] = {};
  bool ok = true;
  for (size_t written = 0; written < metadataReserveBytes && ok; written += sizeof(zeros))
    ok = fwrite(zeros, 1, sizeof(zeros), file) == sizeof(zeros);
  if (ok)
    ok = fflush(file) == 0 && fsync(fileno(file)) == 0;
  if (fclose(file) != 0)
    ok = false;
  return ok;
}
void releaseMetadataReserve() { fs_remove(reservePath); }
bool removePreviousDataset() {
  bool removed = true;
  for (auto path : {journalPath, "/water-test-manifest.json", "/water-test-ack.json", "/water-test-resumed.json",
                    "/water-test-finish.json", "/water-test-boots.json", "/water-test-reserve.bin"}) {
    if (fs_exists(path) && !fs_remove(path))
      removed = false;
    const std::string temporary = std::string(path) + ".tmp";
    if (fs_exists(temporary.c_str()) && !fs_remove(temporary.c_str()))
      removed = false;
  }
  return removed;
}
} // namespace WaterTestStorage
