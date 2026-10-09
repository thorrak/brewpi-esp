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

#pragma once

#include "Brewpi.h"
#include "TempSensor.h"
#include "Pins.h"
#include "TemperatureFormats.h"
#include "Actuator.h"
#include "Sensor.h"
#include "EepromManager.h"
#include "ActuatorAutoOff.h"
#include "EepromStructs.h"
#include "GlycolParams.h"
#include "GlycolCoolingController.h"
#include "GlycolTuning.h"
#include <ArduinoJson.h>

struct ControlContext;


/**
 * \defgroup tempcontrol Temperature PID Control
 * \brief Software implementation of a PID controller.
 *
 * \addtogroup tempcontrol
 * @{
 */

// ===== GLYCOL MODE: Selectable cooling / time-proportional heating =====
// See docs/GLYCOL_COOLING_SELECTION.md for the cooling algorithm selection.

/**
 * Runtime states used for glycol control and diagnostics.
 */
enum GlycolState : uint8_t {
    GLYCOL_IDLE = 0,              //!< Monitoring temperature, waiting to cool
    GLYCOL_COOLING = 1,           //!< Pump on, actively cooling
    GLYCOL_COASTING = 2,          //!< Pump off, temperature still dropping, measuring coast
    GLYCOL_FULL_COOLING = 3,     //!< Continuous cooling demand
    GLYCOL_HEATING = 4            //!< Beer-only heating via time-proportional duty cycle
};

/**
 * Internal reason for WAITING_TO_HEAT while in glycol heating mode.
 * Distinguishes actuator protection delays from the OFF portion of a heating
 * duty cycle while the reported control state is WAITING_TO_HEAT.
 */
enum GlycolHeatingWaitReason : uint8_t {
    GLYCOL_HEATING_WAIT_NONE = 0,
    GLYCOL_HEATING_WAIT_HEAT_OFF_DELAY = 1,
    GLYCOL_HEATING_WAIT_SWITCH_DELAY = 2,
    GLYCOL_HEATING_WAIT_WINDOW_OFF = 3
};

/**
 * State of the current time-proportional heating window.
 */
struct GlycolHeatingWindowState {
    uint16_t period_s;
    uint16_t on_time_s;
    uint16_t elapsed_in_window_s;
    bool on_slice_active;
};

/**
 * Result of actuator protection gating for a potential OFF->ON transition.
 */
struct GlycolHeatingGateResult {
    bool allowed;
    uint16_t wait_time_s;
    GlycolHeatingWaitReason reason;
};

/**
 * Glycol controller state. Only learned cooling tuning is persisted.
 */
struct GlycolRuntimeState {
    GlycolCooling::Controller cooling{};
    GlycolCooling::Output cooling_output{};
    bool clock_initialized = false;
    uint32_t clock_last_ms = 0;
    uint64_t clock_elapsed_ms = 0;
    bool cooling_step_initialized = false;
    uint64_t cooling_last_step_ms = 0;
    bool pid_step_initialized = false;
    uint64_t pid_last_step_ms = 0;
    // Real active-interval ends on the extended monotonic clock. Boot starts at 0.
    double last_heater_active_s = 0;
    double last_pump_active_s = 0;
    double pump_started_s = 0;

    GlycolState state;                    //!< Current glycol state machine state
    uint32_t t_pump_off;                  //!< Timestamp when pump turned off (ms)
    temperature heating_output;           //!< Heat authority from PID (0..pidMax_heat)
    bool heating_window_active;          //!< A duty window has been started
    uint32_t heating_window_start_ms;     //!< Start of current heating duty-cycle window
    uint16_t heating_window_on_time_s;    //!< Requested ON time in current heating window
    GlycolHeatingWaitReason heating_wait_reason; //!< Internal reason for WAITING_TO_HEAT

    void reset();
};

// These two structs are stored in and loaded from EEPROM
// struct ControlSettings was moved to EepromStructs.h
struct ControlVariables{
	temperature beerDiff;
	long_temperature diffIntegral; // also uses 9 fraction bits, but more integer bits to prevent overflow
	temperature beerSlope;
	temperature p;
	temperature i;
	temperature d;
	temperature estimatedPeak;
	temperature negPeakEstimate; // last estimate
	temperature posPeakEstimate;
	temperature negPeak; // last detected peak
	temperature posPeak;
};

enum MinTimesSettingsChoice {
	MIN_TIMES_DEFAULT,			// 0
    MIN_TIMES_LOW_DELAY,		// 1
	MIN_TIMES_CUSTOM			// 2
};

class MinTimes : public JSONSaveable {
public:

	MinTimesSettingsChoice settings_choice;
	MinTimes();

    uint16_t MIN_COOL_OFF_TIME;  //! Minimum cooler off time, in seconds. To prevent short cycling the compressor
    uint16_t MIN_HEAT_OFF_TIME;  //! Minimum heater off time, in seconds. To heat in cycles, not lots of short bursts
    uint16_t MIN_COOL_ON_TIME;  //! Minimum on time for the cooler.
    uint16_t MIN_HEAT_ON_TIME;  //! Minimum on time for the heater.

/**
 * Minimum cooler off time, in seconds.  Used when the controller is in Fridge Constant mode.
 * Larger than MIN_COOL_OFF_TIME. No need for very fast cycling.
 */
    uint16_t MIN_COOL_OFF_TIME_FRIDGE_CONSTANT;
    uint16_t MIN_SWITCH_TIME;  //! Minimum off time between switching between heating and cooling
    uint16_t COOL_PEAK_DETECT_TIME;  //! Time allowed for cooling peak detection
    uint16_t HEAT_PEAK_DETECT_TIME;  //! Time allowed for heating peak detection

    // Glycol mode time-proportional control settings
    uint16_t GLYCOL_WINDOW_PERIOD;  //! Window period for time-proportional control in seconds (default: 1000s)
    uint16_t GLYCOL_MIN_HEAT_ON_TIME;    //! Minimum heating duty slice; cooling uses the selected cooling configuration (2s)

	void toJson(JsonDocument &doc);
    void storeToFilesystem();
    void loadFromFilesystem();
    void setDefaults();

    /**
     * \brief Filename used when reading/writing data to flash
     */
    static constexpr auto filename = "/customMinTimes.json";

};

/**
 * \brief Strings used for JSON keys
 * \see MinTimes
 */
namespace MinTimesKeys {
	constexpr auto SETTINGS_CHOICE = "SETTINGS_CHOICE";
	constexpr auto MIN_COOL_OFF_TIME = "MIN_COOL_OFF_TIME";
	constexpr auto MIN_HEAT_OFF_TIME = "MIN_HEAT_OFF_TIME";
	constexpr auto MIN_COOL_ON_TIME = "MIN_COOL_ON_TIME";
	constexpr auto MIN_HEAT_ON_TIME = "MIN_HEAT_ON_TIME";
	constexpr auto MIN_COOL_OFF_TIME_FRIDGE_CONSTANT = "MIN_COOL_OFF_TIME_FRIDGE_CONSTANT";
	constexpr auto MIN_SWITCH_TIME = "MIN_SWITCH_TIME";
	constexpr auto COOL_PEAK_DETECT_TIME = "COOL_PEAK_DETECT_TIME";
	constexpr auto HEAT_PEAK_DETECT_TIME = "HEAT_PEAK_DETECT_TIME";
	constexpr auto GLYCOL_WINDOW_PERIOD = "GLYCOL_WINDOW_PERIOD";
	constexpr auto GLYCOL_MIN_HEAT_ON_TIME = "GLYCOL_MIN_HEAT_ON_TIME";
};

// struct ControlConstants was moved to EepromStructs.h

namespace Modes {
  constexpr auto fridgeConstant = 'f';
  constexpr auto beerConstant = 'b';
  constexpr auto beerProfile = 'p';
  constexpr auto off = 'o';
  constexpr auto test = 't';
};



/**
 * Temperature control states
 */
enum states {
	IDLE,           //!< Neither heating, nor cooling
	STATE_OFF,      //!< Disabled
	DOOR_OPEN,			//!< Fridge door open. Used by the Display only
	HEATING,				//!< Calling for heat
	COOLING,				//!< Calling for cool
	WAITING_TO_COOL,	//!< Waiting to cool. (Compressor delay)
	WAITING_TO_HEAT,			//!< Waiting to heat. (Compressor delay)
	WAITING_FOR_PEAK_DETECT,	//!< Waiting for peak detection
	COOLING_MIN_TIME,			// 8
	HEATING_MIN_TIME,			// 9
	NUM_STATES
};

#define TC_STATE_MASK 0x7;	// 3 bits

/**
 * \def TEMP_CONTROL_FIELD
 * \brief Compile-time control of making TempControl fields static
 * \see TEMP_CONTROL_STATIC
 */
#if TEMP_CONTROL_STATIC
#define TEMP_CONTROL_METHOD static
#define TEMP_CONTROL_FIELD static
#else
#define TEMP_CONTROL_METHOD
#define TEMP_CONTROL_FIELD
#endif

/**
 * \def TEMP_CONTROL_STATIC
 * \brief Compile-time control of making TempControl static
 *
 * To support multi-chamber, I could have made TempControl non-static, and had
 * a reference to the current instance. But this means each lookup of a field
 * must be done indirectly, which adds to the code size.  Instead, we swap
 * in/out the sensors and control data so that the bulk of the code can work
 * against compile-time resolvable memory references. While the design goes
 * against the grain of typical OO practices, the reduction in code size make
 * it worth it.
 */


/**
 * Temperature control PID implmentation
 *
 * This is the heart of the brewpi system.  It handles turning on and off heat
 * & cool to track a target temperature.
 *
 * Temp Control tracking can be done using several different modes
 *
 * - Beer: Heat & Cool are applied to keep a probe in the fermenting beer at a target.
 * - Fridge: Heat & Cool are applied to keep a probe in the chamber surrounding the beer at a target.
 */
class TempControl{
public:

	TempControl(){};
	~TempControl(){};

	TEMP_CONTROL_METHOD void init();
	TEMP_CONTROL_METHOD void reset();

	TEMP_CONTROL_METHOD void updateTemperatures();
	TEMP_CONTROL_METHOD void updatePID();
	TEMP_CONTROL_METHOD void updateState();
	TEMP_CONTROL_METHOD void updateOutputs();
	TEMP_CONTROL_METHOD void detectPeaks();

	TEMP_CONTROL_METHOD void loadSettings();
	TEMP_CONTROL_METHOD void storeSettings();
	TEMP_CONTROL_METHOD void loadDefaultSettings();

	TEMP_CONTROL_METHOD void loadConstants();
	TEMP_CONTROL_METHOD void storeConstants();
	TEMP_CONTROL_METHOD void loadDefaultConstants();

	//TEMP_CONTROL_METHOD void loadSettingsAndConstants(void);

	TEMP_CONTROL_METHOD uint16_t timeSinceCooling();
 	TEMP_CONTROL_METHOD uint16_t timeSinceHeating();
 	TEMP_CONTROL_METHOD uint16_t timeSinceIdle();

	TEMP_CONTROL_METHOD temperature getBeerTemp();
	TEMP_CONTROL_METHOD temperature getBeerSetting();
	TEMP_CONTROL_METHOD void setBeerTemp(temperature newTemp);

	TEMP_CONTROL_METHOD temperature getFridgeTemp();
	TEMP_CONTROL_METHOD temperature getFridgeSetting();
	TEMP_CONTROL_METHOD void setFridgeTemp(temperature newTemp);

	TEMP_CONTROL_METHOD temperature getRoomTemp();

	TEMP_CONTROL_METHOD void setMode(char newMode, bool force=false);
    TEMP_CONTROL_METHOD void resumeAfterWaterTest(const ControlSettings& saved);

  /**
   * Get current temp control mode
   */
	TEMP_CONTROL_METHOD char getMode() {
		return cs.mode;
	}

  /**
   * Get the current state of the control system.
   */
	TEMP_CONTROL_METHOD unsigned char getState(){
		return state;
	}

  /**
   * Get the current value of the elapsed wait time couter.
   */
	TEMP_CONTROL_METHOD uint16_t getWaitTime(){
		return waitTime;
	}

  /**
   * Reset the elapsed wait time counter back to 0.
   */
	TEMP_CONTROL_METHOD void resetWaitTime(){
		waitTime = 0;
	}

	// TEMP_CONTROL_METHOD void updateWaitTime(uint16_t newTimeLimit, uint16_t newTimeSince);
	TEMP_CONTROL_METHOD void updateWaitTime(uint16_t newTimeLimit, uint16_t newTimeSince){
		if(newTimeSince < newTimeLimit){
			uint16_t newWaitTime = newTimeLimit - newTimeSince;
			if(newWaitTime > waitTime){
				waitTime = newWaitTime;
			}
		}
	}

	TEMP_CONTROL_METHOD bool stateIsCooling();
	TEMP_CONTROL_METHOD bool stateIsHeating();

  /**
   * Check if the current configured mode is Beer
   */
	TEMP_CONTROL_METHOD bool modeIsBeer(){
		return (cs.mode == Modes::beerConstant || cs.mode == Modes::beerProfile);
	}

	TEMP_CONTROL_METHOD void initFilters();

  /**
   * Check if the door is currently open
   */
	TEMP_CONTROL_METHOD bool isDoorOpen() { return doorOpen; }

  /**
   * \brief Get the state to display on the LCD.
   *
   * If the chamber door is closed, this returns the value of getState().
   * If the door is open, the `DOOR_OPEN` state is returned instead.
   */
	TEMP_CONTROL_METHOD unsigned char getDisplayState() {
		return isDoorOpen() ? (unsigned char)DOOR_OPEN : getState();
	}

  TEMP_CONTROL_METHOD void getControlVariablesDoc(JsonDocument& doc);
  TEMP_CONTROL_METHOD void getControlConstantsDoc(JsonDocument& doc);
  TEMP_CONTROL_METHOD void getControlSettingsDoc(JsonDocument& doc);

public:
	TEMP_CONTROL_FIELD TempSensor* beerSensor; //!< Temp sensor monitoring beer
	TEMP_CONTROL_FIELD TempSensor* fridgeSensor; //!< Temp sensor monitoring fridge
	TEMP_CONTROL_FIELD BasicTempSensor* ambientSensor; //!< Ambient room temp sensor
	TEMP_CONTROL_FIELD Actuator* heater; //!< Actuator used to call for heat
	TEMP_CONTROL_FIELD Actuator* cooler; //!< Actuator used to call for cool
	TEMP_CONTROL_FIELD Actuator* light; //!< Actuator to control chamber light
	TEMP_CONTROL_FIELD Actuator* fan; //!< Actuator to control chamber fan
	TEMP_CONTROL_FIELD AutoOffActuator cameraLight;
	TEMP_CONTROL_FIELD Sensor<bool>* door; //!< Chamber door sensor

	// Control parameters
	TEMP_CONTROL_FIELD ControlConstants cc;
	TEMP_CONTROL_FIELD ControlSettings cs;
	TEMP_CONTROL_FIELD ControlVariables cv;

	// Glycol mode: Selectable beer-only cooling
	TEMP_CONTROL_FIELD GlycolConfig glycolConfig;           //!< Heating configuration (persisted)
	TEMP_CONTROL_FIELD GlycolTuningStore glycolTuning;
	TEMP_CONTROL_FIELD GlycolRuntimeState glycolRuntime;    //!< Active cooling and heating state

	TEMP_CONTROL_METHOD void loadGlycolParams();            //!< Load heating settings and cooling tuning into fresh runtime

	TEMP_CONTROL_FIELD uint16_t getMinCoolOnTime();
	TEMP_CONTROL_FIELD uint16_t getMinHeatOnTime();


private:
	/** Build a mode-controller view over the current TempControl state. */
	TEMP_CONTROL_METHOD ControlContext makeControlContext();
	TEMP_CONTROL_METHOD void resetGlycolControl();

	/**
   * Keep track of beer setting stored in EEPROM
   */
	TEMP_CONTROL_FIELD temperature storedBeerSetting;

	// Timers
	TEMP_CONTROL_FIELD uint16_t lastIdleTime; //!< Last time the controller was idle
	TEMP_CONTROL_FIELD uint16_t lastHeatTime; //!< Last time that the controller was heating
	TEMP_CONTROL_FIELD uint16_t lastCoolTime; //!< Last time that the controller was cooling
	TEMP_CONTROL_FIELD uint16_t waitTime;


	// State variables
	TEMP_CONTROL_FIELD uint8_t state; //!< Current controller state
	TEMP_CONTROL_FIELD bool doPosPeakDetect; //!< True if the controller is doing positive peak detection
	TEMP_CONTROL_FIELD bool doNegPeakDetect; //!< True if the controller is doing negative peak detection
	TEMP_CONTROL_FIELD bool doorOpen; //!< True if the chamber door is open

	friend class TempControlState;
};

extern TempControl tempControl;
extern MinTimes minTimes;

/** @} */
