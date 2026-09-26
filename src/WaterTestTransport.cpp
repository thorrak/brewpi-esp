#include "WaterTestTransport.h"
#include "ESP_BP_WiFi.h"
#include <esp_http_client.h>

namespace WaterTestTransport {
namespace {
struct Response {
  std::string body;
  bool tooLong = false;
};
esp_err_t httpEvent(esp_http_client_event_t *e) {
  if (e->event_id == HTTP_EVENT_ON_DATA) {
    auto r = static_cast<Response *>(e->user_data);
    if (r->body.size() + e->data_len > 8192) {
      r->tooLong = true;
      return ESP_FAIL;
    }
    r->body.append(static_cast<const char *>(e->data), e->data_len);
  }
  return ESP_OK;
}
} // namespace
bool send(const std::string &path, bool post, const std::string &body, JsonDocument &response,
          std::string &error) {
  if (!bp_wifi_is_connected()) {
    error = "WiFi disconnected; original data retained.";
    return false;
  }
  Response captured;
  std::string url = std::string(endpoint) + path;
  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.method = post ? HTTP_METHOD_POST : HTTP_METHOD_PUT;
  config.timeout_ms = 6000;
  config.disable_auto_redirect = true;
  config.event_handler = httpEvent;
  config.user_data = &captured;
  auto client = esp_http_client_init(&config);
  if (!client) {
    error = "HTTP client allocation failed.";
    return false;
  }
  esp_http_client_set_header(client, "Content-Type", "application/json");
  esp_http_client_set_header(client, "User-Agent", "BrewPi-WaterTest/1");
  esp_http_client_set_post_field(client, body.data(), body.size());
  esp_err_t result = esp_http_client_perform(client);
  int code = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (result != ESP_OK || (code != 200 && code != 201) || captured.tooLong) {
    error = "HTTP upload pending (status " + std::to_string(code) + ").";
    if (code >= 300 && code < 400)
      error = "Server redirected HTTP; configure the collection API to accept HTTP without redirect.";
    return false;
  }
  if (deserializeJson(response, captured.body) != DeserializationError::Ok) {
    error = "Invalid server acknowledgement.";
    return false;
  }
  return true;
}
} // namespace WaterTestTransport
