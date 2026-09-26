#pragma once
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

using esp_err_t = int;
using httpd_err_code_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, HTTPD_RESP_USE_STRLEN = -1;
struct httpd_req_t {
    const char* uri;
    std::string body, type;
    std::map<std::string, std::string> headers;
};
struct httpServer {
    static const char* getContentType(const char*);
    static esp_err_t handleFileRead(httpd_req_t*, const char*);
    static esp_err_t static_file_handler(httpd_req_t*);
    static esp_err_t not_found_handler(httpd_req_t*, httpd_err_code_t);
};
inline std::string root;
inline unsigned filesystemCalls = 0;
inline bool fs_exists(const char* path) {
    ++filesystemCalls;
    return std::filesystem::exists(root + path);
}
inline FILE* fs_open(const char* path, const char* mode) {
    ++filesystemCalls;
    return fopen((root + path).c_str(), mode);
}
inline void httpd_resp_set_type(httpd_req_t* req, const char* type) { req->type = type; }
inline void httpd_resp_set_hdr(httpd_req_t* req, const char* name, const char* value) {
    req->headers[name] = value;
}
inline int httpd_resp_send_chunk(httpd_req_t* req, const char* data, size_t size) {
    if (size) req->body.append(data, size);
    return ESP_OK;
}
inline int httpd_resp_send(httpd_req_t* req, const char* data, int size) {
    req->body.assign(data, size == HTTPD_RESP_USE_STRLEN ? strlen(data) : size_t(size));
    return ESP_OK;
}
inline size_t native_strlcpy(char* dest, const char* src, size_t size) {
    size_t length = strlen(src);
    if (size) {
        size_t copied = std::min(length, size - 1);
        memcpy(dest, src, copied);
        dest[copied] = '\0';
    }
    return length;
}
inline size_t native_strlcat(char* dest, const char* src, size_t size) {
    size_t length = strlen(dest);
    return length + native_strlcpy(dest + length, src, size - length);
}
#define strlcpy native_strlcpy
#define strlcat native_strlcat
