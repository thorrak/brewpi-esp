#pragma once
#include <ArduinoJson.h>
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/time.h>
#include <unistd.h>
#include <vector>
namespace Native {
inline std::string root;
inline uint64_t clock = 1000000;
inline size_t freeBytes = 512000;
inline int fsyncUntilFail = -1, delays = 0, nextHttpCode = 201;
inline bool connected = true;
inline unsigned randomCounter = static_cast<unsigned>(getpid()), resumes = 0;
inline std::vector<std::string> payloads;
struct Yield {};
} // namespace Native
using temperature = int16_t;
struct ControlSettings {
  char mode = 'b';
  temperature beerSetting = 10000, fridgeSetting = 5000, heatEstimator = 128, coolEstimator = 256;
};
struct Actuator {
  bool state = false;
  std::vector<bool> edges;
  void setActive(bool on) {
    if (on != state)
      edges.push_back(on);
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
namespace GlycolCooling {
enum class Algorithm { PredictiveCoast, PulseDose };
inline const char *selectionName(Algorithm a) {
  return a == Algorithm::PredictiveCoast ? "predictive_coast" : "pulse_dose";
}
} // namespace GlycolCooling
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
inline bool fs_remove(const char *path) { return ::remove((Native::root + path).c_str()) == 0; }
inline FILE *fs_open(const char *path, const char *mode) { return fopen((Native::root + path).c_str(), mode); }
inline int native_fsync(int fd) {
  if (Native::fsyncUntilFail == 0) {
    Native::fsyncUntilFail = -1;
    return -1;
  }
  if (Native::fsyncUntilFail > 0)
    --Native::fsyncUntilFail;
  return ::fsync(fd);
}
#define fsync native_fsync
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, pdPASS = 1, portMAX_DELAY = -1;
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
inline int xQueueReceive(Queue *q, void *p, int) {
  if (q->items.empty())
    return 0;
  memcpy(p, q->items.front().data(), q->size);
  q->items.pop_front();
  return pdPASS;
}
inline int xTaskCreate(void (*)(void *), const char *, unsigned, void *, unsigned, void *) { return pdPASS; }
inline int pdMS_TO_TICKS(int n) { return n; }
inline void vTaskDelay(int) {
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
  std::string body;
};
using esp_http_client_handle_t = Http *;
inline Http *esp_http_client_init(const esp_http_client_config_t *c) {
  assert(c->disable_auto_redirect);
  assert(strncmp(c->url, "http://chill.fermentrack.net/", 27) == 0);
  return new Http{*c, {}};
}
inline int esp_http_client_set_header(Http *, const char *, const char *) { return ESP_OK; }
inline int esp_http_client_set_post_field(Http *c, const char *data, int n) {
  c->body.assign(data, n);
  return ESP_OK;
}
inline int esp_http_client_perform(Http *c) {
  Native::payloads.push_back(c->body);
  JsonDocument payload, response;
  assert(deserializeJson(payload, c->body) == DeserializationError::Ok);
  response["test_id"] = payload["test_id"];
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
  std::string text;
  serializeJson(response, text);
  esp_http_client_event_t event{HTTP_EVENT_ON_DATA, c->config.user_data, text.data(), static_cast<int>(text.size())};
  return c->config.event_handler(&event);
}
inline int esp_http_client_get_status_code(Http *) { return Native::nextHttpCode; }
inline int esp_http_client_cleanup(Http *c) {
  delete c;
  return ESP_OK;
}
