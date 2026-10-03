#include <ArduinoJson.h>
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace Native {
char root[256];
size_t maximumNew = std::numeric_limits<size_t>::max();
size_t largestNew = 0;
bool failSync = false;
}
// The hardware crash was a 7681-byte std::string allocation. Reject these
// allocations during the real save/load functions, rather than merely checking
// output size. ArduinoJson's retained document allocations use malloc separately.
void *operator new(size_t size) {
  Native::largestNew = std::max(Native::largestNew, size);
  if (size > Native::maximumNew)
    throw std::bad_alloc();
  if (void *memory = std::malloc(size))
    return memory;
  throw std::bad_alloc();
}
void *operator new[](size_t size) { return ::operator new(size); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, size_t) noexcept { std::free(memory); }

#define FS_PREFIX Native::root
inline FILE *fs_open(const char *path, const char *mode) {
  char full[512];
  snprintf(full, sizeof(full), "%s%s", Native::root, path);
  return fopen(full, mode);
}
inline bool fs_remove(const char *path) {
  char full[512];
  snprintf(full, sizeof(full), "%s%s", Native::root, path);
  return remove(full) == 0;
}
inline bool fs_exists(const char *path) {
  FILE *file = fs_open(path, "rb");
  if (file)
    fclose(file);
  return file != nullptr;
}
constexpr int ESP_OK = 0;
inline int esp_littlefs_info(const char *, size_t *total, size_t *used) {
  *total = 832 * 1024;
  *used = 0;
  return ESP_OK;
}
inline int checkedFsync(int descriptor) { return Native::failSync ? -1 : ::fsync(descriptor); }
#define fsync checkedFsync
