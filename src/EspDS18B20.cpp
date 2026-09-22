
#include "EspDS18B20.h"
#include "onewire_crc.h"
#include "onewire_cmd.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define DS18B20_CMD_READ_SCRATCHPAD   0xBE
#define DS18B20_CMD_WRITE_SCRATCHPAD  0x4E
#define DS18B20_CMD_COPY_SCRATCHPAD   0x48
#define DS18B20_CMD_CONVERT_TEMP      0x44

// Scratchpad byte indices
#define SCRATCHPAD_TH_USER1  2  // HIGH_ALARM_TEMP - used for reset detection

typedef struct  {
  uint8_t temp_lsb;
  uint8_t temp_msb;
  uint8_t th_user1;
  uint8_t tl_user2;
  uint8_t configuration;
  uint8_t _reserved1;
  uint8_t _reserved2;
  uint8_t _reserved3;
  uint8_t crc_value;
} __attribute__((packed)) ds18b20_scratchpad_t;

typedef struct ds18b20_device_t {
  onewire_bus_handle_t bus;
  onewire_device_address_t addr;
  uint8_t th_user1;
  uint8_t tl_user2;
  ds18b20_resolution_t resolution;
} ds18b20_device_t;

static esp_err_t ds18b20_send_command(ds18b20_device_handle_t ds18b20, uint8_t cmd)
{
  if (ds18b20->addr == 0) {
    uint8_t tx_buffer[2] = {ONEWIRE_CMD_SKIP_ROM, cmd};
    return onewire_bus_write_bytes(ds18b20->bus, tx_buffer, sizeof(tx_buffer));
  }

  uint8_t tx_buffer[10] = {0};
  tx_buffer[0] = ONEWIRE_CMD_MATCH_ROM;
  memcpy(&tx_buffer[1], &ds18b20->addr, sizeof(ds18b20->addr));
  tx_buffer[sizeof(ds18b20->addr) + 1] = cmd;

  return onewire_bus_write_bytes(ds18b20->bus, tx_buffer, sizeof(tx_buffer));
}

/**
 * \brief Read the scratchpad from a DS18B20 sensor
 */
static esp_err_t ds18b20_read_scratchpad(ds18b20_device_handle_t ds18b20, ds18b20_scratchpad_t *scratchpad)
{
  esp_err_t ret = onewire_bus_reset(ds18b20->bus);
  if (ret != ESP_OK) {
    return ret;
  }

  ret = ds18b20_send_command(ds18b20, DS18B20_CMD_READ_SCRATCHPAD);
  if (ret != ESP_OK) {
    return ret;
  }

  ret = onewire_bus_read_bytes(ds18b20->bus, (uint8_t *)scratchpad, sizeof(ds18b20_scratchpad_t));
  if (ret != ESP_OK) {
    return ret;
  }

  if (onewire_crc8(0, (uint8_t *)scratchpad, 8) != scratchpad->crc_value) {
    return ESP_ERR_INVALID_CRC;
  }

  return ESP_OK;
}

/**
 * \brief Write TH, TL, and config to the scratchpad
 */
static esp_err_t ds18b20_write_scratchpad(ds18b20_device_handle_t ds18b20, uint8_t th, uint8_t tl, uint8_t config)
{
  esp_err_t ret = onewire_bus_reset(ds18b20->bus);
  if (ret != ESP_OK) {
    return ret;
  }

  ret = ds18b20_send_command(ds18b20, DS18B20_CMD_WRITE_SCRATCHPAD);
  if (ret != ESP_OK) {
    return ret;
  }

  uint8_t data[3] = {th, tl, config};
  return onewire_bus_write_bytes(ds18b20->bus, data, sizeof(data));
}

/**
 * \brief Save scratchpad to EEPROM
 */
static esp_err_t ds18b20_save_scratchpad(ds18b20_device_handle_t ds18b20)
{
  esp_err_t ret = onewire_bus_reset(ds18b20->bus);
  if (ret != ESP_OK) {
    return ret;
  }

  ret = ds18b20_send_command(ds18b20, DS18B20_CMD_COPY_SCRATCHPAD);
  if (ret != ESP_OK) {
    return ret;
  }

  // Wait for EEPROM write to complete (typically 10ms, max 20ms per datasheet)
  vTaskDelay(pdMS_TO_TICKS(20));
  return ESP_OK;
}

esp_err_t ds18b20_init_connection(ds18b20_device_handle_t ds18b20)
{
  if (!ds18b20) {
    return ESP_ERR_INVALID_ARG;
  }

  ds18b20_scratchpad_t scratchpad;
  esp_err_t ret = ds18b20_read_scratchpad(ds18b20, &scratchpad);
  if (ret != ESP_OK) {
    return ret;
  }

  // Write TH=0 to EEPROM so that on power cycle, it will be 0
  // This only needs to happen once per sensor, but we check to avoid EEPROM wear
  if (scratchpad.th_user1 != 0) {
    ret = ds18b20_write_scratchpad(ds18b20, 0, scratchpad.tl_user2, scratchpad.configuration);
    if (ret != ESP_OK) {
      return ret;
    }

    ret = ds18b20_save_scratchpad(ds18b20);
    if (ret != ESP_OK) {
      return ret;
    }

    // Verify the write was successful
    ret = ds18b20_read_scratchpad(ds18b20, &scratchpad);
    if (ret != ESP_OK || scratchpad.th_user1 != 0) {
      return ESP_FAIL;
    }
  }

  // Now write TH=1 to RAM only (don't save to EEPROM)
  // On power cycle, this will revert to 0 (from EEPROM), indicating a reset
  ret = ds18b20_write_scratchpad(ds18b20, 1, scratchpad.tl_user2, scratchpad.configuration);
  if (ret != ESP_OK) {
    return ret;
  }

  return ESP_OK;
}

esp_err_t ds18b20_get_temperature_raw(ds18b20_device_handle_t ds18b20, int16_t *ret_temperature)
{
  if (!ds18b20 || !ret_temperature) {
    return ESP_ERR_INVALID_ARG;
  }

  ds18b20_scratchpad_t scratchpad;
  esp_err_t ret = ds18b20_read_scratchpad(ds18b20, &scratchpad);
  if (ret != ESP_OK) {
    *ret_temperature = DEVICE_DISCONNECTED_RAW;
    return ret;
  }

  // Check for reset detection: if TH (th_user1) is 0, the sensor has been
  // reset since init_connection() was called. This indicates the sensor
  // needs to be re-initialized.
  if (scratchpad.th_user1 == 0) {
    *ret_temperature = DEVICE_DISCONNECTED_RAW;
    return ESP_ERR_INVALID_STATE;  // Sensor was reset, needs re-init
  }

  const uint8_t lsb_mask[4] = {0x07, 0x03, 0x01, 0x00};
  uint8_t lsb_masked = scratchpad.temp_lsb & (~lsb_mask[scratchpad.configuration >> 5]);
  *ret_temperature = (((int16_t)scratchpad.temp_msb << 8) | lsb_masked);

  return ESP_OK;
}

esp_err_t ds18b20_trigger_all_conversions_no_wait(onewire_bus_handle_t bus)
{
  if (!bus) {
    return ESP_ERR_INVALID_ARG;
  }

  // Reset bus and check if any devices are present
  esp_err_t ret = onewire_bus_reset(bus);
  if (ret != ESP_OK) {
    return ret;
  }

  // Use Skip ROM to address all devices, then send convert command
  uint8_t tx_buffer[2] = {ONEWIRE_CMD_SKIP_ROM, DS18B20_CMD_CONVERT_TEMP};
  return onewire_bus_write_bytes(bus, tx_buffer, sizeof(tx_buffer));
}
