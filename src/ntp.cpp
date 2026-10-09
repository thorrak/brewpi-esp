#include "ntp.h"

#include <esp_wifi.h>
#include <esp_sntp.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <cstdio>
#include <ctime>

namespace {
std::atomic<bool> synced{false};
std::atomic<bool> started{false};

void onTimeSync(struct timeval*) {
    synced.store(true, std::memory_order_release);
}

void syncOnce(void*) {
    // Wait for the first network connection, including initial provisioning.
    // This task never delays sensor acquisition or relay scheduling.
    wifi_ap_record_t accessPoint{};
    while (esp_wifi_sta_get_ap_info(&accessPoint) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.nist.gov");
    esp_sntp_set_time_sync_notification_cb(onTimeSync);
    esp_sntp_init();
    for (unsigned i = 0; i < 150 && !synced.load(std::memory_order_acquire); ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    // Keep the wall clock running locally. No recurring synchronization.
    esp_sntp_stop();
    vTaskDelete(nullptr);
}
}

void initNTP() {
    if (started.exchange(true)) return;
    if (xTaskCreate(syncOnce, "ntp_once", 3072, nullptr, 2, nullptr) != pdPASS) {
        started.store(false);
    }
}

bool isNtpSynced() {
    return synced.load(std::memory_order_acquire);
}

bool getFormattedTime(char* buffer, size_t bufferSize) {
    if (!isNtpSynced()) {
        snprintf(buffer, bufferSize, "0");
        return false;
    }
    time_t now = time(nullptr);
    struct tm utc{};
    gmtime_r(&now, &utc);
    return strftime(buffer, bufferSize, "%Y-%m-%d %H:%M:%S", &utc) != 0;
}
