#include "WaterTestStorage.h"
#include "ESPEepromAccess.h"
#include <string>

namespace WaterTestStorage {
size_t freeBytes() {
  size_t total = 0, used = 0;
  return esp_littlefs_info("spiffs", &total, &used) == ESP_OK && total > used ? total - used : 0;
}
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
