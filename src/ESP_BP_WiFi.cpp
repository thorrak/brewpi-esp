#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "ESP_BP_WiFi.h"

#include <esp_wifi.h>
#include <esp_netif.h>
#include <cstring>

#ifdef CONNECT_VIA_WIFI

#include <string>
#include "Brewpi.h"

#include <thorlog.h>
#include <mdns.h>
#include <esp_event.h>
#include <esp_system.h>
#include <esp_wifi_config.h>
#include <esp_log.h>
#include <Ticks.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <lwip/tcp.h>
#include <fcntl.h>
#include <errno.h>

#include "Version.h" 			// Used in mDNS announce string
#include "Display.h"
#include "EepromManager.h"
#include "rest/rest_send.h"
#include "http_server.h"

#ifdef BREWPI_CHILLSIM_TEST
#if __has_include("ChillsimTestCredentials.h")
#include "ChillsimTestCredentials.h"
#else
#include "ChillsimTestCredentials.example.h"
#endif
#endif


int telnet_server_fd = -1;
int telnet_client_fd = -1;

extern void handleReset();  // Terrible practice. In brewpi-esp8266.cpp.

// Track WiFi connection state to distinguish initial connection from reconnection.
static bool wifi_was_disconnected = false;


// RFC-1123 hostname label: 1-63 chars, alphanumerics and hyphens, no leading
// or trailing hyphen. This matches the rule the provisioning Web UI enforces.
bool isValidmDNSName(const char* mdns_name) {
    size_t len = strlen(mdns_name);
    if (len == 0 || len > 63)
        return false;
    if (mdns_name[0] == '-' || mdns_name[len - 1] == '-')
        return false;
    for (size_t i = 0; i < len; ++i) {
        if (!isalnum((unsigned char)mdns_name[i]) && mdns_name[i] != '-')
            return false;
    }
    return true;
}


// -----------------------------------------------------------------------
// WiFi Config event callbacks
//
// esp_wifi_config (0.2.0+) posts these on the default event loop under the
// WIFI_CFG_EVENT base, so they run on the system event task. Keep them light
// (CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE is raised in sdkconfig.defaults).
// -----------------------------------------------------------------------

// Event callback for WiFi connecting (attempting to connect to a network)
static void on_wifi_connecting(void *arg, esp_event_base_t base, int32_t event_id, void *data) {
    if (data == nullptr) {
        return;
    }
    const char *ssid = (const char *)data;
    Log.info("WiFi connecting to %s\r\n", ssid);

    // Don't clobber the AP screen with "connecting to..." during background reconnect attempts
    wifi_status_t status;
    if (wifi_cfg_get_status(&status) == ESP_OK && status.ap_active) {
        return;
    }

    display.printWiFiConnect();
}

// Event callback for WiFi connected
static void on_wifi_connected(void *arg, esp_event_base_t base, int32_t event_id, void *data) {
    if (data == nullptr) {
        Log.warning("WiFi connected event received with invalid payload\r\n");
        return;
    }
    const wifi_connected_t *info = (const wifi_connected_t *)data;
    Log.notice("WiFi connected to %s, channel %d, RSSI %d\r\n", info->ssid, info->channel, info->rssi);
}

// Event callback for WiFi got IP
static void on_wifi_got_ip(void *arg, esp_event_base_t base, int32_t event_id, void *data) {
    wifi_status_t status;
    if (wifi_cfg_get_status(&status) == ESP_OK) {
        Log.notice("WiFi got IP: %s\r\n", status.ip);

        if (wifi_was_disconnected) {
            Log.notice("Reconnected to WiFi after disconnect\r\n");
            mdns_reset();
            wifi_was_disconnected = false;
        }
    }
}

// Event callback for WiFi disconnected
static void on_wifi_disconnected(void *arg, esp_event_base_t base, int32_t event_id, void *data) {
    if (data == nullptr) {
        Log.warning("WiFi disconnected event received with invalid payload\r\n");
        return;
    }
    const wifi_disconnected_t *info = (const wifi_disconnected_t *)data;
    Log.warning("WiFi disconnected from %s, reason: %d. Auto-reconnect in progress.\r\n", info->ssid, info->reason);
    wifi_was_disconnected = true;
}

// Event callback for AP started
static void on_wifi_ap_started(void *arg, esp_event_base_t base, int32_t event_id, void *data) {
    wifi_ap_status_t ap_status;
    Log.info("WiFi AP started for configuration.\r\n");
    if (wifi_cfg_get_ap_status(&ap_status) == ESP_OK) {
        Log.info("AP started: SSID: %s, IP: %s\r\n", ap_status.ssid, ap_status.ip);
        display.printWiFiStartup();
        esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW_HT20);
    }
}

// Event callback for provisioning stopped — initialize the HTTP server routes
static void on_provisioning_stopped(void *arg, esp_event_base_t base, int32_t event_id, void *data) {
#ifdef ENABLE_HTTP_INTERFACE
    Log.info("WiFi provisioning stopped, initializing HTTP server routes.\r\n");
    http_server.registerRoutes();
#endif
}

// Event callback for variable changes (e.g., mdns_name changed via WiFi manager API)
static void on_var_changed(void *arg, esp_event_base_t base, int32_t event_id, void *data) {
    if (data == nullptr) {
        return;
    }
    const wifi_var_t *var = (const wifi_var_t *)data;

    if (strcmp(var->key, "mdns_name") == 0 && strlen(var->value) > 0) {
        std::string current_mdns = eepromManager.fetchmDNSName();
        if (isValidmDNSName(var->value) && strcmp(var->value, current_mdns.c_str()) != 0) {
            Log.notice("mDNS name changed via WiFi manager: %s\r\n", var->value);
            eepromManager.savemDNSName(var->value);
            mdns_reset();
        }
    }
}


// -----------------------------------------------------------------------
// mDNS management
// -----------------------------------------------------------------------

void mdns_reset() {
    std::string mdns_id;
    mdns_id = eepromManager.fetchmDNSName();

    mdns_free();

    if (mdns_init() == ESP_OK && mdns_hostname_set(mdns_id.c_str()) == ESP_OK) {
        mdns_txt_item_t txt[] = {
            {(char*)"board",    (char*)CONTROLLER_TYPE},
            {(char*)"branch",   (char*)"legacy"},
            {(char*)"version",  (char*)Config::Version::release},
            {(char*)"revision", (char*)FIRMWARE_REVISION},
        };
        mdns_service_add(NULL, "_brewpi", "_tcp", 23, txt, sizeof(txt) / sizeof(txt[0]));

        Log.notice("mDNS responder restarted, hostname: %s.local.\r\n", mdns_id.c_str());
    } else {
        Log.error("Error resetting MDNS responder.\r\n");
    }

    // Sync mDNS name to wifi_cfg's custom variables for persistence
    wifi_cfg_set_var("mdns_name", mdns_id.c_str());
}


// -----------------------------------------------------------------------
// Telnet server
// -----------------------------------------------------------------------

void initWifiServer() {
    telnet_server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (telnet_server_fd < 0) return;

    int opt = 1;
    setsockopt(telnet_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    // TCP_NODELAY equivalent of server.setNoDelay(true)
    setsockopt(telnet_server_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(23);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    bind(telnet_server_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(telnet_server_fd, 1);

    // Make server socket non-blocking
    int flags = fcntl(telnet_server_fd, F_GETFL, 0);
    fcntl(telnet_server_fd, F_SETFL, flags | O_NONBLOCK);

    mdns_reset();
}


// -----------------------------------------------------------------------
// initialize_wifi() - uses esp_wifi_config
// -----------------------------------------------------------------------

void initialize_wifi() {
    display.clear();
    display.printWiFiConnect();

    // Start HTTP server early so we can share it with wifi_cfg
    // This prevents port conflicts when wifi_cfg's HTTP server is torn down
#ifdef ENABLE_HTTP_INTERFACE
    http_server.startServer();
#endif

    // Subscribe to WiFi events. esp_wifi_config (0.2.0+) posts these on the
    // default event loop (created in app_main) under the WIFI_CFG_EVENT base.
    // Registering before wifi_cfg_init() ensures we catch startup events.
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_CFG_EVENT, WIFI_CFG_EVENT_CONNECTING, on_wifi_connecting, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_CFG_EVENT, WIFI_CFG_EVENT_CONNECTED, on_wifi_connected, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_CFG_EVENT, WIFI_CFG_EVENT_GOT_IP, on_wifi_got_ip, NULL));
    // ESP_ERROR_CHECK(esp_event_handler_register(WIFI_CFG_EVENT, WIFI_CFG_EVENT_DISCONNECTED, on_wifi_disconnected, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_CFG_EVENT, WIFI_CFG_EVENT_AP_START, on_wifi_ap_started, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_CFG_EVENT, WIFI_CFG_EVENT_VAR_CHANGED, on_var_changed, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_CFG_EVENT, WIFI_CFG_EVENT_PROVISIONING_STOPPED, on_provisioning_stopped, NULL));

    // Default variables for WiFi Config - mdns_name is used to set the mDNS hostname
    // This provides a default value; if NVS has a stored value, that takes precedence
    static wifi_var_t default_vars[] = {
#ifdef BREWPI_CHILLSIM_TEST
        {"mdns_name", "chillsim"},
#else
        {"mdns_name", "brewpi"},
#endif
    };

    // Configure WiFi Config. Start from the library defaults (required as of
    // esp_wifi_config 0.2.0: wifi_cfg_init() no longer patches unset fields and
    // rejects a zero retry backoff) and override only what BrewPi needs.
    // Struct-value style rather than a designated initialiser: C++ requires
    // designators in declaration order, which WIFI_CFG_DEFAULTS + overrides
    // cannot satisfy.
    wifi_cfg_config_t wifi_config = WIFI_CFG_DEFAULT_CONFIG();

#ifdef BREWPI_CHILLSIM_TEST
    // Reuse the trusted experiment LAN without logging credentials. Existing
    // esp_wifi_config NVS networks still take precedence over these defaults.
    static wifi_network_t test_network = {};
    strlcpy(test_network.ssid, CHILLSIM_WIFI_SSID, sizeof(test_network.ssid));
    strlcpy(test_network.password, CHILLSIM_WIFI_PASSWORD, sizeof(test_network.password));
    test_network.priority = 1;
    if (test_network.ssid[0]) {
        wifi_config.default_networks = &test_network;
        wifi_config.default_network_count = 1;
    }
#endif

    wifi_config.default_vars = default_vars;
    wifi_config.default_var_count = sizeof(default_vars) / sizeof(default_vars[0]);

    // Retry policy: 3 attempts per network, 5 s backoff base, 60 s cap,
    // auto-reconnect on. These match the library defaults; stated explicitly.
    wifi_config.max_retry_per_network = 3;
    wifi_config.retry_interval_ms = 5000;
    wifi_config.retry_max_interval_ms = 60000;
    wifi_config.auto_reconnect = true;

    // If we fail to connect to any known network, start provisioning (SoftAP + captive portal + BLE)
    wifi_config.provisioning_mode = WIFI_PROV_ON_FAILURE;
    // Stop the AP and captive portal and deregister httpd endpoints once we successfully connect
    wifi_config.stop_provisioning_on_connect = true;
    wifi_config.provisioning_teardown_delay_ms = 5000;
    // Unregister captive portal/webui routes after provisioning so BrewPi can register its own
    wifi_config.http_post_prov_mode = WIFI_HTTP_API_ONLY;

    // SoftAP for the captive portal. Only SSID, password and channel differ from
    // the library defaults (192.168.4.1/24, DHCP .2-.20, 4 clients, not hidden).
    strlcpy(wifi_config.default_ap.ssid, WIFI_SETUP_AP_NAME, sizeof(wifi_config.default_ap.ssid));
    strlcpy(wifi_config.default_ap.password, WIFI_SETUP_AP_PASS, sizeof(wifi_config.default_ap.password));
    wifi_config.default_ap.channel = 1;
    wifi_config.always_use_ap_defaults = true;  // Ignore any saved AP config - ensure captive portal is always available and consistent
    wifi_config.enable_ap = true;

    // Share our HTTP server with wifi_cfg. API base path stays at the default
    // /api/wifi; Basic Auth stays off (the default).
#ifdef ENABLE_HTTP_INTERFACE
    wifi_config.http.httpd = http_server.getHandle();
#else
    wifi_config.http.httpd = NULL;
#endif

    // ESP-IDF Network Provisioning over BLE (replaces the pre-0.1.0 custom
    // GATT service). Ignored at build time on targets without BLE.
    wifi_config.prov_ble.device_name = "BrewPiESP-{id}";
    wifi_config.prov_ble.security = WIFI_CFG_PROV_SECURITY_1;
    wifi_config.prov_ble.pop = "brewpi";
    // KEEP_ALL keeps the BT controller + BLE memory alive after the
    // provisioning manager tears down, so bt_scanner can attach via
    // NimBLEDevice::init() without re-initialising the controller.
    wifi_config.prov_ble.memory_policy = WIFI_CFG_PROV_MEM_KEEP_ALL;
    // Clear stored creds after max_failed_attempts so a wrong-password loop
    // accepts a fresh attempt without rebooting.
    wifi_config.prov_ble.reset_on_failure = true;
    wifi_config.prov_ble.max_failed_attempts = 3;

    // Initialize WiFi Config
    esp_err_t err = wifi_cfg_init(&wifi_config);
    if (err != ESP_OK) {
        Log.error("Failed to initialize WiFi Config: %d\r\n", err);
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    // Push the config-file mDNS name into wifi_cfg's variable store now, before
    // provisioning can start, so the captive portal wizard shows the current
    // name. The var store is wiped by wifi_cfg_factory_reset() and would
    // otherwise fall back to the "brewpi" default until we connect.
    wifi_cfg_set_var("mdns_name", eepromManager.fetchmDNSName().c_str());

    // Wait for connection (5 minute timeout)
    err = wifi_cfg_wait_connected(5 * 60 * 1000);
    if (err != ESP_OK) {
        Log.error("WiFi connection timeout. Restarting device.\r\n");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    }

    // wifi_cfg handles its own provisioning teardown after the configured delay
    // (stop_provisioning_on_connect + provisioning_teardown_delay_ms)

    // Sync mDNS name FROM config TO wifi_cfg (config file is the source of truth).
    // The on_var_changed callback handles the reverse direction for real-time changes.
    wifi_cfg_set_var("mdns_name", eepromManager.fetchmDNSName().c_str());

    // Set up telnet server and mDNS
    initWifiServer();
}

void wifi_connection_info(JsonDocument& doc) {
  doc["ssid"] = bp_wifi_get_ssid();
  doc["signalStrength"] = bp_wifi_get_rssi();
}

void display_connect_info_and_create_callback() {
    display.printWiFi();  // Print the WiFi info (mDNS name & IP address)
    vTaskDelay(pdMS_TO_TICKS(5000));
}


void wifi_connect_clients() {
    static unsigned long last_connection_check = 0;

    vTaskDelay(pdMS_TO_TICKS(1));
    if(bp_wifi_is_connected()) {
        // We only accept clients if we do not have a REST target defined
        if(rest_handler.configured_for_fermentrack_rest()) {
            // If we have a telnet client connected, close it
            if (telnet_client_fd >= 0) {
                close(telnet_client_fd);
                telnet_client_fd = -1;
            }
        } else if (telnet_server_fd >= 0) {
            // Try to accept a new connection (non-blocking)
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int new_fd = accept(telnet_server_fd, (struct sockaddr*)&client_addr, &addr_len);
            if (new_fd >= 0) {
                // Close existing client if any
                if (telnet_client_fd >= 0) {
                    close(telnet_client_fd);
                }
                telnet_client_fd = new_fd;
                // Set TCP_NODELAY on client socket
                int opt = 1;
                setsockopt(telnet_client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
                // Make client socket non-blocking
                int flags = fcntl(telnet_client_fd, F_GETFL, 0);
                fcntl(telnet_client_fd, F_SETFL, flags | O_NONBLOCK);
            }
        }
    } else {
        // WiFi is disconnected -- close any telnet client
        if (telnet_client_fd >= 0) {
            close(telnet_client_fd);
            telnet_client_fd = -1;
        }
    }
    vTaskDelay(pdMS_TO_TICKS(1));

    // Additionally, every 3 minutes either attempt to reconnect WiFi, or rebroadcast mdns info
    if(ticks.millis() - last_connection_check >= (3 * 60 * 1000)) {
        last_connection_check = ticks.millis();
        if(!bp_wifi_is_connected()) {
            // If we are disconnected, reconnect.
            // We'll have to wait an additional 3 minutes for mdns to come back up
            vTaskDelay(pdMS_TO_TICKS(150));
            bp_wifi_reconnect();
        } else {
            mdns_reset();  // TODO - Add this to the WiFi.reconnect() process
        }
    }
    vTaskDelay(pdMS_TO_TICKS(1));
}



#else
/*********************** Code for when we don't have WiFi enabled is below  *********************/

void initialize_wifi() {
    // Apparently, the WiFi radio is managed by the bootloader, so not including the libraries isn't the same as
    // disabling WiFi. We'll explicitly disable it if we're running in "serial" mode
    bp_wifi_off();
}

void display_connect_info_and_create_callback() {
    // For now, this is noop when WiFi support is disabled
}
void wifi_connect_clients() {
    // For now, this is noop when WiFi support is disabled
}
#endif


// -----------------------------------------------------------------------
// ESP-IDF WiFi utility functions (always compiled)
// -----------------------------------------------------------------------

bool bp_wifi_is_connected() {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return false;

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) return false;

    return ip_info.ip.addr != 0;
}

const char* bp_wifi_get_ip_str() {
    static char ip_str[16]; // "xxx.xxx.xxx.xxx\0"

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) { strlcpy(ip_str, "0.0.0.0", sizeof(ip_str)); return ip_str; }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) { strlcpy(ip_str, "0.0.0.0", sizeof(ip_str)); return ip_str; }

    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
    return ip_str;
}

uint32_t bp_wifi_get_ip_addr() {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return 0;

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) return 0;

    return ip_info.ip.addr;
}

void bp_wifi_disconnect(bool erase_credentials) {
    wifi_cfg_disconnect();

    if (erase_credentials) {
        wifi_cfg_factory_reset();
    }
}

const char* bp_wifi_get_ssid() {
    static char ssid_buf[33]; // Max SSID length is 32 + null

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        strlcpy(ssid_buf, (const char *)ap_info.ssid, sizeof(ssid_buf));
    } else {
        ssid_buf[0] = '\0';
    }
    return ssid_buf;
}

int8_t bp_wifi_get_rssi() {
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

const char* bp_wifi_get_hostname() {
    const char *hostname = nullptr;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_get_hostname(netif, &hostname);
    }
    return hostname ? hostname : "brewpi";
}

void bp_wifi_reconnect() {
    wifi_cfg_connect(NULL);
}

void bp_wifi_off() {
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
}
