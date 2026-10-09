#pragma once

#include "sdkconfig.h"
#include "esp_http_server.h"
#include <cstdint>

esp_err_t esp_core_dump_image_check();

#if CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
struct esp_core_dump_summary_t {
    char exc_task[16];
    uint8_t app_elf_sha256[65];
};
esp_err_t esp_core_dump_get_summary(esp_core_dump_summary_t*);
esp_err_t esp_core_dump_get_panic_reason(char*, size_t);
#endif
