#pragma once

#ifdef ENABLE_HTTP_INTERFACE
#include <ArduinoJson.h>
#include <esp_http_server.h>

namespace HttpJsonResponse {
esp_err_t send(httpd_req_t* request, JsonDocument& document);
}
#endif
