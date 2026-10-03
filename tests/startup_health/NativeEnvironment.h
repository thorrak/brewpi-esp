#include <ArduinoJson.h>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <list>
#include <string>

using BaseType_t = int;
using SemaphoreHandle_t = void*;
using TaskHandle_t = void*;
using esp_err_t = int;
using onewire_bus_handle_t = void*;
using onewire_device_iter_handle_t = void*;
using ds18b20_device_handle_t = void*;
using long_temperature = int32_t;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_STATE = 1;
constexpr int ESP_ERR_NVS_NO_FREE_PAGES = 2, ESP_ERR_NVS_NEW_VERSION_FOUND = 3;
constexpr int ESP_LOG_ERROR = 0, pdPASS = 1, portMAX_DELAY = -1;
constexpr int TEMP_FIXED_POINT_BITS = 9, C_OFFSET = 0;
constexpr int DEVICE_DISCONNECTED_RAW = -32768;
constexpr int DS18B20_RESOLUTION_12B = 12;
#define BREWPI_SIMULATE 0
#define CONFIG_LWIP_MAX_SOCKETS 16
#define pdMS_TO_TICKS(value) (value)
#define ESP_ERROR_CHECK(value) assert((value) == ESP_OK)
template <typename... T> void ignored_log(T...) {}
#define ESP_LOGW(...) ignored_log(__VA_ARGS__)
#define ESP_LOGI(...) ignored_log(__VA_ARGS__)
#define ESP_LOGE(...) ignored_log(__VA_ARGS__)

struct StopLoop {};
namespace Native {
uint64_t now = 1000000;
bool failMutex = false, failBus = false, failWorker = false, failLoop = false;
bool deviceFound = false, readValid = true;
unsigned busCalls = 0, taskCalls = 0, deletions = 0, delayCalls = 0, stopAfterDelays = 0;
unsigned loopCalls = 0, setupCalls = 0, mutexCalls = 0, samples = 0, busIo = 0;
bool failHttpClientList = false;
void (*worker)(void*) = nullptr;
void* workerArgument = nullptr;
void (*loopTask)(void*) = nullptr;
}
struct HttpServerStub {
    void *getHandle() const { return reinterpret_cast<void*>(6); }
} http_server;
esp_err_t httpd_get_client_list(void *handle, size_t *count, int *clients) {
    assert(handle == http_server.getHandle());
    if (Native::failHttpClientList) return ESP_FAIL;
    assert(*count >= 2);
    clients[0] = 10;
    clients[1] = 11;
    *count = 2;
    return ESP_OK;
}
uint64_t esp_timer_get_time() { return Native::now; }
void esp_log_level_set(const char*, int) {}
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() {
    ++Native::mutexCalls;
    return Native::failMutex ? nullptr : reinterpret_cast<void*>(1);
}
void xSemaphoreTakeRecursive(SemaphoreHandle_t, int) {}
void xSemaphoreGiveRecursive(SemaphoreHandle_t) {}
BaseType_t xTaskCreate(void (*task)(void*), const char*, unsigned stack, void* arg,
                      unsigned, TaskHandle_t* result) {
    assert(stack == 4096);
    ++Native::taskCalls;
    if (Native::failWorker) return 0;
    Native::worker = task;
    Native::workerArgument = arg;
    *result = reinterpret_cast<void*>(2);
    return pdPASS;
}
BaseType_t xTaskCreatePinnedToCore(void (*task)(void*), const char*, unsigned stack, void*,
                                  unsigned, TaskHandle_t*, unsigned) {
    assert(Native::setupCalls == 1 && stack == 8192);
    if (Native::failLoop) return 0;
    Native::loopTask = task;
    return pdPASS;
}
void vTaskDelay(unsigned milliseconds) {
    Native::now += uint64_t(milliseconds) * 1000;
    if (++Native::delayCalls == Native::stopAfterDelays) throw StopLoop{};
}
void brewpiLoop() { ++Native::loopCalls; }
void setup() { ++Native::setupCalls; }
esp_err_t nvs_flash_init() { return ESP_OK; }
esp_err_t nvs_flash_erase() { return ESP_OK; }
esp_err_t esp_netif_init() { return ESP_OK; }
esp_err_t esp_event_loop_create_default() { return ESP_OK; }
uint64_t bytesToAddress(const uint8_t*) { return 0x28; }

struct onewire_bus_config_t { unsigned bus_gpio_num; struct { bool en_pull_up; } flags; };
struct onewire_bus_rmt_config_t { unsigned max_rx_bytes; };
struct onewire_device_t { uint64_t address; };
struct ds18b20_config_t {};
esp_err_t onewire_new_bus_rmt(const onewire_bus_config_t*, const onewire_bus_rmt_config_t*,
                              onewire_bus_handle_t* result) {
    ++Native::busCalls;
    *result = Native::failBus ? nullptr : reinterpret_cast<void*>(3);
    return Native::failBus ? ESP_FAIL : ESP_OK;
}
void onewire_bus_del(onewire_bus_handle_t) { ++Native::deletions; }
void ds18b20_del_device(ds18b20_device_handle_t) {}
esp_err_t onewire_new_device_iter(onewire_bus_handle_t, onewire_device_iter_handle_t* result) {
    ++Native::busIo;
    Native::deviceFound = false;
    *result = reinterpret_cast<void*>(4);
    return ESP_OK;
}
esp_err_t onewire_device_iter_get_next(onewire_device_iter_handle_t, onewire_device_t* result) {
    if (Native::deviceFound) return ESP_FAIL;
    Native::deviceFound = true;
    result->address = 0x28;
    return ESP_OK;
}
void onewire_del_device_iter(onewire_device_iter_handle_t) {}
esp_err_t ds18b20_new_device_from_enumeration(const onewire_device_t*, const ds18b20_config_t*,
                                             ds18b20_device_handle_t* result) {
    *result = reinterpret_cast<void*>(5);
    return ESP_OK;
}
void ds18b20_set_resolution(ds18b20_device_handle_t, int) {}
esp_err_t ds18b20_init_connection(ds18b20_device_handle_t) { return ESP_OK; }
esp_err_t ds18b20_trigger_all_conversions_no_wait(onewire_bus_handle_t) {
    ++Native::busIo;
    return ESP_OK;
}
esp_err_t ds18b20_get_temperature_raw(ds18b20_device_handle_t, int16_t* result) {
    ++Native::busIo;
    *result = 320;
    return Native::readValid ? ESP_OK : ESP_FAIL;
}
namespace WaterTest {
void onSample(uint64_t, int16_t, bool, uint64_t, uint64_t) { ++Native::samples; }
}
