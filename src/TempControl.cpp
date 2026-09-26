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
#include <algorithm>

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
#include <cmath>
#include "GlycolLog.h"
#include "WaterTest.h"

TempControl tempControl;
MinTimes minTimes;

#if TEMP_CONTROL_STATIC

extern ValueSensor<bool> defaultSensor;
extern ValueActuator defaultActuator;
extern DisconnectedTempSensor defaultTempSensor;

// Default-constructed with disconnected sensors so they are never null - we can potentially try to read them before TempControl::init() runs.
static TempSensor defaultBeerTempSensor(TEMP_SENSOR_TYPE_BEER, &defaultTempSensor);
static TempSensor defaultFridgeTempSensor(TEMP_SENSOR_TYPE_FRIDGE, &defaultTempSensor);
TempSensor* TempControl::beerSensor = &defaultBeerTempSensor;
TempSensor* TempControl::fridgeSensor = &defaultFridgeTempSensor;
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

// Glycol mode
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

	// Initialize beer/fridge sensors (they start with defaults, DeviceManager may reconfigure them)
	beerSensor->init();
	fridgeSensor->init();
	
	updateTemperatures();
	reset();

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
    if (WaterTest::controlOwned()) return;
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
        if(beerSensor->getFailedReadCount() > 60 ||
           (useGlycolBeerMode(cs) && beerSensor->readRawCached() == INVALID_TEMP)) {
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
            // Cooling uses the selected beer-only controller; heating is beer-only time-proportional PID.

            // Set fridgeSetting to INVALID_TEMP since it's not used in glycol mode
            cs.fridgeSetting = INVALID_TEMP;

            GlycolMode::Context glycolCtx(controlCtx, glycolLearned, glycolConfig, glycolRuntime, extendedSettings.glycolCoolingAlgorithm);
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

// Discard interrupted cycles, but preserve the actual output-off times so a
// reconnect or mode change cannot bypass actuator protection.
void TempControl::resetGlycolControl() {
    ControlContext controlCtx = makeControlContext();
    GlycolMode::Context glycolCtx(controlCtx, glycolLearned, glycolConfig, glycolRuntime, extendedSettings.glycolCoolingAlgorithm);
    GlycolMode::suspend(glycolCtx);
    // A mode transition into/out of manual test mode cannot rely on
    // updateOutputs(), which normally bypasses commands in that mode.
    // Apply the OFF edge now so the retained timing is the real command edge.
    cooler->setActive(false);
    heater->setActive(false);
    if (cc.lightAsHeater) light->setActive(false);
#ifdef BREWPI_CHILLSIM_TEST
    light->setActive(false);
#endif
    fan->setActive(false);
    if (cs.mode == Modes::test) {
        // Manual commands bypass the cooling cores, including OFF commands
        // immediately before this handoff. Start conservative protection
        // intervals for both directions without discarding learned values.
        const double now = glycolRuntime.clock_elapsed_ms / 1000.0;
        glycolRuntime.cooling.externalOff(now);
        glycolRuntime.cooling_output = glycolRuntime.cooling.output();
        glycolRuntime.last_pump_active_s = glycolRuntime.last_heater_active_s = now;
        glycolRuntime.t_pump_off = ticks.millis();
        lastCoolTime = lastHeatTime = ticks.seconds();
    }
#ifdef ENABLE_GLYCOL_LOGGING
    glycolLog.logTransition(glycolRuntime, beerSensor->readRawCached(), cs.beerSetting);
#endif
    cv.p = cv.i = cv.d = 0;
    cv.diffIntegral = 0;
    waitTime = 0;
    reset();
}

void TempControl::updateState(){
    if (WaterTest::controlOwned()) {
        state = cooler->isActive() ? COOLING : STATE_OFF;
        return;
    }
    //update state
    bool stayIdle = false;
    bool newDoorOpen = door->sense();

    if(newDoorOpen!=doorOpen) {
        doorOpen = newDoorOpen;
        char annotation[64];
        snprintf(annotation, sizeof(annotation), "Fridge door %s", doorOpen ? "opened" : "closed");
        piLink.printTemperatures(0, annotation);
    }

#ifndef BREWPI_CHILLSIM_TEST
    // Manual commands own the relays until an explicit mode transition. Test
    // mode does not require temperature sensors or an automatic setpoint.
    if (cs.mode == Modes::test) return;
#endif

    if(cs.mode == Modes::off){
        if (extendedSettings.glycol) resetGlycolControl();
        state = STATE_OFF;
        stayIdle = true;
    } else {
        // Check for invalid settings or disconnected sensors
        // In glycol mode, fridge sensor is optional; in compressor mode it's required
        bool fridgeRequired = !useGlycolBeerMode(cs);
        bool fridgeInvalid = (fridgeRequired && (!fridgeSensor->isConnected() || cs.fridgeSetting == INVALID_TEMP));
        bool beerInvalid = isBeerMode(cs) &&
            (!beerSensor->isConnected() || cs.beerSetting == INVALID_TEMP ||
             (useGlycolBeerMode(cs) && beerSensor->readRawCached() == INVALID_TEMP));

        if(fridgeInvalid || beerInvalid) {
            // Stay idle when a required sensor is disconnected or settings are invalid
            if (extendedSettings.glycol) resetGlycolControl();
            state = IDLE;
            stayIdle = true;
        }
    }

    ControlContext controlCtx = makeControlContext();

    // ===== GLYCOL MODE STATE MACHINE =====
    // Uses selectable cooling (see docs/GLYCOL_COOLING_SELECTION.md)
    if(useGlycolBeerMode(cs) && !stayIdle) {
        GlycolMode::Context glycolCtx(controlCtx, glycolLearned, glycolConfig, glycolRuntime, extendedSettings.glycolCoolingAlgorithm);
        GlycolMode::updateState(glycolCtx);
        // Glycol mode uses its own state machine - skip compressor mode logic
        return;
    }

    ChamberMode::Context chamberCtx(controlCtx, doPosPeakDetect, doNegPeakDetect);
    ChamberMode::updateState(chamberCtx, stayIdle);
}



void TempControl::updateOutputs() {
    if (WaterTest::controlOwned()) return;
#ifndef BREWPI_CHILLSIM_TEST
	if (cs.mode==Modes::test)
		return;
#endif
		
	cameraLight.update();
	bool heating = stateIsHeating();
#ifdef BREWPI_CHILLSIM_TEST
	heating = false; // Physical test build cannot energize either heating path.
#endif
	bool cooling = stateIsCooling();
	cooler->setActive(cooling);		
	heater->setActive(!cc.lightAsHeater && heating);	
	// A light assigned as the glycol heater must obey the same interlock.
	bool lightActive = extendedSettings.glycol && cc.lightAsHeater
		? heating
		: isDoorOpen() || (cc.lightAsHeater && heating) || cameraLightState.isActive();
	#ifdef BREWPI_CHILLSIM_TEST
	lightActive = false;
	#endif
	light->setActive(lightActive);
	fan->setActive(heating || cooling);
#ifdef ENABLE_GLYCOL_LOGGING
    if (extendedSettings.glycol) {
        glycolLog.logTransition(glycolRuntime, beerSensor->readRawCached(), cs.beerSetting);
    }
#endif
}


void TempControl::detectPeaks(){
    if (WaterTest::controlOwned()) return;
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
	logDebug("TempControl::setMode from %c to %c", cs.mode, newMode);

	// In glycol mode, fridge constant doesn't make sense - redirect to beer constant
	if(extendedSettings.glycol && newMode == Modes::fridgeConstant) {
		newMode = Modes::beerConstant;
	}

	// WAITING_TO_HEAT also represents a normal glycol PWM off slice. Repeated
	// settings payloads must not restart that window when the mode is unchanged.
	if(extendedSettings.glycol && newMode == cs.mode && !force) {
		return;
	}

	if((extendedSettings.glycol && force) || newMode != cs.mode || state == WAITING_TO_HEAT || state == WAITING_TO_COOL || state == WAITING_FOR_PEAK_DETECT){
		if (extendedSettings.glycol) resetGlycolControl();
		state = IDLE;
		force = true;
	}
	if (force) {
		cs.mode = newMode;
		if(newMode == Modes::off){
			cs.beerSetting = INVALID_TEMP;
			cs.fridgeSetting = INVALID_TEMP;
		}
		// Apply the OFF edge before a filesystem write can delay the command.
		if (extendedSettings.glycol) updateOutputs();
		TempControl::storeSettings();
	}
}

void TempControl::resumeAfterWaterTest(const ControlSettings& saved) {
    // Reset incomplete learning/filters, and conservatively begin fresh relay
    // protection intervals at the handoff. This never emits an ON command.
    cooler->setActive(false);
    heater->setActive(false);
    light->setActive(false);
    fan->setActive(false);
    cs = saved;
    state = STATE_OFF;
    cv.p = cv.i = cv.d = 0;
    cv.diffIntegral = 0;
    waitTime = 0;
    reset();
    glycolRuntime.reset();
    const uint32_t nowMs = ticks.millis();
    glycolRuntime.clock_initialized = true;
    glycolRuntime.clock_last_ms = nowMs;
    glycolRuntime.clock_elapsed_ms = nowMs;
    glycolRuntime.last_pump_active_s = nowMs / 1000.0;
    glycolRuntime.last_heater_active_s = nowMs / 1000.0;
    glycolRuntime.cooling.reset(extendedSettings.glycolCoolingAlgorithm);
    glycolRuntime.cooling.externalOff(nowMs / 1000.0);
    lastCoolTime = lastHeatTime = lastIdleTime = ticks.seconds();
    initFilters();
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
	if (extendedSettings.glycol) updateOutputs();
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
#ifdef BREWPI_CHILLSIM_TEST
  if (true) { // Identify the dedicated test image before enabling glycol mode.
#else
  if (extendedSettings.glycol) {
#endif
    // Explicit units: display format never changes these internal quantities.
    JsonObject cooling = doc["glycolCooling"].to<JsonObject>();
    const auto& output = glycolRuntime.cooling_output;
    cooling["algorithm"] = glycolRuntime.cooling.algorithmVersion();
    cooling["selection"] = GlycolCooling::selectionName(glycolRuntime.cooling.selection());
    cooling["requestedSelection"] = GlycolCooling::selectionName(extendedSettings.glycolCoolingAlgorithm);
    cooling["switchPending"] = glycolRuntime.cooling.switchPending() ||
        glycolRuntime.cooling.selection() != extendedSettings.glycolCoolingAlgorithm;
    cooling["phase"] = GlycolCooling::Controller::phaseName(output.phase);
    cooling["pumpOn"] = output.pump_on;
    cooling["fullCooling"] = output.full_cooling;
    cooling["temperatureC"] = output.temperature_c;
    cooling["setpointC"] = cs.beerSetting == INVALID_TEMP ? 0.0 :
        (static_cast<int32_t>(cs.beerSetting) - C_OFFSET) / 512.0;
    cooling["setpointValid"] = cs.beerSetting != INVALID_TEMP;
    temperature raw = beerSensor->readRawCached();
    cooling["sensorValid"] = raw != INVALID_TEMP && beerSensor->isConnected();
    cooling["sensorConnected"] = beerSensor->isConnected();
    cooling["sensorFailedReads"] = beerSensor->getFailedReadCount();
    if (raw == INVALID_TEMP) cooling["rawC"] = nullptr;
    else cooling["rawC"] = (static_cast<int32_t>(raw) - C_OFFSET) / 512.0;
    temperature glycolRaw = fridgeSensor->readRawCached();
    cooling["glycolSensorValid"] = glycolRaw != INVALID_TEMP && fridgeSensor->isConnected();
    if (glycolRaw == INVALID_TEMP) cooling["glycolRawC"] = nullptr;
    else cooling["glycolRawC"] = (static_cast<int32_t>(glycolRaw) - C_OFFSET) / 512.0;
    cooling["uptimeMillis"] = glycolRuntime.clock_elapsed_ms;
    cooling["coastAgeSeconds"] = output.phase == GlycolCooling::Phase::Coast
        ? static_cast<uint32_t>(ticks.millis() - glycolRuntime.t_pump_off) / 1000.0 : 0.0;
    cooling["coolerActive"] = cooler != &defaultActuator && cooler->isActive();
    cooling["heaterActive"] = heater != &defaultActuator && heater->isActive();
    cooling["lightActive"] = light != &defaultActuator && light->isActive();
#ifdef BREWPI_CHILLSIM_TEST
    cooling["coolingOnlyBuild"] = true;
#else
    cooling["coolingOnlyBuild"] = false;
#endif
    cooling["rateCPerSecond"] = output.rate_c_per_s;
    if (glycolRuntime.cooling.selection() == GlycolCooling::Algorithm::PredictiveCoast) {
        cooling["coastSeconds"] = output.coast_s;
        cooling["budgetGainCPerPumpSecond"] = output.budget_gain_c_per_s;
        cooling["responseUpdates"] = output.response_updates;
    } else {
        cooling["gainCPerPumpSecond"] = output.gain_c_per_on_s;
    }
    cooling["learningUpdates"] = output.learning_updates;
    // Null pulse budget means continuous demand (JSON has no Infinity).
    if (!std::isfinite(output.pulse_budget_s)) cooling["pulseBudgetSeconds"] = nullptr;
    else cooling["pulseBudgetSeconds"] = output.pulse_budget_s;
    cooling["actualOnSeconds"] = output.pump_on
        ? glycolRuntime.clock_elapsed_ms / 1000.0 - glycolRuntime.pump_started_s : 0.0;
    cooling["lastCompletedOnSeconds"] = output.actual_on_s;
    cooling["predictedEndpointC"] = output.predicted_endpoint_c;
    cooling["minOnSeconds"] = glycolRuntime.cooling.minOnSeconds();
    cooling["minOffSeconds"] = glycolRuntime.cooling.minOffSeconds();
    cooling["learningPersistence"] = "RAM only";
    cooling["legacyCoolingSettingsIgnored"] = true;
  }
}

/**
 * \brief Get current control constants as JsonDocument
 *
 * \param doc - Reference to JsonDocument to populate
 */
void TempControl::getControlConstantsDoc(JsonDocument& doc) {
  char tempFmt[2] = {cc.tempFormat, '\0'};
  doc["tempFormat"] = tempFmt;

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
  doc["KpHeat"] = fixedPointToDouble(cc.Kp_heat, Config::TempFormat::fixedPointDecimals);
  doc["KiHeat"] = fixedPointToDouble(cc.Ki_heat, Config::TempFormat::fixedPointDecimals);
  doc["KdHeat"] = fixedPointToDouble(cc.Kd_heat, Config::TempFormat::fixedPointDecimals);
  doc["pidMaxHeat"] = tempDiffToDouble(cc.pidMax_heat, Config::TempFormat::tempDiffDecimals);
#ifdef BREWPI_CHILLSIM_TEST
  if (true) {
#else
  if (extendedSettings.glycol) {
#endif
    JsonObject cooling = doc["glycolCoolingConfig"].to<JsonObject>();
    cooling["algorithm"] = glycolRuntime.cooling.algorithmVersion();
    cooling["selection"] = GlycolCooling::selectionName(glycolRuntime.cooling.selection());
    if (glycolRuntime.cooling.selection() == GlycolCooling::Algorithm::PredictiveCoast) {
        const auto& config = glycolRuntime.cooling.predictiveConfiguration();
        cooling["min_on_s"] = config.min_on_s;
        cooling["min_off_s"] = config.min_off_s;
        cooling["rate_window_s"] = config.rate_window_s;
        cooling["measurement_window_s"] = config.measurement_window_s;
        cooling["deadband_c"] = config.deadband_c;
        cooling["learning_fraction"] = config.learning_fraction;
        cooling["observe_coast_s"] = config.observe_coast_s;
        cooling["max_observe_coast_s"] = config.max_observe_coast_s;
        cooling["initial_coast_s"] = config.initial_coast_s;
        cooling["min_coast_estimate_s"] = config.min_coast_estimate_s;
        cooling["max_coast_estimate_s"] = config.max_coast_estimate_s;
        cooling["rate_floor_c_per_s"] = config.rate_floor_c_per_s;
        cooling["near_target_c"] = config.near_target_c;
        cooling["startup_pulse_s"] = config.startup_pulse_s;
        cooling["startup_budget_c_per_s"] = config.startup_budget_c_per_s;
        cooling["restart_margin_c"] = config.restart_margin_c;
        cooling["budget_learning_fraction"] = config.budget_learning_fraction;
        cooling["minimum_budget_gain_c_per_s"] = config.minimum_budget_gain_c_per_s;
        cooling["maximum_blind_budget_s"] = config.maximum_blind_budget_s;
    } else {
        const auto& config = glycolRuntime.cooling.doseConfiguration();
        cooling["min_on_s"] = config.min_on_s;
        cooling["min_off_s"] = config.min_off_s;
        cooling["rate_window_s"] = config.rate_window_s;
        cooling["measurement_window_s"] = config.measurement_window_s;
        cooling["deadband_c"] = config.deadband_c;
        cooling["learning_fraction"] = config.learning_fraction;
        cooling["observe_coast_s"] = config.observe_coast_s;
        cooling["max_observe_coast_s"] = config.max_observe_coast_s;
        cooling["initial_gain_c_per_on_s"] = config.initial_gain_c_per_on_s;
        cooling["minimum_gain_c_per_on_s"] = config.minimum_gain_c_per_on_s;
        cooling["maximum_gain_c_per_on_s"] = config.maximum_gain_c_per_on_s;
        cooling["dose_fraction"] = config.dose_fraction;
        cooling["initial_probe_s"] = config.initial_probe_s;
        cooling["far_probe_s"] = config.far_probe_s;
        cooling["far_error_c"] = config.far_error_c;
        cooling["saturation_dose_s"] = config.saturation_dose_s;
        cooling["unresolved_error_c"] = config.unresolved_error_c;
        cooling["stop_horizon_s"] = config.stop_horizon_s;
        cooling["settled_rate_c_per_s"] = config.settled_rate_c_per_s;
    }
  }
}


/**
 * \brief Get current control settings as a JsonDocument
 *
 * \param doc - Reference to JsonDocument to populate
 */
void TempControl::getControlSettingsDoc(JsonDocument& doc) {
  char modeFmt[2] = {cs.mode, '\0'};
  doc["mode"] = modeFmt;
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
        MIN_COOL_OFF_TIME = 30;
        MIN_HEAT_OFF_TIME = 30;
        MIN_COOL_ON_TIME = 30;
        MIN_HEAT_ON_TIME = 30;

        MIN_COOL_OFF_TIME_FRIDGE_CONSTANT = 30;
        MIN_SWITCH_TIME = 60;
        COOL_PEAK_DETECT_TIME = 300;
        HEAT_PEAK_DETECT_TIME = 300;

        // Time-proportional control settings
        GLYCOL_WINDOW_PERIOD = 1000;
        GLYCOL_MIN_ON_TIME = 10;
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
// GLYCOL MODE: Selectable cooling runtime
// See docs/GLYCOL_COOLING_SELECTION.md for design documentation
// ============================================================================



// ----- GlycolRuntimeState -----

void GlycolRuntimeState::reset() {
    cooling.reset();
    cooling_output = cooling.output();
    clock_initialized = false;
    clock_last_ms = 0;
    clock_elapsed_ms = 0;
    cooling_step_initialized = false;
    cooling_last_step_ms = 0;
    pid_step_initialized = false;
    pid_last_step_ms = 0;
    last_heater_active_s = 0;
    last_pump_active_s = 0;
    pump_started_s = 0;
    state = GLYCOL_IDLE;
    t_pump_off = 0;
    heating_output = 0;
    heating_window_active = false;
    heating_window_start_ms = 0;
    heating_window_on_time_s = 0;
    heating_wait_reason = GLYCOL_HEATING_WAIT_NONE;
}

// ----- TempControl Glycol Methods -----

void TempControl::loadGlycolParams() {
    glycolLearned.loadFromFilesystem();
    glycolConfig.loadFromFilesystem();
    glycolRuntime.reset();
}
