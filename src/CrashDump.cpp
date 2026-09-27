#include "CrashDump.h"
#include "HttpJsonResponse.h"

#ifdef ENABLE_HTTP_INTERFACE

#include <ArduinoJson.h>
#include <sdkconfig.h>
#include <cstdint>

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include <esp_core_dump.h>
#include <esp_partition.h>
#endif

namespace {

esp_err_t sendJson(httpd_req_t* request, JsonDocument& document) {
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return HttpJsonResponse::send(request, document);
}

esp_err_t sendError(httpd_req_t* request, const char* status, const char* error) {
    httpd_resp_set_status(request, status);
    JsonDocument document;
    document["error"] = error;
    return sendJson(request, document);
}

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH

enum class State { Available, Empty, MissingPartition, Corrupt, ReadError };

struct Dump {
    const esp_partition_t* partition = nullptr;
    size_t size = 0;
    State state = State::MissingPartition;
    esp_err_t error = ESP_OK;
};

Dump inspect() {
    Dump dump;
    dump.partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                             ESP_PARTITION_SUBTYPE_DATA_COREDUMP, nullptr);
    if (!dump.partition) return dump;

    // The flash image begins with six 32-bit header fields and ends with a checksum.
    constexpr size_t headerSize = 6 * sizeof(uint32_t);
#if CONFIG_ESP_COREDUMP_CHECKSUM_SHA256
    constexpr size_t checksumSize = 32;
#else
    constexpr size_t checksumSize = sizeof(uint32_t);
#endif
    constexpr size_t minimumSize = headerSize + checksumSize;
    if (dump.partition->size < minimumSize) {
        dump.state = State::Corrupt;
        dump.error = ESP_ERR_INVALID_SIZE;
        return dump;
    }

    uint32_t length = 0;
    dump.error = esp_partition_read(dump.partition, 0, &length, sizeof(length));
    if (dump.error != ESP_OK) {
        dump.state = State::ReadError;
        return dump;
    }
    if (length == UINT32_MAX) {
        dump.state = State::Empty;
        return dump;
    }
    if (length < minimumSize || length > dump.partition->size) {
        dump.state = State::Corrupt;
        dump.error = ESP_ERR_INVALID_SIZE;
        return dump;
    }

    dump.error = esp_core_dump_image_check();
    if (dump.error == ESP_ERR_INVALID_SIZE || dump.error == ESP_ERR_INVALID_CRC) {
        dump.state = State::Corrupt;
    } else if (dump.error != ESP_OK) {
        dump.state = State::ReadError;
    } else {
        dump.state = State::Available;
        dump.size = length;
    }
    return dump;
}

const char* stateName(State state) {
    switch (state) {
        case State::Available: return "available";
        case State::Empty: return "empty";
        case State::MissingPartition: return "missing_partition";
        case State::Corrupt: return "corrupt";
        case State::ReadError: return "read_error";
    }
    return "read_error";
}

#endif

esp_err_t metadata(httpd_req_t* request) {
    JsonDocument document;
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    const Dump dump = inspect();
    document["enabled"] = true;
    document["available"] = dump.state == State::Available;
    document["status"] = stateName(dump.state);
    document["size"] = dump.size;
    if (dump.error != ESP_OK) document["error"] = esp_err_to_name(dump.error);
    if (dump.state == State::Available) {
        document["download"] = "/api/crash-dump/download/";
#if CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
        esp_core_dump_summary_t summary = {};
        if (esp_core_dump_get_summary(&summary) == ESP_OK) {
            summary.exc_task[sizeof(summary.exc_task) - 1] = '\0';
            summary.app_elf_sha256[sizeof(summary.app_elf_sha256) - 1] = '\0';
            document["crashed_task"] = summary.exc_task;
            document["app_elf_sha256"] = reinterpret_cast<char*>(summary.app_elf_sha256);
        }
        char reason[200] = {};
        if (esp_core_dump_get_panic_reason(reason, sizeof(reason)) == ESP_OK) {
            reason[sizeof(reason) - 1] = '\0';
            document["panic_reason"] = reason;
        }
#endif
    }
#else
    document["enabled"] = false;
    document["available"] = false;
    document["status"] = "disabled";
    document["size"] = 0;
#endif
    return sendJson(request, document);
}

esp_err_t download(httpd_req_t* request) {
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    const Dump dump = inspect();
    switch (dump.state) {
        case State::Empty:
            return sendError(request, "404 Not Found", "No crash dump is saved.");
        case State::MissingPartition:
            return sendError(request, "503 Service Unavailable", "The crash dump partition is missing.");
        case State::Corrupt:
            return sendError(request, "422 Unprocessable Content", "The saved crash dump is incomplete or corrupt.");
        case State::ReadError:
            return sendError(request, "503 Service Unavailable", "The crash dump could not be read.");
        case State::Available:
            break;
    }

    char buffer[1024];
    for (size_t offset = 0; offset < dump.size;) {
        const size_t remaining = dump.size - offset;
        const size_t count = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        const esp_err_t readError = esp_partition_read(dump.partition, offset, buffer, count);
        if (readError != ESP_OK) {
            if (offset == 0) {
                return sendError(request, "503 Service Unavailable", "The crash dump could not be read.");
            }
            // Closing the connection without the final chunk marks a partial download as failed.
            return ESP_FAIL;
        }
        if (offset == 0) {
            httpd_resp_set_type(request, "application/octet-stream");
            httpd_resp_set_hdr(request, "Content-Disposition", "attachment; filename=brewpi-crash-dump.bin");
            httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        }
        const esp_err_t sendError = httpd_resp_send_chunk(request, buffer, count);
        if (sendError != ESP_OK) return sendError;
        offset += count;
    }
    return httpd_resp_send_chunk(request, nullptr, 0);
#else
    return sendError(request, "503 Service Unavailable", "Crash dump capture is disabled.");
#endif
}

}

esp_err_t CrashDump::registerRoutes(httpd_handle_t server) {
    httpd_uri_t route = {};
    route.uri = "/api/crash-dump/";
    route.method = HTTP_GET;
    route.handler = metadata;
    esp_err_t error = httpd_register_uri_handler(server, &route);
    if (error != ESP_OK && error != ESP_ERR_HTTPD_HANDLER_EXISTS) return error;
    route.uri = "/api/crash-dump/download/";
    route.handler = download;
    error = httpd_register_uri_handler(server, &route);
    return error == ESP_ERR_HTTPD_HANDLER_EXISTS ? ESP_OK : error;
}

#endif
