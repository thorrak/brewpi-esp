/*
 * Copyright 2012-2013 BrewPi/Elco Jacobs.
 *
 * This file is part of BrewPi.
 * 
 * BrewPi is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * BrewPi is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with BrewPi.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "Brewpi.h"

#include "Pins.h"
#include <limits.h>
#include <math.h>

#include "TemperatureFormats.h"
#include "TempControl.h"
#include "PiLink.h"
#include "TempSensor.h"
#include "Ticks.h"
#include "TempSensorMock.h"
#include "EepromManager.h"
#include "TempSensorDisconnected.h"
#include "RotaryEncoder.h"
#include "ChamberMode.h"
#include "GlycolMode.h"

TempControl tempControl;
MinTimes minTimes;

#if TEMP_CONTROL_STATIC

extern ValueSensor<bool> defaultSensor;
extern ValueActuator defaultActuator;
extern DisconnectedTempSensor defaultTempSensor;

// These sensors are switched out to implement multi-chamber.
TempSensor* TempControl::beerSensor;
TempSensor* TempControl::fridgeSensor;
BasicTempSensor* TempControl::ambientSensor = &defaultTempSensor;
temperature ambientTemp = TEMP_SENSOR_DISCONNECTED;  // Updated in the updateTemperatures() function to prevent reading from the sensor in an async web response


Actuator* TempControl::heater = &defaultActuator;
Actuator* TempControl::cooler = &defaultActuator;
Actuator* TempControl::light = &defaultActuator;
Actuator* TempControl::fan = &defaultActuator;

ValueActuator cameraLightState;
AutoOffActuator TempControl::cameraLight(600, &cameraLightState);	// timeout 10 min
Sensor<bool>* TempControl::door = &defaultSensor;

// Control parameters
ControlConstants TempControl::cc;
ControlSettings TempControl::cs;
ControlVariables TempControl::cv;

// Glycol mode: Predictive bang-bang control parameters
GlycolLearnedParams TempControl::glycolLearned;
GlycolConfig TempControl::glycolConfig;
GlycolRuntimeState TempControl::glycolRuntime;
    
    // State variables
uint8_t TempControl::state;
bool TempControl::doPosPeakDetect;
bool TempControl::doNegPeakDetect;
bool TempControl::doorOpen;
    
    // keep track of beer setting stored in EEPROM
temperature TempControl::storedBeerSetting;
    
    // Timers
uint16_t TempControl::lastIdleTime;
uint16_t TempControl::lastHeatTime;
uint16_t TempControl::lastCoolTime;
uint16_t TempControl::waitTime;
#endif


#ifndef min
#define min _min
#endif

#ifndef max
#define max _max
#endif

namespace {

bool isBeerMode(const ControlSettings& settings) {
    return settings.mode == Modes::beerConstant || settings.mode == Modes::beerProfile;
}

bool useGlycolBeerMode(const ControlSettings& settings) {
    return extendedSettings.glycol && isBeerMode(settings);
}

} // namespace

ControlContext TempControl::makeControlContext() {
    return ControlContext{
        cc,
        cs,
        cv,
        minTimes,
        beerSensor,
        fridgeSensor,
        heater,
        cooler,
        light,
        state,
        lastIdleTime,
        lastHeatTime,
        lastCoolTime,
        waitTime,
    };
}


/**
 * Initialize the temp control system.  Done at startup.
 */
void TempControl::init(){
    state=IDLE;
    cs.mode = Modes::off;

    minTimes.setDefaults();  // Update the min times before we initialize temp control

    cameraLight.setActive(false);

    // this is for cases where the device manager hasn't configured beer/fridge sensor.	
    if (beerSensor==NULL) {
        beerSensor = new TempSensor(TEMP_SENSOR_TYPE_BEER, &defaultTempSensor);
        beerSensor->init();
    }
        
    if (fridgeSensor==NULL) {
        fridgeSensor = new TempSensor(TEMP_SENSOR_TYPE_FRIDGE, &defaultTempSensor);
        fridgeSensor->init();
    }
    
    updateTemperatures();
    reset();

    // Initialize glycol mode parameters
    loadGlycolParams();

    // Do not allow heating/cooling directly after reset.
    // A failing script + CRON + Arduino uno (which resets on serial connect) could damage the compressor
    // For test purposes, set these to -3600 to eliminate waiting after reset
    lastHeatTime = 0;
    lastCoolTime = 0;
}


/**
 * Reset the peak detect flags
 */
void TempControl::reset(){
    doPosPeakDetect=false;
    doNegPeakDetect=false;
}


/**
 * Get an update from a sensor.
 *
 * @param sensor - Sensor to check
 */
void updateSensor(TempSensor* sensor) {
    sensor->update();
    if(!sensor->isConnected()) {
        sensor->init();
    }
}


/**
 * Get the current cached room temperature.
 *
 * @return Current cached room temperature
 */
temperature TempControl::getRoomTemp() {
    return ambientTemp;
}


/**
 * Update all installed temp sensors.
 *
 * This updates beer, fridge & room sensors.
 */
void TempControl::updateTemperatures(){
    
    updateSensor(beerSensor);
    updateSensor(fridgeSensor);

    ambientTemp = ambientSensor->read();  // Update ambient sensor here rather to prevent being updated as part of an async web response
    
    // If no sensor is connected, this does nothing.
    // This prevents a delay in serial response because the value is not up to date.
    if(ambientTemp == TEMP_SENSOR_DISCONNECTED){
        ambientSensor->init(); // try to reconnect a disconnected, but installed sensor
    }
}

void TempControl::updatePID(){
    static unsigned char integralUpdateCounter = 0;
    if(isBeerMode(cs)){
        if(cs.beerSetting == INVALID_TEMP){
            // beer setting is not updated yet
            // set fridge to unknown too
            cs.fridgeSetting = INVALID_TEMP;
            return;
        }

        // Allow PID to continue using cached filter values for up to 60 seconds during temporary disconnections.
        // The filters retain their last valid values, providing resilience against brief sensor dropouts.
        // After 60 failed reads (~60 seconds), the cached data is too stale to be reliable.
        if(beerSensor->getFailedReadCount() > 60) {
            return;
        }

        // fridgeSensor is required for compressor-based cooling - In glycol mode, only the beer sensor is required
        if(!extendedSettings.glycol && fridgeSensor->getFailedReadCount() > 60) {
            return;
        }
        
        // In compressor cooling, fridge setting is calculated with PID algorithm. Beer temperature error is input to PID
        // In glycol chilling, still calculate beer temperature error and slope - used by both modes
        cv.beerDiff =  cs.beerSetting - beerSensor->readSlowFiltered();
        cv.beerSlope = beerSensor->readSlope();

        ControlContext controlCtx = makeControlContext();

        if(useGlycolBeerMode(cs)) {
            // ===== GLYCOL MODE =====
            // Cooling is predictive bang-bang; heating is beer-only time-proportional PID.

            // Set fridgeSetting to INVALID_TEMP since it's not used in glycol mode
            cs.fridgeSetting = INVALID_TEMP;

            GlycolMode::Context glycolCtx(controlCtx, glycolLearned, glycolConfig, glycolRuntime);
            GlycolMode::updatePID(glycolCtx, integralUpdateCounter);

        } else {
            ChamberMode::Context chamberCtx(controlCtx, doPosPeakDetect, doNegPeakDetect);
            ChamberMode::updatePID(chamberCtx, integralUpdateCounter);
        }
    }
    else if(cs.mode == Modes::fridgeConstant){
        // FridgeTemperature is set manually, use INVALID_TEMP to indicate beer temp is not active
        cs.beerSetting = INVALID_TEMP;
    }
}

void TempControl::updateState(){
    //update state
    bool stayIdle = false;
    bool newDoorOpen = door->sense();
        
    if(newDoorOpen!=doorOpen) {
        doorOpen = newDoorOpen;
        String annotation = "";
        annotation += "Fridge door ";
        annotation += doorOpen ? "opened" : "closed";
        piLink.printTemperatures(0, annotation.c_str());
    }

    if(cs.mode == Modes::off){
        state = STATE_OFF;
        stayIdle = true;
    } else {
        // Check for invalid settings or disconnected sensors
        // In glycol mode, fridge sensor is optional; in compressor mode it's required
        bool fridgeRequired = !extendedSettings.glycol;
        bool fridgeInvalid = (fridgeRequired && (!fridgeSensor->isConnected() || cs.fridgeSetting == INVALID_TEMP));
        bool beerInvalid = (!beerSensor->isConnected() && isBeerMode(cs));

        if(fridgeInvalid || beerInvalid) {
            // Stay idle when a required sensor is disconnected or settings are invalid
            state = IDLE;
            stayIdle = true;
        }
    }

    ControlContext controlCtx = makeControlContext();

    // ===== GLYCOL MODE STATE MACHINE =====
    // Uses predictive bang-bang control (see GLYCOL_COOLING_ALGORITHM.md)
    if(useGlycolBeerMode(cs) && !stayIdle) {
        GlycolMode::Context glycolCtx(controlCtx, glycolLearned, glycolConfig, glycolRuntime);
        if (GlycolMode::updateState(glycolCtx)) {
            storeGlycolParams();
        }
        // Glycol mode uses its own state machine - skip compressor mode logic
        return;
    }

    ChamberMode::Context chamberCtx(controlCtx, doPosPeakDetect, doNegPeakDetect);
    ChamberMode::updateState(chamberCtx, stayIdle);
}

void TempControl::updateOutputs() {
    if (cs.mode==Modes::test)
        return;
        
    cameraLight.update();
    bool heating = stateIsHeating();
    bool cooling = stateIsCooling();
    cooler->setActive(cooling);		
    heater->setActive(!cc.lightAsHeater && heating);	
    light->setActive(isDoorOpen() || (cc.lightAsHeater && heating) || cameraLightState.isActive());	
    fan->setActive(heating || cooling);
}


void TempControl::detectPeaks(){
    // Peak detection auto-tuning is designed for compressor mode
    // In glycol mode, use overshoot prediction instead
    if(extendedSettings.glycol) {
        return;
    }
    ControlContext controlCtx = makeControlContext();
    ChamberMode::Context chamberCtx(controlCtx, doPosPeakDetect, doNegPeakDetect);
    // Mode controllers own their learning algorithms; TempControl owns
    // persistence and performs it only when learned settings actually change.
    if (ChamberMode::detectPeaks(chamberCtx)) {
        storeSettings();
    }
}

/**
 * Get time since the cooler was last ran
 */
uint16_t TempControl::timeSinceCooling(){
    return ticks.timeSince(lastCoolTime);
}

/**
 * Get time since the heater was last ran
 */
uint16_t TempControl::timeSinceHeating(){
    return ticks.timeSince(lastHeatTime);
}

/**
 * Get time that the controller has been neither cooling nor heating
 */
uint16_t TempControl::timeSinceIdle(){
    return ticks.timeSince(lastIdleTime);
}

/**
 * Load default settings
 */
void TempControl::loadDefaultSettings(){
    cs.setDefaults();
#if BREWPI_EMULATE
    setMode(Modes::beerConstant);
#else	
    setMode(Modes::off);
#endif	
}

/**
 * Store control constants to EEPROM.
 */
void TempControl::storeConstants() {
    // Now that control constants are an object, use that for loading/saving
    cc.storeToFilesystem();
}

/**
 * Load control constants from EEPROM
 */
void TempControl::loadConstants(){
  // Now that control constants are an object, use that for loading/saving
  cc.loadFromFilesystem();
  initFilters();
}


/**
 * Write new settings to EEPROM to be able to reload them after a reset
 * The update functions only write to EEPROM if the value has changed
 */
void TempControl::storeSettings(){
    cs.storeToFilesystem();
    storedBeerSetting = cs.beerSetting;
}

/**
 * Read settings from EEPROM
 */
void TempControl::loadSettings(){
  cs.loadFromFilesystem();
    logDebug("loaded settings");
    storedBeerSetting = cs.beerSetting;
    setMode(cs.mode, true);		// force the mode update
}

/**
 * Load default control constants
 */
void TempControl::loadDefaultConstants(){
  // Rather than using memcpy to copy over a default struct of settings, use the class method
  // (We have the flash space to do this the less flash-conscious way)
  cc.setDefaults();
    initFilters();
}

/**
 * Initialize the fridge & beer sensor filter coefficients
 *
 * @see CascadedFilter
 */
void TempControl::initFilters()
{
    fridgeSensor->setFastFilterCoefficients(cc.fridgeFastFilter);
    fridgeSensor->setSlowFilterCoefficients(cc.fridgeSlowFilter);
    fridgeSensor->setSlopeFilterCoefficients(cc.fridgeSlopeFilter);
    beerSensor->setFastFilterCoefficients(cc.beerFastFilter);
    beerSensor->setSlowFilterCoefficients(cc.beerSlowFilter);
    beerSensor->setSlopeFilterCoefficients(cc.beerSlopeFilter);		
}


/**
 * Set control mode
 *
 * @param newMode - New control mode
 * @param force - Set the mode & reset control state, even if controler is already in the requested mode
 */
void TempControl::setMode(char newMode, bool force){
    // In glycol mode, redirect fridge constant to beer constant
    // (Ideally, this won't ever get triggered, but handling it here just in case the web interface is old or out of sync)
    if(extendedSettings.glycol && newMode == Modes::fridgeConstant) {
        logDebug("Glycol mode: redirecting fridge constant to beer constant");
        newMode = Modes::beerConstant;
    }

    logDebug("TempControl::setMode from %c to %c", cs.mode, newMode);

    if(newMode != cs.mode || state == WAITING_TO_HEAT || state == WAITING_TO_COOL || state == WAITING_FOR_PEAK_DETECT){
        state = IDLE;
        force = true;
    }
    if (force) {
        cs.mode = newMode;
        if(newMode == Modes::off){
            cs.beerSetting = INVALID_TEMP;
            cs.fridgeSetting = INVALID_TEMP;
        }
        TempControl::storeSettings();
    }
}


/**
 * Get current beer temperature
 */
temperature TempControl::getBeerTemp(){
    if(beerSensor->isConnected()){
        return beerSensor->readFastFiltered();
    }
    else{
        return INVALID_TEMP;
    }
}

/**
 * Get current beer target temperature
 */
temperature TempControl::getBeerSetting(){
    return cs.beerSetting;
}


/**
 * Get current fridge temperature
 */
temperature TempControl::getFridgeTemp(){
    if(fridgeSensor->isConnected()){
        return fridgeSensor->readFastFiltered();
    } else {
        return INVALID_TEMP;
    }
}

/**
 * Get current fridge target temperature
 */
temperature TempControl::getFridgeSetting(){
    return cs.fridgeSetting;
}


/**
 * Set desired beer temperature
 *
 * @param newTemp - new target temperature
 */
void TempControl::setBeerTemp(temperature newTemp){
    temperature oldBeerSetting = cs.beerSetting;
    cs.beerSetting= newTemp;
    if(abs(oldBeerSetting - newTemp) > intToTempDiff(1)/2){ // more than half degree C difference with old setting
        reset(); // reset controller
    }
    updatePID();
    updateState();
    if(cs.mode != Modes::beerProfile || abs(storedBeerSetting - newTemp) > intToTempDiff(1)/4){
        // more than 1/4 degree C difference with EEPROM
        // Do not store settings every time in profile mode, because EEPROM has limited number of write cycles.
        // A temperature ramp would cause a lot of writes
        // If Raspberry Pi is connected, it will update the settings anyway. This is just a safety feature.
        TempControl::storeSettings();
    }
}

/**
 * Set desired fridge temperature
 *
 * @param newTemp - New target temperature
 */
void TempControl::setFridgeTemp(temperature newTemp){
    cs.fridgeSetting = newTemp;
    reset(); // reset peak detection and PID
    updatePID();
    updateState();
    TempControl::storeSettings();
}

/**
 * Check if current state is cooling (or waiting to cool)
 */
bool TempControl::stateIsCooling(){
    return (state==COOLING || state==COOLING_MIN_TIME);
}

/**
 * Check if current state is heating (or waiting to heat)
 */
bool TempControl::stateIsHeating(){
    return (state==HEATING || state==HEATING_MIN_TIME);
}


/**
 * \brief Get current control variables as JsonDocument
 *
 * \param doc - Reference to JsonDocument to populate
 */
void TempControl::getControlVariablesDoc(JsonDocument& doc) {
  doc["beerDiff"] = tempDiffToDouble(cv.beerDiff, Config::TempFormat::tempDiffDecimals);
  doc["diffIntegral"] = tempDiffToDouble(cv.diffIntegral, Config::TempFormat::tempDiffDecimals);
  doc["beerSlope"] = tempDiffToDouble(cv.beerSlope, Config::TempFormat::tempDiffDecimals);

  doc["p"] = fixedPointToDouble(cv.p, Config::TempFormat::fixedPointDecimals);
  doc["i"] = fixedPointToDouble(cv.i, Config::TempFormat::fixedPointDecimals);
  doc["d"] = fixedPointToDouble(cv.d, Config::TempFormat::fixedPointDecimals);

  doc["estPeak"] = tempToDouble(cv.estimatedPeak, Config::TempFormat::tempDecimals);
  doc["negPeakEst"] = tempToDouble(cv.negPeakEstimate, Config::TempFormat::tempDecimals);
  doc["posPeakEst"] = tempToDouble(cv.posPeakEstimate, Config::TempFormat::tempDecimals);
  doc["negPeak"] = tempToDouble(cv.negPeak, Config::TempFormat::tempDecimals);
  doc["posPeak"] = tempToDouble(cv.posPeak, Config::TempFormat::tempDecimals);
}

/**
 * \brief Get current control constants as JsonDocument
 *
 * \param doc - Reference to JsonDocument to populate
 */
void TempControl::getControlConstantsDoc(JsonDocument& doc) {
  doc["tempFormat"] = String(cc.tempFormat);

  doc["tempSetMin"] = tempToDouble(cc.tempSettingMin, Config::TempFormat::tempDecimals);
  doc["tempSetMax"] = tempToDouble(cc.tempSettingMax, Config::TempFormat::tempDecimals);
  doc["pidMax"] = tempDiffToDouble(cc.pidMax, Config::TempFormat::tempDiffDecimals);
  doc["Kp"] = fixedPointToDouble(cc.Kp, Config::TempFormat::fixedPointDecimals);
  doc["Ki"] = fixedPointToDouble(cc.Ki, Config::TempFormat::fixedPointDecimals);
  doc["Kd"] = fixedPointToDouble(cc.Kd, Config::TempFormat::fixedPointDecimals);

  doc["iMaxErr"] = tempDiffToDouble(cc.iMaxError, Config::TempFormat::tempDiffDecimals);
  doc["idleRangeH"] = tempDiffToDouble(cc.idleRangeHigh, Config::TempFormat::tempDiffDecimals);
  doc["idleRangeL"] = tempDiffToDouble(cc.idleRangeLow, Config::TempFormat::tempDiffDecimals);
  doc["heatTargetH"] = tempDiffToDouble(cc.heatingTargetUpper, Config::TempFormat::tempDiffDecimals);
  doc["heatTargetL"] = tempDiffToDouble(cc.heatingTargetLower, Config::TempFormat::tempDiffDecimals);
  doc["coolTargetH"] = tempDiffToDouble(cc.coolingTargetUpper, Config::TempFormat::tempDiffDecimals);
  doc["coolTargetL"] = tempDiffToDouble(cc.coolingTargetLower, Config::TempFormat::tempDiffDecimals);
  doc["maxHeatTimeForEst"] = tempControl.cc.maxHeatTimeForEstimate;
  doc["maxCoolTimeForEst"] = tempControl.cc.maxCoolTimeForEstimate;
  doc["fridgeFastFilt"] = tempControl.cc.fridgeFastFilter;
  doc["fridgeSlowFilt"] = tempControl.cc.fridgeSlowFilter;
  doc["fridgeSlopeFilt"] = tempControl.cc.fridgeSlopeFilter;
  doc["beerFastFilt"] = tempControl.cc.beerFastFilter;
  doc["beerSlowFilt"] = tempControl.cc.beerSlowFilter;
  doc["beerSlopeFilt"] = tempControl.cc.beerSlopeFilter;
  doc["lah"] = tempControl.cc.lightAsHeater;
  doc["hs"] = tempControl.cc.rotaryHalfSteps;

  // Glycol mode: Separate heating PID constants (always used in glycol mode)
  doc["KpHeat"] = fixedPointToDouble(tempControl.cc.Kp_heat, Config::TempFormat::fixedPointDecimals);
  doc["KiHeat"] = fixedPointToDouble(tempControl.cc.Ki_heat, Config::TempFormat::fixedPointDecimals);
  doc["KdHeat"] = fixedPointToDouble(tempControl.cc.Kd_heat, Config::TempFormat::fixedPointDecimals);
  doc["pidMaxHeat"] = tempDiffToDouble(tempControl.cc.pidMax_heat, Config::TempFormat::tempDiffDecimals);
}


/**
 * \brief Get current control settings as a JsonDocument
 *
 * \param doc - Reference to JsonDocument to populate
 */
void TempControl::getControlSettingsDoc(JsonDocument& doc) {
  doc["mode"] = String(cs.mode);
  doc["beerSet"] = tempToDouble(cs.beerSetting, Config::TempFormat::tempDecimals);
  doc["fridgeSet"] = tempToDouble(cs.fridgeSetting, Config::TempFormat::tempDecimals);
  doc["heatEst"] = fixedPointToDouble(cs.heatEstimator, Config::TempFormat::fixedPointDecimals);
  doc["coolEst"] = fixedPointToDouble(cs.coolEstimator, Config::TempFormat::fixedPointDecimals);
}



MinTimes::MinTimes() {
    settings_choice = MIN_TIMES_DEFAULT;
    setDefaults();
}

void MinTimes::setDefaults() {
    // Glycol mode has different timing requirements than compressor mode
    // Glycol systems can respond faster and don't need compressor protection delays
    if(extendedSettings.glycol && settings_choice != MIN_TIMES_CUSTOM) {
        // Glycol Mode - Time-proportional control with 1000s window
        MIN_COOL_OFF_TIME = 30;  // Legacy, not used in time-proportional mode
        MIN_HEAT_OFF_TIME = 30;
        MIN_COOL_ON_TIME = 30;
        MIN_HEAT_ON_TIME = 30;

        MIN_COOL_OFF_TIME_FRIDGE_CONSTANT = 30;
        MIN_SWITCH_TIME = 60;
        COOL_PEAK_DETECT_TIME = 300;
        HEAT_PEAK_DETECT_TIME = 300;

        // Time-proportional control settings
        GLYCOL_WINDOW_PERIOD = 1000;  // 1000s (~16.67 min) window for duty cycle control
        GLYCOL_MIN_ON_TIME = 10;      // 10s minimum on-time (1% minimum duty cycle)
    } else if(settings_choice == MIN_TIMES_DEFAULT) {
        // Compressor Mode - Normal Delay
        MIN_COOL_OFF_TIME = 300;
        MIN_HEAT_OFF_TIME = 300;
        MIN_COOL_ON_TIME = 180;
        MIN_HEAT_ON_TIME = 180;

        MIN_COOL_OFF_TIME_FRIDGE_CONSTANT= 600;
        MIN_SWITCH_TIME = 600;
        COOL_PEAK_DETECT_TIME = 1800;
        HEAT_PEAK_DETECT_TIME = 900;

        // Glycol settings (not used in compressor mode, but set for consistency)
        GLYCOL_WINDOW_PERIOD = 1000;
        GLYCOL_MIN_ON_TIME = 10;
    } else if(settings_choice == MIN_TIMES_LOW_DELAY) {
        // Compressor Mode - Low Delay
        MIN_COOL_OFF_TIME = 60;
        MIN_HEAT_OFF_TIME = 300;
        MIN_COOL_ON_TIME = 20;
        MIN_HEAT_ON_TIME = 180;

        MIN_COOL_OFF_TIME_FRIDGE_CONSTANT= 60;
        MIN_SWITCH_TIME = 600;
        COOL_PEAK_DETECT_TIME = 1800;
        HEAT_PEAK_DETECT_TIME = 900;

        // Glycol settings (not used in compressor mode, but set for consistency)
        GLYCOL_WINDOW_PERIOD = 1000;
        GLYCOL_MIN_ON_TIME = 10;
    } else {
        // Custom Delay -- Effectively a noop, as the defaults are set when the json gets loaded
    }
}

uint16_t TempControl::getMinCoolOnTime() {
    return minTimes.MIN_COOL_ON_TIME;
}

uint16_t TempControl::getMinHeatOnTime() {
    return minTimes.MIN_HEAT_ON_TIME;
}


/**
 * \brief Store min times to the filesystem
 */
void MinTimes::storeToFilesystem() {
    JsonDocument doc;

    toJson(doc);

    writeJsonToFile(MinTimes::filename, doc);  // Write the json to the file
}

void MinTimes::loadFromFilesystem() {
    // We start by setting the defaults, as we use them as the alternative to loaded values if the keys don't exist
    setDefaults();

    JsonDocument json_doc;
    json_doc = readJsonFromFile(MinTimes::filename);

    // Load the settings "default" choice from the JSON doc
    if(json_doc[MinTimesKeys::SETTINGS_CHOICE].is<MinTimesSettingsChoice>()) settings_choice = json_doc[MinTimesKeys::SETTINGS_CHOICE];

    // Load the constants from the JSON Doc
    if(json_doc[MinTimesKeys::MIN_COOL_OFF_TIME].is<uint16_t>()) MIN_COOL_OFF_TIME = json_doc[MinTimesKeys::MIN_COOL_OFF_TIME];
    if(json_doc[MinTimesKeys::MIN_HEAT_OFF_TIME].is<uint16_t>()) MIN_HEAT_OFF_TIME = json_doc[MinTimesKeys::MIN_HEAT_OFF_TIME];
    if(json_doc[MinTimesKeys::MIN_COOL_ON_TIME].is<uint16_t>()) MIN_COOL_ON_TIME = json_doc[MinTimesKeys::MIN_COOL_ON_TIME];
    if(json_doc[MinTimesKeys::MIN_HEAT_ON_TIME].is<uint16_t>()) MIN_HEAT_ON_TIME = json_doc[MinTimesKeys::MIN_HEAT_ON_TIME];
    
    if(json_doc[MinTimesKeys::MIN_COOL_OFF_TIME_FRIDGE_CONSTANT].is<uint16_t>()) MIN_COOL_OFF_TIME_FRIDGE_CONSTANT = json_doc[MinTimesKeys::MIN_COOL_OFF_TIME_FRIDGE_CONSTANT];
    if(json_doc[MinTimesKeys::MIN_SWITCH_TIME].is<uint16_t>()) MIN_SWITCH_TIME = json_doc[MinTimesKeys::MIN_SWITCH_TIME];
    if(json_doc[MinTimesKeys::COOL_PEAK_DETECT_TIME].is<uint16_t>()) COOL_PEAK_DETECT_TIME = json_doc[MinTimesKeys::COOL_PEAK_DETECT_TIME];
    if(json_doc[MinTimesKeys::HEAT_PEAK_DETECT_TIME].is<uint16_t>()) HEAT_PEAK_DETECT_TIME = json_doc[MinTimesKeys::HEAT_PEAK_DETECT_TIME];

    // Glycol mode time-proportional control settings
    if(json_doc[MinTimesKeys::GLYCOL_WINDOW_PERIOD].is<uint16_t>()) GLYCOL_WINDOW_PERIOD = json_doc[MinTimesKeys::GLYCOL_WINDOW_PERIOD];
    if(json_doc[MinTimesKeys::GLYCOL_MIN_ON_TIME].is<uint16_t>()) GLYCOL_MIN_ON_TIME = json_doc[MinTimesKeys::GLYCOL_MIN_ON_TIME];
}



/**
 * \brief Serialize min times to JSON
 */
void MinTimes::toJson(JsonDocument &doc) {
    // Load the constants into the JSON Doc
    doc[MinTimesKeys::SETTINGS_CHOICE] = settings_choice;

    doc[MinTimesKeys::MIN_COOL_OFF_TIME] = MIN_COOL_OFF_TIME;
    doc[MinTimesKeys::MIN_HEAT_OFF_TIME] = MIN_HEAT_OFF_TIME;
    doc[MinTimesKeys::MIN_COOL_ON_TIME] = MIN_COOL_ON_TIME;
    doc[MinTimesKeys::MIN_HEAT_ON_TIME] = MIN_HEAT_ON_TIME;

    doc[MinTimesKeys::MIN_COOL_OFF_TIME_FRIDGE_CONSTANT] = MIN_COOL_OFF_TIME_FRIDGE_CONSTANT;
    doc[MinTimesKeys::MIN_SWITCH_TIME] = MIN_SWITCH_TIME;
    doc[MinTimesKeys::COOL_PEAK_DETECT_TIME] = COOL_PEAK_DETECT_TIME;
    doc[MinTimesKeys::HEAT_PEAK_DETECT_TIME] = HEAT_PEAK_DETECT_TIME;

    // Glycol mode time-proportional control settings
    doc[MinTimesKeys::GLYCOL_WINDOW_PERIOD] = GLYCOL_WINDOW_PERIOD;
    doc[MinTimesKeys::GLYCOL_MIN_ON_TIME] = GLYCOL_MIN_ON_TIME;
}


// ============================================================================
// GLYCOL MODE: Predictive Bang-Bang Control Implementation
// See GLYCOL_COOLING_ALGORITHM.md for design documentation
// ============================================================================



// ----- GlycolRuntimeState -----

void GlycolRuntimeState::reset() {
    state = GLYCOL_IDLE;
    t_pump_on = 0;
    t_pump_off = 0;
    emergency_entry_time = 0;
    temp_at_pump_on = 0;
    temp_at_pump_off = 0;
    min_temp_reached = 0;
    cooling_rate_at_pump_off = 0;
    current_cooling_rate = 0;
    cooling_confirmed = false;
    negative_rate_count = 0;
    setpoint_changed_this_cycle = false;
    cooling_duration_s = 0;
    heating_output = 0;
    heating_window_start_ms = 0;
    heating_window_on_time_s = 0;
    heating_wait_reason = GLYCOL_HEATING_WAIT_NONE;
    force_minimum_cooling = false;
    rate_buffer_head = 0;
    rate_buffer_count = 0;
}

// ----- TempControl Glycol Methods -----

void TempControl::loadGlycolParams() {
    glycolLearned.loadFromFilesystem();
    glycolConfig.loadFromFilesystem();
    glycolRuntime.reset();
}

void TempControl::storeGlycolParams() {
    glycolLearned.storeToFilesystem();
}
