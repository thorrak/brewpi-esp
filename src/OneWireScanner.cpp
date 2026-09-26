#include "OneWireScanner.h"

#include "EspDS18B20.h"
#include "NumberFormats.h"
#include "WaterTest.h"
#include "onewire_device.h"
#include "onewire_bus_impl_rmt.h"

#include <esp_log.h>
#include <esp_timer.h>

static const char* TAG = "ow_scanner";

// DS18B20 power-on default value: 85°C in raw 16-bit format (85 * 16).
// A sensor reports this after power-on until its first conversion completes.
#define DEVICE_POWERON_RAW 1360

// Worker-loop cadence: aim for one convert/read cycle every ~2s.
#define WORKER_CYCLE_MS          2000

// Wait for DS18B20 conversion (12-bit resolution = 750ms, plus margin).
#define CONVERSION_WAIT_MS       760

// How often the worker enumerates the bus to discover new devices.
#define ENUMERATION_INTERVAL_US  (15ULL * 1000ULL * 1000ULL)

// Worker task stack size (words). Device iteration and ds18b20 calls are not
// deep, but we're also logging and touching std::list.
#define WORKER_STACK_WORDS       4096

// Worker task priority. Above idle, below timer/IPC tasks.
#define WORKER_PRIORITY          5

// After this many consecutive ds18b20_init_connection failures on a single
// handle, the worker destroys the handle so the next enumeration pass can
// attach a fresh one. Without this, a stale handle whose device has gone
// unresponsive would retry every ENUMERATION_INTERVAL forever.
#define ONEWIRE_MAX_INIT_FAILURES 3


OneWireScanner ow_scanner;
std::list<onewire_device_record> lOneWireDevices;


// ---------------------------------------------------------------------------
// onewire_device_record
// ---------------------------------------------------------------------------

bool onewire_device_record::isConnected() const {
    if (!hasData) return false;
    uint64_t now = esp_timer_get_time();
    return now <= m_lastUpdate + ONEWIRE_CONNECTED_TIMEOUT_US;
}

long_temperature onewire_device_record::getTempFixedPoint() const {
    // DS18B20 raw format: 12-bit signed fixed point, 1/16°C per LSB (== 4 fractional bits).
    // Convert to our fixed-point representation (TEMP_FIXED_POINT_BITS fractional bits)
    // and apply the C→internal offset.
    const uint8_t shift = TEMP_FIXED_POINT_BITS - 4;  // 4 = DS18B20 fractional bits
    long_temperature t = ((long_temperature)rawTemp) << shift;
    return t + C_OFFSET;
}


// ---------------------------------------------------------------------------
// OneWireScanner
// ---------------------------------------------------------------------------

OneWireScanner::OneWireScanner()
  : m_pin(0), m_bus(nullptr), m_task(nullptr), m_list_mutex(nullptr),
    m_rescan_requested(false),
    m_last_successful_read_us(0), m_last_bus_reset_us(0),
    m_last_enumeration_us(0) {}


bool OneWireScanner::init(uint8_t pin) {
    if (m_task != nullptr) {
        return m_bus != nullptr;
    }

    // Silence ESP-IDF's "reset bus failed: no devices found" warning from the
    // device iterator; an empty bus is valid and the worker reports its own
    // state via the bus_failed() interface.
    esp_log_level_set("1-wire.device", ESP_LOG_ERROR);

    if (m_list_mutex == nullptr) {
        m_list_mutex = xSemaphoreCreateRecursiveMutex();
        if (m_list_mutex == nullptr) {
            ESP_LOGW(TAG, "failed to allocate list mutex");
            return false;
        }
    }

    m_pin = pin;
    if (!create_bus()) {
        ESP_LOGE(TAG, "failed to create OneWire bus on pin %u", (unsigned)pin);
        return false;
    }

    BaseType_t ok = xTaskCreate(&OneWireScanner::task_trampoline,
                                "ow_scanner",
                                WORKER_STACK_WORDS,
                                this,
                                WORKER_PRIORITY,
                                &m_task);
    if (ok != pdPASS) {
        ESP_LOGW(TAG, "failed to start worker task");
        destroy_bus();
        m_task = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "worker task started on pin %u", (unsigned)pin);
    return true;
}


onewire_device_record* OneWireScanner::get(uint64_t address) {
    if (m_list_mutex == nullptr) return nullptr;

    xSemaphoreTakeRecursive(m_list_mutex, portMAX_DELAY);
    onewire_device_record* found = nullptr;
    for (auto& rec : lOneWireDevices) {
        if (rec.deviceAddress == address) {
            found = &rec;
            break;
        }
    }
    xSemaphoreGiveRecursive(m_list_mutex);
    // Record pointers are stable for the lifetime of the program (std::list
    // never erases, never reallocates), so it's safe to return without holding
    // the lock.
    return found;
}


onewire_device_record* OneWireScanner::get_by_bytes(const uint8_t* address_bytes) {
    return get(bytesToAddress(address_bytes));
}


bool OneWireScanner::bus_failed() const {
    uint64_t now = esp_timer_get_time();
    uint64_t last_ok = m_last_successful_read_us.load(std::memory_order_relaxed);
    // Never failed if we've just started up and no time has passed.
    if (last_ok == 0 && now < ONEWIRE_BUS_RECOVERY_TIMEOUT_US) {
        return false;
    }
    return (now - last_ok) > ONEWIRE_BUS_RECOVERY_TIMEOUT_US;
}


// ---------------------------------------------------------------------------
// Bus lifecycle (called only from worker task after init())
// ---------------------------------------------------------------------------

bool OneWireScanner::create_bus() {
    onewire_bus_config_t bus_config = {
        .bus_gpio_num = m_pin,
        .flags = { .en_pull_up = true },
    };
    onewire_bus_rmt_config_t rmt_config = {
        .max_rx_bytes = 10,
    };

    if (onewire_new_bus_rmt(&bus_config, &rmt_config, &m_bus) != ESP_OK) {
        m_bus = nullptr;
        return false;
    }
    return true;
}


void OneWireScanner::destroy_bus() {
    // Free per-device handles before the bus — they reference bus state.
    for (auto& rec : lOneWireDevices) {
        if (rec.handle) {
            ds18b20_del_device(rec.handle);
            rec.handle = nullptr;
        }
        rec.initialized = false;
        rec.m_init_failures = 0;
    }
    if (m_bus) {
        onewire_bus_del(m_bus);
        m_bus = nullptr;
    }
}


void OneWireScanner::try_bus_recovery() {
    uint64_t now = esp_timer_get_time();
    if (m_last_bus_reset_us != 0 && (now - m_last_bus_reset_us) < ONEWIRE_BUS_RECOVERY_COOLDOWN_US) {
        return;
    }
    ESP_LOGW(TAG, "bus appears dead, tearing down and recreating");
    destroy_bus();
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!create_bus()) {
        ESP_LOGE(TAG, "bus recreate failed");
    } else {
        ESP_LOGI(TAG, "bus recreated successfully");
    }
    m_last_bus_reset_us = esp_timer_get_time();
    // Force a rescan on the next cycle so devices re-attach to the new bus.
    m_last_enumeration_us = 0;
}


// ---------------------------------------------------------------------------
// Enumeration, conversion, read
// ---------------------------------------------------------------------------

void OneWireScanner::enumerate_bus() {
    if (!m_bus) return;

    onewire_device_iter_handle_t iter = nullptr;
    if (onewire_new_device_iter(m_bus, &iter) != ESP_OK) {
        return;
    }

    onewire_device_t next;
    while (onewire_device_iter_get_next(iter, &next) == ESP_OK) {
        // Only handle DS18B20 family (0x28). Family ID is the low byte of the 64-bit ROM ID.
        uint8_t family = (uint8_t)(next.address & 0xFF);
        if (family != 0x28) continue;

        // Find-or-create under the list mutex (brief) so concurrent readers
        // never see a half-linked node. Bus I/O stays outside the lock.
        onewire_device_record* rec = nullptr;
        bool is_new = false;
        xSemaphoreTakeRecursive(m_list_mutex, portMAX_DELAY);
        for (auto& existing : lOneWireDevices) {
            if (existing.deviceAddress == next.address) { rec = &existing; break; }
        }
        if (rec == nullptr) {
            lOneWireDevices.emplace_back(next.address);
            rec = &lOneWireDevices.back();
            is_new = true;
        }
        xSemaphoreGiveRecursive(m_list_mutex);

        if (is_new) {
            ESP_LOGI(TAG, "discovered device %016llX", (unsigned long long)next.address);
        }

        // Attach a fresh ds18b20 handle if the device doesn't have one yet
        // (or lost it during a bus recovery). Readers never touch handle.
        if (rec->handle == nullptr) {
            ds18b20_config_t ds_cfg = {};
            if (ds18b20_new_device_from_enumeration(&next, &ds_cfg, &rec->handle) != ESP_OK) {
                rec->handle = nullptr;
                continue;
            }
            // Set 12-bit resolution once per handle. DS18B20 default is
            // already 12-bit, so a failure here is harmless — we don't retry.
            // Re-running this every init cycle was producing spammy
            // "ds18b20_set_resolution: reset bus error" logs against devices
            // whose handle had gone stale.
            ds18b20_set_resolution(rec->handle, DS18B20_RESOLUTION_12B);
            rec->initialized = false;
            rec->m_init_failures = 0;
        }
    }
    onewire_del_device_iter(iter);

    // For any record whose handle exists but hasn't had init_connection run
    // (either because we just attached it, or because the sensor power-cycled
    // and tripped the TH reset-detection marker), run it now. If it keeps
    // failing, destroy the handle so the next enumeration re-attaches fresh.
    // Worker-only — no locking required.
    for (auto& rec : lOneWireDevices) {
        if (rec.handle && !rec.initialized) {
            if (ds18b20_init_connection(rec.handle) == ESP_OK) {
                rec.initialized = true;
                rec.m_init_failures = 0;
            } else if (++rec.m_init_failures >= ONEWIRE_MAX_INIT_FAILURES) {
                ESP_LOGW(TAG, "dropping stale handle for %016llX after %u failed inits",
                         (unsigned long long)rec.deviceAddress,
                         (unsigned)rec.m_init_failures);
                ds18b20_del_device(rec.handle);
                rec.handle = nullptr;
                rec.m_init_failures = 0;
            }
        }
    }
}


esp_err_t OneWireScanner::trigger_broadcast_conversion() {
    if (!m_bus) return ESP_ERR_INVALID_STATE;
    return ds18b20_trigger_all_conversions_no_wait(m_bus);
}


bool OneWireScanner::read_all_devices() {
    bool any_ok = false;
    for (auto& rec : lOneWireDevices) {
        if (!rec.handle || !rec.initialized) {
            WaterTest::onSample(rec.deviceAddress, 0, false,
                                m_conversion_start_us, esp_timer_get_time());
            continue;
        }

        int16_t temp = 0;
        esp_err_t rc = ds18b20_get_temperature_raw(rec.handle, &temp);
        const uint64_t read_us = esp_timer_get_time();
        const bool valid = rc == ESP_OK && temp != DEVICE_POWERON_RAW &&
                           temp != DEVICE_DISCONNECTED_RAW && temp >= -880 && temp <= 2000;
        WaterTest::onSample(rec.deviceAddress, temp, valid, m_conversion_start_us, read_us);
        if (rc == ESP_ERR_INVALID_STATE) {
            // TH marker read back as 0 — sensor was reset since init_connection.
            // Drop the initialized flag so the next enumeration pass re-seeds it.
            ESP_LOGI(TAG, "%016llX tripped reset-detection, will re-init",
                     (unsigned long long)rec.deviceAddress);
            rec.initialized = false;
            continue;
        }
        if (rc != ESP_OK) {
            ESP_LOGW(TAG, "%016llX read failed, rc=0x%x",
                     (unsigned long long)rec.deviceAddress, (unsigned)rc);
            continue;
        }

        // Reject the power-on default; wait for a real conversion result.
        if (temp == DEVICE_POWERON_RAW) {
            ESP_LOGI(TAG, "%016llX returned DEVICE_POWERON_RAW, ignoring",
                     (unsigned long long)rec.deviceAddress);
            continue;
        }
        if (temp == DEVICE_DISCONNECTED_RAW) continue;

        // Log the first read of a device so the user sees an initial value
        // alongside the "discovered" line. Temperature is printed as
        // whole-degree + 4-digit fraction (raw is 1/16°C per LSB) so we don't
        // depend on the float printf option being enabled. Subsequent
        // steady-state reads don't log — real events (disconnect, re-init,
        // failed reads) continue to log at INFO/WARN.
        if (!rec.hasData) {
            int whole = (int)temp / 16;
            int frac  = ((int)temp < 0 ? -(int)temp : (int)temp) % 16;
            frac = frac * 625;  // 1/16 = 0.0625, scale to 4 decimal digits
            ESP_LOGI(TAG, "%016llX first read: raw=%d (%s%d.%04d C)",
                     (unsigned long long)rec.deviceAddress,
                     (int)temp,
                     ((int)temp < 0 && whole == 0) ? "-" : "",
                     whole, frac);
        }

        rec.rawTemp = temp;
        rec.m_lastUpdate = read_us;
        rec.hasData = true;
        any_ok = true;
    }
    return any_ok;
}


// ---------------------------------------------------------------------------
// Worker task
// ---------------------------------------------------------------------------

void OneWireScanner::task_trampoline(void* arg) {
    static_cast<OneWireScanner*>(arg)->task_loop();
}


void OneWireScanner::task_loop() {
    // Prime the success timestamp so bus_failed() doesn't trip immediately.
    m_last_successful_read_us.store(esp_timer_get_time(), std::memory_order_relaxed);

    for (;;) {
        uint64_t cycle_start = esp_timer_get_time();

        // Enumerate when requested, when due by interval, or as long as the
        // cache is empty — an enumeration that failed silently at boot
        // (bus not yet stable, presence glitch) should retry promptly rather
        // than waiting the full 15s interval.
        bool enumerate_now = m_rescan_requested
                             || m_last_enumeration_us == 0
                             || (cycle_start - m_last_enumeration_us) >= ENUMERATION_INTERVAL_US
                             || lOneWireDevices.empty();
        if (enumerate_now) {
            m_rescan_requested = false;
            enumerate_bus();
            m_last_enumeration_us = esp_timer_get_time();
        }

        // Broadcast a conversion trigger, wait, read everyone.
        m_conversion_start_us = esp_timer_get_time();
        esp_err_t rc = trigger_broadcast_conversion();
        if (rc == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(CONVERSION_WAIT_MS));
            if (read_all_devices()) {
                m_last_successful_read_us.store(esp_timer_get_time(), std::memory_order_relaxed);
            }
        } else {
            ESP_LOGW(TAG, "broadcast convert failed, rc=0x%x", (unsigned)rc);
            for (const auto& rec : lOneWireDevices) {
                WaterTest::onSample(rec.deviceAddress, 0, false,
                                    m_conversion_start_us, esp_timer_get_time());
            }
        }

        // Log connection-state transitions so a silent dropout becomes visible.
        for (auto& rec : lOneWireDevices) {
            bool now_connected = rec.isConnected();
            if (now_connected != rec.m_last_reported_connected) {
                ESP_LOGI(TAG, "%016llX %s",
                         (unsigned long long)rec.deviceAddress,
                         now_connected ? "connected" : "disconnected");
                rec.m_last_reported_connected = now_connected;
            }
        }

        // Bus recovery if nothing has succeeded in a long time.
        if (bus_failed()) {
            try_bus_recovery();
        }

        // Sleep out the rest of the cycle budget.
        uint64_t elapsed_ms = (esp_timer_get_time() - cycle_start) / 1000ULL;
        if (elapsed_ms < WORKER_CYCLE_MS) {
            vTaskDelay(pdMS_TO_TICKS(WORKER_CYCLE_MS - elapsed_ms));
        } else {
            // Yield at minimum so we don't starve other tasks.
            vTaskDelay(1);
        }
    }
}
