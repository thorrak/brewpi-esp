#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
constexpr esp_err_t ESP_FAIL = -1;
constexpr esp_err_t ESP_ERR_NOT_FOUND = 0x105;
constexpr esp_err_t ESP_ERR_INVALID_SIZE = 0x104;
constexpr esp_err_t ESP_ERR_INVALID_CRC = 0x109;
constexpr esp_err_t ESP_ERR_HTTPD_HANDLER_EXISTS = 0x7006;
constexpr int HTTP_GET = 0;
using httpd_handle_t = void*;

struct httpd_req_t {
    std::string body;
    std::string status = "200 OK";
    std::string type;
    std::map<std::string, std::string> headers;
    std::vector<size_t> chunks;
    size_t failChunk = 0;
    bool completed = false;
};

struct httpd_uri_t {
    const char* uri = nullptr;
    int method = HTTP_GET;
    esp_err_t (*handler)(httpd_req_t*) = nullptr;
    void* user_ctx = nullptr;
};

const char* esp_err_to_name(esp_err_t error);
esp_err_t httpd_resp_set_type(httpd_req_t*, const char*);
esp_err_t httpd_resp_set_status(httpd_req_t*, const char*);
esp_err_t httpd_resp_set_hdr(httpd_req_t*, const char*, const char*);
esp_err_t httpd_resp_send(httpd_req_t*, const char*, size_t);
esp_err_t httpd_resp_send_chunk(httpd_req_t*, const char*, size_t);
esp_err_t httpd_register_uri_handler(httpd_handle_t, const httpd_uri_t*);
