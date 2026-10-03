#include <ArduinoJson.h>
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0, ESP_FAIL = -1;
constexpr size_t HTTPD_RESP_USE_STRLEN = size_t(-1);

struct httpd_req_t {
    explicit httpd_req_t(std::string input)
        : content_len(input.size()), input(std::move(input)) {}
    size_t content_len;
    std::string input, response;
    const char *status = "200 OK", *type = nullptr;
    size_t received = 0, receiveCalls = 0;
};

namespace WaterTest {
bool owned = false;
bool controlOwned() { return owned; }
}

class httpServer {
public:
    static esp_err_t parseJsonBody(httpd_req_t *, JsonDocument &);
};

int httpd_req_recv(httpd_req_t *request, char *buffer, size_t size) {
    ++request->receiveCalls;
    const size_t count = std::min({size, request->input.size() - request->received, size_t(3)});
    if (count) std::memcpy(buffer, request->input.data() + request->received, count);
    request->received += count;
    return int(count);
}
esp_err_t httpd_resp_set_status(httpd_req_t *request, const char *status) {
    request->status = status;
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *request, const char *type) {
    request->type = type;
    return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *request, const char *data, size_t length) {
    request->response.assign(data, length == HTTPD_RESP_USE_STRLEN ? std::strlen(data) : length);
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *request, const char *data) {
    return httpd_resp_send(request, data, HTTPD_RESP_USE_STRLEN);
}

bool rejectRequestAllocation = false;
size_t requestAllocationCalls = 0;
void *allocateRequest(size_t size) {
    ++requestAllocationCalls;
    return rejectRequestAllocation ? nullptr : std::malloc(size);
}
#define malloc allocateRequest
#include "production_put_handler.h"
#undef malloc

unsigned handlerCalls = 0;
bool acceptSettings = true;
bool updateSettings(const JsonDocument &document, bool propagate) {
    ++handlerCalls;
    assert(propagate && document["setting"] == 17);
    return acceptSettings;
}

JsonDocument result(const httpd_req_t &request, const char *status) {
    assert(std::strcmp(request.status, status) == 0);
    assert(request.type && std::strcmp(request.type, "application/json") == 0);
    JsonDocument document;
    assert(deserializeJson(document, request.response) == DeserializationError::Ok);
    return document;
}

int main() {
    for (const char *input : {"{malformed", ""}) {
        httpd_req_t request(input);
        assert(put_json_handler<updateSettings>(&request) == ESP_OK);
        assert(result(request, "400 Bad Request")["status"] == "error");
        assert(handlerCalls == 0);
    }

    rejectRequestAllocation = true;
    const size_t allocationsBefore = requestAllocationCalls;
    httpd_req_t unavailable("{\"setting\":17}");
    assert(put_json_handler<updateSettings>(&unavailable) == ESP_OK);
    assert(result(unavailable, "400 Bad Request")["status"] == "error");
    assert(handlerCalls == 0 && unavailable.receiveCalls == 0);
    assert(requestAllocationCalls == allocationsBefore + 1);
    rejectRequestAllocation = false;

    WaterTest::owned = true;
    httpd_req_t conflict("{\"setting\":17}");
    assert(put_json_handler<updateSettings>(&conflict) == ESP_OK);
    const auto conflictResult = result(conflict, "409 Conflict");
    assert(conflictResult["status"] == false && conflictResult["error"].is<const char *>());
    assert(handlerCalls == 0 && conflict.receiveCalls == 0);
    WaterTest::owned = false;

    acceptSettings = false;
    httpd_req_t rejected("{\"setting\":17}");
    assert(put_json_handler<updateSettings>(&rejected) == ESP_OK);
    assert(result(rejected, "400 Bad Request")["status"] == "error");
    assert(handlerCalls == 1);

    acceptSettings = true;
    httpd_req_t accepted("{\"setting\":17}");
    assert(put_json_handler<updateSettings>(&accepted) == ESP_OK);
    assert(result(accepted, "200 OK")["status"] == "ok");
    assert(handlerCalls == 2 && accepted.received == accepted.content_len);
    puts("PUT requests: parse/allocation failures and ownership conflicts cannot mutate settings; validation and success statuses passed.");
}
