#ifdef ENABLE_HTTP_INTERFACE

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <thorlog.h>
#include <thorlog_espidf.h>
#include <ArduinoJson.h>
#include <esp_http_server.h>
#include <cmath>

#include "ESPEepromAccess.h"
#include <esp_system.h>
#include <esp_heap_caps.h>

#include "uptime.h"
#include "resetreasons.h"
#include "http_server.h"
#include "TempControl.h"
#include "JsonMessages.h"
#include "DeviceManager.h"
#include "JsonKeys.h"
#include "rest/rest_send.h"
#include "EepromManager.h"
#include "SettingsManager.h"
#include "ESP_BP_WiFi.h"
#include "GlycolLog.h"


httpServer http_server;


// ============================================================================
// JSON helpers
// ============================================================================

esp_err_t httpServer::sendJsonDoc(httpd_req_t *req, JsonDocument &doc) {
    std::string output;
    serializeJson(doc, output);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, output.c_str(), output.length());
}

esp_err_t httpServer::parseJsonBody(httpd_req_t *req, JsonDocument &doc) {
    int remaining = req->content_len;
    if (remaining <= 0 || remaining > 4096) {
        return ESP_FAIL;
    }

    char *buf = (char *)malloc(remaining + 1);
    if (!buf) return ESP_FAIL;

    int received = 0;
    while (remaining > 0) {
        int ret = httpd_req_recv(req, buf + received, remaining);
        if (ret <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        received += ret;
        remaining -= ret;
    }
    buf[received] = '\0';

    DeserializationError err = deserializeJson(doc, buf, received);
    free(buf);
    return (err == DeserializationError::Ok) ? ESP_OK : ESP_FAIL;
}


// ============================================================================
// GET JSON handler template
// ============================================================================

template<void (*Handler)(JsonDocument&)>
static esp_err_t get_json_handler(httpd_req_t *req) {
    JsonDocument doc;
    Handler(doc);
    return httpServer::sendJsonDoc(req, doc);
}


// ============================================================================
// PUT JSON handler template
// ============================================================================

template<bool (*Handler)(const JsonDocument&, bool)>
static esp_err_t put_json_handler(httpd_req_t *req) {
    JsonDocument doc;
    if (httpServer::parseJsonBody(req, doc) != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\"}", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;  // We handled it, just sent error
    }

    if (Handler(doc, true)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"ok\"}", HTTPD_RESP_USE_STRLEN);
    } else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "{\"status\":\"error\"}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}


// ============================================================================
// Settings Page Handlers (process incoming JSON PUT data)
// ============================================================================

bool processUpstreamConfigUpdateJson(const JsonDocument& json, bool triggerUpstreamUpdate) {
    uint8_t failCount = 0;
    bool saveSettings = false;

    // Snapshot pre-update credentials so we can drop them if the host changes but the
    // form re-submitted the old apiKey/username (credentials are tied to a specific host).
    char prev_host[sizeof(upstreamSettings.upstreamHost)];
    char prev_apiKey[sizeof(upstreamSettings.apiKey)];
    char prev_username[sizeof(upstreamSettings.username)];
    strlcpy(prev_host, upstreamSettings.upstreamHost, sizeof(prev_host));
    strlcpy(prev_apiKey, upstreamSettings.apiKey, sizeof(prev_apiKey));
    strlcpy(prev_username, upstreamSettings.username, sizeof(prev_username));

    // Upstream Host
    if(json[UpstreamSettingsKeys::upstreamHost].is<const char*>()) {
        if (strlen(json[UpstreamSettingsKeys::upstreamHost]) <= 0) {
            upstreamSettings.upstreamHost[0] = '\0';
            upstreamSettings.deviceID[0] = '\0';
            Log.notice("Settings update, [upstreamHost]: unset.\r\n");
        } else if (strlen(json[UpstreamSettingsKeys::upstreamHost]) >= 128 ) {
            Log.warning("Settings update error, [upstreamHost]:(%s) not valid.\r\n", json[UpstreamSettingsKeys::upstreamHost].as<const char*>());
            failCount++;
        } else {
            if(strcmp(json[UpstreamSettingsKeys::upstreamHost], upstreamSettings.upstreamHost) != 0) {
                strlcpy(upstreamSettings.upstreamHost, json[UpstreamSettingsKeys::upstreamHost].as<const char*>(), 128);
                upstreamSettings.deviceID[0] = '\0';
                Log.notice("Settings update, [upstreamHost]:(%s) applied.\r\n", json[UpstreamSettingsKeys::upstreamHost].as<const char*>());
                saveSettings = true;
            }
        }
    }

    // Upstream Port
    if(json[UpstreamSettingsKeys::upstreamPort].is<uint16_t>()) {
        if((json[UpstreamSettingsKeys::upstreamPort] <= 0) || (json[UpstreamSettingsKeys::upstreamPort] > 65535)) {
            Log.warning("Invalid [upstreamPort]:(%u) received.\r\n", json[UpstreamSettingsKeys::upstreamPort].as<uint16_t>());
            failCount++;
        } else {
            upstreamSettings.upstreamPort = json[UpstreamSettingsKeys::upstreamPort];
            upstreamSettings.deviceID[0] = '\0';
            Log.warning("Settings update, [upstreamPort]:(%d) applied.\r\n", json[UpstreamSettingsKeys::upstreamPort].as<uint16_t>());
            saveSettings = true;
        }
    } else {
        Log.warning("Invalid [upstreamPort]:(%s) received (wrong type).\r\n", json[UpstreamSettingsKeys::upstreamPort].as<const char*>());
        failCount++;
    }

    // Upstream Username
    if(json[UpstreamSettingsKeys::username].is<const char*>()) {
        if (strlen(json[UpstreamSettingsKeys::username]) <= 0) {
            upstreamSettings.username[0] = '\0';
            upstreamSettings.deviceID[0] = '\0';
            Log.notice("Settings update, [username]: unset.\r\n");
        } else if (strlen(json[UpstreamSettingsKeys::username]) >= 128 ) {
            Log.warning("Settings update error, [username]:(%s) not valid.\r\n", json[UpstreamSettingsKeys::username].as<const char*>());
            failCount++;
        } else {
            if(strcmp(json[UpstreamSettingsKeys::username], upstreamSettings.username) != 0) {
                strlcpy(upstreamSettings.username, json[UpstreamSettingsKeys::username].as<const char*>(), 128);
                upstreamSettings.deviceID[0] = '\0';
                Log.notice("Settings update, [username]:(%s) applied.\r\n", json[UpstreamSettingsKeys::username].as<const char*>());
                saveSettings = true;
            }
        }
    }

    // Upstream API Key
    if(json[UpstreamSettingsKeys::apiKey].is<const char*>()) {
        if (strlen(json[UpstreamSettingsKeys::apiKey]) <= 0) {
            upstreamSettings.apiKey[0] = '\0';
            upstreamSettings.deviceID[0] = '\0';
            Log.notice("Settings update, [apiKey]: unset.\r\n");
        } else if (strlen(json[UpstreamSettingsKeys::apiKey]) >= 40 ) {
            Log.warning("Settings update error, [apiKey]:(%s) not valid.\r\n", json[UpstreamSettingsKeys::apiKey].as<const char*>());
            failCount++;
        } else {
            if(strcmp(json[UpstreamSettingsKeys::apiKey], upstreamSettings.apiKey) != 0) {
                strlcpy(upstreamSettings.apiKey, json[UpstreamSettingsKeys::apiKey].as<const char*>(), sizeof(upstreamSettings.apiKey));
                upstreamSettings.deviceID[0] = '\0';
                Log.notice("Settings update, [apiKey]:(%s) applied.\r\n", json[UpstreamSettingsKeys::apiKey].as<const char*>());
                saveSettings = true;
            }
        }
    }

    // Device Name (optional, only used during registration)
    if(json[UpstreamSettingsKeys::deviceName].is<const char*>()) {
        if (strlen(json[UpstreamSettingsKeys::deviceName]) >= sizeof(rest_handler.pendingDeviceName)) {
            Log.warning("Settings update error, [name]:(%s) too long.\r\n", json[UpstreamSettingsKeys::deviceName].as<const char*>());
            failCount++;
        } else {
            strlcpy(rest_handler.pendingDeviceName, json[UpstreamSettingsKeys::deviceName].as<const char*>(), sizeof(rest_handler.pendingDeviceName));
            Log.notice("Settings update, [name]:(%s) applied.\r\n", json[UpstreamSettingsKeys::deviceName].as<const char*>());
        }
    } else {
        rest_handler.pendingDeviceName[0] = '\0';
    }

    // If the host changed, drop any apiKey/username that wasn't explicitly updated by
    // this request — those credentials belong to the previous Fermentrack instance and
    // would otherwise produce misleading registration errors against the new host.
    if (failCount == 0 && strcmp(prev_host, upstreamSettings.upstreamHost) != 0) {
        if (strlen(upstreamSettings.apiKey) > 0 &&
            strcmp(upstreamSettings.apiKey, prev_apiKey) == 0) {
            upstreamSettings.apiKey[0] = '\0';
            upstreamSettings.deviceID[0] = '\0';
            Log.notice("Host changed, clearing stale apiKey.\r\n");
            saveSettings = true;
        }
        if (strlen(upstreamSettings.username) > 0 &&
            strcmp(upstreamSettings.username, prev_username) == 0) {
            upstreamSettings.username[0] = '\0';
            Log.notice("Host changed, clearing stale username.\r\n");
            saveSettings = true;
        }
    }

    // Save
    if (failCount) {
        Log.error("Error: Invalid upstream configuration.\r\n");
    } else {
        if(saveSettings == true) {
            upstreamSettings.storeToFilesystem();
        }
        upstreamSettings.upstreamRegistrationError = UpstreamSettings::upstreamRegErrorT::NOT_ATTEMPTED_REGISTRATION;
        rest_handler.force_register_attempt = true;
    }
    return failCount == 0;
}


bool processDeviceUpdateJson(const JsonDocument& json, bool triggerUpstreamUpdate) {
    DeviceDefinition dev;
    if(!json[DeviceDefinitionKeys::chamber].is<uint8_t>() || !json[DeviceDefinitionKeys::beer].is<uint8_t>() ||
        !json[DeviceDefinitionKeys::function].is<uint8_t>() || !json[DeviceDefinitionKeys::hardware].is<uint8_t>()
    )
    {
        Log.warning("Invalid device definition received - missing required keys (c/f/h/b).\r\n");
        return 1;
    }

    switch(json[DeviceDefinitionKeys::hardware].as<uint8_t>()) {
        case DEVICE_HARDWARE_PIN:
            if(!json[DeviceDefinitionKeys::pin].is<int>() || !(json[DeviceDefinitionKeys::invert].is<bool>() || json[DeviceDefinitionKeys::invert].is<const char *>() || json[DeviceDefinitionKeys::invert].is<uint8_t>())) {
                Log.warning("Invalid device definition received - missing required keys (p/x).\r\n");
                return 1;
            }
            break;
        case DEVICE_HARDWARE_ONEWIRE_TEMP:
        case DEVICE_HARDWARE_BLUETOOTH_INKBIRD:
        case DEVICE_HARDWARE_BLUETOOTH_TILT:
            if(!json[DeviceDefinitionKeys::address].is<const char*>()) {
                Log.warning("Invalid device definition received - missing required keys (a).\r\n");
                return 1;
            }
            break;
        case DEVICE_HARDWARE_TPLINK_SWITCH:
            if(!json[DeviceDefinitionKeys::address].is<const char*>() || !json[DeviceDefinitionKeys::child_id].is<const char*>()) {
                Log.warning("Invalid device definition received - missing required keys (a).\r\n");
                return 1;
            }
            break;
        default:
            break;
    }
    http_server.dev = DeviceManager::readJsonIntoDeviceDef(json);
    http_server.device_definition_update_requested = true;
    return true;
}


void httpServer::processQueuedDeviceDefinition() {
    if(device_definition_update_requested) {
        deviceManager.updateDeviceDefinition(dev);
        device_definition_update_requested = false;
    }
}


void httpServer::processQueuedActions() {
    if(config_reset_requested) {
        Log.notice("Processing config reset request\r\n");
        vTaskDelay(pdMS_TO_TICKS(500));
        if(eepromManager.initializeEeprom()) {
            logInfo(INFO_EEPROM_INITIALIZED);
            settingsManager.loadSettings();
        }
        config_reset_requested = false;
    }

    if(wifi_reset_requested) {
        Log.notice("Processing WiFi reset request\r\n");
        vTaskDelay(pdMS_TO_TICKS(500));
        upstreamSettings.setDefaults();
        upstreamSettings.storeToFilesystem();
        bp_wifi_disconnect(true);  // Disconnect and erase stored WiFi credentials
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }

    if(restart_requested) {
        Log.notice("Processing restart request\r\n");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
}


bool processUpdateModeJson(const JsonDocument& json, bool triggerUpstreamUpdate) {
    uint8_t failCount = 0;
    bool saveSettings = false;

    // Temperature Control Mode
    if(json[ModeUpdateKeys::mode].is<const char *>()) {
        if (strlen(json[ModeUpdateKeys::mode]) == 1) {
            char new_mode = json[ModeUpdateKeys::mode].as<const char *>()[0];
            if (new_mode == Modes::fridgeConstant || new_mode == Modes::beerConstant || new_mode == Modes::beerProfile ||
                new_mode == Modes::off || new_mode == Modes::test) {
                if(new_mode != tempControl.getMode()) {
                    tempControl.setMode(new_mode);
                    Log.notice("Settings update, [newMode]:(%c) applied.\r\n", new_mode);
                    saveSettings = true;
                } else {
                    Log.notice("Settings update, [newMode]:(%c) NOT applied - no change.\r\n", new_mode);
                }
            } else {
                Log.warning("Settings update error, [newMode]:(%c) not valid.\r\n", new_mode);
                failCount++;
            }
        } else {
            Log.warning("Settings update error, [newMode]:(%s) not a valid type.\r\n", json[ModeUpdateKeys::mode].as<const char*>());
            failCount++;
        }
    }

    // Set Point
    if(json[ModeUpdateKeys::setpoint].is<double>()) {
        if(tempControl.getMode() != Modes::fridgeConstant && tempControl.getMode() != Modes::beerConstant && tempControl.getMode() != Modes::beerProfile) {
            Log.info("Settings update error, [setpoint]:(%s) current mode (%c) does not take a setpoint.\r\n", json[ModeUpdateKeys::setpoint].as<const char*>(), tempControl.getMode());
        } else {
            char modeString[7];
            snprintf(modeString, 7, "%.1f", json[ModeUpdateKeys::setpoint].as<double>());
            temperature newTemp = stringToTemp(modeString);

            if(tempControl.getMode() == Modes::fridgeConstant) {
                tempControl.setFridgeTemp(newTemp);
                saveSettings = true;
            } else if(tempControl.getMode() == Modes::beerConstant || tempControl.getMode() == Modes::beerProfile) {
                tempControl.setBeerTemp(newTemp);
                saveSettings = true;
            } else {
                Log.error("Settings update error, [setpoint]:(%s) current mode (%c) does not take a setpoint (should never be reached).\r\n", modeString, tempControl.getMode());
            }
        }
    }

    if (failCount) {
        Log.error("Error: Invalid upstream configuration.\r\n");
    } else {
        if(saveSettings == true) {
            // TODO - Force upstream cascade/send
        }
    }
    return failCount == 0;
}



bool processExtendedSettingsJson(const JsonDocument& json, bool triggerUpstreamUpdate) {
    // Validate the whole partial update before setters can change mode, relay
    // timing, displays, or flash. Older clients need not send the new selector.
    if (!ExtendedSettings::validateSettingsJson(json)) {
        Log.error("Error: Invalid extended settings configuration.\r\n");
        return false;
    }
    const JsonVariantConst choice = json[MinTimesKeys::SETTINGS_CHOICE];
    if (!choice.isUnbound() &&
        (!choice.is<uint8_t>() || choice.as<uint8_t>() > MIN_TIMES_CUSTOM)) return false;
    const char *timeKeys[] = {
        MinTimesKeys::MIN_COOL_OFF_TIME, MinTimesKeys::MIN_HEAT_OFF_TIME,
        MinTimesKeys::MIN_COOL_ON_TIME, MinTimesKeys::MIN_HEAT_ON_TIME,
        MinTimesKeys::MIN_COOL_OFF_TIME_FRIDGE_CONSTANT, MinTimesKeys::MIN_SWITCH_TIME,
        MinTimesKeys::COOL_PEAK_DETECT_TIME, MinTimesKeys::HEAT_PEAK_DETECT_TIME
    };
    for (const char *key : timeKeys) {
        if (!json[key].isUnbound() && !json[key].is<uint16_t>()) return false;
    }
    bool saveSettings = false;
    bool saveMinTimes = false;

    // The runtime applies this requested selection at a safe pump-OFF boundary.
    if (json[ExtendedSettingsKeys::glycolCoolingAlgorithm].is<const char *>()) {
        GlycolCooling::Algorithm requested = extendedSettings.glycolCoolingAlgorithm;
        GlycolCooling::parseAlgorithm(json[ExtendedSettingsKeys::glycolCoolingAlgorithm].as<const char *>(), requested);
        if (extendedSettings.glycolCoolingAlgorithm != requested) {
            extendedSettings.setGlycolCoolingAlgorithm(requested);
            saveSettings = true;
        }
    }

    // Glycol Mode
    if(json[ExtendedSettingsKeys::glycol].is<bool>()) {
        if(extendedSettings.glycol != json[ExtendedSettingsKeys::glycol].as<bool>()) {
            extendedSettings.setGlycol(json[ExtendedSettingsKeys::glycol].as<bool>());
            saveSettings = true;
        }
    }

    // Large TFT flag
    if(json[ExtendedSettingsKeys::largeTFT].is<bool>()) {
        if(extendedSettings.largeTFT != json[ExtendedSettingsKeys::largeTFT].as<bool>()) {
            extendedSettings.setLargeTFT(json[ExtendedSettingsKeys::largeTFT].as<bool>());
            saveSettings = true;
        }
    }

    // Invert TFT Flag
    if(json[ExtendedSettingsKeys::invertTFT].is<bool>()) {
        if(extendedSettings.invertTFT != json[ExtendedSettingsKeys::invertTFT].as<bool>()) {
            extendedSettings.setInvertTFT(json[ExtendedSettingsKeys::invertTFT].as<bool>());
            saveSettings = true;
        }
    }

    // Reset Screen on Pin Toggle Flag
    if(json[ExtendedSettingsKeys::resetScreenOnPin].is<bool>()) {
        if(extendedSettings.resetScreenOnPin != json[ExtendedSettingsKeys::resetScreenOnPin].as<bool>()) {
            extendedSettings.setResetScreenOnPin(json[ExtendedSettingsKeys::resetScreenOnPin].as<bool>());
            saveSettings = true;
        }
    }


#ifdef HAS_BLUETOOTH
    // Tilt Gravity Sensor
    if(json[ExtendedSettingsKeys::tiltGravSensor].is<std::string>()) {
        if(extendedSettings.tiltGravSensor != NimBLEAddress(json[ExtendedSettingsKeys::tiltGravSensor].as<std::string>(), 1)) {
            extendedSettings.setTiltGravSensor(NimBLEAddress(json[ExtendedSettingsKeys::tiltGravSensor].as<std::string>(), 1));
            saveSettings = true;
        }
    }
#endif

    // SETTINGS_CHOICE
    if(json[MinTimesKeys::SETTINGS_CHOICE].is<uint8_t>()) {
        if(minTimes.settings_choice != json[MinTimesKeys::SETTINGS_CHOICE].as<uint8_t>() && json[MinTimesKeys::SETTINGS_CHOICE].as<uint8_t>() <= MIN_TIMES_CUSTOM) {
            minTimes.settings_choice = (MinTimesSettingsChoice) json[MinTimesKeys::SETTINGS_CHOICE].as<uint8_t>();
            saveMinTimes = true;
        }
    }


    if(minTimes.settings_choice == MIN_TIMES_CUSTOM) {
        if(json[MinTimesKeys::MIN_COOL_OFF_TIME].is<uint16_t>()) {
            if(minTimes.MIN_COOL_OFF_TIME != json[MinTimesKeys::MIN_COOL_OFF_TIME].as<uint16_t>()) {
                minTimes.MIN_COOL_OFF_TIME = json[MinTimesKeys::MIN_COOL_OFF_TIME].as<uint16_t>();
                saveMinTimes = true;
            }
        }

        if(json[MinTimesKeys::MIN_HEAT_OFF_TIME].is<uint16_t>()) {
            if(minTimes.MIN_HEAT_OFF_TIME != json[MinTimesKeys::MIN_HEAT_OFF_TIME].as<uint16_t>()) {
                minTimes.MIN_HEAT_OFF_TIME = json[MinTimesKeys::MIN_HEAT_OFF_TIME].as<uint16_t>();
                saveMinTimes = true;
            }
        }

        if(json[MinTimesKeys::MIN_COOL_ON_TIME].is<uint16_t>()) {
            if(minTimes.MIN_COOL_ON_TIME != json[MinTimesKeys::MIN_COOL_ON_TIME].as<uint16_t>()) {
                minTimes.MIN_COOL_ON_TIME = json[MinTimesKeys::MIN_COOL_ON_TIME].as<uint16_t>();
                saveMinTimes = true;
            }
        }

        if(json[MinTimesKeys::MIN_HEAT_ON_TIME].is<uint16_t>()) {
            if(minTimes.MIN_HEAT_ON_TIME != json[MinTimesKeys::MIN_HEAT_ON_TIME].as<uint16_t>()) {
                minTimes.MIN_HEAT_ON_TIME = json[MinTimesKeys::MIN_HEAT_ON_TIME].as<uint16_t>();
                saveMinTimes = true;
            }
        }

        if(json[MinTimesKeys::MIN_COOL_OFF_TIME_FRIDGE_CONSTANT].is<uint16_t>()) {
            if(minTimes.MIN_COOL_OFF_TIME_FRIDGE_CONSTANT != json[MinTimesKeys::MIN_COOL_OFF_TIME_FRIDGE_CONSTANT].as<uint16_t>()) {
                minTimes.MIN_COOL_OFF_TIME_FRIDGE_CONSTANT = json[MinTimesKeys::MIN_COOL_OFF_TIME_FRIDGE_CONSTANT].as<uint16_t>();
                saveMinTimes = true;
            }
        }

        if(json[MinTimesKeys::MIN_SWITCH_TIME].is<uint16_t>()) {
            if(minTimes.MIN_SWITCH_TIME != json[MinTimesKeys::MIN_SWITCH_TIME].as<uint16_t>()) {
                minTimes.MIN_SWITCH_TIME = json[MinTimesKeys::MIN_SWITCH_TIME].as<uint16_t>();
                saveMinTimes = true;
            }
        }

        if(json[MinTimesKeys::COOL_PEAK_DETECT_TIME].is<uint16_t>()) {
            if(minTimes.COOL_PEAK_DETECT_TIME != json[MinTimesKeys::COOL_PEAK_DETECT_TIME].as<uint16_t>()) {
                minTimes.COOL_PEAK_DETECT_TIME = json[MinTimesKeys::COOL_PEAK_DETECT_TIME].as<uint16_t>();
                saveMinTimes = true;
            }
        }

        if(json[MinTimesKeys::HEAT_PEAK_DETECT_TIME].is<uint16_t>()) {
            if(minTimes.HEAT_PEAK_DETECT_TIME != json[MinTimesKeys::HEAT_PEAK_DETECT_TIME].as<uint16_t>()) {
                minTimes.HEAT_PEAK_DETECT_TIME = json[MinTimesKeys::HEAT_PEAK_DETECT_TIME].as<uint16_t>();
                saveMinTimes = true;
            }
        }
    }

    // Save
    if(saveSettings == true) {
        extendedSettings.storeToFilesystem();
    }
    if(saveMinTimes == true) {
        minTimes.setDefaults();
        minTimes.storeToFilesystem();
    }
    return true;
}


bool processControlConstantsJson(const JsonDocument& json, bool triggerUpstreamUpdate) {
    uint8_t failCount = 0;
    bool saveSettings = false;

    // Validate all supplied heating constants before changing live settings.
    // Keep the signed derivative gain (the default KdHeat is negative), but
    // reject values that cannot fit the controller's fixed-point fields.
    struct HeatingConstantUpdate {
        const char* key;
        temperature ControlConstants::* field;
        bool temperatureDifference;
        bool supplied = false;
        temperature value = 0;
    };
    HeatingConstantUpdate heatingUpdates[] = {
        {ControlConstantsKeys::kpHeat, &ControlConstants::Kp_heat, false},
        {ControlConstantsKeys::kiHeat, &ControlConstants::Ki_heat, false},
        {ControlConstantsKeys::kdHeat, &ControlConstants::Kd_heat, false},
        {ControlConstantsKeys::pidMaxHeat, &ControlConstants::pidMax_heat, true},
    };
    char requestedTempFormat = tempControl.cc.tempFormat;
    if (json["tempFormat"].is<const char*>()) {
        const char* format = json["tempFormat"].as<const char*>();
        if (strcmp(format, "C") == 0 || strcmp(format, "F") == 0) {
            requestedTempFormat = format[0];
        }
    }
    for (auto& update : heatingUpdates) {
        JsonVariantConst input = json[update.key];
        if (input.isUnbound()) {
            continue;
        }
        if (!input.is<double>()) {
            Log.warning("Invalid heating constant [%s]: expected a number.\r\n", update.key);
            return false;
        }
        double value = input.as<double>();
        double scale = TEMP_FIXED_POINT_SCALE;
        if (update.temperatureDifference && requestedTempFormat == 'F') {
            scale *= 5.0 / 9.0;
        }
        double fixedValue = value * scale;
        double lowerBound = update.temperatureDifference ? 0.0 : MIN_TEMP;
        if (!std::isfinite(value) || !std::isfinite(fixedValue) ||
            fixedValue < lowerBound || fixedValue > MAX_TEMP) {
            Log.warning("Invalid heating constant [%s]: outside fixed-point range.\r\n", update.key);
            return false;
        }
        update.value = static_cast<temperature>(std::round(fixedValue));
        update.supplied = true;
    }

    // Temperature Format
    if(json["tempFormat"].is<const char *>()) {
        const char* formatStr = json["tempFormat"].as<const char *>();
        if(strlen(formatStr) == 1) {
            char format = formatStr[0];
            if(format == 'C' || format == 'F') {
                if(tempControl.cc.tempFormat != format) {
                    tempControl.cc.tempFormat = format;
                    saveSettings = true;
                    Log.notice("Settings update, [tempFormat]:(%c) applied.\r\n", format);
                }
            } else {
                Log.warning("Invalid [tempFormat]:(%c) received.\r\n", format);
                failCount++;
            }
        } else {
            Log.warning("Invalid [tempFormat]:(%s) received (wrong length).\r\n", formatStr);
            failCount++;
        }
    }

    if(json["tempSetMin"].is<double>()) {
        char tempStr[8];
        snprintf(tempStr, sizeof(tempStr), "%.1f", json["tempSetMin"].as<double>());
        temperature newTemp = stringToTemp(tempStr);
        if(tempControl.cc.tempSettingMin != newTemp) {
            tempControl.cc.tempSettingMin = newTemp;
            saveSettings = true;
            Log.notice("Settings update, [tempSetMin]:(%s) applied.\r\n", tempStr);
        }
    }

    if(json["tempSetMax"].is<double>()) {
        char tempStr[8];
        snprintf(tempStr, sizeof(tempStr), "%.1f", json["tempSetMax"].as<double>());
        temperature newTemp = stringToTemp(tempStr);
        if(tempControl.cc.tempSettingMax != newTemp) {
            tempControl.cc.tempSettingMax = newTemp;
            saveSettings = true;
            Log.notice("Settings update, [tempSetMax]:(%s) applied.\r\n", tempStr);
        }
    }

    if(json["Kp"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.3f", json["Kp"].as<double>());
        temperature newVal = stringToFixedPoint(valStr);
        if(tempControl.cc.Kp != newVal) {
            tempControl.cc.Kp = newVal;
            saveSettings = true;
            Log.notice("Settings update, [Kp]:(%s) applied.\r\n", valStr);
        }
    }

    if(json["Ki"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.3f", json["Ki"].as<double>());
        temperature newVal = stringToFixedPoint(valStr);
        if(tempControl.cc.Ki != newVal) {
            tempControl.cc.Ki = newVal;
            saveSettings = true;
            Log.notice("Settings update, [Ki]:(%s) applied.\r\n", valStr);
        }
    }

    if(json["Kd"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.3f", json["Kd"].as<double>());
        temperature newVal = stringToFixedPoint(valStr);
        if(tempControl.cc.Kd != newVal) {
            tempControl.cc.Kd = newVal;
            saveSettings = true;
            Log.notice("Settings update, [Kd]:(%s) applied.\r\n", valStr);
        }
    }

    if(json["pidMax"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.1f", json["pidMax"].as<double>());
        temperature newVal = stringToTempDiff(valStr);
        if(tempControl.cc.pidMax != newVal) {
            tempControl.cc.pidMax = newVal;
            saveSettings = true;
            Log.notice("Settings update, [pidMax]:(%s) applied.\r\n", valStr);
        }
    }

    if(json["iMaxErr"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.1f", json["iMaxErr"].as<double>());
        temperature newVal = stringToTempDiff(valStr);
        if(tempControl.cc.iMaxError != newVal) {
            tempControl.cc.iMaxError = newVal;
            saveSettings = true;
            Log.notice("Settings update, [iMaxErr]:(%s) applied.\r\n", valStr);
        }
    }

    if(json["idleRangeH"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.1f", json["idleRangeH"].as<double>());
        temperature newVal = stringToTempDiff(valStr);
        if(tempControl.cc.idleRangeHigh != newVal) {
            tempControl.cc.idleRangeHigh = newVal;
            saveSettings = true;
            Log.notice("Settings update, [idleRangeH]:(%s) applied.\r\n", valStr);
        }
    }

    if(json["idleRangeL"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.1f", json["idleRangeL"].as<double>());
        temperature newVal = stringToTempDiff(valStr);
        if(tempControl.cc.idleRangeLow != newVal) {
            tempControl.cc.idleRangeLow = newVal;
            saveSettings = true;
            Log.notice("Settings update, [idleRangeL]:(%s) applied.\r\n", valStr);
        }
    }

    if(json["heatTargetH"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.1f", json["heatTargetH"].as<double>());
        temperature newVal = stringToTempDiff(valStr);
        if(tempControl.cc.heatingTargetUpper != newVal) {
            tempControl.cc.heatingTargetUpper = newVal;
            saveSettings = true;
            Log.notice("Settings update, [heatTargetH]:(%s) applied.\r\n", valStr);
        }
    }

    if(json["heatTargetL"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.1f", json["heatTargetL"].as<double>());
        temperature newVal = stringToTempDiff(valStr);
        if(tempControl.cc.heatingTargetLower != newVal) {
            tempControl.cc.heatingTargetLower = newVal;
            saveSettings = true;
            Log.notice("Settings update, [heatTargetL]:(%s) applied.\r\n", valStr);
        }
    }

    if(json["coolTargetH"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.1f", json["coolTargetH"].as<double>());
        temperature newVal = stringToTempDiff(valStr);
        if(tempControl.cc.coolingTargetUpper != newVal) {
            tempControl.cc.coolingTargetUpper = newVal;
            saveSettings = true;
            Log.notice("Settings update, [coolTargetH]:(%s) applied.\r\n", valStr);
        }
    }

    if(json["coolTargetL"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.1f", json["coolTargetL"].as<double>());
        temperature newVal = stringToTempDiff(valStr);
        if(tempControl.cc.coolingTargetLower != newVal) {
            tempControl.cc.coolingTargetLower = newVal;
            saveSettings = true;
            Log.notice("Settings update, [coolTargetL]:(%s) applied.\r\n", valStr);
        }
    }

    if(json["maxHeatTimeForEst"].is<uint16_t>()) {
        uint16_t newVal = json["maxHeatTimeForEst"].as<uint16_t>();
        if(tempControl.cc.maxHeatTimeForEstimate != newVal) {
            tempControl.cc.maxHeatTimeForEstimate = newVal;
            saveSettings = true;
            Log.notice("Settings update, [maxHeatTimeForEst]:(%u) applied.\r\n", newVal);
        }
    }

    if(json["maxCoolTimeForEst"].is<uint16_t>()) {
        uint16_t newVal = json["maxCoolTimeForEst"].as<uint16_t>();
        if(tempControl.cc.maxCoolTimeForEstimate != newVal) {
            tempControl.cc.maxCoolTimeForEstimate = newVal;
            saveSettings = true;
            Log.notice("Settings update, [maxCoolTimeForEst]:(%u) applied.\r\n", newVal);
        }
    }

    if(json["fridgeFastFilt"].is<uint8_t>()) {
        uint8_t newVal = json["fridgeFastFilt"].as<uint8_t>();
        if(tempControl.cc.fridgeFastFilter != newVal) {
            tempControl.cc.fridgeFastFilter = newVal;
            tempControl.fridgeSensor->setFastFilterCoefficients(newVal);
            saveSettings = true;
            Log.notice("Settings update, [fridgeFastFilt]:(%u) applied.\r\n", newVal);
        }
    }

    if(json["fridgeSlowFilt"].is<uint8_t>()) {
        uint8_t newVal = json["fridgeSlowFilt"].as<uint8_t>();
        if(tempControl.cc.fridgeSlowFilter != newVal) {
            tempControl.cc.fridgeSlowFilter = newVal;
            tempControl.fridgeSensor->setSlowFilterCoefficients(newVal);
            saveSettings = true;
            Log.notice("Settings update, [fridgeSlowFilt]:(%u) applied.\r\n", newVal);
        }
    }

    if(json["fridgeSlopeFilt"].is<uint8_t>()) {
        uint8_t newVal = json["fridgeSlopeFilt"].as<uint8_t>();
        if(tempControl.cc.fridgeSlopeFilter != newVal) {
            tempControl.cc.fridgeSlopeFilter = newVal;
            tempControl.fridgeSensor->setSlopeFilterCoefficients(newVal);
            saveSettings = true;
            Log.notice("Settings update, [fridgeSlopeFilt]:(%u) applied.\r\n", newVal);
        }
    }

    if(json["beerFastFilt"].is<uint8_t>()) {
        uint8_t newVal = json["beerFastFilt"].as<uint8_t>();
        if(tempControl.cc.beerFastFilter != newVal) {
            tempControl.cc.beerFastFilter = newVal;
            tempControl.beerSensor->setFastFilterCoefficients(newVal);
            saveSettings = true;
            Log.notice("Settings update, [beerFastFilt]:(%u) applied.\r\n", newVal);
        }
    }

    if(json["beerSlowFilt"].is<uint8_t>()) {
        uint8_t newVal = json["beerSlowFilt"].as<uint8_t>();
        if(tempControl.cc.beerSlowFilter != newVal) {
            tempControl.cc.beerSlowFilter = newVal;
            tempControl.beerSensor->setSlowFilterCoefficients(newVal);
            saveSettings = true;
            Log.notice("Settings update, [beerSlowFilt]:(%u) applied.\r\n", newVal);
        }
    }

    if(json["beerSlopeFilt"].is<uint8_t>()) {
        uint8_t newVal = json["beerSlopeFilt"].as<uint8_t>();
        if(tempControl.cc.beerSlopeFilter != newVal) {
            tempControl.cc.beerSlopeFilter = newVal;
            tempControl.beerSensor->setSlopeFilterCoefficients(newVal);
            saveSettings = true;
            Log.notice("Settings update, [beerSlopeFilt]:(%u) applied.\r\n", newVal);
        }
    }

    if(json["lah"].is<bool>()) {
        uint8_t newVal = json["lah"].as<bool>() ? 1 : 0;
        if(tempControl.cc.lightAsHeater != newVal) {
            tempControl.cc.lightAsHeater = newVal;
            saveSettings = true;
            Log.notice("Settings update, [lah]:(%u) applied.\r\n", newVal);
        }
    }

    if(json["hs"].is<bool>()) {
        uint8_t newVal = json["hs"].as<bool>() ? 1 : 0;
        if(tempControl.cc.rotaryHalfSteps != newVal) {
            tempControl.cc.rotaryHalfSteps = newVal;
            saveSettings = true;
            Log.notice("Settings update, [hs]:(%u) applied.\r\n", newVal);
        }
    }

    if(failCount) {
        Log.error("Error: Invalid control constants configuration.\r\n");
    } else {
        for (const auto& update : heatingUpdates) {
            temperature& currentValue = tempControl.cc.*(update.field);
            if (update.supplied && currentValue != update.value) {
                currentValue = update.value;
                saveSettings = true;
                Log.notice("Settings update, [%s] applied.\r\n", update.key);
            }
        }
        if(saveSettings) {
            TempControl::storeConstants();
        }
    }
    return failCount == 0;
}


bool processActionJson(const JsonDocument& json, bool triggerUpstreamUpdate) {
    if(!json["action"].is<const char*>()) {
        Log.warning("Action error - Action key is not a string.\r\n");
        return false;
    }

    const char* action = json["action"].as<const char*>();

    if(strcmp(action, "restart") == 0) {
        Log.notice("Action [restart] received\r\n");
        http_server.restart_requested = true;
        return true;
    }

    if(strcmp(action, "reset_connection") == 0) {
        Log.notice("Action [reset_connection] received\r\n");
        http_server.wifi_reset_requested = true;
        http_server.restart_requested = true;
        return true;
    }

    if(strcmp(action, "reset_config") == 0) {
        Log.notice("Action [reset_config] received\r\n");
        http_server.config_reset_requested = true;
        http_server.restart_requested = true;
        return true;
    }

#ifdef ENABLE_GLYCOL_LOGGING
    if(strcmp(action, "clear_glycol_log") == 0) {
        Log.notice("Action [clear_glycol_log] received\r\n");
        glycolLog.clearLog();
        return true;
    }
#endif

    Log.warning("Action error - Unknown action: %s\r\n", action);
    return false;
}


// ============================================================================
// GET JSON data providers
// ============================================================================

void serveExtendedSettings(JsonDocument &doc) {
    JsonDocument extended_settings;
    JsonDocument min_times;

    extendedSettings.toJson(extended_settings);
    minTimes.toJson(min_times);

    doc["extendedSettings"] = extended_settings;
    doc["minTimes"] = min_times;
}

void serveUpstreamSettings(JsonDocument &doc) {
    upstreamSettings.toJson(doc);
}

void uptime(JsonDocument &doc) {
    Log.verbose("Serving uptime.\r\n");
    doc["days"] = uptimeDays();
    doc["hours"] = uptimeHours();
    doc["minutes"] = uptimeMinutes();
    doc["seconds"] = uptimeSeconds();
    doc["millis"] = uptimeMillis();
}

void heap(JsonDocument &doc) {
    Log.verbose("Serving heap information.\r\n");
    const uint32_t free = esp_get_free_heap_size();
    const uint32_t max = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    const uint8_t frag = 100 - (max * 100) / free;
    doc["free"] = free;
    doc["max"] = max;
    doc["frag"] = frag;
}

void reset_reason(JsonDocument &doc) {
    Log.verbose("Serving reset reason.\r\n");
    const int reset = (int)esp_reset_reason();
    doc["reason"] = resetReason[reset];
    doc["description"] = resetDescription[reset];
}


// ============================================================================
// Static file serving
// ============================================================================

static bool endsWith(const char* str, const char* suffix) {
    size_t strLen = strlen(str);
    size_t suffixLen = strlen(suffix);
    if (suffixLen > strLen) return false;
    return strcmp(str + strLen - suffixLen, suffix) == 0;
}

const char* httpServer::getContentType(const char* filename) {
    if (endsWith(filename, ".htm")) return "text/html";
    if (endsWith(filename, ".html")) return "text/html";
    if (endsWith(filename, ".css")) return "text/css";
    if (endsWith(filename, ".js")) return "application/javascript";
    if (endsWith(filename, ".png")) return "image/png";
    if (endsWith(filename, ".gif")) return "image/gif";
    if (endsWith(filename, ".jpg")) return "image/jpeg";
    if (endsWith(filename, ".ico")) return "image/x-icon";
    if (endsWith(filename, ".xml")) return "text/xml";
    if (endsWith(filename, ".pdf")) return "application/x-pdf";
    if (endsWith(filename, ".zip")) return "application/x-zip";
    if (endsWith(filename, ".gz")) return "application/x-gzip";
    if (endsWith(filename, ".svg")) return "image/svg+xml";
    return "text/plain";
}

esp_err_t httpServer::handleFileRead(httpd_req_t *req, const char* path) {
    char fullPath[256];
    strlcpy(fullPath, path, sizeof(fullPath));

    size_t len = strlen(fullPath);
    if (len > 0 && fullPath[len - 1] == '/') {
        strlcat(fullPath, "index.html", sizeof(fullPath));
    }

    const char* contentType = getContentType(fullPath);

    // Check for gzipped version
    char gzPath[260];
    snprintf(gzPath, sizeof(gzPath), "%s.gz", fullPath);

    bool isGzipped = fs_exists(gzPath);

    if (!isGzipped && !fs_exists(fullPath)) {
        return ESP_FAIL;  // File not found
    }

    httpd_resp_set_type(req, contentType);
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=600");

    const char* fileToOpen = isGzipped ? gzPath : fullPath;
    if (isGzipped) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }

    FILE* f = fs_open(fileToOpen, "r");
    if (!f) {
        return ESP_FAIL;
    }

    char buf[512];
    size_t bytesRead;
    while ((bytesRead = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, bytesRead) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, nullptr, 0);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, nullptr, 0);  // Finalize chunked response
    return ESP_OK;
}


// Handler for static file routes (serves /index.html for SPA routes)
esp_err_t httpServer::static_file_handler(httpd_req_t *req) {
    return handleFileRead(req, "/index.html");
}

// 404 handler — tries filesystem, otherwise sends 404
esp_err_t httpServer::not_found_handler(httpd_req_t *req, httpd_err_code_t err) {
    if (handleFileRead(req, req->uri) == ESP_OK) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Not Found", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}


// ============================================================================
// Route registration
// ============================================================================

void httpServer::setStaticPages() {
    // Root and index
    const httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = static_file_handler, .user_ctx = nullptr };
    httpd_register_uri_handler(server_handle, &uri_root);

    const httpd_uri_t uri_index = { .uri = "/index.html", .method = HTTP_GET, .handler = static_file_handler, .user_ctx = nullptr };
    httpd_register_uri_handler(server_handle, &uri_index);

    // Vue SPA routes - registered individually since esp_http_server needs static uri structs

    const httpd_uri_t uri_upstream = { .uri = "/upstream", .method = HTTP_GET, .handler = static_file_handler, .user_ctx = nullptr };
    httpd_register_uri_handler(server_handle, &uri_upstream);
    const httpd_uri_t uri_devices = { .uri = "/devices", .method = HTTP_GET, .handler = static_file_handler, .user_ctx = nullptr };
    httpd_register_uri_handler(server_handle, &uri_devices);
    const httpd_uri_t uri_about = { .uri = "/about", .method = HTTP_GET, .handler = static_file_handler, .user_ctx = nullptr };
    httpd_register_uri_handler(server_handle, &uri_about);
    const httpd_uri_t uri_settings = { .uri = "/settings", .method = HTTP_GET, .handler = static_file_handler, .user_ctx = nullptr };
    httpd_register_uri_handler(server_handle, &uri_settings);
}


void httpServer::setJsonPages() {
    struct { const char* uri; esp_err_t (*handler)(httpd_req_t*); } endpoints[] = {
        { "/api/version/",           get_json_handler<versionInfoJson> },
        { "/api/lcd/",               get_json_handler<getLcdContentJson> },
        { "/api/temps/",             get_json_handler<printTemperaturesJson> },
        { "/api/cs/",                get_json_handler<TempControl::getControlSettingsDoc> },
        { "/api/cc/",                get_json_handler<TempControl::getControlConstantsDoc> },
        { "/api/cv/",                get_json_handler<TempControl::getControlVariablesDoc> },
        { "/api/all_temp_control/",  get_json_handler<getFullTemperatureControlJson> },
        { "/api/devices/",           get_json_handler<DeviceManager::enumerateHardware> },
        { "/api/extended/",          get_json_handler<serveExtendedSettings> },
        { "/api/upstream/",          get_json_handler<serveUpstreamSettings> },
        { "/api/uptime/",            get_json_handler<uptime> },
        { "/api/heap/",              get_json_handler<heap> },
        { "/api/resetreason/",       get_json_handler<reset_reason> },
    };

    for (const auto& ep : endpoints) {
        httpd_uri_t uri = { .uri = ep.uri, .method = HTTP_GET, .handler = ep.handler, .user_ctx = nullptr };
        httpd_register_uri_handler(server_handle, &uri);
    }
}


void httpServer::setPutPages() {
    struct { const char* uri; esp_err_t (*handler)(httpd_req_t*); } endpoints[] = {
        { "/api/upstream/",  put_json_handler<processUpstreamConfigUpdateJson> },
        { "/api/devices/",   put_json_handler<processDeviceUpdateJson> },
        { "/api/mode/",      put_json_handler<processUpdateModeJson> },
        { "/api/extended/",  put_json_handler<processExtendedSettingsJson> },
        { "/api/cc/",        put_json_handler<processControlConstantsJson> },
        { "/api/action/",    put_json_handler<processActionJson> },
    };

    for (const auto& ep : endpoints) {
        httpd_uri_t uri = { .uri = ep.uri, .method = HTTP_PUT, .handler = ep.handler, .user_ctx = nullptr };
        httpd_register_uri_handler(server_handle, &uri);
    }
}


void httpServer::startServer() {
    if (server_handle != nullptr) return;  // Already started

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;
    config.lru_purge_enable = true;
    config.max_uri_handlers = 64;
    config.max_resp_headers = 8;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_open_sockets = 7;
    config.stack_size = 8192;

    esp_err_t ret = httpd_start(&server_handle, &config);
    if (ret != ESP_OK) {
        Log.error("Failed to start HTTP server: %s\r\n", esp_err_to_name(ret));
        return;
    }
    Log.notice("HTTP server started on port %d.\r\n", WEB_SERVER_PORT);
}

void httpServer::registerRoutes() {
    if (server_handle == nullptr) return;

    setStaticPages();
    setJsonPages();
    setPutPages();

    // Register 404 handler for file serving fallback
    httpd_register_err_handler(server_handle, HTTPD_404_NOT_FOUND, not_found_handler);

    Log.notice("HTTP routes registered. Open: http://%s.local/ to view application.\r\n", bp_wifi_get_hostname());
}

void httpServer::init() {
    startServer();
    registerRoutes();
}


#endif
