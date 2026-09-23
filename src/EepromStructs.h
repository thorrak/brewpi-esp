#pragma once

#include "TemperatureFormats.h"
#include "CoolingAlgorithm.h"
typedef uint8_t DeviceAddress[8];

#ifdef HAS_BLUETOOTH
#include <NimBLEDevice.h>
#endif

#include <ArduinoJson.h>


/*
 * \addtogroup tempcontrol
 * @{
 */


/**
 * \brief Data that can be persisted as JSON
 */
class JSONSaveable {
protected:
    static void writeJsonToFile(const char *filename, const JsonDocument& json_doc);
    static JsonDocument readJsonFromFile(const char*filename);

};


/**
 * \brief PID Control constants
 */
class ControlConstants : public JSONSaveable {
public:
    ControlConstants();

    temperature tempSettingMin; //<! Minimum valid control temperature
    temperature tempSettingMax; //<! Maximum valid control temperature
    temperature Kp;
    temperature Ki;
    temperature Kd;
    temperature iMaxError;
    temperature idleRangeHigh;
    temperature idleRangeLow;
    temperature heatingTargetUpper;
    temperature heatingTargetLower;
    temperature coolingTargetUpper;
    temperature coolingTargetLower;
    uint16_t maxHeatTimeForEstimate; //!< max time for heat estimate in seconds
    uint16_t maxCoolTimeForEstimate; //!< max time for heat estimate in seconds
    // for the filter coefficients the b value is stored. a is calculated from b.
    uint8_t fridgeFastFilter;	//!< for display, logging and on-off control
    uint8_t fridgeSlowFilter;	//!< for peak detection
    uint8_t fridgeSlopeFilter;	//!< not used in current control algorithm
    uint8_t beerFastFilter;	//!< for display and logging
    uint8_t beerSlowFilter;	//!< for on/off control algorithm
    uint8_t beerSlopeFilter;	//!< for PID calculation
    uint8_t lightAsHeater;		//!< Use the light to heat rather than the configured heater device
    uint8_t rotaryHalfSteps; //!< Define whether to use full or half steps for the rotary encoder
    temperature pidMax;
    temperature Kp_heat;    //!< Separate heating Kp for glycol mode
    temperature Ki_heat;    //!< Separate heating Ki for glycol mode
    temperature Kd_heat;    //!< Separate heating Kd for glycol mode
    temperature pidMax_heat; //!< Separate heating pidMax for glycol mode
    char tempFormat; //!< Temperature format (F/C)

    void toJson(JsonDocument &doc);
    void storeToFilesystem();
    void loadFromFilesystem();
    void setDefaults();

    /**
     * \brief Filename used when reading/writing data to flash
     */
    static constexpr auto filename = "/controlConstants.json";
private:
};

/** @} */

struct ControlSettings : public JSONSaveable {
public:
    ControlSettings();

    temperature beerSetting;
    temperature fridgeSetting;
    temperature heatEstimator; // updated automatically by self learning algorithm
    temperature coolEstimator; // updated automatically by self learning algorithm
    char mode;

    void toJson(JsonDocument &doc);
    void storeToFilesystem();
    void loadFromFilesystem();
    void setDefaults();

    /**
     * \brief Filename used when reading/writing data to flash
     */
    static constexpr auto filename = "/controlSettings.json";
};

/*
 * \addtogroup hardware
 * @{
 */



/**
 * \brief Describes the logical function of a device.
 */
enum DeviceFunction {
	DEVICE_NONE = 0, //!< Used as a sentry to mark end of list
	// chamber devices
	DEVICE_CHAMBER_DOOR = 1,	//!< Chamber door switch sensor
	DEVICE_CHAMBER_HEAT = 2,  //!< Chamber heater actuator
	DEVICE_CHAMBER_COOL = 3,  //!< Chamber cooler actuator
	DEVICE_CHAMBER_LIGHT = 4,	//!< Chamber light actuator
	DEVICE_CHAMBER_TEMP = 5,  //!< Chamber temp sensor
	DEVICE_CHAMBER_ROOM_TEMP = 6,	//!< Ambient room temp sensor
	DEVICE_CHAMBER_FAN = 7,			//!< A fan in the chamber
	DEVICE_CHAMBER_RESERVED1 = 8,	//!< Reserved for future use
	// carboy devices
	DEVICE_BEER_FIRST = 9,                //!< First beer temp sensor
	DEVICE_BEER_TEMP = DEVICE_BEER_FIRST,	//!< Primary beer temp sensor
	DEVICE_BEER_TEMP2 = 10,								//!< Secondary beer temp sensor
	DEVICE_BEER_HEAT = 11,                //!< Individual beer heater actuator
  DEVICE_BEER_COOL = 12,				        //!< Individual beer cooler actuator
	DEVICE_BEER_SG = 13,									//!< Beer SG sensor
	DEVICE_BEER_RESERVED1 = 14, //!< Reserved for future use
  DEVICE_BEER_RESERVED2 = 15,	//!< Reserved for future use
	DEVICE_MAX = 16
};



/**
 * \brief The concrete type of the device.
 */
enum DeviceHardware {
	DEVICE_HARDWARE_NONE = 0,
	DEVICE_HARDWARE_PIN = 1, //!< A digital pin, either input or output
	DEVICE_HARDWARE_ONEWIRE_TEMP = 2,	//<! A onewire temperature sensor
  // DS2413 is no longer supported - leaving this here (but commented out) for historical reference
//	DEVICE_HARDWARE_ONEWIRE_2413 = 3,	//<! A onewire 2-channel PIO input or output.
// Skipping 4, as that is used in "modern" brewpi
  DEVICE_HARDWARE_BLUETOOTH_INKBIRD = 5,
  DEVICE_HARDWARE_BLUETOOTH_TILT = 6,
  DEVICE_HARDWARE_TPLINK_SWITCH = 7,
};


/**
 * A union of all device types.
 */
class DeviceConfig : public JSONSaveable {
public:
  DeviceConfig() {setDefaults();};

	uint8_t chamber;		//!< Chamber assignment. 0 means no chamber. 1 is the first chamber.
	uint8_t beer;				//!< Beer assignment.  0 means no beer, 1 is the first beer


	DeviceFunction deviceFunction;				// The function of the device to configure
	DeviceHardware deviceHardware;				// flag to indicate the runtime type of device
	struct Hardware {
		uint8_t pinNr;                // the arduino pin nr this device is connected to (0 if wireless)
		bool invert;                  // for actuators/sensors, controls if the signal value is inverted.
		bool deactivate;              // disable this device - the device will not be installed.
#ifdef HAS_BLUETOOTH
    NimBLEAddress btAddress;
#endif
#ifdef EXTERN_SENSOR_ACTUATOR_SUPPORT
    char tplink_mac[18];                 // TP Link MAC address
    char tplink_child_id[3];             // TP Link Child ID (for multi-plug switches)
#endif
		DeviceAddress address;        // for onewire devices, if address[0]==0 then use the first matching device type, otherwise use the device with the specific address

    int8_t /* fixed4_4 */ calibration;	// for temp sensors (deviceHardware==2), calibration adjustment to add to sensor readings
                      // this is intentionally chosen to match the raw value precision returned by the ds18b20 sensors
	} hw;

    void toJson(JsonDocument &doc);
    void fromJson(JsonDocument json_doc);
    void storeToFilesystem(uint8_t devID);
    void loadFromFilesystem(uint8_t devID);
    void setDefaults();

    static void deviceFilename(char * fname, uint8_t devid);

};
/** @} */

/**
 * \brief Extended (non-stock-BrewPi) hardware settings
 */
class ExtendedSettings : public JSONSaveable {
public:
    ExtendedSettings();

    bool invertTFT;  //<! Whether or not to invert the TFT
    bool largeTFT;  //<! Whether or not to use a large TFT
    bool glycol;  //<! Whether or not to use glycol mode
    GlycolCooling::Algorithm glycolCoolingAlgorithm; //!< Requested cooling controller; applied at a safe pump-OFF boundary
    bool resetScreenOnPin;  //<! Whether or not to reset the screen when an ActuatorArduinoPin toggles

    #ifdef HAS_BLUETOOTH
    NimBLEAddress tiltGravSensor; //<! The color of the Tilt hydrometer used for gravity
    void setTiltGravSensor(NimBLEAddress setting);
    #endif


    void toJson(JsonDocument &doc);
    void storeToFilesystem();
    void loadFromFilesystem();
    void setDefaults();
    void processSettingKeypair(JsonPair kv);
    static bool validateSettingsJson(const JsonDocument &doc);

    void setGlycol(bool setting);
    bool setGlycolCoolingAlgorithm(GlycolCooling::Algorithm setting);
    void setInvertTFT(bool setting);
    void setLargeTFT(bool setting);
    void setResetScreenOnPin(bool setting);

    /**
     * \brief Filename used when reading/writing data to flash
     */
    static constexpr auto filename = "/extendedSettings.json";
private:
};


/**
 * \brief REST Interface settings
 */
class UpstreamSettings : public JSONSaveable {
public:
    enum class upstreamRegErrorT {
      NO_ERROR,                   // 0
      NO_GUID,                    // 1
      NO_USERNAME,                // 2
      INVALID_USER,               // 3
      NO_BREWHOUSE_ON_USER,       // 4
      NO_HARDWARE_TYPE,           // 5
      NO_FIRMWARE_VERSION,        // 6
      INVALID_API_KEY,            // 7
      NOT_ATTEMPTED_REGISTRATION, // 8
      REGISTRATION_ENDPOINT_ERR,  // 9
    };

    UpstreamSettings();

    char upstreamHost[128];  // Hostname (or IP address) of the upstream server
    uint16_t upstreamPort;   // Port of the upstream server (defaults to 80)
    char username[128];      // Hostname (or IP address) of the upstream server
    char deviceID[40];       // UUID of this device, as assigned by upstream server
    char apiKey[40];         // API key (uuid4 format) for the brewhouse this device is assigned to, as assigned by upstream server 

    upstreamRegErrorT upstreamRegistrationError;  //<! Error code from last upstream registration attempt

    void toJson(JsonDocument &doc);
    void storeToFilesystem();
    void loadFromFilesystem();
    void setDefaults();
    void processSettingKeypair(JsonPair kv);

    bool isRegistered();  //<! Whether or not this device has been registered with the upstream server

    /**
     * \brief Filename used when reading/writing data to flash
     */
    static constexpr auto filename = "/upstreamSettings.json";
private:
};

#ifdef HAS_BLUETOOTH
extern NimBLEAddress NoTiltDevice;
#endif
