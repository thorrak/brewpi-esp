#include "WaterTestStorage.h"
#include "ESPEepromAccess.h"
#include <string>
#include <unistd.h>

namespace WaterTestStorage {
bool readJson(const char *path, JsonDocument &doc) {
  FILE *f = fs_open(path, "rb");
  if (!f)
    return false;
  char buffer[512];
  std::string text;
  size_t n;
  while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) {
    text.append(buffer, n);
    if (text.size() > 16384) {
      fclose(f);
      return false;
    }
  }
  fclose(f);
  return deserializeJson(doc, text) == DeserializationError::Ok;
}
bool atomicJson(const char *path, const JsonDocument &doc) {
  std::string target = std::string(FS_PREFIX) + path, tmp = target + ".tmp", text;
  serializeJson(doc, text);
  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f)
    return false;
  bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
  ok = fflush(f) == 0 && ok;
  ok = fsync(fileno(f)) == 0 && ok;
  ok = fclose(f) == 0 && ok;
  if (ok)
    ok = rename(tmp.c_str(), target.c_str()) == 0;
  if (!ok)
    remove(tmp.c_str());
  return ok;
}
size_t freeBytes() {
  size_t total = 0, used = 0;
  return esp_littlefs_info("spiffs", &total, &used) == ESP_OK && total > used ? total - used : 0;
}
bool allocateReserve() {
  FILE *f = fs_open(reservePath, "wb");
  if (!f)
    return false;
  uint8_t zero[256] = {};
  bool ok = true;
  for (unsigned i = 0; i < 32; ++i)
    if (fwrite(zero, 1, sizeof(zero), f) != sizeof(zero)) {
      ok = false;
      break;
    }
  ok = fflush(f) == 0 && ok;
  ok = fsync(fileno(f)) == 0 && ok;
  fclose(f);
  return ok;
}
bool removePreviousDataset() {
  // Only a submitted, released dataset reaches replacement. Retire its manifest
  // first: a failure here preserves the complete old dataset; later failures
  // leave orphan files that recovery ignores and the next start must remove.
  // Publishing the new manifest is allowed only after this entire cleanup passes.
  for (auto path : {manifestPath, ackPath, resumedPath, finishPath, bootsPath, reservePath, journalPath})
    if (fs_exists(path) && !fs_remove(path))
      return false;
  return true;
}
} // namespace WaterTestStorage
