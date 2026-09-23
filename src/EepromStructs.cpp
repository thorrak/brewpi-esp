#include <ArduinoJson.h>
#include <cstdlib>

#include "ESPEepromAccess.h"
#include "getGuid.h"
#include "EepromStructs.h"
#include "TemperatureFormats.h"
#include "TempControl.h" // For Modes definition
#include "JsonKeys.h"
#include "PiLink.h"
#include "Display.h"

#ifdef HAS_BLUETOOTH
// Tilts have address type 1, so replicating that here (even though this is wrong based on the address)
NimBLEAddress NoTiltDevice = NimBLEAddress("00:00:00:00:00:00", 1);
#endif


void JSONSaveable::writeJsonToFile(const char *filename, const ArduinoJson::JsonDocument& json_doc) {
    FILE* file_out = fs_open(filename, "w");

    if (!file_out) {
        // If the above fails, we weren't able to open the file for writing
        return;
    }

    size_t len = measureJson(json_doc);
    char* buf = (char*)malloc(len + 1);
    if (buf) {
        serializeJson(json_doc, buf, len + 1);
        fwrite(buf, 1, len, file_out);
        free(buf);
    }

    fclose(file_out);
}


ArduinoJson::JsonDocument JSONSaveable::readJsonFromFile(const char *filename) {
    JsonDocument json_doc;

    FILE* file_in = fs_open(filename, "r");
    if (!file_in) {
        // If the above fails, we weren't able to open the file for reading
        piLink.print("Failed to open file: ");
        piLink.print(filename);
        piLink.printNewLine();
        return json_doc;
    }

    fseek(file_in, 0, SEEK_END);
    size_t size = ftell(file_in);
    fseek(file_in, 0, SEEK_SET);

    char* buf = (char*)malloc(size + 1);
    if (buf) {
        fread(buf, 1, size, file_in);
        buf[size] = '\0';
        deserializeJson(json_doc, buf, size);
        free(buf);
    }

    fclose(file_in);
    return json_doc;
}



/**
 * \brief Constructor
 *
 * \see setDefaults
 */
ControlConstants::ControlConstants() {
    setDefaults();
}

/**
 * \brief Set reasonable default values for control constants
 */
void ControlConstants::setDefaults() {
    tempSettingMin = intToTemp(1);	// +1 deg Celsius
    tempSettingMax = intToTemp(30);	// +30 deg Celsius

    // control defines, also in fixed point format (7 int bits, 9 frac bits), so multiplied by 2^9=512
    Kp = intToTempDiff(5);	// +5
    Ki = intToTempDiff(1)/4; // +0.25
    Kd = intToTempDiff(-3)/2;	// -1.5
    iMaxError = intToTempDiff(5)/10;  // 0.5 deg

    // Stay Idle when fridge temperature is in this range
    idleRangeHigh = intToTempDiff(1);	// +1 deg Celsius
    idleRangeLow = intToTempDiff(-1);	// -1 deg Celsius

    // when peak falls between these limits, its good.
    heatingTargetUpper = intToTempDiff(3)/10;	// +0.3 deg Celsius
    heatingTargetLower = intToTempDiff(-2)/10;	// -0.2 deg Celsius
    coolingTargetUpper = intToTempDiff(2)/10;	// +0.2 deg Celsius
    coolingTargetLower = intToTempDiff(-3)/10;	// -0.3 deg Celsius

    // maximum history to take into account, in seconds
    maxHeatTimeForEstimate = 600;
    maxCoolTimeForEstimate = 1200,

    // Set filter coefficients. This is the b value. See FilterFixed.h for delay times.
    // The delay time is 3.33 * 2^b * number of cascades
    fridgeFastFilter = 1u;
    fridgeSlowFilter = 4u;
    fridgeSlopeFilter = 3u;
    beerFastFilter = 3u;
    beerSlowFilter = 4u;
    beerSlopeFilter = 4u;

    lightAsHeater = 0;
    rotaryHalfSteps = 0;
    pidMax = intToTempDiff(10);	// +/- 10 deg Celsius
    Kp_heat = Kp;
    Ki_heat = Ki;
    Kd_heat = Kd;
    pidMax_heat = pidMax;
    tempFormat = 'C';
}


/**
 * \brief Serialize control constants to JSON
 */
void ControlConstants::toJson(JsonDocument &doc) {
    // Load the constants into the JSON Doc
    doc[ControlConstantsKeys::tempMin] = tempSettingMin;
    doc[ControlConstantsKeys::tempMax] = tempSettingMax;

    doc[ControlConstantsKeys::kp] = Kp;
    doc[ControlConstantsKeys::ki] = Ki;
    doc[ControlConstantsKeys::kd] = Kd;
    doc[ControlConstantsKeys::maxError] = iMaxError;

    doc[ControlConstantsKeys::idleHigh] = idleRangeHigh;
    doc[ControlConstantsKeys::idleLow] = idleRangeLow;

    doc[ControlConstantsKeys::heatingUpper] = heatingTargetUpper;
    doc[ControlConstantsKeys::heatingLower] = heatingTargetLower;
    doc[ControlConstantsKeys::coolingUpper] = coolingTargetUpper;
    doc[ControlConstantsKeys::coolingLower] = coolingTargetLower;

    doc[ControlConstantsKeys::maxHeatEst] = maxHeatTimeForEstimate;
    doc[ControlConstantsKeys::maxCoolEst] = maxCoolTimeForEstimate;

    doc[ControlConstantsKeys::fridgeFilterFast] = fridgeFastFilter;
    doc[ControlConstantsKeys::fridgeFilterSlow] = fridgeSlowFilter;
    doc[ControlConstantsKeys::fridgeFilterSlope] = fridgeSlopeFilter;
    doc[ControlConstantsKeys::beerFilterFast] = beerFastFilter;
    doc[ControlConstantsKeys::beerFilterSlow] = beerSlowFilter;
    doc[ControlConstantsKeys::beerFilterSlope] = beerSlopeFilter;

    doc[ControlConstantsKeys::lightHeater] = lightAsHeater;
    doc[ControlConstantsKeys::rotaryHalfSteps] = rotaryHalfSteps;
    doc[ControlConstantsKeys::pidMax] = pidMax;
    doc[ControlConstantsKeys::kpHeat] = Kp_heat;
    doc[ControlConstantsKeys::kiHeat] = Ki_heat;
    doc[ControlConstantsKeys::kdHeat] = Kd_heat;
    doc[ControlConstantsKeys::pidMaxHeat] = pidMax_heat;
    char formatStr[2];
    formatStr[0] = tempFormat;
    formatStr[1] = '\0';
    doc[ControlConstantsKeys::tempFormat] = formatStr;
}

void ControlConstants::storeToFilesystem() {
    JsonDocument doc;

    toJson(doc);

    writeJsonToFile(ControlConstants::filename, doc);  // Write the json to the file
}

void ControlConstants::loadFromFilesystem() {
    // We start by setting the defaults, as we use them as the alternative to loaded values if the keys don't exist
    setDefaults();

    JsonDocument json_doc;
    json_doc = readJsonFromFile(ControlConstants::filename);

    // Load the constants from the JSON Doc
    if(json_doc[ControlConstantsKeys::tempMin].is<temperature>()) tempSettingMin = json_doc[ControlConstantsKeys::tempMin];
    if(json_doc[ControlConstantsKeys::tempMax].is<temperature>()) tempSettingMax = json_doc[ControlConstantsKeys::tempMax];

    if(json_doc[ControlConstantsKeys::kp].is<temperature>()) Kp = json_doc[ControlConstantsKeys::kp];
    if(json_doc[ControlConstantsKeys::ki].is<temperature>()) Ki = json_doc[ControlConstantsKeys::ki];
    if(json_doc[ControlConstantsKeys::kd].is<temperature>()) Kd = json_doc[ControlConstantsKeys::kd];
    if(json_doc[ControlConstantsKeys::maxError].is<temperature>()) iMaxError = json_doc[ControlConstantsKeys::maxError];

    if(json_doc[ControlConstantsKeys::idleHigh].is<temperature>()) idleRangeHigh = json_doc[ControlConstantsKeys::idleHigh];
    if(json_doc[ControlConstantsKeys::idleLow].is<temperature>()) idleRangeLow = json_doc[ControlConstantsKeys::idleLow];

    if(json_doc[ControlConstantsKeys::heatingUpper].is<temperature>()) heatingTargetUpper = json_doc[ControlConstantsKeys::heatingUpper];
    if(json_doc[ControlConstantsKeys::heatingLower].is<temperature>()) heatingTargetLower = json_doc[ControlConstantsKeys::heatingLower];
    if(json_doc[ControlConstantsKeys::coolingUpper].is<temperature>()) coolingTargetUpper = json_doc[ControlConstantsKeys::coolingUpper];
    if(json_doc[ControlConstantsKeys::coolingLower].is<temperature>()) coolingTargetLower = json_doc[ControlConstantsKeys::coolingLower];

    maxHeatTimeForEstimate = json_doc[ControlConstantsKeys::maxHeatEst] | maxHeatTimeForEstimate;
    maxCoolTimeForEstimate = json_doc[ControlConstantsKeys::maxCoolEst] | maxCoolTimeForEstimate;

    fridgeFastFilter = json_doc[ControlConstantsKeys::fridgeFilterFast] | fridgeFastFilter;
    fridgeSlowFilter = json_doc[ControlConstantsKeys::fridgeFilterSlow] | fridgeSlowFilter;
    fridgeSlopeFilter = json_doc[ControlConstantsKeys::fridgeFilterSlope] | fridgeSlopeFilter;
    beerFastFilter = json_doc[ControlConstantsKeys::beerFilterFast] | beerFastFilter;
    beerSlowFilter = json_doc[ControlConstantsKeys::beerFilterSlow] | beerSlowFilter;
    beerSlopeFilter = json_doc[ControlConstantsKeys::beerFilterSlope] | beerSlopeFilter;

    lightAsHeater = json_doc[ControlConstantsKeys::lightHeater] | lightAsHeater;
    rotaryHalfSteps = json_doc[ControlConstantsKeys::rotaryHalfSteps] | rotaryHalfSteps;
    if(json_doc[ControlConstantsKeys::pidMax].is<temperature>()) pidMax = json_doc[ControlConstantsKeys::pidMax];
    if(json_doc[ControlConstantsKeys::kpHeat].is<temperature>()) Kp_heat = json_doc[ControlConstantsKeys::kpHeat];
    if(json_doc[ControlConstantsKeys::kiHeat].is<temperature>()) Ki_heat = json_doc[ControlConstantsKeys::kiHeat];
    if(json_doc[ControlConstantsKeys::kdHeat].is<temperature>()) Kd_heat = json_doc[ControlConstantsKeys::kdHeat];
    if(json_doc[ControlConstantsKeys::pidMaxHeat].is<temperature>()) pidMax_heat = json_doc[ControlConstantsKeys::pidMaxHeat];

    if(json_doc[ControlConstantsKeys::tempFormat].is<const char *>()) {
        // This gets a bit strange due to the 6.20 changes to ArduinoJson
        char buf[2];
        strlcpy(buf, json_doc[ControlConstantsKeys::tempFormat].as<const char *>(), 2);
        tempFormat = buf[0];
    }
}




/**
 * \brief Constructor
 * \see setDefaults
 */
ControlSettings::ControlSettings() {
    // When creating an instance of the class, set defaults. Loading is done explicitly.
    setDefaults();
}


/**
 * \brief Set reasonable defaults for the control settings
 */
void ControlSettings::setDefaults() {
    // TODO - Check if I need to do setMode here
    beerSetting = intToTemp(20);
    fridgeSetting = intToTemp(20);
    heatEstimator = intToTempDiff(2)/10; // 0.2
    coolEstimator=intToTempDiff(5);
    mode = Modes::off;  // We do NOT do call set_mode here - that is handled in TempControl::loadDefaultSettings()
}


void ControlSettings::toJson(JsonDocument &doc) {
    // Load the settings into the JSON Doc
    doc[ControlSettingsKeys::beer] = beerSetting;
    doc[ControlSettingsKeys::fridge] = fridgeSetting;
    doc[ControlSettingsKeys::heatEst] = heatEstimator;
    doc[ControlSettingsKeys::coolEst] = coolEstimator;

    char modeStr[2];
    modeStr[0] = mode;
    modeStr[1] = '\0';
    doc[ControlSettingsKeys::mode] = modeStr;
}


void ControlSettings::storeToFilesystem() {
    JsonDocument doc;

    toJson(doc);

    writeJsonToFile(ControlSettings::filename, doc);  // Write the json to the file
}


void ControlSettings::loadFromFilesystem() {
    // We start by setting the defaults, as we use them as the alternative to loaded values if the keys don't exist
    setDefaults();

    JsonDocument json_doc;
    json_doc = readJsonFromFile(ControlSettings::filename);

    // Load the settings from the JSON Doc
    if(json_doc[ControlSettingsKeys::beer].is<temperature>()) beerSetting = json_doc[ControlSettingsKeys::beer];
    if(json_doc[ControlSettingsKeys::fridge].is<temperature>()) fridgeSetting = json_doc[ControlSettingsKeys::fridge];
    if(json_doc[ControlSettingsKeys::heatEst].is<temperature>()) heatEstimator = json_doc[ControlSettingsKeys::heatEst];
    if(json_doc[ControlSettingsKeys::coolEst].is<temperature>()) coolEstimator = json_doc[ControlSettingsKeys::coolEst];

    if(json_doc[ControlSettingsKeys::mode].is<const char *>()) {
        // This gets a bit strange due to the 6.20 changes to ArduinoJson
        char buf[2];
        strlcpy(buf, json_doc[ControlSettingsKeys::mode].as<const char *>(), 2);
        mode = buf[0];
    }
}



/**
 * \brief Constructor
 *
 * \see setDefaults
 */
ExtendedSettings::ExtendedSettings() {
    setDefaults();
}

/**
 * \brief Set default values for extended settings
 */
void ExtendedSettings::setDefaults() {
    invertTFT = false;
    glycol = false;
    glycolCoolingAlgorithm = GlycolCooling::Algorithm::PredictiveCoast;
    largeTFT = false;
    resetScreenOnPin = false;
#ifdef HAS_BLUETOOTH
    tiltGravSensor = NoTiltDevice;
#endif

}


/**
 * \brief Serialize extended settings to JSON
 */
void ExtendedSettings::toJson(JsonDocument &doc) {
    // Load the settings into the JSON Doc
    doc[ExtendedSettingsKeys::invertTFT] = invertTFT;
    doc[ExtendedSettingsKeys::glycol] = glycol;
    doc[ExtendedSettingsKeys::glycolCoolingAlgorithm] = GlycolCooling::selectionName(glycolCoolingAlgorithm);
    doc[ExtendedSettingsKeys::largeTFT] = largeTFT;
    doc[ExtendedSettingsKeys::resetScreenOnPin] = resetScreenOnPin;
#ifdef HAS_BLUETOOTH
    doc[ExtendedSettingsKeys::tiltGravSensor] = tiltGravSensor.toString();
#endif
}

/**
 * \brief Store extended settings to the filesystem
 */
void ExtendedSettings::storeToFilesystem() {
    JsonDocument doc;

    toJson(doc);

    writeJsonToFile(ExtendedSettings::filename, doc);  // Write the json to the file
}

/**
 * \brief Load extended settings from the filesystem
 */
void ExtendedSettings::loadFromFilesystem() {
    // We start by setting the defaults, as we use them as the alternative to loaded values if the keys don't exist
    setDefaults();

    JsonDocument json_doc;
    json_doc = readJsonFromFile(ExtendedSettings::filename);

    // Load the constants from the JSON Doc
    if(json_doc[ExtendedSettingsKeys::invertTFT].is<bool>()) invertTFT = json_doc[ExtendedSettingsKeys::invertTFT];
    if(json_doc[ExtendedSettingsKeys::glycol].is<bool>()) glycol = json_doc[ExtendedSettingsKeys::glycol];
    if(json_doc[ExtendedSettingsKeys::glycolCoolingAlgorithm].is<const char *>()) {
        // Missing or invalid values keep the predictive default used by older firmware.
        GlycolCooling::parseAlgorithm(json_doc[ExtendedSettingsKeys::glycolCoolingAlgorithm].as<const char *>(), glycolCoolingAlgorithm);
    }
    if(json_doc[ExtendedSettingsKeys::largeTFT].is<bool>()) largeTFT = json_doc[ExtendedSettingsKeys::largeTFT];
    if(json_doc[ExtendedSettingsKeys::resetScreenOnPin].is<bool>()) resetScreenOnPin = json_doc[ExtendedSettingsKeys::resetScreenOnPin];
#ifdef HAS_BLUETOOTH
    // Tilts use address type 1 ("random", which (correctly!) indicates they didn't buy a MAC block)
    if(json_doc[ExtendedSettingsKeys::tiltGravSensor].is<std::string>()) tiltGravSensor = NimBLEAddress(json_doc[ExtendedSettingsKeys::tiltGravSensor].as<std::string>(), 1);
#endif
}

/**
 * Validate all supplied extended settings before any setter can have side effects.
 * Omitted keys are unchanged, allowing old clients and selector-only updates.
 */
bool ExtendedSettings::validateSettingsJson(const JsonDocument &doc) {
    if (!doc.is<JsonObjectConst>()) return false;
    const char *boolKeys[] = {
        ExtendedSettingsKeys::invertTFT, ExtendedSettingsKeys::glycol,
        ExtendedSettingsKeys::largeTFT, ExtendedSettingsKeys::resetScreenOnPin
    };
    for (const char *key : boolKeys) {
        if (!doc[key].isUnbound() && !doc[key].is<bool>()) return false;
    }
    const JsonVariantConst selection = doc[ExtendedSettingsKeys::glycolCoolingAlgorithm];
    if (!selection.isUnbound()) {
        GlycolCooling::Algorithm parsed;
        if (!selection.is<const char *>() ||
            !GlycolCooling::parseAlgorithm(selection.as<const char *>(), parsed)) return false;
    }
#ifdef HAS_BLUETOOTH
    const JsonVariantConst tilt = doc[ExtendedSettingsKeys::tiltGravSensor];
    if (!tilt.isUnbound() && !tilt.is<std::string>()) return false;
#endif
    return true;
}

/**
 * \brief Process a single setting key/value pair
 *
 * \param kv - The parsed JsonPair of the setting
 */
void ExtendedSettings::processSettingKeypair(JsonPair kv) {
  // A good chunk of the conversions want a string representation of the value,
  // but the brewpi script presents the data as a number.  Prep a string
  // version in case we need it for this value.
  // NOTE: str_value is currently unused but retained for future use.
  char str_value[32] = "";
  if (kv.value().is<const char *>())
    strlcpy(str_value, kv.value().as<const char *>(), sizeof(str_value));
  else if (kv.value().is<float>()) {
    snprintf(str_value, sizeof(str_value), "%g", kv.value().as<float>());
  }

  if (kv.key() == ExtendedSettingsKeys::invertTFT) {
    setInvertTFT(kv.value().as<bool>());
  } else if (kv.key() == ExtendedSettingsKeys::glycol) {
    setGlycol(kv.value().as<bool>());
  } else if (kv.key() == ExtendedSettingsKeys::glycolCoolingAlgorithm) {
    GlycolCooling::Algorithm parsed;
    if (kv.value().is<const char *>() &&
        GlycolCooling::parseAlgorithm(kv.value().as<const char *>(), parsed)) {
      setGlycolCoolingAlgorithm(parsed);
    }
  } else if (kv.key() == ExtendedSettingsKeys::largeTFT) {
    setLargeTFT(kv.value().as<bool>());
  } else if (kv.key() == ExtendedSettingsKeys::resetScreenOnPin) {
    setResetScreenOnPin(kv.value().as<bool>());
  } 
  #ifdef HAS_BLUETOOTH
  else if (kv.key() == ExtendedSettingsKeys::tiltGravSensor) {
    // Tilts use address type 1 ("random", which (correctly!) indicates they didn't buy a MAC block)
    NimBLEAddress addr = NimBLEAddress(kv.value().as<std::string>(), 1);
    setTiltGravSensor(addr);
  }
  #endif
}

/**
 * \brief Set the glycol mode
 *
 * When switching between compressor and glycol modes, the controller is
 * set to 'off' to prevent unexpected behavior. Sensor assignments remain
 * unchanged.
 *
 * \param setting - The new setting
 */
void ExtendedSettings::setGlycol(bool setting) {
    if (glycol != setting) {
        // Controller type is changing - force mode to 'off'
        // This is done via TempControl to ensure proper state management
        extern TempControl tempControl;
        tempControl.setMode(Modes::off, true);
    }
    glycol = setting;
    minTimes.setDefaults();
    storeToFilesystem();
}

bool ExtendedSettings::setGlycolCoolingAlgorithm(GlycolCooling::Algorithm setting) {
    if (setting != GlycolCooling::Algorithm::PredictiveCoast &&
        setting != GlycolCooling::Algorithm::PulseDose) return false;
    // Queue the selection only. GlycolMode handles relay timing and discards
    // interrupted observations when it switches controllers on its next tick.
    glycolCoolingAlgorithm = setting;
    return true;
}


/**
 * \brief Set the large TFT mode
 *
 * \param setting - The new setting
 */
void ExtendedSettings::setLargeTFT(bool setting) {
    largeTFT = setting;
    display.init();
    display.printStationaryText();
    display.printState();
}

/**
 * \brief Set the TFT inversion mode
 *
 * \param setting - The new setting
 */
void ExtendedSettings::setInvertTFT(bool setting) {
    invertTFT = setting;
    display.init();
    display.printStationaryText();
	display.printState();
}

/**
 * \brief Set if the screen should be reset when an ArduinoActuatorPin (ie. relay) toggles
 *
 * \param setting - The new setting
 */
void ExtendedSettings::setResetScreenOnPin(bool setting) {
    resetScreenOnPin = setting;

    // The only thing we must do here is change the setting, but we'll reinit the screen as well 
    // in case the user is enabling this as a result of their screen being frozen
    if(setting)
        display.printAll();
}

#ifdef HAS_BLUETOOTH
/**
 * \brief Set the Tilt color to be used as the gravity sensor
 *
 * \param setting - The new setting
 */
void ExtendedSettings::setTiltGravSensor(NimBLEAddress setting) {
    tiltGravSensor = setting;
}
#endif


/**
 * \brief Constructor
 *
 * \see setDefaults
 */
UpstreamSettings::UpstreamSettings() {
    setDefaults();
}

/**
 * \brief Set default values for extended settings
 */
void UpstreamSettings::setDefaults() {
    upstreamHost[0] = '\0';
    upstreamPort = 80;
    deviceID[0] = '\0';
    username[0] = '\0';
    apiKey[0] = '\0';
    upstreamRegistrationError = upstreamRegErrorT::NOT_ATTEMPTED_REGISTRATION;
}


/**
 * \brief Serialize extended settings to JSON
 */
void UpstreamSettings::toJson(JsonDocument &doc) {
    char guid[20];
    getGuid(guid);

    // Load the settings into the JSON Doc
    doc[UpstreamSettingsKeys::upstreamHost] = upstreamHost;
    doc[UpstreamSettingsKeys::upstreamPort] = upstreamPort;
    doc[UpstreamSettingsKeys::deviceID] = deviceID;
    doc[UpstreamSettingsKeys::username] = username;
    doc[UpstreamSettingsKeys::apiKey] = apiKey;
    doc[UpstreamSettingsKeys::upstreamRegistrationError] = (uint16_t) upstreamRegistrationError;
    doc[UpstreamSettingsKeys::guid] = guid;
}

/**
 * \brief Store extended settings to the filesystem
 */
void UpstreamSettings::storeToFilesystem() {
    JsonDocument doc;
    toJson(doc);

    writeJsonToFile(UpstreamSettings::filename, doc);  // Write the json to the file
}

/**
 * \brief Load extended settings from the filesystem
 */
void UpstreamSettings::loadFromFilesystem() {
    // We start by setting the defaults, as we use them as the alternative to loaded values if the keys don't exist
    setDefaults();

    JsonDocument json_doc;
    json_doc = readJsonFromFile(UpstreamSettings::filename);

    // Load the constants from the JSON Doc
    if(json_doc[UpstreamSettingsKeys::upstreamHost].is<const char *>()) strlcpy(upstreamHost, json_doc[UpstreamSettingsKeys::upstreamHost], 128);
    if(json_doc[UpstreamSettingsKeys::upstreamPort].is<uint16_t>()) upstreamPort = json_doc[UpstreamSettingsKeys::upstreamPort];
    if(json_doc[UpstreamSettingsKeys::deviceID].is<const char *>()) strlcpy(deviceID, json_doc[UpstreamSettingsKeys::deviceID], 40);
    if(json_doc[UpstreamSettingsKeys::username].is<const char *>()) strlcpy(username, json_doc[UpstreamSettingsKeys::username], 128);
    if(json_doc[UpstreamSettingsKeys::apiKey].is<const char*>()) strlcpy(apiKey, json_doc[UpstreamSettingsKeys::apiKey], 40);

}

/**
 * \brief Process a single setting key/value pair
 *
 * \param kv - The parsed JsonPair of the setting
 */
void UpstreamSettings::processSettingKeypair(JsonPair kv) {
  if (kv.key() == UpstreamSettingsKeys::upstreamHost) {
    strlcpy(upstreamHost, kv.value().as<const char *>(), 128);
  } else if (kv.key() == UpstreamSettingsKeys::upstreamPort) {
    upstreamPort = kv.value().as<uint16_t>();
  } else if (kv.key() == UpstreamSettingsKeys::deviceID) {
    strlcpy(deviceID, kv.value().as<const char *>(), 40);
  } else if (kv.key() == UpstreamSettingsKeys::username) {
    strlcpy(username, kv.value().as<const char *>(), 128);
  } else if (kv.key() == UpstreamSettingsKeys::apiKey) {
    strlcpy(apiKey, kv.value().as<const char *>(), 40);
  }

}


/**
 * @brief Check if the device is registered with the upstream server
 * 
 * @return true - The device is registered
 * @return false - The device is not registered
 */
bool UpstreamSettings::isRegistered() {
    return (upstreamRegistrationError == UpstreamSettings::upstreamRegErrorT::NO_ERROR && strlen(deviceID) > 0 && strlen(apiKey) > 0);
}
