#ifdef ENABLE_HTTP_INTERFACE

#include <Arduino.h>
#include <ArduinoLog.h>
#include <ArduinoJson.h>
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>

#include "ESPEepromAccess.h"  // Defines FILESYSTEM (and includes the approprite headers)

#include "uptime.h"
#include "resetreasons.h"
#include "http_server.h"
#include "TempControl.h"
// #include "sendData.h"
// #include "serialhandler.h"
#include "JsonMessages.h"
#include "DeviceManager.h"
#include "JsonKeys.h"
#include "rest/rest_send.h"
#include "EepromManager.h"
#include "SettingsManager.h"

#include "extended_async_json_handler.h"

#ifdef ENABLE_GLYCOL_LOGGING
#include "GlycolLog.h"
#endif


httpServer http_server;
AsyncWebServer asyncWebServer(WEB_SERVER_PORT);



// Settings Page Handlers

bool processUpstreamConfigUpdateJson(const JsonDocument& json, bool triggerUpstreamUpdate) {
    uint8_t failCount = 0;
    bool saveSettings = false;

    // Upstream Host
    if(json[UpstreamSettingsKeys::upstreamHost].is<const char*>()) {
        if (strlen(json[UpstreamSettingsKeys::upstreamHost]) <= 0) {
            // The user unset the upstream host - Clear it from memory
            upstreamSettings.upstreamHost[0] = '\0';
            upstreamSettings.deviceID[0] = '\0';  // Also clear the device ID
            Log.notice(F("Settings update, [upstreamHost]: unset.\r\n"));
        } else if (strlen(json[UpstreamSettingsKeys::upstreamHost]) >= 128 ) {
            Log.warning(F("Settings update error, [upstreamHost]:(%s) not valid.\r\n"), json[UpstreamSettingsKeys::upstreamHost].as<const char*>());
            failCount++;
        } else {
            // Valid - Update
            if(strcmp(json[UpstreamSettingsKeys::upstreamHost], upstreamSettings.upstreamHost) != 0) {
                strlcpy(upstreamSettings.upstreamHost, json[UpstreamSettingsKeys::upstreamHost].as<const char*>(), 128);
                upstreamSettings.deviceID[0] = '\0';  // Also clear the device ID
                Log.notice(F("Settings update, [upstreamHost]:(%s) applied.\r\n"), json[UpstreamSettingsKeys::upstreamHost].as<const char*>());
                saveSettings = true;
            }
        }
    }

    // Upstream Port
    if(json[UpstreamSettingsKeys::upstreamPort].is<uint16_t>()) {
        if((json[UpstreamSettingsKeys::upstreamPort] <= 0) || (json[UpstreamSettingsKeys::upstreamPort] > 65535)) {
            // This is actually impossible to reach, unless the port is 0.
            Log.warning("Invalid [upstreamPort]:(%u) received.\r\n", json[UpstreamSettingsKeys::upstreamPort].as<uint16_t>());
            failCount++;
        } else {
            //Valid - Update
            upstreamSettings.upstreamPort = json[UpstreamSettingsKeys::upstreamPort];
            upstreamSettings.deviceID[0] = '\0';  // Also clear the device ID
            Log.warning("Settings update, [upstreamPort]:(%d) applied.\r\n", json[UpstreamSettingsKeys::upstreamPort].as<uint16_t>());
            saveSettings = true;
        }
    } else {
        Log.warning("Invalid [upstreamPort]:(%s) received (wrong type).\r\n", json[UpstreamSettingsKeys::upstreamPort].as<const char*>());
        failCount++;
    }

    // Device ID
    // NOTE - We're not allowing the device ID to be changed via the API for now. Instead, it has to be reset with the upstream
    // if(json[UpstreamSettingsKeys::deviceID].is<const char*>()) {
    //     if (strlen(json[UpstreamSettingsKeys::deviceID]) <= 0) {
    //         // The user unset the upstream host - Clear it from memory
    //         upstreamSettings.deviceID[0] = '\0';
    //         Log.notice(F("Settings update, [deviceID]: unset.\r\n"));
    //     } else if (strlen(json[UpstreamSettingsKeys::deviceID]) >= 40 ) {
    //         Log.warning(F("Settings update error, [deviceID]:(%s) not valid.\r\n"), json[UpstreamSettingsKeys::deviceID].as<const char*>());
    //         failCount++;
    //     } else {
    //         if(strcmp(json[UpstreamSettingsKeys::deviceID], upstreamSettings.deviceID) != 0) {
    //             strlcpy(upstreamSettings.deviceID, json[UpstreamSettingsKeys::deviceID].as<const char*>(), 40);
    //             Log.notice(F("Settings update, [deviceID]:(%s) applied.\r\n"), json[UpstreamSettingsKeys::deviceID].as<const char*>());
    //             saveSettings = true;
    //         }
    //     }
    // }


    // Upstream Username
    if(json[UpstreamSettingsKeys::username].is<const char*>()) {
        if (strlen(json[UpstreamSettingsKeys::username]) <= 0) {
            // The user unset the upstream host - Clear it from memory
            upstreamSettings.username[0] = '\0';
            upstreamSettings.deviceID[0] = '\0';  // Also clear the device ID
            Log.notice(F("Settings update, [username]: unset.\r\n"));
        } else if (strlen(json[UpstreamSettingsKeys::username]) >= 128 ) {
            Log.warning(F("Settings update error, [username]:(%s) not valid.\r\n"), json[UpstreamSettingsKeys::username].as<const char*>());
            failCount++;
        } else {
            // Valid - Update
            if(strcmp(json[UpstreamSettingsKeys::username], upstreamSettings.username) != 0) {
                strlcpy(upstreamSettings.username, json[UpstreamSettingsKeys::username].as<const char*>(), 128);
                upstreamSettings.deviceID[0] = '\0';  // Also clear the device ID
                Log.notice(F("Settings update, [username]:(%s) applied.\r\n"), json[UpstreamSettingsKeys::username].as<const char*>());
                saveSettings = true;
            }
        }
    }

    // Upstream API Key
    // NOTE - Unused as of Jan 2024
    if(json[UpstreamSettingsKeys::apiKey].is<const char*>()) {
        if (strlen(json[UpstreamSettingsKeys::apiKey]) <= 0) {
            // The user unset the upstream host - Clear it from memory
            upstreamSettings.apiKey[0] = '\0';
            upstreamSettings.deviceID[0] = '\0';  // Also clear the device ID
            Log.notice(F("Settings update, [apiKey]: unset.\r\n"));
        } else if (strlen(json[UpstreamSettingsKeys::apiKey]) >= 40 ) {
            Log.warning(F("Settings update error, [apiKey]:(%s) not valid.\r\n"), json[UpstreamSettingsKeys::apiKey].as<const char*>());
            failCount++;
        } else {
            // Valid - Update
            if(strcmp(json[UpstreamSettingsKeys::apiKey], upstreamSettings.apiKey) != 0) {
                strlcpy(upstreamSettings.apiKey, json[UpstreamSettingsKeys::apiKey].as<const char*>(), sizeof(upstreamSettings.apiKey));
                upstreamSettings.deviceID[0] = '\0';  // Also clear the device ID
                Log.notice(F("Settings update, [apiKey]:(%s) applied.\r\n"), json[UpstreamSettingsKeys::apiKey].as<const char*>());
                saveSettings = true;
            }
        }
    }

    // Device Name (optional, only used during registration)
    if(json[UpstreamSettingsKeys::deviceName].is<const char*>()) {
        if (strlen(json[UpstreamSettingsKeys::deviceName]) >= sizeof(rest_handler.pendingDeviceName)) {
            Log.warning(F("Settings update error, [name]:(%s) too long.\r\n"), json[UpstreamSettingsKeys::deviceName].as<const char*>());
            failCount++;
        } else {
            strlcpy(rest_handler.pendingDeviceName, json[UpstreamSettingsKeys::deviceName].as<const char*>(), sizeof(rest_handler.pendingDeviceName));
            Log.notice(F("Settings update, [name]:(%s) applied.\r\n"), json[UpstreamSettingsKeys::deviceName].as<const char*>());
        }
    } else {
        // No name provided - clear any pending name
        rest_handler.pendingDeviceName[0] = '\0';
    }

    // Save
    if (failCount) {
        Log.error(F("Error: Invalid upstream configuration.\r\n"));
    } else {
        if(saveSettings == true) {
            upstreamSettings.storeToFilesystem();
        }
        upstreamSettings.upstreamRegistrationError = UpstreamSettings::upstreamRegErrorT::NOT_ATTEMPTED_REGISTRATION;
        rest_handler.register_device_ticker = true;
    }
    return failCount == 0;
}


bool processDeviceUpdateJson(const JsonDocument& json, bool triggerUpstreamUpdate) {
    DeviceDefinition dev;
    // Check for universally required keys
    if(!json[DeviceDefinitionKeys::chamber].is<uint8_t>() || !json[DeviceDefinitionKeys::beer].is<uint8_t>() || 
        !json[DeviceDefinitionKeys::function].is<uint8_t>() || !json[DeviceDefinitionKeys::hardware].is<uint8_t>()
    //    || !json[DeviceDefinitionKeys::deactivated].is<bool>()
    ) 
    {
        // We don't actually parse deactivated, so commenting out the check. If we add it later, we will need to check that we don't need to do
        // shenanigans like we do with invert below to handle all the various ways it can be sent to us.
        Log.warning(F("Invalid device definition received - missing required keys (c/f/h/b).\r\n"));
        return 1;
    }

    switch(json[DeviceDefinitionKeys::hardware].as<uint8_t>()) {
        case DEVICE_HARDWARE_PIN:

            if(!json[DeviceDefinitionKeys::pin].is<int>() || !(json[DeviceDefinitionKeys::invert].is<bool>() || json[DeviceDefinitionKeys::invert].is<const char *>() || json[DeviceDefinitionKeys::invert].is<uint8_t>())) {
                Log.warning(F("Invalid device definition received - missing required keys (p/x).\r\n"));
                return 1;
            }
            break;
        case DEVICE_HARDWARE_ONEWIRE_TEMP:
        case DEVICE_HARDWARE_BLUETOOTH_INKBIRD:
        case DEVICE_HARDWARE_BLUETOOTH_TILT:
            if(!json[DeviceDefinitionKeys::address].is<const char*>()) {
                Log.warning(F("Invalid device definition received - missing required keys (a).\r\n"));
                return 1;
            }
            break;
        case DEVICE_HARDWARE_TPLINK_SWITCH:
            if(!json[DeviceDefinitionKeys::address].is<const char*>() || !json[DeviceDefinitionKeys::child_id].is<const char*>()) {
                Log.warning(F("Invalid device definition received - missing required keys (a).\r\n"));
                return 1;
            }
            break;
        default:
            break;
    }
    http_server.dev = DeviceManager::readJsonIntoDeviceDef(json);                  // Parse the JSON into a DeviceDefinition object
    http_server.device_definition_update_requested = true;
    // dev = DeviceManager::readJsonIntoDeviceDef(json);                  // Parse the JSON into a DeviceDefinition object
    // TODO - Trigger upstream update
    return true;
}


// Allows us to process the device definition update in the main loop rather than in the async handler
void httpServer::processQueuedDeviceDefinition() {
    if(device_definition_update_requested) {
        /*DeviceConfig print =*/
        deviceManager.updateDeviceDefinition(dev);   // Save the device definition (if valid)
        device_definition_update_requested = false;
    }
}


// Allows us to process action requests in the main loop rather than in the async handler
void httpServer::processQueuedActions() {
    // Process config reset first (before restart)
    if(config_reset_requested) {
        Log.notice(F("Processing config reset request\r\n"));
        delay(500);  // Need to give the response time to be sent/processed
        if(eepromManager.initializeEeprom()) {
            logInfo(INFO_EEPROM_INITIALIZED);
            settingsManager.loadSettings();
        }
        config_reset_requested = false;
    }

    // Process WiFi/connection reset (this will also restart)
    if(wifi_reset_requested) {
        Log.notice(F("Processing WiFi reset request\r\n"));
        delay(500);  // Need to give the response time to be sent/processed
        // Reset the upstream settings
        upstreamSettings.setDefaults();
        upstreamSettings.storeToFilesystem();
        // Disconnect WiFi and restart
        WiFi.disconnect(false, true);
        delay(500);
        ESP.restart();
    }

    // Process simple restart last (if no wifi_reset was requested)
    if(restart_requested) {
        Log.notice(F("Processing restart request\r\n"));
        delay(500);  // Need to give the response time to be sent/processed
        ESP.restart();
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
                // Mode is valid - Update
                if(new_mode != tempControl.getMode()) {
                    tempControl.setMode(new_mode);
                    Log.notice(F("Settings update, [newMode]:(%c) applied.\r\n"), new_mode);
                    saveSettings = true;
                } else {
                    Log.notice(F("Settings update, [newMode]:(%c) NOT applied - no change.\r\n"), new_mode);
                }
            } else {
                Log.warning(F("Settings update error, [newMode]:(%c) not valid.\r\n"), new_mode);
                failCount++;
            }
        } else {
            Log.warning(F("Settings update error, [newMode]:(%s) not a valid type.\r\n"), json[ModeUpdateKeys::mode].as<const char*>());
            failCount++;
        }
    }


    // Set Point
    if(json[ModeUpdateKeys::setpoint].is<double>()) {
        if(tempControl.getMode() != Modes::fridgeConstant && tempControl.getMode() != Modes::beerConstant && tempControl.getMode() != Modes::beerProfile) {
            Log.info(F("Settings update error, [setpoint]:(%s) current mode (%c) does not take a setpoint.\r\n"), json[ModeUpdateKeys::setpoint].as<const char*>(), tempControl.getMode());
        } else {
            char modeString[7];
            snprintf(modeString, 7, "%.1f", json[ModeUpdateKeys::setpoint].as<double>());

            temperature newTemp = stringToTemp(modeString);  // Also constrains the value to the valid range

            if(tempControl.getMode() == Modes::fridgeConstant) {
                tempControl.setFridgeTemp(newTemp);
                saveSettings = true;
            } else if(tempControl.getMode() == Modes::beerConstant || tempControl.getMode() == Modes::beerProfile) {
                tempControl.setBeerTemp(newTemp);
                saveSettings = true;
            } else {
                Log.error(F("Settings update error, [setpoint]:(%s) current mode (%c) does not take a setpoint (should never be reached).\r\n"), modeString, tempControl.getMode());
            }
        }
    }


    // Save
    if (failCount) {
        Log.error(F("Error: Invalid upstream configuration.\r\n"));
    } else {
        if(saveSettings == true) {
            // TODO - Force upstream cascade/send
            // upstreamSettings.storeToFilesystem();
        }
    }
    return failCount == 0;
}



bool processExtendedSettingsJson(const JsonDocument& json, bool triggerUpstreamUpdate) {
    uint8_t failCount = 0;
    bool saveSettings = false;
    bool saveMinTimes = false; 

    // Glycol Mode
    if(json[ExtendedSettingsKeys::glycol].is<bool>()) {
        if(extendedSettings.glycol != json[ExtendedSettingsKeys::glycol].as<bool>()) {
            extendedSettings.setGlycol(json[ExtendedSettingsKeys::glycol].as<bool>());
            saveSettings = true;
        }
    } else {
        Log.warning(F("Invalid [glycol]:(%s) received (wrong type).\r\n"), json[ExtendedSettingsKeys::glycol]);
        failCount++;
    }

    // Large TFT flag
    if(json[ExtendedSettingsKeys::largeTFT].is<bool>()) {
        if(extendedSettings.largeTFT != json[ExtendedSettingsKeys::largeTFT].as<bool>()) {
            extendedSettings.setLargeTFT(json[ExtendedSettingsKeys::largeTFT].as<bool>());
            saveSettings = true;
        }
    } else {
        Log.warning(F("Invalid [largeTFT]:(%s) received (wrong type).\r\n"), json[ExtendedSettingsKeys::largeTFT]);
        failCount++;
    }

    // Invert TFT Flag
    if(json[ExtendedSettingsKeys::invertTFT].is<bool>()) {
        if(extendedSettings.invertTFT != json[ExtendedSettingsKeys::invertTFT].as<bool>()) {
            extendedSettings.setInvertTFT(json[ExtendedSettingsKeys::invertTFT].as<bool>());
            saveSettings = true;
        }
    } else {
        Log.warning(F("Invalid [invertTFT]:(%s) received (wrong type).\r\n"), json[ExtendedSettingsKeys::invertTFT]);
        failCount++;
    }

    // Reset Screen on Pin Toggle Flag
    if(json[ExtendedSettingsKeys::resetScreenOnPin].is<bool>()) {
        if(extendedSettings.resetScreenOnPin != json[ExtendedSettingsKeys::resetScreenOnPin].as<bool>()) {
            extendedSettings.setResetScreenOnPin(json[ExtendedSettingsKeys::resetScreenOnPin].as<bool>());
            saveSettings = true;
        }
    } else {
        Log.warning(F("Invalid [resetScreenOnPin]:(%s) received (wrong type).\r\n"), json[ExtendedSettingsKeys::resetScreenOnPin]);
        failCount++;
    }


#ifdef HAS_BLUETOOTH
    // Tilt Gravity Sensor
    if(json[ExtendedSettingsKeys::tiltGravSensor].is<std::string>()) {
        // Validate that it's valid
        if(extendedSettings.tiltGravSensor != NimBLEAddress(json[ExtendedSettingsKeys::tiltGravSensor].as<std::string>(), 1)) {
            // Tilts use address type 1 ("random", which (correctly!) indicates they didn't buy a MAC block)
            extendedSettings.setTiltGravSensor(NimBLEAddress(json[ExtendedSettingsKeys::tiltGravSensor].as<std::string>(), 1));
            saveSettings = true;
        }
    }
#endif

    // SETTINGS_CHOICE
    if(json[MinTimesKeys::SETTINGS_CHOICE].is<uint8_t>()) {
        // Validate that it's valid and different
        if(minTimes.settings_choice != json[MinTimesKeys::SETTINGS_CHOICE].as<uint8_t>() && json[MinTimesKeys::SETTINGS_CHOICE].as<uint8_t>() <= MIN_TIMES_CUSTOM) {
            minTimes.settings_choice = (MinTimesSettingsChoice) json[MinTimesKeys::SETTINGS_CHOICE].as<uint8_t>();
            saveMinTimes = true;
        }
    }


    if(minTimes.settings_choice == MIN_TIMES_CUSTOM) {
        // We only care about the other keys if we're in custom mode -- otherwise the call to setDefaults below will overwrite them

        // MIN_COOL_OFF_TIME
        if(json[MinTimesKeys::MIN_COOL_OFF_TIME].is<uint16_t>()) {
            if(minTimes.MIN_COOL_OFF_TIME != json[MinTimesKeys::MIN_COOL_OFF_TIME].as<uint16_t>()) {
                minTimes.MIN_COOL_OFF_TIME = json[MinTimesKeys::MIN_COOL_OFF_TIME].as<uint16_t>();
                saveMinTimes = true;
            }
        }


        // MIN_HEAT_OFF_TIME
        if(json[MinTimesKeys::MIN_HEAT_OFF_TIME].is<uint16_t>()) {
            if(minTimes.MIN_HEAT_OFF_TIME != json[MinTimesKeys::MIN_HEAT_OFF_TIME].as<uint16_t>()) {
                minTimes.MIN_HEAT_OFF_TIME = json[MinTimesKeys::MIN_HEAT_OFF_TIME].as<uint16_t>();
                saveMinTimes = true;
            }
        }

        // MIN_COOL_ON_TIME
        if(json[MinTimesKeys::MIN_COOL_ON_TIME].is<uint16_t>()) {
            if(minTimes.MIN_COOL_ON_TIME != json[MinTimesKeys::MIN_COOL_ON_TIME].as<uint16_t>()) {
                minTimes.MIN_COOL_ON_TIME = json[MinTimesKeys::MIN_COOL_ON_TIME].as<uint16_t>();
                saveMinTimes = true;
            }
        }

        // MIN_HEAT_ON_TIME
        if(json[MinTimesKeys::MIN_HEAT_ON_TIME].is<uint16_t>()) {
            if(minTimes.MIN_HEAT_ON_TIME != json[MinTimesKeys::MIN_HEAT_ON_TIME].as<uint16_t>()) {
                minTimes.MIN_HEAT_ON_TIME = json[MinTimesKeys::MIN_HEAT_ON_TIME].as<uint16_t>();
                saveMinTimes = true;
            }
        }


        // MIN_COOL_OFF_TIME_FRIDGE_CONSTANT
        if(json[MinTimesKeys::MIN_COOL_OFF_TIME_FRIDGE_CONSTANT].is<uint16_t>()) {
            if(minTimes.MIN_COOL_OFF_TIME_FRIDGE_CONSTANT != json[MinTimesKeys::MIN_COOL_OFF_TIME_FRIDGE_CONSTANT].as<uint16_t>()) {
                minTimes.MIN_COOL_OFF_TIME_FRIDGE_CONSTANT = json[MinTimesKeys::MIN_COOL_OFF_TIME_FRIDGE_CONSTANT].as<uint16_t>();
                saveMinTimes = true;
            }
        }

        // MIN_SWITCH_TIME
        if(json[MinTimesKeys::MIN_SWITCH_TIME].is<uint16_t>()) {
            if(minTimes.MIN_SWITCH_TIME != json[MinTimesKeys::MIN_SWITCH_TIME].as<uint16_t>()) {
                minTimes.MIN_SWITCH_TIME = json[MinTimesKeys::MIN_SWITCH_TIME].as<uint16_t>();
                saveMinTimes = true;
            }
        }

        // COOL_PEAK_DETECT_TIME
        if(json[MinTimesKeys::COOL_PEAK_DETECT_TIME].is<uint16_t>()) {
            if(minTimes.COOL_PEAK_DETECT_TIME != json[MinTimesKeys::COOL_PEAK_DETECT_TIME].as<uint16_t>()) {
                minTimes.COOL_PEAK_DETECT_TIME = json[MinTimesKeys::COOL_PEAK_DETECT_TIME].as<uint16_t>();
                saveMinTimes = true;
            }
        }


        // HEAT_PEAK_DETECT_TIME
        if(json[MinTimesKeys::HEAT_PEAK_DETECT_TIME].is<uint16_t>()) {
            if(minTimes.HEAT_PEAK_DETECT_TIME != json[MinTimesKeys::HEAT_PEAK_DETECT_TIME].as<uint16_t>()) {
                minTimes.HEAT_PEAK_DETECT_TIME = json[MinTimesKeys::HEAT_PEAK_DETECT_TIME].as<uint16_t>();
                saveMinTimes = true;
            }
        }
    }

    // Save
    if (failCount) {
        Log.error(F("Error: Invalid extended settings configuration.\r\n"));
    } else {
        if(saveSettings == true) {
            extendedSettings.storeToFilesystem();
            // TODO - Force upstream cascade/send
        }
        if(saveMinTimes == true) {
            minTimes.setDefaults(); // This will set defaults if defaults/lowdelay mode is set -- otherwise its a noop for custom mode
            minTimes.storeToFilesystem();
            // TODO - Force upstream cascade/send
        }
    }
    return failCount == 0;
}


bool processControlConstantsJson(const JsonDocument& json, bool triggerUpstreamUpdate) {
    uint8_t failCount = 0;
    bool saveSettings = false;

    // Temperature Format
    if(json["tempFormat"].is<const char *>()) {
        const char* formatStr = json["tempFormat"].as<const char *>();
        if(strlen(formatStr) == 1) {
            char format = formatStr[0];
            if(format == 'C' || format == 'F') {
                if(tempControl.cc.tempFormat != format) {
                    tempControl.cc.tempFormat = format;
                    saveSettings = true;
                    Log.notice(F("Settings update, [tempFormat]:(%c) applied.\r\n"), format);
                }
            } else {
                Log.warning(F("Invalid [tempFormat]:(%c) received.\r\n"), format);
                failCount++;
            }
        } else {
            Log.warning(F("Invalid [tempFormat]:(%s) received (wrong length).\r\n"), formatStr);
            failCount++;
        }
    }

    // Temperature settings (use stringToTemp for conversion)
    // tempSetMin
    if(json["tempSetMin"].is<double>()) {
        char tempStr[8];
        snprintf(tempStr, sizeof(tempStr), "%.1f", json["tempSetMin"].as<double>());
        temperature newTemp = stringToTemp(tempStr);
        if(tempControl.cc.tempSettingMin != newTemp) {
            tempControl.cc.tempSettingMin = newTemp;
            saveSettings = true;
            Log.notice(F("Settings update, [tempSetMin]:(%s) applied.\r\n"), tempStr);
        }
    }

    // tempSetMax
    if(json["tempSetMax"].is<double>()) {
        char tempStr[8];
        snprintf(tempStr, sizeof(tempStr), "%.1f", json["tempSetMax"].as<double>());
        temperature newTemp = stringToTemp(tempStr);
        if(tempControl.cc.tempSettingMax != newTemp) {
            tempControl.cc.tempSettingMax = newTemp;
            saveSettings = true;
            Log.notice(F("Settings update, [tempSetMax]:(%s) applied.\r\n"), tempStr);
        }
    }

    // PID settings (use stringToFixedPoint for Kp, Ki, Kd)
    // Kp
    if(json["Kp"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.3f", json["Kp"].as<double>());
        temperature newVal = stringToFixedPoint(valStr);
        if(tempControl.cc.Kp != newVal) {
            tempControl.cc.Kp = newVal;
            saveSettings = true;
            Log.notice(F("Settings update, [Kp]:(%s) applied.\r\n"), valStr);
        }
    }

    // Ki
    if(json["Ki"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.3f", json["Ki"].as<double>());
        temperature newVal = stringToFixedPoint(valStr);
        if(tempControl.cc.Ki != newVal) {
            tempControl.cc.Ki = newVal;
            saveSettings = true;
            Log.notice(F("Settings update, [Ki]:(%s) applied.\r\n"), valStr);
        }
    }

    // Kd
    if(json["Kd"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.3f", json["Kd"].as<double>());
        temperature newVal = stringToFixedPoint(valStr);
        if(tempControl.cc.Kd != newVal) {
            tempControl.cc.Kd = newVal;
            saveSettings = true;
            Log.notice(F("Settings update, [Kd]:(%s) applied.\r\n"), valStr);
        }
    }

    // KpHeat
    if(json["KpHeat"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.3f", json["KpHeat"].as<double>());
        temperature newVal = stringToFixedPoint(valStr);
        if(tempControl.cc.Kp_heat != newVal) {
            tempControl.cc.Kp_heat = newVal;
            saveSettings = true;
            Log.notice(F("Settings update, [KpHeat]:(%s) applied.\r\n"), valStr);
        }
    }

    // KiHeat
    if(json["KiHeat"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.3f", json["KiHeat"].as<double>());
        temperature newVal = stringToFixedPoint(valStr);
        if(tempControl.cc.Ki_heat != newVal) {
            tempControl.cc.Ki_heat = newVal;
            saveSettings = true;
            Log.notice(F("Settings update, [KiHeat]:(%s) applied.\r\n"), valStr);
        }
    }

    // KdHeat
    if(json["KdHeat"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.3f", json["KdHeat"].as<double>());
        temperature newVal = stringToFixedPoint(valStr);
        if(tempControl.cc.Kd_heat != newVal) {
            tempControl.cc.Kd_heat = newVal;
            saveSettings = true;
            Log.notice(F("Settings update, [KdHeat]:(%s) applied.\r\n"), valStr);
        }
    }

    // Temperature difference settings (use stringToTempDiff)
    // pidMax
    if(json["pidMax"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.1f", json["pidMax"].as<double>());
        temperature newVal = stringToTempDiff(valStr);
        if(tempControl.cc.pidMax != newVal) {
            tempControl.cc.pidMax = newVal;
            saveSettings = true;
            Log.notice(F("Settings update, [pidMax]:(%s) applied.\r\n"), valStr);
        }
    }

    // pidMaxHeat
    if(json["pidMaxHeat"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.1f", json["pidMaxHeat"].as<double>());
        temperature newVal = stringToTempDiff(valStr);
        if(tempControl.cc.pidMax_heat != newVal) {
            tempControl.cc.pidMax_heat = newVal;
            saveSettings = true;
            Log.notice(F("Settings update, [pidMaxHeat]:(%s) applied.\r\n"), valStr);
        }
    }

    // iMaxErr
    if(json["iMaxErr"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.1f", json["iMaxErr"].as<double>());
        temperature newVal = stringToTempDiff(valStr);
        if(tempControl.cc.iMaxError != newVal) {
            tempControl.cc.iMaxError = newVal;
            saveSettings = true;
            Log.notice(F("Settings update, [iMaxErr]:(%s) applied.\r\n"), valStr);
        }
    }

    // idleRangeH
    if(json["idleRangeH"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.1f", json["idleRangeH"].as<double>());
        temperature newVal = stringToTempDiff(valStr);
        if(tempControl.cc.idleRangeHigh != newVal) {
            tempControl.cc.idleRangeHigh = newVal;
            saveSettings = true;
            Log.notice(F("Settings update, [idleRangeH]:(%s) applied.\r\n"), valStr);
        }
    }

    // idleRangeL
    if(json["idleRangeL"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.1f", json["idleRangeL"].as<double>());
        temperature newVal = stringToTempDiff(valStr);
        if(tempControl.cc.idleRangeLow != newVal) {
            tempControl.cc.idleRangeLow = newVal;
            saveSettings = true;
            Log.notice(F("Settings update, [idleRangeL]:(%s) applied.\r\n"), valStr);
        }
    }

    // heatTargetH
    if(json["heatTargetH"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.1f", json["heatTargetH"].as<double>());
        temperature newVal = stringToTempDiff(valStr);
        if(tempControl.cc.heatingTargetUpper != newVal) {
            tempControl.cc.heatingTargetUpper = newVal;
            saveSettings = true;
            Log.notice(F("Settings update, [heatTargetH]:(%s) applied.\r\n"), valStr);
        }
    }

    // heatTargetL
    if(json["heatTargetL"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.1f", json["heatTargetL"].as<double>());
        temperature newVal = stringToTempDiff(valStr);
        if(tempControl.cc.heatingTargetLower != newVal) {
            tempControl.cc.heatingTargetLower = newVal;
            saveSettings = true;
            Log.notice(F("Settings update, [heatTargetL]:(%s) applied.\r\n"), valStr);
        }
    }

    // coolTargetH
    if(json["coolTargetH"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.1f", json["coolTargetH"].as<double>());
        temperature newVal = stringToTempDiff(valStr);
        if(tempControl.cc.coolingTargetUpper != newVal) {
            tempControl.cc.coolingTargetUpper = newVal;
            saveSettings = true;
            Log.notice(F("Settings update, [coolTargetH]:(%s) applied.\r\n"), valStr);
        }
    }

    // coolTargetL
    if(json["coolTargetL"].is<double>()) {
        char valStr[8];
        snprintf(valStr, sizeof(valStr), "%.1f", json["coolTargetL"].as<double>());
        temperature newVal = stringToTempDiff(valStr);
        if(tempControl.cc.coolingTargetLower != newVal) {
            tempControl.cc.coolingTargetLower = newVal;
            saveSettings = true;
            Log.notice(F("Settings update, [coolTargetL]:(%s) applied.\r\n"), valStr);
        }
    }

    // Estimate time settings (uint16_t)
    // maxHeatTimeForEst
    if(json["maxHeatTimeForEst"].is<uint16_t>()) {
        uint16_t newVal = json["maxHeatTimeForEst"].as<uint16_t>();
        if(tempControl.cc.maxHeatTimeForEstimate != newVal) {
            tempControl.cc.maxHeatTimeForEstimate = newVal;
            saveSettings = true;
            Log.notice(F("Settings update, [maxHeatTimeForEst]:(%u) applied.\r\n"), newVal);
        }
    }

    // maxCoolTimeForEst
    if(json["maxCoolTimeForEst"].is<uint16_t>()) {
        uint16_t newVal = json["maxCoolTimeForEst"].as<uint16_t>();
        if(tempControl.cc.maxCoolTimeForEstimate != newVal) {
            tempControl.cc.maxCoolTimeForEstimate = newVal;
            saveSettings = true;
            Log.notice(F("Settings update, [maxCoolTimeForEst]:(%u) applied.\r\n"), newVal);
        }
    }

    // Filter coefficients (uint8_t) - update both cc member and sensor
    // fridgeFastFilt
    if(json["fridgeFastFilt"].is<uint8_t>()) {
        uint8_t newVal = json["fridgeFastFilt"].as<uint8_t>();
        if(tempControl.cc.fridgeFastFilter != newVal) {
            tempControl.cc.fridgeFastFilter = newVal;
            tempControl.fridgeSensor->setFastFilterCoefficients(newVal);
            saveSettings = true;
            Log.notice(F("Settings update, [fridgeFastFilt]:(%u) applied.\r\n"), newVal);
        }
    }

    // fridgeSlowFilt
    if(json["fridgeSlowFilt"].is<uint8_t>()) {
        uint8_t newVal = json["fridgeSlowFilt"].as<uint8_t>();
        if(tempControl.cc.fridgeSlowFilter != newVal) {
            tempControl.cc.fridgeSlowFilter = newVal;
            tempControl.fridgeSensor->setSlowFilterCoefficients(newVal);
            saveSettings = true;
            Log.notice(F("Settings update, [fridgeSlowFilt]:(%u) applied.\r\n"), newVal);
        }
    }

    // fridgeSlopeFilt
    if(json["fridgeSlopeFilt"].is<uint8_t>()) {
        uint8_t newVal = json["fridgeSlopeFilt"].as<uint8_t>();
        if(tempControl.cc.fridgeSlopeFilter != newVal) {
            tempControl.cc.fridgeSlopeFilter = newVal;
            tempControl.fridgeSensor->setSlopeFilterCoefficients(newVal);
            saveSettings = true;
            Log.notice(F("Settings update, [fridgeSlopeFilt]:(%u) applied.\r\n"), newVal);
        }
    }

    // beerFastFilt
    if(json["beerFastFilt"].is<uint8_t>()) {
        uint8_t newVal = json["beerFastFilt"].as<uint8_t>();
        if(tempControl.cc.beerFastFilter != newVal) {
            tempControl.cc.beerFastFilter = newVal;
            tempControl.beerSensor->setFastFilterCoefficients(newVal);
            saveSettings = true;
            Log.notice(F("Settings update, [beerFastFilt]:(%u) applied.\r\n"), newVal);
        }
    }

    // beerSlowFilt
    if(json["beerSlowFilt"].is<uint8_t>()) {
        uint8_t newVal = json["beerSlowFilt"].as<uint8_t>();
        if(tempControl.cc.beerSlowFilter != newVal) {
            tempControl.cc.beerSlowFilter = newVal;
            tempControl.beerSensor->setSlowFilterCoefficients(newVal);
            saveSettings = true;
            Log.notice(F("Settings update, [beerSlowFilt]:(%u) applied.\r\n"), newVal);
        }
    }

    // beerSlopeFilt
    if(json["beerSlopeFilt"].is<uint8_t>()) {
        uint8_t newVal = json["beerSlopeFilt"].as<uint8_t>();
        if(tempControl.cc.beerSlopeFilter != newVal) {
            tempControl.cc.beerSlopeFilter = newVal;
            tempControl.beerSensor->setSlopeFilterCoefficients(newVal);
            saveSettings = true;
            Log.notice(F("Settings update, [beerSlopeFilt]:(%u) applied.\r\n"), newVal);
        }
    }

    // Boolean/uint8_t settings
    // lah (lightAsHeater)
    if(json["lah"].is<bool>()) {
        uint8_t newVal = json["lah"].as<bool>() ? 1 : 0;
        if(tempControl.cc.lightAsHeater != newVal) {
            tempControl.cc.lightAsHeater = newVal;
            saveSettings = true;
            Log.notice(F("Settings update, [lah]:(%u) applied.\r\n"), newVal);
        }
    }

    // hs (rotaryHalfSteps)
    if(json["hs"].is<bool>()) {
        uint8_t newVal = json["hs"].as<bool>() ? 1 : 0;
        if(tempControl.cc.rotaryHalfSteps != newVal) {
            tempControl.cc.rotaryHalfSteps = newVal;
            saveSettings = true;
            Log.notice(F("Settings update, [hs]:(%u) applied.\r\n"), newVal);
        }
    }

    // Save
    if(failCount) {
        Log.error(F("Error: Invalid control constants configuration.\r\n"));
    } else {
        if(saveSettings) {
            TempControl::storeConstants();
            // TODO - Force upstream cascade/send
        }
    }
    return failCount == 0;
}


bool processActionJson(const JsonDocument& json, bool triggerUpstreamUpdate) {

    if(!json["action"].is<const char*>()) {
        Log.warning(F("Action error - Action key is not a string.\r\n"));
        return false;
    }

    const char* action = json["action"].as<const char*>();

    if(strcmp(action, "restart") == 0) {
        Log.notice(F("Action [restart] received\r\n"));
        http_server.restart_requested = true;
        return true;
    }

    if(strcmp(action, "reset_connection") == 0) {
        Log.notice(F("Action [reset_connection] received\r\n"));
        http_server.wifi_reset_requested = true;
        http_server.restart_requested = true;  // A restart is implicit in wifi_reset_requested, but explicitly specifying here anyways
        return true;
    }

    if(strcmp(action, "reset_config") == 0) {
        Log.notice(F("Action [reset_config] received\r\n"));
        http_server.config_reset_requested = true;
        http_server.restart_requested = true;  // A restart is generally triggered when setting config_reset_requested, but explicitly specifying here anyways
        return true;
    }

#ifdef ENABLE_GLYCOL_LOGGING
    if(strcmp(action, "clear_glycol_log") == 0) {
        Log.notice(F("Action [clear_glycol_log] received\r\n"));
        glycolLog.clearLog();
        return true;
    }
#endif

    Log.warning(F("Action error - Unknown action: %s\r\n"), action);
    return false;
}



//-----------------------------------------------------------------------------------------


// There may be a way to combine the following using virtual functions, but I'm not going to worry about that for now
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


// About Page Handlers
void uptime(JsonDocument &doc) {
    Log.verbose(F("Serving uptime.\r\n"));

    doc["days"] = uptimeDays();
    doc["hours"] = uptimeHours();
    doc["minutes"] = uptimeMinutes();;
    doc["seconds"] = uptimeSeconds();
    doc["millis"] = uptimeMillis();

}


void heap(JsonDocument &doc) {
    Log.verbose(F("Serving heap information.\r\n"));

    const uint32_t free = ESP.getFreeHeap();
#ifdef ESP32
    const uint32_t max = ESP.getMaxAllocHeap();
#elif defined(ESP8266)
    const uint32_t max = ESP.getMaxFreeBlockSize();
#endif
    const uint8_t frag = 100 - (max * 100) / free;

    doc["free"] = free;
    doc["max"] = max;
    doc["frag"] = frag;
}


void reset_reason(JsonDocument &doc) {
    Log.verbose(F("Serving reset reason.\r\n"));

#ifdef ESP32
    const int reset = (int)esp_reset_reason();
    doc["reason"] = resetReason[reset];
    doc["description"] = resetDescription[reset];
#elif defined(ESP8266)
    doc["reason"] = ESP.getResetReason();
    doc["description"] = "N/A";
#endif

}


// Static page routing
void httpServer::setStaticPages() {
    // Define the base static page handlers
    asyncWebServer.serveStatic("/", FILESYSTEM, "/index.html").setCacheControl("max-age=600");
    asyncWebServer.serveStatic("/index.html", FILESYSTEM, "/index.html").setCacheControl("max-age=600");

    // Define Vue routes
    const char* vueRoutes[] = {
        "/upstream", 
        "/devices",
        "/about", 
        "/settings"
    };


    // Serve static pages for Vue routes and their trailing-slash versions
    for (const char* route : vueRoutes) {
        asyncWebServer.serveStatic(route, FILESYSTEM, "/index.html").setCacheControl("max-age=600");

        // Serve the same route with a trailing slash
        String routeWithSlash = String(route) + "/";
        asyncWebServer.serveStatic(routeWithSlash.c_str(), FILESYSTEM, "/index.html").setCacheControl("max-age=600");
    }

    // Legacy static page handlers
    // TODO - Determine if this can be deleted
    asyncWebServer.serveStatic("/404/", FILESYSTEM, "/404.html").setCacheControl("max-age=600");

}


void httpServer::setJsonPages() {
    struct Endpoint {
        const char* path;
        void (*handler)(JsonDocument&);
    };

    const Endpoint endpoints[] = {
        {"/api/version/", versionInfoJson},
        {"/api/lcd/", getLcdContentJson},
        {"/api/temps/", printTemperaturesJson},
        {"/api/cs/", tempControl.getControlSettingsDoc},
        {"/api/cc/", tempControl.getControlConstantsDoc},
        {"/api/cv/", tempControl.getControlVariablesDoc},
        {"/api/all_temp_control/", getFullTemperatureControlJson},
        {"/api/devices/", DeviceManager::enumerateHardware},
        {"/api/extended/", serveExtendedSettings},
        {"/api/upstream/", serveUpstreamSettings},
        {"/api/uptime/", uptime},
        {"/api/heap/", heap},
        {"/api/resetreason/", reset_reason},
    };

    for (const auto& endpoint : endpoints) {
        asyncWebServer.addHandler(new GetAsyncCallbackJsonWebHandler(endpoint.path, endpoint.handler));
    }
}


void httpServer::setPutPages() {
    struct Endpoint {
        const char* path;
        bool (*handler)(const JsonDocument&, bool);
    };

    const Endpoint endpoints[] = {
        {"/api/upstream/", processUpstreamConfigUpdateJson},
        {"/api/devices/", processDeviceUpdateJson},
        {"/api/mode/", processUpdateModeJson},
        {"/api/extended/", processExtendedSettingsJson},
        {"/api/cc/", processControlConstantsJson},
        {"/api/action/", processActionJson},
    };

    for (const auto& endpoint : endpoints) {
        asyncWebServer.addHandler(new PutAsyncCallbackJsonWebHandler(endpoint.path, endpoint.handler));
    }
}


void httpServer::init() {
    setStaticPages();
    setJsonPages();
    setPutPages();

    // File not found handler
    asyncWebServer.onNotFound([](AsyncWebServerRequest *request) {
        if (!http_server.handleFileRead(request, request->url())) {
            request->send(404, "text/plain", "Not Found");
        }
    });

    // DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");

    asyncWebServer.begin();
    Log.notice(F("HTTP server started. Open: http://%s.local/ to view application.\r\n"), WiFi.getHostname());
}



#endif
