#pragma once

#ifdef ENABLE_HTTP_INTERFACE
#include <esp_http_server.h>

namespace CrashDump {
esp_err_t registerRoutes(httpd_handle_t server);
}
#endif
