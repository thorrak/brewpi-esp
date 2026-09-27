#include "CrashDump.h"
#include "esp_core_dump.h"
#include "esp_partition.h"
#include <ArduinoJson.h>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>

namespace {
std::map<std::string, httpd_uri_t> routes;
esp_partition_t partition = {0x2f0000, 4096};
std::vector<uint8_t> flash;
bool partitionPresent = true;
size_t reads = 0;
size_t failRead = 0;
unsigned integrityChecks = 0;
unsigned summaryReads = 0;
esp_err_t integrityError = ESP_OK;
esp_err_t summaryError = ESP_OK;
esp_err_t registrationError = ESP_OK;
unsigned registrationCalls = 0;

void reset(uint32_t length = 2301) {
    partition.size = 4096;
    flash.resize(partition.size);
    for (size_t i = 0; i < flash.size(); ++i) flash[i] = static_cast<uint8_t>(i * 37);
    memcpy(flash.data(), &length, sizeof(length));
    partitionPresent = true;
    reads = failRead = 0;
    integrityChecks = summaryReads = 0;
    integrityError = summaryError = ESP_OK;
}

JsonDocument metadata(const char* expectedStatus) {
    httpd_req_t request;
    assert(routes.at("/api/crash-dump/").handler(&request) == ESP_OK);
    assert(request.type == "application/json");
    assert(request.headers.at("Cache-Control") == "no-store");
    assert(request.completed);
    JsonDocument document;
    assert(deserializeJson(document, request.body) == DeserializationError::Ok);
    assert(document["status"].as<std::string>() == expectedStatus);
    return document;
}

httpd_req_t downloadError(const char* expectedStatus) {
    httpd_req_t request;
    assert(routes.at("/api/crash-dump/download/").handler(&request) == ESP_OK);
    assert(request.status == expectedStatus);
    assert(request.type == "application/json");
    assert(!request.chunks.empty() && request.chunks.back() == 0);
    assert(request.completed);
    return request;
}
}

const char* esp_err_to_name(esp_err_t error) {
    switch (error) {
        case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
        case ESP_ERR_INVALID_CRC: return "ESP_ERR_INVALID_CRC";
        default: return "ESP_FAIL";
    }
}

esp_err_t httpd_resp_set_type(httpd_req_t* request, const char* value) {
    request->type = value;
    return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t* request, const char* value) {
    request->status = value;
    return ESP_OK;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t* request, const char* name, const char* value) {
    request->headers[name] = value;
    return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t* request, const char* data, size_t size) {
    request->body.assign(data, size);
    request->completed = true;
    return ESP_OK;
}
esp_err_t httpd_resp_send_chunk(httpd_req_t* request, const char* data, size_t size) {
    request->chunks.push_back(size);
    if (request->chunks.size() == request->failChunk) return ESP_FAIL;
    if (size) request->body.append(data, size);
    else request->completed = true;
    return ESP_OK;
}
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t* route) {
    ++registrationCalls;
    if (registrationError != ESP_OK) return registrationError;
    if (routes.count(route->uri)) return ESP_ERR_HTTPD_HANDLER_EXISTS;
    routes.emplace(route->uri, *route);
    return ESP_OK;
}

const esp_partition_t* esp_partition_find_first(int type, int subtype, const char* label) {
    assert(type == ESP_PARTITION_TYPE_DATA);
    assert(subtype == ESP_PARTITION_SUBTYPE_DATA_COREDUMP);
    assert(label == nullptr);
    return partitionPresent ? &partition : nullptr;
}
esp_err_t esp_partition_read(const esp_partition_t* source, size_t offset, void* data, size_t size) {
    assert(source == &partition);
    assert(offset <= partition.size && size <= partition.size - offset);
    assert(size <= 1024);
    if (++reads == failRead) return ESP_FAIL;
    memcpy(data, flash.data() + offset, size);
    return ESP_OK;
}
esp_err_t esp_core_dump_image_check() {
    ++integrityChecks;
    return integrityError;
}
#if CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
esp_err_t esp_core_dump_get_summary(esp_core_dump_summary_t* summary) {
    ++summaryReads;
    // Fill both arrays to capacity to exercise termination of returned metadata.
    memset(summary->exc_task, 't', sizeof(summary->exc_task));
    memset(summary->app_elf_sha256, 'a', sizeof(summary->app_elf_sha256));
    return summaryError;
}
esp_err_t esp_core_dump_get_panic_reason(char* reason, size_t size) {
    memset(reason, 'p', size);
    return summaryError;
}
#endif

int main() {
    registrationError = ESP_FAIL;
    assert(CrashDump::registerRoutes(nullptr) == ESP_FAIL);
    assert(registrationCalls == 1);
    registrationError = ESP_OK;
    assert(CrashDump::registerRoutes(nullptr) == ESP_OK);
    assert(routes.size() == 2);
    assert(CrashDump::registerRoutes(nullptr) == ESP_OK);
    routes.erase("/api/crash-dump/download/");
    assert(CrashDump::registerRoutes(nullptr) == ESP_OK);
    assert(routes.size() == 2);

    reset();
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    const auto original = flash;
    auto info = metadata("available");
    assert(info["enabled"] == true && info["available"] == true);
    assert(info["size"] == 2301);
    assert(integrityChecks == 1);
#if CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    assert(summaryReads == 1);
    assert(info["crashed_task"].as<std::string>() == std::string(15, 't'));
    assert(info["app_elf_sha256"].as<std::string>() == std::string(64, 'a'));
    assert(info["panic_reason"].as<std::string>() == std::string(199, 'p'));
    summaryError = ESP_FAIL;
    info = metadata("available");
    assert(info["crashed_task"].isNull() && info["panic_reason"].isNull());
#else
    assert(info["crashed_task"].isNull());
#endif

    reset();
    httpd_req_t request;
    assert(routes.at("/api/crash-dump/download/").handler(&request) == ESP_OK);
    assert(request.type == "application/octet-stream");
    assert(request.completed);
    assert(request.body == std::string(reinterpret_cast<const char*>(flash.data()), 2301));
    assert(request.chunks == std::vector<size_t>({1024, 1024, 253, 0}));
    assert(request.headers.at("Cache-Control") == "no-store");
    assert(request.headers.at("Content-Disposition").find(".bin") != std::string::npos);
    assert(flash == original);
    assert(summaryReads == 0);

    reset(UINT32_MAX);
    info = metadata("empty");
    assert(info["available"] == false);
    downloadError("404 Not Found");
    assert(integrityChecks == 0);

    reset();
    partitionPresent = false;
    metadata("missing_partition");
    downloadError("503 Service Unavailable");
    assert(reads == 0 && integrityChecks == 0);

    for (uint32_t length : {0U, 1U, 4U, 24U, 27U, 4097U, UINT32_MAX - 1}) {
        reset(length);
        info = metadata("corrupt");
        assert(info["error"] == "ESP_ERR_INVALID_SIZE");
        downloadError("422 Unprocessable Content");
        assert(integrityChecks == 0 && summaryReads == 0);
    }
#if CONFIG_ESP_COREDUMP_CHECKSUM_SHA256
    reset(32);
    metadata("corrupt");
    assert(integrityChecks == 0);
#endif
    reset();
    partition.size = 3;
    metadata("corrupt");
    assert(reads == 0);

    for (esp_err_t error : {ESP_ERR_INVALID_CRC, ESP_ERR_INVALID_SIZE}) {
        reset();
        integrityError = error;
        metadata("corrupt");
        downloadError("422 Unprocessable Content");
        assert(summaryReads == 0);
    }
    reset();
    integrityError = ESP_FAIL;
    metadata("read_error");
    downloadError("503 Service Unavailable");

    reset();
    failRead = 1;
    metadata("read_error");
    assert(integrityChecks == 0);
    reset();
    failRead = 1;
    downloadError("503 Service Unavailable");

    // The header succeeds but the first chunk cannot be read.
    reset();
    failRead = 2;
    request = downloadError("503 Service Unavailable");
    assert(request.headers.count("Content-Disposition") == 0);

    // After any data was sent, abort without a success terminator on flash or network errors.
    reset();
    failRead = 3;
    request = {};
    assert(routes.at("/api/crash-dump/download/").handler(&request) == ESP_FAIL);
    assert(!request.completed && request.body.size() == 1024);
    assert(request.chunks == std::vector<size_t>({1024}));
    assert(flash == original);

    reset();
    request = {};
    request.failChunk = 2;
    assert(routes.at("/api/crash-dump/download/").handler(&request) == ESP_FAIL);
    assert(!request.completed && request.body.size() == 1024);
    assert(reads == 3);
    assert(flash == original);

    // A dump filling the partition must end exactly at its boundary.
    reset(4096);
    request = {};
    assert(routes.at("/api/crash-dump/download/").handler(&request) == ESP_OK);
    assert(request.body.size() == 4096 && request.completed);
#else
    auto info = metadata("disabled");
    assert(info["enabled"] == false && info["available"] == false);
    downloadError("503 Service Unavailable");
    assert(reads == 0 && integrityChecks == 0 && summaryReads == 0);
#endif
    std::cout << "Crash dump routes passed\n";
}
