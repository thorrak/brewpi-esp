#pragma once
#include <ArduinoJson.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/time.h>
#include <unistd.h>
#include <vector>
namespace Native {
inline std::string root;
inline uint64_t clock = 1000000, fsyncDelayUs = 0;
inline std::string removeFailurePath, openFailurePath;
inline std::function<void()> fsyncHook, truncateHook;
inline std::function<void(int)> delayHook;
inline bool failUploadAllocation = false;
inline bool failHistoryAllocation = false;
inline unsigned historyAllocations = 0;
inline size_t historyAllocationSize = 0;
inline std::function<void()> uploadStartHook;
inline void *allocateHistory(size_t size) {
  ++historyAllocations;
  historyAllocationSize = size;
  return failHistoryAllocation ? nullptr : std::malloc(size);
}
inline bool invalidUploadAcknowledgement = false;
inline int nextHttpResult = 0, nextSocketError = 0, nextConnectionError = 0, nextHeaderResult = 0;
inline bool failHttpAllocation = false;
inline unsigned httpInitializations = 0, httpOpens = 0, httpWrites = 0, httpReads = 0;
inline size_t maxHttpWrite = 0, maxHttpRead = 0, shortHttpWrite = SIZE_MAX;
inline size_t failWriteAfter = SIZE_MAX, failReadAfter = SIZE_MAX;
inline bool incompleteHttpResponse = false;
inline std::vector<std::string> partialPayloads;
inline std::vector<size_t> declaredPayloadLengths;
inline std::string nextHttpBody;
inline unsigned httpCleanups = 0;
inline void *uploadWorkspace = nullptr;
inline size_t uploadAllocationSize = 0, uploadAllocationLimit = SIZE_MAX;
inline std::vector<size_t> uploadAllocationRequests;
inline size_t freeBytes = 650000;
inline int fsyncUntilFail = -1, delays = 0, nextHttpCode = 201;
inline bool connected = true;
inline unsigned randomCounter = static_cast<unsigned>(getpid()), resumes = 0;
inline unsigned taskCreates = 0, taskDeletes = 0;
inline bool failTaskCreation = false;
inline void (*scheduledTask)(void *) = nullptr;
inline void *scheduledTaskArgument = nullptr;
inline std::vector<std::string> payloads;
struct Yield {};
struct TaskDeleted : Yield {};
} // namespace Native
constexpr unsigned MALLOC_CAP_8BIT = 1;
inline void *heap_caps_malloc(size_t size, unsigned capabilities) {
  assert(capabilities == MALLOC_CAP_8BIT);
  assert(Native::uploadWorkspace == nullptr);
  Native::uploadAllocationSize = size;
  Native::uploadAllocationRequests.push_back(size);
  if (Native::failUploadAllocation || size > Native::uploadAllocationLimit)
    return nullptr;
  void *memory = std::malloc(size);
  Native::uploadWorkspace = memory;
  return memory;
}
inline void heap_caps_free(void *memory) {
  if (memory == Native::uploadWorkspace)
    Native::uploadWorkspace = nullptr;
  std::free(memory);
}
inline size_t heap_caps_get_free_size(unsigned capabilities) { assert(capabilities == MALLOC_CAP_8BIT); return 32768; }
inline size_t heap_caps_get_largest_free_block(unsigned capabilities) { assert(capabilities == MALLOC_CAP_8BIT); return 2048; }
using temperature = int16_t;
struct ControlSettings {
  char mode = 'b';
  temperature beerSetting = 10000, fridgeSetting = 5000, heatEstimator = 128, coolEstimator = 256;
};
struct Actuator {
  bool state = false;
  std::vector<bool> edges;
  std::vector<uint64_t> edgeTimes;
  void setActive(bool on) {
    if (on != state) {
      edges.push_back(on);
      edgeTimes.push_back(Native::clock);
    }
    state = on;
  }
  bool isActive() { return state; }
};
namespace Config {
namespace EepromFormat {
constexpr unsigned MAX_DEVICES = 4;
}
namespace Version {
constexpr const char *release = "test";
constexpr const char *git_sha = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr bool git_dirty = true;
} // namespace Version
} // namespace Config
namespace Modes {
constexpr char off = 'o';
}
#include "GlycolCoolingController.h"
struct Cooling {
  double minOn = 2, minOff = 2;
  double minOnSeconds() { return minOn; }
  double minOffSeconds() { return minOff; }
};
struct TempController {
  Actuator pump, heat, lamp, blower;
  Actuator *cooler = &pump;
  Actuator *heater = &heat;
  Actuator *light = &lamp;
  Actuator *fan = &blower;
  struct {
    bool lightAsHeater = false;
  } cc;
  struct {
    Cooling cooling;
  } glycolRuntime;
  ControlSettings cs;
  void resumeAfterWaterTest(const ControlSettings &saved) {
    cs = saved;
    ++Native::resumes;
  }
};
inline TempController tempControl;
struct {
  bool glycol = true;
  GlycolCooling::Algorithm glycolCoolingAlgorithm = GlycolCooling::Algorithm::PredictiveCoast;
} inline extendedSettings;
struct {
  uint16_t MIN_COOL_ON_TIME = 180, MIN_COOL_OFF_TIME = 300;
} inline minTimes;
enum DeviceFunction {
  DEVICE_NONE,
  DEVICE_CHAMBER_HEAT = 2,
  DEVICE_CHAMBER_COOL = 3,
  DEVICE_CHAMBER_TEMP = 5,
  DEVICE_BEER_TEMP = 9
};
enum DeviceHardware { DEVICE_HARDWARE_NONE, DEVICE_HARDWARE_PIN, DEVICE_HARDWARE_ONEWIRE_TEMP };
struct DeviceConfig {
  uint8_t chamber = 1;
  DeviceFunction deviceFunction = DEVICE_NONE;
  DeviceHardware deviceHardware = DEVICE_HARDWARE_NONE;
  struct {
    uint8_t pinNr = 0;
    bool invert = false, deactivate = false;
    uint8_t address[8] = {};
    int8_t calibration = 0;
  } hw;
};
struct {
  DeviceConfig devices[Config::EepromFormat::MAX_DEVICES];
  DeviceConfig fetchDevice(unsigned i) { return devices[i]; }
} inline eepromManager;
#define FIRMWARE_REVISION "test-revision"
#define CONTROLLER_TYPE "native-test"
#define FS_PREFIX Native::root.c_str()
inline bool fs_exists(const char *path) { return std::filesystem::exists(Native::root + path); }
inline bool fs_remove(const char *path) {
  if (Native::removeFailurePath == path)
    return false;
  return ::remove((Native::root + path).c_str()) == 0;
}
inline FILE *fs_open(const char *path, const char *mode) {
  if (Native::openFailurePath == path)
    return nullptr;
  return fopen((Native::root + path).c_str(), mode);
}
inline int native_fsync(int fd) {
  Native::clock += Native::fsyncDelayUs;
  if (Native::fsyncHook)
    Native::fsyncHook();
  if (Native::fsyncUntilFail == 0) {
    Native::fsyncUntilFail = -1;
    return -1;
  }
  if (Native::fsyncUntilFail > 0)
    --Native::fsyncUntilFail;
  return ::fsync(fd);
}
#define fsync native_fsync
inline int native_ftruncate(int fd, off_t size) {
  if (Native::truncateHook)
    Native::truncateHook();
  return ::ftruncate(fd, size);
}
#define ftruncate native_ftruncate
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, pdPASS = 1, portMAX_DELAY = -1;
constexpr int ESP_ERR_TIMEOUT = 263, ESP_ERR_HTTP_EAGAIN = 28679;
constexpr int ESP_ERR_NO_MEM = 257, ESP_ERR_HTTP_CONNECT = 28674, ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOST = 32769;
inline const char *esp_err_to_name(int error) {
  return error == ESP_ERR_HTTP_CONNECT ? "ESP_ERR_HTTP_CONNECT"
       : error == ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOST ? "ESP_ERR_ESP_TLS_CANNOT_RESOLVE_HOST"
       : error == ESP_ERR_TIMEOUT ? "ESP_ERR_TIMEOUT" : error == ESP_ERR_HTTP_EAGAIN ? "ESP_ERR_HTTP_EAGAIN"
       : error == ESP_ERR_NO_MEM ? "ESP_ERR_NO_MEM" : error == ESP_OK ? "ESP_OK" : "ESP_FAIL";
}
inline int esp_littlefs_info(const char *, size_t *total, size_t *used) {
  *total = Native::freeBytes;
  *used = 0;
  return ESP_OK;
}
inline int64_t esp_timer_get_time() { return Native::clock; }
inline int esp_reset_reason() { return 1; }
inline void esp_fill_random(void *p, size_t n) {
  auto bytes = static_cast<uint8_t *>(p);
  for (size_t i = 0; i < n; ++i)
    bytes[i] = (++Native::randomCounter * 73) % 255;
}
inline bool isNtpSynced() { return false; }
inline void getGuid(char *p) { strcpy(p, "AABBCCDDEEFF0011"); }
inline bool bp_wifi_is_connected() { return Native::connected; }
using SemaphoreHandle_t = void *;
inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return reinterpret_cast<void *>(1); }
inline int xSemaphoreTakeRecursive(SemaphoreHandle_t, int) { return pdPASS; }
inline int xSemaphoreGiveRecursive(SemaphoreHandle_t) { return pdPASS; }
struct Queue {
  size_t capacity, size;
  std::deque<std::vector<uint8_t>> items;
};
using QueueHandle_t = Queue *;
inline QueueHandle_t xQueueCreate(size_t count, size_t size) { return new Queue{count, size, {}}; }
inline int xQueueSend(Queue *q, const void *p, int) {
  if (q->items.size() >= q->capacity)
    return 0;
  auto b = static_cast<const uint8_t *>(p);
  q->items.emplace_back(b, b + q->size);
  return pdPASS;
}
inline size_t uxQueueMessagesWaiting(Queue *q) { return q->items.size(); }
inline int xQueueReceive(Queue *q, void *p, int) {
  if (q->items.empty())
    return 0;
  memcpy(p, q->items.front().data(), q->size);
  q->items.pop_front();
  return pdPASS;
}
inline int xTaskCreate(void (*entry)(void *), const char *, unsigned stackSize, void *argument, unsigned, void *) {
  if (Native::uploadStartHook)
    Native::uploadStartHook();
  ++Native::taskCreates;
  assert(stackSize == 12288);
  if (Native::failTaskCreation)
    return 0;
  assert(Native::scheduledTask == nullptr);
  Native::scheduledTask = entry;
  Native::scheduledTaskArgument = argument;
  return pdPASS;
}
inline void vTaskDelete(void *handle) {
  assert(handle == nullptr);
  ++Native::taskDeletes;
  Native::scheduledTask = nullptr;
  Native::scheduledTaskArgument = nullptr;
  throw Native::TaskDeleted{};
}
inline int pdMS_TO_TICKS(int n) { return n; }
inline void vTaskDelay(int milliseconds) {
  if (Native::delayHook)
    Native::delayHook(milliseconds);
  if (Native::delays-- <= 0)
    throw Native::Yield{};
}
enum esp_http_client_method_t { HTTP_METHOD_POST, HTTP_METHOD_PUT };
constexpr int HTTP_EVENT_ON_DATA = 1;
struct esp_http_client_event_t {
  int event_id;
  void *user_data;
  const void *data;
  int data_len;
};
struct esp_http_client_config_t {
  const char *url;
  esp_http_client_method_t method;
  int timeout_ms;
  bool disable_auto_redirect;
  esp_err_t (*event_handler)(esp_http_client_event_t *);
  void *user_data;
};
struct Http {
  esp_http_client_config_t config;
  std::string body, response;
  size_t expectedLength = 0, responseRead = 0;
  bool opened = false, captured = false;
};
using esp_http_client_handle_t = Http *;
inline Http *esp_http_client_init(const esp_http_client_config_t *c) {
  if (Native::uploadStartHook)
    Native::uploadStartHook();
  ++Native::httpInitializations;
  assert(c->disable_auto_redirect);
  assert(strncmp(c->url, "http://chill.fermentrack.net/", 27) == 0);
  if (Native::failHttpAllocation)
    return nullptr;
  return new Http{*c, {}, {}};
}
inline int esp_http_client_set_header(Http *, const char *, const char *) { return Native::nextHeaderResult; }
inline int esp_http_client_open(Http *c, int length) {
  ++Native::httpOpens;
  assert(length > 0);
  c->expectedLength = static_cast<size_t>(length);
  Native::declaredPayloadLengths.push_back(c->expectedLength);
  c->opened = Native::nextHttpResult == ESP_OK;
  return Native::nextHttpResult;
}
inline int esp_http_client_write(Http *c, const char *data, int length) {
  assert(c->opened && length > 0);
  ++Native::httpWrites;
  Native::maxHttpWrite = std::max(Native::maxHttpWrite, static_cast<size_t>(length));
  assert(c->body.size() + static_cast<size_t>(length) <= c->expectedLength);
  if (c->body.size() >= Native::failWriteAfter)
    return -1;
  const size_t accepted = std::min({static_cast<size_t>(length), Native::shortHttpWrite,
                                  Native::failWriteAfter - c->body.size()});
  c->body.append(data, accepted);
  return static_cast<int>(accepted);
}
inline int64_t esp_http_client_fetch_headers(Http *c) {
  assert(c->opened && c->body.size() == c->expectedLength);
  assert(!c->captured);
  c->captured = true;
  Native::payloads.emplace_back(c->body);
  JsonDocument payload, response;
  assert(deserializeJson(payload, c->body) == DeserializationError::Ok);
  response["test_id"] = payload["test_id"];
  if (Native::invalidUploadAcknowledgement)
    response["test_id"] = "unacknowledged-test";
  response["device_guid"] = payload["device_guid"];
  response["status"] = "stored";
  if (payload["batch_id"].is<const char *>()) {
    response["batch_id"] = payload["batch_id"];
    auto a = response["accepted_ranges"].to<JsonArray>().add<JsonArray>();
    a.add(payload["first_seq"]);
    a.add(payload["last_seq"]);
  }
  if (payload["final_outputs"].is<JsonObject>()) {
    response["upload_status"] = "complete";
    response["finish_received"] = true;
    response["missing_record_count"] = 0;
  }
  serializeJson(response, c->response);
  if (!Native::nextHttpBody.empty())
    c->response = Native::nextHttpBody;
  return static_cast<int64_t>(c->response.size());
}
inline int esp_http_client_read(Http *c, char *data, int length) {
  assert(c->captured && length > 0);
  ++Native::httpReads;
  Native::maxHttpRead = std::max(Native::maxHttpRead, static_cast<size_t>(length));
  if (c->responseRead >= Native::failReadAfter)
    return -1;
  const size_t count = std::min({static_cast<size_t>(length), c->response.size() - c->responseRead,
                               Native::failReadAfter - c->responseRead});
  std::memcpy(data, c->response.data() + c->responseRead, count);
  c->responseRead += count;
  return static_cast<int>(count);
}
inline bool esp_http_client_is_complete_data_received(Http *c) {
  return c->captured && c->responseRead == c->response.size() && !Native::incompleteHttpResponse;
}
inline int esp_http_client_get_status_code(Http *) { return Native::nextHttpCode; }
inline int esp_http_client_get_errno(Http *c) { assert(c); return Native::nextSocketError; }
inline int esp_http_client_get_and_clear_last_tls_error(Http *c, int *, int *) {
  assert(c);
  return Native::nextConnectionError;
}
inline int esp_http_client_cleanup(Http *c) {
  ++Native::httpCleanups;
  if (c->opened && !c->captured)
    Native::partialPayloads.push_back(c->body);
  delete c;
  return ESP_OK;
}
