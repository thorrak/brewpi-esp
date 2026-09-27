#include "HttpJsonResponse.h"

#include <algorithm>
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>
#include <string>

namespace {
bool forbidNew = false;
}

void* operator new(size_t size) {
    assert(!forbidNew);
    if (void* memory = malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { free(memory); }
void operator delete[](void* memory) noexcept { free(memory); }
#if defined(__cpp_sized_deallocation)
void operator delete(void* memory, size_t) noexcept { free(memory); }
void operator delete[](void* memory, size_t) noexcept { free(memory); }
#endif

namespace {

esp_err_t sendWithoutNew(httpd_req_t& request, JsonDocument& document) {
    forbidNew = true;
    const auto result = HttpJsonResponse::send(&request, document);
    forbidNew = false;
    return result;
}

void append(httpd_req_t* request, const char* data, size_t size) {
    assert(size <= request->body.size() - request->bodySize);
    if (size) memcpy(request->body.data() + request->bodySize, data, size);
    request->bodySize += size;
}

std::string body(const httpd_req_t& request) {
    return std::string(request.body.data(), request.bodySize);
}

void checkResponse(JsonDocument& document, const std::string& expected) {
    httpd_req_t request;
    assert(sendWithoutNew(request, document) == ESP_OK);
    assert(strcmp(request.type, "application/json") == 0);
    assert(strcmp(request.status, "200 OK") == 0);
    assert(request.completed && request.sendCalls == 0);
    assert(request.chunkCalls == (expected.size() + 511) / 512 + 1);
    assert(request.chunks[request.chunkCalls - 1] == 0);
    for (size_t i = 0; i + 1 < request.chunkCalls; ++i)
        assert(request.chunks[i] > 0 && request.chunks[i] <= 512);
    assert(body(request) == expected);
    JsonDocument parsed;
    assert(deserializeJson(parsed, request.body.data(), request.bodySize) == DeserializationError::Ok);
}

class RejectingAllocator : public ArduinoJson::Allocator {
public:
    bool reject = false;
    size_t deallocations = 0;

    void* allocate(size_t size) override { return reject ? nullptr : malloc(size); }
    void* reallocate(void* memory, size_t size) override {
        return reject ? nullptr : realloc(memory, size);
    }
    void deallocate(void* memory) override {
        if (memory) ++deallocations;
        free(memory);
    }
};

void overflow(JsonDocument& document, RejectingAllocator& allocator) {
    document.clear();
    allocator.reject = false;
    document["kept"] = 1;
    assert(!document.overflowed());
    allocator.reject = true;
    document["missing"] = std::string(4000, 'x');
    assert(document.overflowed());
}

}

esp_err_t httpd_resp_set_type(httpd_req_t* request, const char* value) {
    ++request->typeCalls;
    if (request->typeError != ESP_OK) return request->typeError;
    request->type = value;
    return ESP_OK;
}

esp_err_t httpd_resp_set_status(httpd_req_t* request, const char* value) {
    ++request->statusCalls;
    if (request->statusError != ESP_OK) return request->statusError;
    request->status = value;
    return ESP_OK;
}

esp_err_t httpd_resp_send(httpd_req_t* request, const char* data, size_t size) {
    ++request->sendCalls;
    if (request->sendError != ESP_OK) return request->sendError;
    append(request, data, size);
    request->completed = true;
    return ESP_OK;
}

esp_err_t httpd_resp_send_chunk(httpd_req_t* request, const char* data, size_t size) {
    assert(request->chunkCalls < request->chunks.size());
    assert(size <= 512);
    request->chunks[request->chunkCalls++] = size;
    if (request->chunkCalls == request->failChunk) return request->chunkError;
    append(request, data, size);
    if (size == 0) request->completed = true;
    return ESP_OK;
}

int main() {
    JsonDocument document;
    checkResponse(document, "null");
    document.to<JsonObject>();
    checkResponse(document, "{}");
    document.set(true);
    checkResponse(document, "true");

    for (size_t length : {510U, 511U, 512U, 1022U, 1023U, 1024U}) {
        document.set(std::string(length, 'x'));
        checkResponse(document, '"' + std::string(length, 'x') + '"');
    }

    document.clear();
    auto records = document["nested"]["records"].to<JsonArray>();
    for (unsigned i = 0; i < 100; ++i) {
        auto record = records.add<JsonObject>();
        record["index"] = i;
        record["value"] = "quotes\" and backslashes\\\n\t\r and UTF-8 \xc2\xb0 C";
        record["temperature"] = 18.125;
    }
    document["large_string"] = std::string(2000, 'z');
    std::string expected;
    serializeJson(document, expected);
    assert(expected.size() > 4096 && !document.overflowed());
    checkResponse(document, expected);

    const size_t finalCall = (expected.size() + 511) / 512 + 1;
    for (size_t failure : {size_t(1), size_t(3), finalCall - 1, finalCall}) {
        httpd_req_t request;
        request.failChunk = failure;
        assert(sendWithoutNew(request, document) == request.chunkError);
        assert(request.chunkCalls == failure && !request.completed);
        assert(request.bodySize == std::min((failure - 1) * 512, expected.size()));
        assert(expected.compare(0, request.bodySize, body(request)) == 0);
        if (failure < finalCall) assert(request.chunks[failure - 1] != 0);
    }

    httpd_req_t typeFailure;
    typeFailure.typeError = -43;
    assert(sendWithoutNew(typeFailure, document) == -43);
    assert(typeFailure.chunkCalls == 0 && typeFailure.sendCalls == 0);

    httpd_req_t existingStatus;
    existingStatus.status = "422 Unprocessable Content";
    assert(sendWithoutNew(existingStatus, document) == ESP_OK);
    assert(strcmp(existingStatus.status, "422 Unprocessable Content") == 0);

    RejectingAllocator allocator;
    JsonDocument partial(&allocator);
    overflow(partial, allocator);
    const auto before = allocator.deallocations;
    httpd_req_t unavailable;
    assert(sendWithoutNew(unavailable, partial) == ESP_OK);
    assert(strcmp(unavailable.status, "503 Service Unavailable") == 0);
    assert(strcmp(unavailable.type, "application/json") == 0);
    assert(unavailable.completed && unavailable.chunkCalls == 0 && unavailable.sendCalls == 1);
    assert(partial.isNull() && !partial.overflowed() && allocator.deallocations > before);
    JsonDocument error;
    assert(deserializeJson(error, unavailable.body.data(), unavailable.bodySize) == DeserializationError::Ok);
    assert(error["error"].is<const char*>() && error["kept"].isNull());

    for (int failure = 0; failure < 3; ++failure) {
        overflow(partial, allocator);
        httpd_req_t request;
        if (failure == 0) request.statusError = -44;
        if (failure == 1) request.typeError = -44;
        if (failure == 2) request.sendError = -44;
        assert(sendWithoutNew(request, partial) == -44);
        assert(!request.completed && request.chunkCalls == 0 && request.bodySize == 0);
        assert(request.statusCalls == 1);
        assert(request.typeCalls == (failure == 0 ? 0 : 1));
        assert(request.sendCalls == (failure == 2 ? 1 : 0));
    }

    std::cout << "Bounded JSON streaming and low-memory error responses passed.\n";
}
