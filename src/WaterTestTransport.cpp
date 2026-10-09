#include "WaterTestTransport.h"
#include "ESP_BP_WiFi.h"
#include <esp_http_client.h>
#include <esp_err.h>
#include <esp_timer.h>
#include <cstring>
#include <algorithm>
#include <climits>
#include <cstdlib>

namespace WaterTestTransport {
namespace {
constexpr size_t responseLimit = 8192;
constexpr int64_t requestTimeoutUs = 30000000;
// Include one 1024-byte variant pool plus a temporarily grown 1024-byte
// JSON string and bookkeeping. No individual body-sized allocation is needed.
constexpr size_t responseMemoryLimit = 2304;

// The response fields that we keep have a small, fixed memory budget. Unknown
// fields are parsed and skipped without retaining their strings or arrays.
class ResponseAllocator : public ArduinoJson::Allocator {
public:
  void *allocate(size_t length) override { return reallocate(nullptr, length); }
  void deallocate(void *pointer) override {
    if (!pointer)
      return;
    auto header = static_cast<Header *>(pointer) - 1;
    used_ -= sizeof(Header) + header->length;
    std::free(header);
  }
  void *reallocate(void *pointer, size_t length) override {
    auto header = pointer ? static_cast<Header *>(pointer) - 1 : nullptr;
    const size_t previous = header ? sizeof(Header) + header->length : 0;
    const size_t remaining = responseMemoryLimit - (used_ - previous);
    if (sizeof(Header) > remaining || length > remaining - sizeof(Header))
      return nullptr;
    auto replacement = static_cast<Header *>(std::realloc(header, sizeof(Header) + length));
    if (!replacement)
      return nullptr;
    replacement->length = length;
    used_ = used_ - previous + sizeof(Header) + length;
    return replacement + 1;
  }
private:
  struct alignas(std::max_align_t) Header { size_t length; };
  size_t used_ = 0;
};

class RequestWriter {
public:
  RequestWriter(esp_http_client_handle_t client, size_t expected, int64_t deadline)
      : client_(client), expected_(expected), deadline_(deadline) {}
  static bool sink(void *context, const char *data, size_t length) {
    return static_cast<RequestWriter *>(context)->write(data, length);
  }
  bool finish() { return accepted_ == expected_ && flush(); }
  esp_err_t result() const { return result_; }
private:
  bool write(const char *data, size_t length) {
    if (result_ != ESP_OK || length > expected_ - accepted_)
      return false;
    while (length) {
      const size_t count = std::min(length, sizeof(buffer_) - buffered_);
      memcpy(buffer_ + buffered_, data, count);
      buffered_ += count;
      accepted_ += count;
      data += count;
      length -= count;
      if (buffered_ == sizeof(buffer_) && !flush())
        return false;
    }
    return true;
  }
  bool flush() {
    size_t offset = 0;
    while (offset < buffered_) {
      if (esp_timer_get_time() >= deadline_) {
        result_ = ESP_ERR_TIMEOUT;
        return false;
      }
      const int count = esp_http_client_write(client_, buffer_ + offset, int(buffered_ - offset));
      if (count <= 0 || size_t(count) > buffered_ - offset) {
        result_ = count == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_HTTP_EAGAIN : ESP_FAIL;
        return false;
      }
      offset += size_t(count);
    }
    buffered_ = 0;
    return true;
  }
  esp_http_client_handle_t client_;
  size_t expected_, accepted_ = 0, buffered_ = 0;
  int64_t deadline_;
  esp_err_t result_ = ESP_OK;
  char buffer_[512];
};

class ResponseReader {
public:
  ResponseReader(esp_http_client_handle_t client, int64_t deadline) : client_(client), deadline_(deadline) {}
  int read() {
    if (position_ < buffered_)
      return static_cast<unsigned char>(buffer_[position_++]);
    if (result_ != ESP_OK || tooLong_ || ended_)
      return -1;
    if (esp_timer_get_time() >= deadline_) {
      result_ = ESP_ERR_TIMEOUT;
      return -1;
    }
    const int count = esp_http_client_read(client_, buffer_, int(std::min(sizeof(buffer_), responseLimit - received_ + 1)));
    if (count < 0) {
      result_ = count == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_HTTP_EAGAIN : ESP_FAIL;
      return -1;
    }
    if (!count) {
      ended_ = true;
      if (!esp_http_client_is_complete_data_received(client_))
        result_ = ESP_FAIL;
      return -1;
    }
    received_ += size_t(count);
    if (received_ > responseLimit) {
      tooLong_ = true;
      return -1;
    }
    buffered_ = size_t(count);
    position_ = 1;
    return static_cast<unsigned char>(buffer_[0]);
  }
  size_t readBytes(char *buffer, size_t length) {
    size_t count = 0;
    int c;
    while (count < length && (c = read()) >= 0)
      buffer[count++] = static_cast<char>(c);
    return count;
  }
  // Parsing an object may stop at its closing brace. Require a complete HTTP
  // body with no additional JSON, garbage or silently truncated chunk stream.
  bool finish() {
    bool whitespaceOnly = true;
    int c;
    while ((c = read()) >= 0)
      whitespaceOnly = whitespaceOnly && (c == ' ' || c == '\t' || c == '\r' || c == '\n');
    return whitespaceOnly && ended_ && result_ == ESP_OK && !tooLong_;
  }
  esp_err_t result() const { return result_; }
  bool tooLong() const { return tooLong_; }
private:
  esp_http_client_handle_t client_;
  int64_t deadline_;
  size_t received_ = 0, position_ = 0, buffered_ = 0;
  esp_err_t result_ = ESP_OK;
  bool tooLong_ = false, ended_ = false;
  char buffer_[256];
};

// Only display an API error string, never HTML or an arbitrary request echo.
// Bound the displayed message and flatten controls for the status page.
std::string serverError(JsonVariantConst document) {
  const char *message = document["error"].is<const char *>() ? document["error"].as<const char *>()
                                                          : document["detail"].as<const char *>();
  if (!message)
    return {};
  constexpr size_t limit = 256;
  const size_t length = std::strlen(message);
  size_t count = std::min(length, limit);
  while (count && count < length && (static_cast<unsigned char>(message[count]) & 0xc0) == 0x80)
    --count;
  std::string result;
  for (size_t n = 0; n < count; ++n) {
    const unsigned char c = static_cast<unsigned char>(message[n]);
    result += c < 32 || c == 127 ? ' ' : static_cast<char>(c);
  }
  if (length > count)
    result += "...";
  return result;
}
} // namespace
bool send(const std::string &path, bool post, const BodySource &body, JsonDocument &response,
          std::string &error) {
  response.clear();
  if (!body.write || body.length > INT_MAX) {
    error = "Invalid upload body source.";
    return false;
  }
  if (!bp_wifi_is_connected()) {
    error = "WiFi disconnected; original data retained.";
    return false;
  }
  const int64_t deadline = esp_timer_get_time() + requestTimeoutUs;
  std::string url = std::string(endpoint) + path;
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = post ? HTTP_METHOD_POST : HTTP_METHOD_PUT;
  config.timeout_ms = 6000;
  config.disable_auto_redirect = true;
  auto client = esp_http_client_init(&config);
  if (!client) {
    error = "HTTP client allocation failed.";
    return false;
  }
  esp_err_t result = esp_http_client_set_header(client, "Content-Type", "application/json");
  if (result == ESP_OK)
    result = esp_http_client_set_header(client, "User-Agent", "BrewPi-WaterTest/1");
  if (result != ESP_OK) {
    error = std::string("HTTP request setup failed (") + esp_err_to_name(result) + ").";
    esp_http_client_cleanup(client);
    return false;
  }
  result = esp_http_client_open(client, int(body.length));
  if (result == ESP_OK) {
    RequestWriter writer(client, body.length, deadline);
    if (!body.write(body.context, RequestWriter::sink, &writer) || !writer.finish())
      result = writer.result() == ESP_OK ? ESP_FAIL : writer.result();
  }
  int64_t contentLength = -1;
  if (result == ESP_OK) {
    if (esp_timer_get_time() >= deadline)
      result = ESP_ERR_TIMEOUT;
    else {
      contentLength = esp_http_client_fetch_headers(client);
      if (contentLength < 0)
        result = contentLength == -ESP_ERR_HTTP_EAGAIN ? ESP_ERR_HTTP_EAGAIN : ESP_FAIL;
    }
  }
  const int code = esp_http_client_get_status_code(client);
  bool tooLong = contentLength > int64_t(responseLimit);
  ResponseAllocator allocator;
  JsonDocument document(&allocator);
  bool validJson = false;
  if (result == ESP_OK && !tooLong) {
    JsonDocument filter;
    for (const char *key : {"test_id", "device_guid", "status", "batch_id", "accepted_ranges",
                            "upload_status", "missing_record_count", "finish_received", "error", "detail"})
      filter[key] = true;
    ResponseReader reader(client, deadline);
    if (!filter.overflowed()) {
      const auto parsed = deserializeJson(document, reader, DeserializationOption::Filter(filter));
      validJson = parsed == DeserializationError::Ok && reader.finish();
    }
    result = reader.result();
    tooLong = reader.tooLong();
  }
  // Capture transport diagnostics before cleanup destroys the connection state.
  const int socketError = result != ESP_OK ? esp_http_client_get_errno(client) : 0;
  const esp_err_t connectionError = result != ESP_OK
      ? esp_http_client_get_and_clear_last_tls_error(client, nullptr, nullptr) : ESP_OK;
  esp_http_client_cleanup(client);
  if (result != ESP_OK || (code != 200 && code != 201) || tooLong) {
    error = code > 0 ? "HTTP upload pending (status " + std::to_string(code) + ")."
                     : "HTTP upload pending; no server response.";
    if (result != ESP_OK) {
      error += std::string(" Transport: ") + esp_err_to_name(result);
      if (connectionError != ESP_OK && connectionError != ESP_FAIL && connectionError != result)
        error += std::string("; ") + esp_err_to_name(connectionError);
      if (socketError > 0)
        error += "; socket error " + std::to_string(socketError);
      error += ".";
    }
    if (tooLong)
      error += " Server reply exceeded 8192 bytes.";
    else if (validJson) {
      const auto message = serverError(document.as<JsonVariantConst>());
      if (!message.empty())
        error += " Server: " + message;
    }
    if (code >= 300 && code < 400)
      error += " Configure the collection API to accept HTTP without redirect.";
    return false;
  }
  if (!validJson || !response.set(document) || response.overflowed()) {
    response.clear();
    error = "Invalid server acknowledgement.";
    return false;
  }
  return true;
}
} // namespace WaterTestTransport
