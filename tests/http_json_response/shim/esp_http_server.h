#pragma once

#include <array>
#include <cstddef>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;

struct httpd_req_t {
    const char* status = "200 OK";
    const char* type = nullptr;
    std::array<char, 32768> body{};
    std::array<size_t, 128> chunks{};
    size_t bodySize = 0;
    size_t chunkCalls = 0;
    size_t failChunk = 0;
    size_t typeCalls = 0;
    size_t statusCalls = 0;
    size_t sendCalls = 0;
    bool completed = false;
    esp_err_t typeError = ESP_OK;
    esp_err_t statusError = ESP_OK;
    esp_err_t sendError = ESP_OK;
    esp_err_t chunkError = -42;
};

esp_err_t httpd_resp_set_type(httpd_req_t* request, const char* type);
esp_err_t httpd_resp_set_status(httpd_req_t* request, const char* status);
esp_err_t httpd_resp_send(httpd_req_t* request, const char* data, size_t size);
esp_err_t httpd_resp_send_chunk(httpd_req_t* request, const char* data, size_t size);
