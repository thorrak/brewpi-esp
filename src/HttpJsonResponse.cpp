#include "HttpJsonResponse.h"

#ifdef ENABLE_HTTP_INTERFACE

#include <cstdint>
#include <cstring>

namespace {

class ChunkWriter {
public:
    explicit ChunkWriter(httpd_req_t* request) : request_(request) {}

    size_t write(uint8_t byte) {
        return write(&byte, 1);
    }

    size_t write(const uint8_t* data, size_t size) {
        if (error_ != ESP_OK) return 0;
        size_t copied = 0;
        while (copied < size) {
            const size_t available = sizeof(buffer_) - used_;
            const size_t remaining = size - copied;
            const size_t count = remaining < available ? remaining : available;
            memcpy(buffer_ + used_, data + copied, count);
            used_ += count;
            copied += count;
            if (used_ == sizeof(buffer_) && flush() != ESP_OK) return 0;
        }
        return copied;
    }

    esp_err_t finish() {
        if (flush() != ESP_OK) return error_;
        return httpd_resp_send_chunk(request_, nullptr, 0);
    }

private:
    esp_err_t flush() {
        if (error_ != ESP_OK || used_ == 0) return error_;
        error_ = httpd_resp_send_chunk(request_, buffer_, used_);
        used_ = 0;
        return error_;
    }

    httpd_req_t* request_;
    char buffer_[512];
    size_t used_ = 0;
    esp_err_t error_ = ESP_OK;
};

}

esp_err_t HttpJsonResponse::send(httpd_req_t* request, JsonDocument& document) {
    if (document.overflowed()) {
        document.clear();
        esp_err_t error = httpd_resp_set_status(request, "503 Service Unavailable");
        if (error != ESP_OK) return error;
        error = httpd_resp_set_type(request, "application/json");
        if (error != ESP_OK) return error;
        constexpr char body[] = "{\"error\":\"Not enough memory to prepare the response.\"}";
        return httpd_resp_send(request, body, sizeof(body) - 1);
    }

    const esp_err_t error = httpd_resp_set_type(request, "application/json");
    if (error != ESP_OK) return error;
    ChunkWriter writer(request);
    serializeJson(document, writer);
    return writer.finish();
}

#endif
