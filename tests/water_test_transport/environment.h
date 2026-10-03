#include <ArduinoJson.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_TIMEOUT = 0x107, ESP_ERR_HTTP_EAGAIN = 0x7007;
constexpr int HTTP_METHOD_POST = 1, HTTP_METHOD_PUT = 2;
struct esp_http_client_config_t {
  const char *url = nullptr;
  int method = 0, timeout_ms = 0;
  bool disable_auto_redirect = false;
};
namespace NativeTransport {
std::string request, reply;
size_t offset = 0, largestWrite = 0;
int writtenLength = 0, initCalls = 0, cleanupCalls = 0, fetchCalls = 0;
int code = 201, socketError = 0, connectionError = ESP_OK;
int openResult = ESP_OK, headerResult = ESP_OK, readResult = ESP_OK;
int writeAfter = -1, maxWrite = 7, maxRead = 3;
int64_t now = 0, advance = 1000;
bool connected = true, allocationFailure = false, unknownLength = false, truncated = false;
void reset(const std::string &response = "{\"test_id\":\"123\",\"device_guid\":\"456\",\"status\":\"stored\",\"batch_id\":\"123-00000000\",\"accepted_ranges\":[[1,12]]}") {
  request.clear(); reply = response; offset = largestWrite = 0;
  writtenLength = initCalls = cleanupCalls = fetchCalls = 0;
  code = 201; socketError = 0; connectionError = ESP_OK;
  openResult = headerResult = readResult = ESP_OK;
  writeAfter = -1; maxWrite = 7; maxRead = 3;
  now = 0; advance = 1000;
  connected = true; allocationFailure = unknownLength = truncated = false;
}
}
using esp_http_client_handle_t = void *;
inline bool bp_wifi_is_connected() { return NativeTransport::connected; }
inline int64_t esp_timer_get_time() { return NativeTransport::now; }
inline const char *esp_err_to_name(int code) {
  if (code == ESP_OK) return "ESP_OK";
  if (code == ESP_FAIL) return "ESP_FAIL";
  if (code == ESP_ERR_TIMEOUT) return "ESP_ERR_TIMEOUT";
  if (code == ESP_ERR_HTTP_EAGAIN) return "ESP_ERR_HTTP_EAGAIN";
  return "ESP_ERR_CONNECTION_FAILED";
}
inline esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
  assert(config->timeout_ms == 6000 && config->disable_auto_redirect);
  assert(std::string(config->url).find("http://chill.fermentrack.net/") == 0);
  ++NativeTransport::initCalls;
  return NativeTransport::allocationFailure ? nullptr : &NativeTransport::code;
}
inline int esp_http_client_set_header(esp_http_client_handle_t, const char *, const char *) {
  return NativeTransport::headerResult;
}
inline int esp_http_client_open(esp_http_client_handle_t, int length) {
  NativeTransport::writtenLength = length;
  return NativeTransport::openResult;
}
inline int esp_http_client_write(esp_http_client_handle_t, const char *bytes, int length) {
  using namespace NativeTransport;
  now += advance;
  largestWrite = std::max(largestWrite, size_t(length));
  if (writeAfter >= 0 && request.size() >= size_t(writeAfter)) return -1;
  const int count = std::min(length, maxWrite);
  request.append(bytes, size_t(count));
  return count;
}
inline int64_t esp_http_client_fetch_headers(esp_http_client_handle_t) {
  ++NativeTransport::fetchCalls;
  return NativeTransport::unknownLength ? 0 : int64_t(NativeTransport::reply.size());
}
inline int esp_http_client_get_status_code(esp_http_client_handle_t) { return NativeTransport::code; }
inline int esp_http_client_read(esp_http_client_handle_t, char *data, int length) {
  using namespace NativeTransport;
  now += advance;
  if (readResult != ESP_OK) return readResult;
  const size_t count = std::min(std::min(size_t(length), size_t(maxRead)), reply.size() - offset);
  memcpy(data, reply.data() + offset, count);
  offset += count;
  return int(count);
}
inline bool esp_http_client_is_complete_data_received(esp_http_client_handle_t) {
  return NativeTransport::offset == NativeTransport::reply.size() && !NativeTransport::truncated;
}
inline int esp_http_client_get_errno(esp_http_client_handle_t) { return NativeTransport::socketError; }
inline int esp_http_client_get_and_clear_last_tls_error(esp_http_client_handle_t, void *, void *) {
  return NativeTransport::connectionError;
}
inline int esp_http_client_cleanup(esp_http_client_handle_t) { ++NativeTransport::cleanupCalls; return ESP_OK; }
