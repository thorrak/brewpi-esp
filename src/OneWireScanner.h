#ifndef BREWPI_ONEWIRE_SCANNER_H
#define BREWPI_ONEWIRE_SCANNER_H

#include "Brewpi.h"
#include "TemperatureFormats.h"
#include "OneWireSensorPolicy.h"
#include "onewire_bus.h"
#include "ds18b20.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <atomic>
#include <list>
#include <stdint.h>

// If no device on the bus has been read successfully for this long, the worker
// tears down the bus and recreates it.
#define ONEWIRE_BUS_RECOVERY_TIMEOUT_US (45ULL * 1000ULL * 1000ULL)

// Minimum gap between bus teardown+recreate attempts.
#define ONEWIRE_BUS_RECOVERY_COOLDOWN_US (120ULL * 1000ULL * 1000ULL)


/**
 * \brief Cached record of a OneWire temperature sensor.
 *
 * Analogous to the \ref inkbird class for BLE sensors. The OneWire worker task
 * maintains the cache; other tasks only read from it. Records are created but
 * never deleted (BLE pattern), so pointers returned by OneWireScanner::get()
 * remain valid for the lifetime of the program.
 */
class onewire_device_record {
public:
    explicit onewire_device_record(uint64_t addr)
      : deviceAddress(addr), handle(nullptr), rawTemp(0), m_lastUpdate(0),
        hasData(false), initialized(false), m_init_failures(0),
        m_last_reported_connected(false) {}

    // Device ROM ID.
    uint64_t deviceAddress;

    // ds18b20 SDK device handle, owned by the scanner task.
    // May be reset to nullptr during bus recovery; the worker re-attaches on
    // the next enumeration.
    ds18b20_device_handle_t handle;

    // Last raw temperature reading from the sensor.
    int16_t rawTemp;

    // Microsecond timestamp of the last successful read (esp_timer_get_time()).
    uint64_t m_lastUpdate;

    // True once at least one successful read has populated rawTemp.
    bool hasData;

    // True once ds18b20_init_connection has been run against the current handle.
    // Cleared whenever the bus is recreated or the handle is replaced.
    bool initialized;

    // Consecutive ds18b20_init_connection failures on the current handle.
    // After ONEWIRE_MAX_INIT_FAILURES the handle is destroyed so the next
    // enumeration pass can attach a fresh one, rather than banging on a
    // stale handle every cycle.
    uint8_t m_init_failures;

    // Last reported connection state — used by the worker to log transitions.
    // Written/read only from the worker task.
    bool m_last_reported_connected;

    bool isConnected() const;

    int16_t getTemp() const { return rawTemp; }

    long_temperature getTempFixedPoint() const;

    bool operator==(const onewire_device_record& o) const { return deviceAddress == o.deviceAddress; }
    bool operator!=(const onewire_device_record& o) const { return !(*this == o); }
};


// Forward-declared here so the OneWireScanner::for_each template can reference
// it; defined in OneWireScanner.cpp.
extern std::list<onewire_device_record> lOneWireDevices;


/**
 * \brief Worker-driven OneWire bus scanner.
 *
 * Owns the onewire_bus_handle and runs a FreeRTOS task that:
 *   - periodically enumerates the bus to discover new devices,
 *   - broadcasts a temperature conversion to all sensors,
 *   - waits for conversion, reads each scratchpad, and updates the cache,
 *   - recovers the bus if all reads have been failing for too long.
 *
 * Callers (DeviceManager, OneWireTempSensor) read from the cache via get(); no
 * bus I/O happens on their tasks.
 */
class OneWireScanner {
public:
    OneWireScanner();

    /**
     * \brief Initialize the bus on the given pin and start the worker task.
     * Idempotent — subsequent calls are no-ops.
     * @returns true if the bus is up and the worker is running.
     */
    bool init(uint8_t pin);

    /**
     * \brief Look up a cached device record by 64-bit address.
     * @returns pointer to the record, or nullptr if never discovered.
     *          Pointer remains valid for the lifetime of the program.
     */
    onewire_device_record* get(uint64_t address);

    /**
     * \brief Look up by 8-byte address array (as used by DeviceConfig).
     */
    onewire_device_record* get_by_bytes(const uint8_t* address_bytes);

    /**
     * \brief True if the worker hasn't seen a successful read from any device
     * for longer than ONEWIRE_BUS_RECOVERY_TIMEOUT_US. Analogous to
     * btScanner::scanning_failed().
     */
    bool bus_failed() const;

    /**
     * \brief Nudge the worker to run an enumeration on its next cycle.
     * Safe to call from any task.
     */
    void request_rescan() { m_rescan_requested = true; }

    /**
     * \brief True once init() has brought up the bus and worker task.
     */
    bool is_running() const { return m_task != nullptr; }

    /**
     * \brief Invoke \p fn(record) for each discovered device, under the list
     * mutex. Use this instead of iterating lOneWireDevices directly from
     * anywhere other than the scanner's own worker task.
     */
    template <typename Fn>
    void for_each(Fn fn) {
        if (m_list_mutex == nullptr) return;
        xSemaphoreTakeRecursive(m_list_mutex, portMAX_DELAY);
        for (auto& rec : lOneWireDevices) fn(rec);
        xSemaphoreGiveRecursive(m_list_mutex);
    }

private:
    static void task_trampoline(void* arg);
    void task_loop();

    bool create_bus();
    void destroy_bus();
    void try_bus_recovery();

    void enumerate_bus();
    esp_err_t trigger_broadcast_conversion();
    bool read_all_devices();

    uint8_t m_pin;
    onewire_bus_handle_t m_bus;
    TaskHandle_t m_task;
    SemaphoreHandle_t m_list_mutex;   // guards lOneWireDevices structural ops
    volatile bool m_rescan_requested;

    // Bus health: written by worker, read by any task via bus_failed().
    // Atomic to avoid torn 64-bit reads on 32-bit Xtensa.
    std::atomic<uint64_t> m_last_successful_read_us;

    // Worker-only.
    uint64_t m_last_bus_reset_us;
    uint64_t m_last_enumeration_us;
    // Used only for WaterTest sample timestamps; remove with the WaterTest hooks.
    uint64_t m_conversion_start_us = 0;
};

extern OneWireScanner ow_scanner;

#endif // BREWPI_ONEWIRE_SCANNER_H
