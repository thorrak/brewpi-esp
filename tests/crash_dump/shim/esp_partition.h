#pragma once

#include "esp_http_server.h"
#include <cstdint>

constexpr int ESP_PARTITION_TYPE_DATA = 1;
constexpr int ESP_PARTITION_SUBTYPE_DATA_COREDUMP = 3;

struct esp_partition_t {
    uint32_t address;
    uint32_t size;
};

const esp_partition_t* esp_partition_find_first(int type, int subtype, const char* label);
esp_err_t esp_partition_read(const esp_partition_t*, size_t offset, void* destination, size_t size);
