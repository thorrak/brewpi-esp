/*
 * Copyright 2013 Matthew McGowan
 * Copyright 2013 BrewPi/Elco Jacobs.
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

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "Brewpi.h"
#include "BrewpiStrings.h"
#include "DeviceManager.h"
#include <driver/gpio.h>

#ifndef HIGH
#define HIGH 1
#endif
#ifndef LOW
#define LOW 0
#endif
#include "TempControl.h"
#include "Actuator.h"
#include "Sensor.h"
#include "TempSensorDisconnected.h"
#include "TempSensorExternal.h"
#include "PiLink.h"
#include "DeviceNameManager.h"
#include <ArduinoJson.h>
#include "JsonKeys.h"
#include "NumberFormats.h"
#include <thorlog.h>
#include <thorlog_espidf.h>
#include <esp_log.h>


#include "OneWireTempSensor.h"
#include "OneWireScanner.h"

#include "ActuatorArduinoPin.h"
#include "SensorArduinoPin.h"

#ifdef HAS_BLUETOOTH
#include "InkbirdTempSensor.h"
#include "TiltTempSensor.h"
#include "wireless/BTScanner.h"
#endif

#ifdef EXTERN_SENSOR_ACTUATOR_SUPPORT
#include "tplink/TPLinkScanner.h"
#include "ActuatorTPLink.h"
#endif


/*
 * Defaults for sensors, actuators and temperature sensors when not defined in the eeprom.
 */

ValueSensor<bool> defaultSensor(false);			// off
ValueActuator defaultActuator;
DisconnectedTempSensor defaultTempSensor;


bool DeviceManager::initOneWireBuses() {
#if !BREWPI_SIMULATE
  // Bring up the scanner, which creates the bus and starts its worker task.
  // All subsequent OneWire I/O (enumeration, conversion, read) happens inside
  // the worker — no bus handle is exposed to the rest of the firmware.
  return ow_scanner.init(oneWirePin);
#else
  return true;
#endif
}

bool DeviceManager::isValidOneWirePin(uint8_t pin) {
#if !BREWPI_SIMULATE
  return pin == oneWirePin;
#else
  return false;
#endif
}



/**
 * Check if a given BasicTempSensor is the default temp sensor
 *
 * @param sensor - Pointer to the sensor to check
 * @returns true if the provided sensor is the default, otherwise false.
 */
bool DeviceManager::isDefaultTempSensor(BasicTempSensor* sensor) {
	return sensor==&defaultTempSensor;
}

/**
 * Sets devices to their unconfigured states. Each device is initialized to a static no-op instance.
 * This method is idempotent, and is called each time the eeprom is reset.
 */
void DeviceManager::setupUnconfiguredDevices()
{
	// right now, uninstall doesn't care about chamber/beer distinction.
	// but this will need to match beer/function when multiferment is available
	DeviceConfig cfg;
	for (uint8_t i=0; i<DEVICE_MAX; i++) {
		cfg.deviceFunction = DeviceFunction(i);
		uninstallDevice(cfg);
	}
}


/**
 * Creates a new device for the given config.
 */
void* DeviceManager::createDevice(DeviceConfig& config, DeviceType dt)
{
	switch (config.deviceHardware) {
		case DEVICE_HARDWARE_NONE:
			break;
		case DEVICE_HARDWARE_PIN:
			if (dt==DEVICETYPE_SWITCH_SENSOR)
			#if BREWPI_SIMULATE
				return new ValueSensor<bool>(false);
			#else
				return new DigitalPinSensor(config.hw.pinNr, config.hw.invert);
			#endif
			else
#if BREWPI_SIMULATE
				return new ValueActuator();
#else
				// use hardware actuators even for simulator
				return new DigitalPinActuator(config.hw.pinNr, config.hw.invert);
#endif
		case DEVICE_HARDWARE_ONEWIRE_TEMP:
		#if BREWPI_SIMULATE
			return new ExternalTempSensor(false);// initially disconnected, so init doesn't populate the filters with the default value of 0.0
		#else
			return new OneWireTempSensor(config.hw.address, config.hw.calibration);
		#endif

#ifdef HAS_BLUETOOTH
		case DEVICE_HARDWARE_BLUETOOTH_INKBIRD:
			return new InkbirdTempSensor(config.hw.btAddress, config.hw.calibration);
		case DEVICE_HARDWARE_BLUETOOTH_TILT:
			return new TiltTempSensor(config.hw.btAddress, config.hw.calibration);
#else
		case DEVICE_HARDWARE_BLUETOOTH_INKBIRD:
		case DEVICE_HARDWARE_BLUETOOTH_TILT:
		Log.error("Bluetooth not supported");
			return nullptr;
#endif

#ifdef EXTERN_SENSOR_ACTUATOR_SUPPORT
		case DEVICE_HARDWARE_TPLINK_SWITCH:
			return new TPLinkActuator(config.hw.tplink_mac, config.hw.tplink_child_id);
#else
		case DEVICE_HARDWARE_TPLINK_SWITCH:
		Log.error("TPLink not supported");
			return nullptr;
#endif

	}
	return nullptr;
}

/**
 * Returns the pointer to where the device pointer resides.
 *
 * This can be used to delete the current device and install a new one.  For
 * Temperature sensors, the returned pointer points to a TempSensor*. The basic
 * device can be fetched by calling TempSensor::getSensor().
 *
 * @param config
 */
inline void** deviceTarget(DeviceConfig& config)
{
	// for multichamber, will write directly to the multi-chamber managed storage.
	// later...
	if (config.chamber>1 || config.beer>1)
		return nullptr;

	void** ppv;
	switch (config.deviceFunction) {
	case DEVICE_CHAMBER_ROOM_TEMP:
		ppv = (void**)&tempControl.ambientSensor;
		break;
	case DEVICE_CHAMBER_DOOR:
		ppv = (void**)&tempControl.door;
		break;
	case DEVICE_CHAMBER_LIGHT:
		ppv = (void**)&tempControl.light;
		break;
	case DEVICE_CHAMBER_HEAT:
		ppv = (void**)&tempControl.heater;
		break;
	case DEVICE_CHAMBER_COOL:
		ppv = (void**)&tempControl.cooler;
		break;
	case DEVICE_CHAMBER_TEMP:
		ppv = (void**)&tempControl.fridgeSensor;
		break;
	case DEVICE_CHAMBER_FAN:
		ppv = (void**)&tempControl.fan;
		break;

	case DEVICE_BEER_TEMP:
		ppv = (void**)&tempControl.beerSensor;
		break;
	default:
		ppv = nullptr;
	}
	return ppv;
}

// A pointer to a "temp sensor" may be a TempSensor* or a BasicTempSensor* .
// These functions allow uniform treatment.
inline bool isBasicSensor(DeviceFunction function) {
	// currently only ambient sensor is basic. The others are wrapped in a TempSensor.
	return function==DEVICE_CHAMBER_ROOM_TEMP;
}

inline BasicTempSensor& unwrapSensor(DeviceFunction f, void* pv) {
	return isBasicSensor(f) ? *(BasicTempSensor*)pv : ((TempSensor*)pv)->sensor();
}

inline void setSensor(DeviceFunction f, void** ppv, BasicTempSensor* sensor) {
	if (isBasicSensor(f))
		*ppv = sensor;
	else
		((TempSensor*)*ppv)->setSensor(sensor);
}


/**
 * Removes an installed device.
 *
 * @param config - The device to remove. The fields that are used are
 *		chamber, beer, hardware and function.
 */
void DeviceManager::uninstallDevice(DeviceConfig& config)
{
	DeviceType dt = deviceType(config.deviceFunction);
	void** ppv = deviceTarget(config);
	bool deviceFound = false;
	if (ppv==nullptr)
		return;

	BasicTempSensor* s;
	switch(dt) {
		case DEVICETYPE_NONE:
			break;
		case DEVICETYPE_TEMP_SENSOR:
			// sensor may be wrapped in a TempSensor class, or may stand alone.
			s = &unwrapSensor(config.deviceFunction, *ppv);
#ifdef HAS_BLUETOOTH
			if(config.deviceFunction == DEVICE_BEER_TEMP && config.deviceHardware == DEVICE_HARDWARE_BLUETOOTH_TILT) {
				// If the device is a Tilt, then let's unset the gravity sensor
				extendedSettings.setTiltGravSensor(NoTiltDevice);
				extendedSettings.storeToFilesystem();
			}
#endif

			if (s!=&defaultTempSensor) {
				setSensor(config.deviceFunction, ppv, &defaultTempSensor);
//				DEBUG_ONLY(logInfoInt(INFO_UNINSTALL_TEMP_SENSOR, config.deviceFunction));
				delete s;
				deviceFound = true;
			}
			break;
		case DEVICETYPE_SWITCH_ACTUATOR:
			if (*ppv!=&defaultActuator) {
//				DEBUG_ONLY(logInfoInt(INFO_UNINSTALL_ACTUATOR, config.deviceFunction));
				if(((Actuator*)*ppv)->isActive())
					((Actuator*)*ppv)->setActive(false);
				delete (Actuator*)*ppv;
				*ppv = &defaultActuator;
				deviceFound = true;
			}
			break;
		case DEVICETYPE_SWITCH_SENSOR:
			if (*ppv!=&defaultSensor) {
//				DEBUG_ONLY(logInfoInt(INFO_UNINSTALL_SWITCH_SENSOR, config.deviceFunction));
				delete (SwitchSensor*)*ppv;
				*ppv = &defaultSensor;
				deviceFound = true;
			}
			break;
	}
	if(deviceFound)
		eepromManager.deleteDeviceWithFunction(config.deviceFunction);
}


/**
 * Creates and installs a device in the current chamber.
 *
 * @param config
 * @return true if a device was installed. false if the config is not complete.
 */
void DeviceManager::installDevice(DeviceConfig& config)
{
	DeviceType dt = deviceType(config.deviceFunction);
	void** ppv = deviceTarget(config);
	if (ppv==nullptr || config.hw.deactivate)
		return;

	BasicTempSensor* s;
	TempSensor* ts;
	switch(dt) {
		case DEVICETYPE_NONE:
			break;
		case DEVICETYPE_TEMP_SENSOR:
			DEBUG_ONLY(logInfoInt(INFO_INSTALL_TEMP_SENSOR, config.deviceFunction));
			// sensor may be wrapped in a TempSensor class, or may stand alone.
			s = (BasicTempSensor*)createDevice(config, dt);
			if (*ppv==nullptr){
				logErrorInt(ERROR_OUT_OF_MEMORY_FOR_DEVICE, config.deviceFunction);
			}
			if (isBasicSensor(config.deviceFunction)) {
				s->init();
				*ppv = s;
			} else {
				ts = ((TempSensor*)*ppv);
				ts->setSensor(s);
				ts->init();
			}
#if BREWPI_SIMULATE
			((ExternalTempSensor*)s)->setConnected(true);	// now connect the sensor after init is called
#endif
			break;
		case DEVICETYPE_SWITCH_ACTUATOR:
		case DEVICETYPE_SWITCH_SENSOR:
			DEBUG_ONLY(logInfoInt(INFO_INSTALL_DEVICE, config.deviceFunction));
			*ppv = createDevice(config, dt);
#if (BREWPI_DEBUG > 0)
			if (*ppv==nullptr)
				logErrorInt(ERROR_OUT_OF_MEMORY_FOR_DEVICE, config.deviceFunction);
#endif
			break;
	}
}


// the special cases are placed at the end. All others should map directly to an int8_t via atoi().
// const char DeviceDefinition::ORDER[12] = "icbfhpxndja";


/**
 * \brief Parse JSON doc and populate a DeviceDefinition
 *
 * \param doc - JSON document containing DeviceDefinition parameters
 * @return populated DeviceDefinition
 */
DeviceDefinition DeviceManager::readJsonIntoDeviceDef(const JsonDocument& doc) {
	DeviceDefinition dev;

	if(doc[DeviceDefinitionKeys::hardware].is<uint8_t>()) {
		dev.deviceHardware = doc[DeviceDefinitionKeys::hardware].as<uint8_t>();
	}

	const char* address = doc[DeviceDefinitionKeys::address].as<const char *>();

	if(address) {
	  switch(dev.deviceHardware) {
		case DEVICE_HARDWARE_ONEWIRE_TEMP:
			parseBytes(dev.address, address, 8);
			break;
#ifdef HAS_BLUETOOTH
		case DEVICE_HARDWARE_BLUETOOTH_INKBIRD:
			dev.btAddress = NimBLEAddress(doc[DeviceDefinitionKeys::address].as<std::string>(), 0);
			break;
		case DEVICE_HARDWARE_BLUETOOTH_TILT:
			// Tilts use address type 1 ("random", which (correctly!) indicates they didn't buy a MAC block)
			dev.btAddress = NimBLEAddress(doc[DeviceDefinitionKeys::address].as<std::string>(), 1);
			break;
#endif
#ifdef EXTERN_SENSOR_ACTUATOR_SUPPORT
		case DEVICE_HARDWARE_TPLINK_SWITCH:
			snprintf(dev.tplink_mac, 18, "%s", doc[DeviceDefinitionKeys::address].as<const char *>());
			snprintf(dev.tplink_child_id, 3, "%s", doc[DeviceDefinitionKeys::child_id].as<const char *>());
			break;
#endif
		default:
			break;
	  }
  }

	if(doc[DeviceDefinitionKeys::calibrateadjust].is<double>()) {
		temperature tempDiff = 0;
		char buff[10];
		snprintf(buff, sizeof(buff), "%.6f", doc[DeviceDefinitionKeys::calibrateadjust].as<double>());
		tempDiff = stringToTempDiff(buff);
		dev.calibrationAdjust = fixed4_4(tempDiff >> (TEMP_FIXED_POINT_BITS - TEMP_CALIBRATION_OFFSET_PRECISION));
	} else if(doc[DeviceDefinitionKeys::calibrateadjust].is<const char *>()) {
		temperature tempDiff = stringToTempDiff(doc[DeviceDefinitionKeys::calibrateadjust].as<const char *>());
		dev.calibrationAdjust = fixed4_4(tempDiff >> (TEMP_FIXED_POINT_BITS - TEMP_CALIBRATION_OFFSET_PRECISION));
	}

	// dev.id defaults to -1, so if this fails, the device won't get processed by deviceManager.updateDeviceDefinition
	if(doc[DeviceDefinitionKeys::index].is<uint8_t>()) dev.id = doc[DeviceDefinitionKeys::index].as<uint8_t>();

	if(doc[DeviceDefinitionKeys::chamber].is<uint8_t>()) dev.chamber = doc[DeviceDefinitionKeys::chamber].as<uint8_t>();

	if(doc[DeviceDefinitionKeys::beer].is<uint8_t>()) dev.beer = doc[DeviceDefinitionKeys::beer].as<uint8_t>();

	if(doc[DeviceDefinitionKeys::function].is<uint8_t>()) dev.deviceFunction = doc[DeviceDefinitionKeys::function].as<uint8_t>();

	dev.deactivate = false;

	if(doc[DeviceDefinitionKeys::pin].is<uint8_t>())
		dev.pinNr = doc[DeviceDefinitionKeys::pin].as<uint8_t>();

	if(doc[DeviceDefinitionKeys::invert].is<bool>())
		dev.invert = doc[DeviceDefinitionKeys::invert].as<bool>() ? 1 : 0;
	else if(doc[DeviceDefinitionKeys::invert].is<const char *>())
		dev.invert = doc[DeviceDefinitionKeys::invert].as<const char*>()[0] == '1' ? 1 : 0;
	else if (doc[DeviceDefinitionKeys::invert].is<uint8_t>())
		dev.invert = doc[DeviceDefinitionKeys::invert].as<uint8_t>();

  return dev;
}

/**
 * \brief Check if value is within a range
 * \param val - Value to check
 * \param min - Lower bound
 * \param max - Upper bound
 */
bool inRangeUInt8(uint8_t val, uint8_t min, int8_t max) {
	return min<=val && val<=max;
}

/**
 * \brief Check if value is within a range
 * \param val - Value to check
 * \param min - Lower bound
 * \param max - Upper bound
 */
bool inRangeInt8(int8_t val, int8_t min, int8_t max) {
	return min<=val && val<=max;
}


/**
 * \brief Set target to value if value is set
 * \param value - Source value
 * \param target - Variable to set, if value is set
 */
void assignIfSet(int8_t value, uint8_t* target) {
	if (value>=0)
		*target = (uint8_t)value;
}

/**
 * \brief Safely updates the device definition.
 *
 * Only changes that result in a valid device, with no conflicts with other
 * devices are allowed.
 */
DeviceConfig DeviceManager::updateDeviceDefinition(DeviceDefinition dev)
{
	// save the original device so we can revert
	DeviceConfig target;
	DeviceConfig original;


	if (!inRangeInt8(dev.id, 0, Config::EepromFormat::MAX_DEVICES))	{
		// no device id given, or it's out of range, can't do anything else.
		// piLink.print_fmt("Out of range: %d", dev.id);
		// piLink.printNewLine();
		original.setDefaults();  // This may create some issues, but it's better than returning a null pointer
		return original;
	}

  if(Config::forceDeviceDefaults) {
    // Overwrite the chamber/beer number to prevent user error.
    dev.chamber = 1;

    // Check if device function is beer specific
    if (dev.deviceFunction >= DEVICE_BEER_FIRST && dev.deviceFunction < DEVICE_MAX)
      dev.beer = 1;
    else
      dev.beer = 0;
  }

	// Fetch either the saved device (or an empty one if a saved one doesn't exist) 
	original = eepromManager.fetchDevice(dev.id);
	target = original;


	target.chamber = dev.chamber;
	target.beer = dev.beer;
	target.deviceFunction = (DeviceFunction) dev.deviceFunction;
	target.deviceHardware = (DeviceHardware) dev.deviceHardware;

	// If this is a OneWire device, force the pin number if forceDeviceDefaults is selected
	if(Config::forceDeviceDefaults && dev.deviceHardware == DEVICE_HARDWARE_ONEWIRE_TEMP)
		target.hw.pinNr = oneWirePin;
	else
		target.hw.pinNr = dev.pinNr;


	if (dev.deviceHardware == DEVICE_HARDWARE_ONEWIRE_TEMP
		|| dev.deviceHardware == DEVICE_HARDWARE_BLUETOOTH_INKBIRD || dev.deviceHardware == DEVICE_HARDWARE_BLUETOOTH_TILT 
		)
		target.hw.calibration = dev.calibrationAdjust;

	target.hw.invert = (bool) dev.invert;


#ifdef HAS_BLUETOOTH
	if(dev.deviceHardware == DEVICE_HARDWARE_BLUETOOTH_INKBIRD || dev.deviceHardware == DEVICE_HARDWARE_BLUETOOTH_TILT) {
		target.hw.btAddress = dev.btAddress;
	} else
#endif
#ifdef EXTERN_SENSOR_ACTUATOR_SUPPORT
	if(dev.deviceHardware == DEVICE_HARDWARE_TPLINK_SWITCH) {
		snprintf(target.hw.tplink_mac, 18, "%s", dev.tplink_mac);
		snprintf(target.hw.tplink_child_id, 3, "%s", dev.tplink_child_id);
	} else
#endif
	if (dev.address[0] != 0xFF && dev.deviceHardware == DEVICE_HARDWARE_ONEWIRE_TEMP) { // first byte is family identifier. I don't have a complete list, but so far 0xFF is not used.
		memcpy(target.hw.address, dev.address, 8);
	}

	target.hw.deactivate = (bool) dev.deactivate;

	// setting function to none clears all other fields.
	if (target.deviceFunction==DEVICE_NONE)
		target.setDefaults();

	bool valid = isDeviceValid(target, original, dev.id);
	if (valid) {
		// remove the device associated with the previous function
		uninstallDevice(original);
		// also remove any existing device for the new function, since install overwrites any existing definition.
		uninstallDevice(target);
		installDevice(target);
		eepromManager.storeDevice(target, dev.id);

#ifdef HAS_BLUETOOTH
		if(dev.deviceFunction == DEVICE_BEER_TEMP) {
			if(dev.deviceHardware == DEVICE_HARDWARE_BLUETOOTH_TILT) {
				// If the device is a Tilt, then let's assume that the user wants it to also be the gravity sensor
				extendedSettings.setTiltGravSensor(target.hw.btAddress);
				extendedSettings.storeToFilesystem();
			} else {
				// Otherwise, let's unset the gravity sensor
				// TODO - Determine if I really want to do this here (as opposed to when uninstalling the Tilt)
				extendedSettings.setTiltGravSensor(NoTiltDevice);
				extendedSettings.storeToFilesystem();
			}

		}
#endif

		return target;
	}
	else {
		logError(ERROR_DEVICE_DEFINITION_UPDATE_SPEC_INVALID);
		return original;
	}
}

/**
 * Determines if a given device definition is valid.
 *
 * Validity is defiend by:
 * - Chamber & beer must be within bounds
 * - Device function must match the chamber/beer spec, and must not already be defined for the same chamber/beer combination - Not Implemented
 * - Device hardware type must be applicable with the device function
 * - pinNr must be unique for digital pin devices - Not Implemented
 * - pinNr must be a valid OneWire bus for one wire devices.
 * - For OneWire temp devices, address must be unique. - Not Implemented
 */
bool DeviceManager::isDeviceValid(DeviceConfig& config, DeviceConfig& original, int8_t deviceIndex)
{
	/* Implemented checks to ensure the system will not crash when supplied with invalid data.
	   More refined checks that may cause confusing results are not yet implemented. See todo below. */

	/* chamber and beer within range.*/
	if (!inRangeUInt8(config.chamber, 0, Config::EepromFormat::MAX_CHAMBERS))
	{
		logErrorInt(ERROR_INVALID_CHAMBER, config.chamber);
		return false;
	}

	/* 0 is allowed - represents a chamber device not assigned to a specific beer */
	if (!inRangeUInt8(config.beer, 0, Config::EepromFormat::MAX_BEERS))
	{
		logErrorInt(ERROR_INVALID_BEER, config.beer);
		return false;
	}

	if (!inRangeUInt8(config.deviceFunction, 0, DEVICE_MAX-1))
	{
		logErrorInt(ERROR_INVALID_DEVICE_FUNCTION, config.deviceFunction);
		return false;
	}

	DeviceOwner owner = deviceOwner(config.deviceFunction);
	if (!((owner==DEVICE_OWNER_BEER && config.beer) || (owner==DEVICE_OWNER_CHAMBER && config.chamber)
		|| (owner==DEVICE_OWNER_NONE && !config.beer && !config.chamber)))
	{
		logErrorIntIntInt(ERROR_INVALID_DEVICE_CONFIG_OWNER, owner, config.beer, config.chamber);
		return false;
	}

	// todo - find device at another index with the same chamber/beer/function spec.
	// with duplicate function defined for the same beer, that they will both try to create/delete the device in the target location.
	// The highest id will win.
	DeviceType dt = deviceType(config.deviceFunction);
	if (!isAssignable(dt, config.deviceHardware)) {
		logErrorIntInt(ERROR_CANNOT_ASSIGN_TO_HARDWARE, dt, config.deviceHardware);
		return false;
	}

	// todo - check pinNr uniqueness for direct digital I/O devices?

	/* pinNr for a onewire device must be a valid bus. While this won't cause a crash, it's a good idea to validate this. */
	if (isOneWire(config.deviceHardware)) {
		if (!isValidOneWirePin(config.hw.pinNr)) {
			logErrorInt(ERROR_NOT_ONEWIRE_BUS, config.hw.pinNr);
			return false;
		}
	}
	else {		// regular pin device
		// todo - could verify that the pin nr corresponds to enumActuatorPins/enumSensorPins
	}

	// todo - for onewire temp, ensure address is unique
	return true;
}


/**
 * Check if DeviceHardware definition is for a device which is "invertable"
 *
 * @param hw - DeviceHardware definition
 */
inline bool hasInvert(DeviceHardware hw)
{
	return hw==DEVICE_HARDWARE_PIN;
}


/**
 * \brief Check if DeviceHardware definition is for a OneWire device
 *
 * \param hw - DeviceHardware definition
 */
inline bool hasOnewire(DeviceHardware hw)
{
	return hw==DEVICE_HARDWARE_ONEWIRE_TEMP;
}


/**
 * \brief Add device information to a JsonDocument
 *
 * Used for outputting device information
 */
void DeviceManager::serializeJsonDevice(JsonDocument& doc, device_slot_t slot, DeviceConfig& config, const char* value) {
	JsonDocument deviceObj;
	config.toJson(deviceObj);

	if(strlen(value) > 0) {
		// Use a local copy so ArduinoJson copies the value rather than just linking the pointer
		char val_copy[32];
		strlcpy(val_copy, value, sizeof(val_copy));
		deviceObj[DeviceDefinitionKeys::value] = val_copy;
	}

	deviceObj[DeviceDefinitionKeys::index] = slot;

	doc.add(deviceObj);
	return;
}



/**
 * \brief EnumDevicesCallback function that adds the device to a JsonDocument
 *
 * \see serializeJsonDevice
 */
void DeviceManager::outputEnumeratedDevices(DeviceConfig* config, void* pv, JsonDocument* doc)
{
	DeviceOutput* out = (DeviceOutput*)pv;
  serializeJsonDevice(*doc, out->slot, *config, out->value);
}

bool DeviceManager::enumDevice(DeviceDisplay& dd, DeviceConfig& dc, uint8_t idx)
{
	if (dd.id==-1)
		return (dd.empty || dc.deviceFunction);	// if enumerating all devices, honor the unused request param
	else
		return (dd.id==idx);						// enumerate only the specific device requested
}


/**
 * \brief Compare device addresses
 */
inline bool matchAddress(uint8_t* detected, uint8_t* configured, uint8_t count) {
	if (!configured[0])
		return true;
	while (count-->0) {
		if (detected[count]!=configured[count])
			return false;
	}
	return true;
}

/**
 * \brief Find a device based on it's location.
 * A device's location is:
 *   - pinNr  for simple digital pin devices
 *   - pinNr+address for one-wire devices
 *   - btAddress for bluetooth devices (Tilt/Inkbird)
 *   - tplink_mac+tplink_child_id for tplink devices
 */
device_slot_t findHardwareDevice(DeviceConfig& find)
{
	DeviceConfig config;
	for (device_slot_t slot= 0; slot<Config::EepromFormat::MAX_DEVICES ; slot++) {
		config = eepromManager.fetchDevice(slot);

		if (find.deviceHardware==config.deviceHardware) {
			bool match = true;
			switch (find.deviceHardware) {
#ifdef HAS_BLUETOOTH
				case DEVICE_HARDWARE_BLUETOOTH_TILT:
				case DEVICE_HARDWARE_BLUETOOTH_INKBIRD:
					match &= find.hw.btAddress == config.hw.btAddress;
					break;
#endif
#ifdef EXTERN_SENSOR_ACTUATOR_SUPPORT
				case DEVICE_HARDWARE_TPLINK_SWITCH:
					if (strcmp(find.hw.tplink_mac, config.hw.tplink_mac) == 0 && strcmp(find.hw.tplink_child_id, config.hw.tplink_child_id) == 0)
						match &= true;
					else
						match &= false;
					break;
#endif

				case DEVICE_HARDWARE_ONEWIRE_TEMP:
					match &= matchAddress(find.hw.address, config.hw.address, 8);
					// fall through
				case DEVICE_HARDWARE_PIN:
					match &= find.hw.pinNr==config.hw.pinNr;
				default:	// this should not happen - if it does the device will be returned as matching.
					break;
			}
			if (match)
				return slot;
		}
	}
	return INVALID_SLOT;
}


/**
 * \brief Read a temp sensor device and convert the value into a string.
 *
 * **Warning:** the read value does not include any calibration offset.
 */
inline void DeviceManager::readTempSensorValue(DeviceHardware hw_type, DeviceConfig::Hardware hw, char* out)
{
#if !BREWPI_SIMULATE
	temperature temp = INVALID_TEMP;

	if(hw_type == DEVICE_HARDWARE_ONEWIRE_TEMP) {
		// Report the uncalibrated cached reading if the scanner has one.
		onewire_device_record* rec = ow_scanner.get_by_bytes(hw.address);
		if (rec && rec->isConnected()) {
			long_temperature t = rec->getTempFixedPoint();
			temp = constrainTemp(t, MIN_TEMP, MAX_TEMP);
		}
	}
#ifdef HAS_BLUETOOTH
	else if(hw_type == DEVICE_HARDWARE_BLUETOOTH_INKBIRD) {
		temp = bt_scanner.get_inkbird(hw.btAddress)->getTempFixedPoint();
	} else if(hw_type == DEVICE_HARDWARE_BLUETOOTH_TILT) {
		temp = bt_scanner.get_tilt(hw.btAddress)->getTempFixedPoint();
	}
#endif

	tempToString(out, temp, 3, 9);
#else
	strcpy(out, "0.00");
#endif
}


/**
 * \brief Process a found hardware device
 *
 * Used from the various enumerate* methods.
 */
void DeviceManager::handleEnumeratedDevice(DeviceConfig config_in, EnumerateHardware& h, EnumDevicesCallback callback, JsonDocument* doc)
{
	DeviceOutput out;
	DeviceConfig config = config_in;

	if (h.function && !isAssignable(deviceType(DeviceFunction(h.function)), config.deviceHardware))
		return; // device not applicable for required function

	out.slot = findHardwareDevice(config);
	DEBUG_ONLY(logInfoInt(INFO_MATCHING_DEVICE, out.slot));

	if (isDefinedSlot(out.slot)) {
		if (h.unused)	// only list unused devices, and this one is already used
			return;
		// display the actual matched value
		config = eepromManager.fetchDevice(out.slot);
	}

	out.value[0] = 0;
	if (h.values) {
		switch (config.deviceHardware) {
			case DEVICE_HARDWARE_ONEWIRE_TEMP:
#if HAS_BLUETOOTH
			case DEVICE_HARDWARE_BLUETOOTH_INKBIRD:
			case DEVICE_HARDWARE_BLUETOOTH_TILT:
#endif
				readTempSensorValue(config.deviceHardware, config.hw, out.value);
				break;

      // unassigned pins could be input or output so we can't determine any
      // other details from here.  values can be read once the pin has been
      // assigned a function
			default:
				break;
		}
	}

	callback(&config, &out, doc);
}


/**
 * \brief Enumerate the "pin" devices.
 *
 * Pin devices are those that are attached directly to a pin, not on a bus like OneWire
 */
void DeviceManager::enumeratePinDevices(EnumerateHardware& h, EnumDevicesCallback callback, JsonDocument* doc)
{
	DeviceConfig config;
	config.deviceHardware = DEVICE_HARDWARE_PIN;
	config.chamber = 1; // chamber 1 is default

	int8_t pin;
	for (uint8_t count=0; (pin=deviceManager.enumerateActuatorPins(count))>=0; count++) {
		if (h.pin!=-1 && h.pin!=pin)
			continue;
		config.hw.pinNr = pin;
		config.hw.invert = true; // make inverted default, because shiels have transistor on them
		handleEnumeratedDevice(config, h, callback, doc);
	}

	for (uint8_t count=0; (pin=deviceManager.enumerateSensorPins(count))>=0; count++) {
		if (h.pin!=-1 && h.pin!=pin)
			continue;
		config.hw.pinNr = pin;
		handleEnumeratedDevice(config, h, callback, doc);
	}
}


/**
 * \brief Enumerate all OneWire devices the scanner has discovered.
 *
 * Reads the cached device list maintained by OneWireScanner — no live bus I/O
 * happens here. A newly-plugged sensor will appear within one scanner cycle
 * (~15s by default). This mirrors how BLE sensors are enumerated.
 */
void DeviceManager::enumerateOneWireDevices(EnumerateHardware& h, EnumDevicesCallback callback, JsonDocument* doc)
{
#if !BREWPI_SIMULATE
	// Filter by pin if specified: we only have a single bus on oneWirePin.
	if (h.pin != -1 && h.pin != oneWirePin) return;

	DeviceConfig config;
	config.hw.pinNr = oneWirePin;
	config.chamber = 1; // chamber 1 is default
	config.deviceHardware = DEVICE_HARDWARE_ONEWIRE_TEMP;

	ow_scanner.for_each([&](const onewire_device_record& rec) {
		addressToBytes(rec.deviceAddress, config.hw.address);
		handleEnumeratedDevice(config, h, callback, doc);
	});
#endif
}


#ifdef HAS_BLUETOOTH
/**
 * \brief Enumerate all Inkbird devices
 *
 * \param h - Hardware spec, used to filter sensors
 * \param callback - Callback function, called for every found hardware device
 * \param output -
 * \param doc - JsonDocument to populate
 */
void DeviceManager::enumerateInkbirdDevices(EnumerateHardware& h, EnumDevicesCallback callback, JsonDocument* doc)
{
	DeviceConfig config;
	config.hw.pinNr = 0;  			// 0 for wireless devices
	config.chamber = 1; 			// chamber 1 is default
	config.deviceHardware = DEVICE_HARDWARE_BLUETOOTH_INKBIRD;

	for(inkbird & ib : lInkbirds) {
		config.hw.btAddress = ib.deviceAddress;
		handleEnumeratedDevice(config, h, callback, doc);
    }
}

/**
 * \brief Enumerate all Tilt devices
 *
 * \param h - Hardware spec, used to filter sensors
 * \param callback - Callback function, called for every found hardware device
 * \param output -
 * \param doc - JsonDocument to populate
 */
void DeviceManager::enumerateTiltDevices(EnumerateHardware& h, EnumDevicesCallback callback, JsonDocument* doc)
{
	DeviceConfig config;
	config.hw.pinNr = 0;  			// 0 for wireless devices
	config.chamber = 1; 			// chamber 1 is default
	config.deviceHardware = DEVICE_HARDWARE_BLUETOOTH_TILT;

	for(tilt & th : lTilts) {
		config.hw.btAddress = th.deviceAddress;
		handleEnumeratedDevice(config, h, callback, doc);
    }
}
#endif

#ifdef EXTERN_SENSOR_ACTUATOR_SUPPORT
/**
 * \brief Enumerate all TPLink Kasa Smart Plug devices
 *
 * \param h - Hardware spec, used to filter sensors
 * \param callback - Callback function, called for every found hardware device
 * \param output -
 * \param doc - JsonDocument to populate
 */
void DeviceManager::enumerateTplinkDevices(EnumerateHardware& h, EnumDevicesCallback callback, JsonDocument* doc)
{
	DeviceConfig config;
	config.hw.pinNr = 0;  			// 0 for wireless devices
	config.chamber = 1; 			// chamber 1 is default
	config.deviceHardware = DEVICE_HARDWARE_TPLINK_SWITCH;

	for(TPLinkPlug & tp : tp_link_scanner.lTPLinkPlugs) {
		strcpy(config.hw.tplink_mac, tp.device_mac);
		strcpy(config.hw.tplink_child_id, tp.child_id);
		handleEnumeratedDevice(config, h, callback, doc);
    }
}
#endif

/**
 * \brief Output devices matching hardware spec passed in
 */
void DeviceManager::enumerateHardware(JsonDocument& doc, EnumerateHardware spec)
{

	// Initialize the document as an array
	doc.to<JsonArray>();

	if (spec.hardware==-1 || isOneWire(DeviceHardware(spec.hardware))) {
		enumerateOneWireDevices(spec, outputEnumeratedDevices, &doc);
	}
	if (spec.hardware==-1 || isDigitalPin(DeviceHardware(spec.hardware))) {
		enumeratePinDevices(spec, outputEnumeratedDevices, &doc);
	}
#ifdef HAS_BLUETOOTH
	if (spec.hardware==-1 || spec.hardware==DEVICE_HARDWARE_BLUETOOTH_INKBIRD) {
		enumerateInkbirdDevices(spec, outputEnumeratedDevices, &doc);
	}
	if (spec.hardware==-1 || spec.hardware==DEVICE_HARDWARE_BLUETOOTH_TILT) {
		enumerateTiltDevices(spec, outputEnumeratedDevices, &doc);
	}
#endif

#ifdef EXTERN_SENSOR_ACTUATOR_SUPPORT
	if (spec.hardware==-1 || spec.hardware==DEVICE_HARDWARE_TPLINK_SWITCH) {
		enumerateTplinkDevices(spec, outputEnumeratedDevices, &doc);
	}
#endif

}

/**
 * \brief Output devices matching default hardware spec (all devices, no values)
 */
void DeviceManager::enumerateHardware(JsonDocument& doc)
{
	EnumerateHardware spec;
	enumerateHardware(doc, spec);
}

/**
 * \brief Parse JSON into a DeviceDisplay struct
 */
void DeviceManager::readJsonIntoDeviceDisplay(DeviceDisplay& dev) {
  JsonDocument doc;
  piLink.receiveJsonMessage(doc);

  JsonVariant id = doc[DeviceDisplayKeys::index];
  if(!id.isNull())
    dev.id = id.as<int8_t>();

  JsonVariant value = doc[DeviceDisplayKeys::value];
  if(!value.isNull())
    dev.value = value.as<int8_t>();

  JsonVariant write = doc[DeviceDisplayKeys::write];
  if(!write.isNull())
    dev.write = write.as<int8_t>();

  JsonVariant empty = doc[DeviceDisplayKeys::empty];
  if(!empty.isNull())
    dev.empty = empty.as<int8_t>();
}


/**
 * \brief Parse JSON into an EnumerateHardware struct
 */
void DeviceManager::readJsonIntoHardwareSpec(EnumerateHardware& hw) {
  JsonDocument doc;
  piLink.receiveJsonMessage(doc);

  JsonVariant hardware = doc[EnumerateHardwareKeys::hardware];
  if(!hardware.isNull())
    hw.hardware = hardware.as<int8_t>();

  JsonVariant pin = doc[EnumerateHardwareKeys::pin];
  if(!pin.isNull())
    hw.pin = pin.as<int8_t>();

  JsonVariant values = doc[EnumerateHardwareKeys::values];
  if(!values.isNull())
    hw.values = values.as<int8_t>();

  JsonVariant unused = doc[EnumerateHardwareKeys::unused];
  if(!unused.isNull())
    hw.unused = unused.as<int8_t>();

  JsonVariant function = doc[EnumerateHardwareKeys::function];
  if(!function.isNull())
    hw.function = function.as<int8_t>();
}


void UpdateDeviceState(DeviceDisplay& dd, DeviceConfig& dc, char* val)
{
	DeviceType dt = deviceType(dc.deviceFunction);
	if (dt==DEVICETYPE_NONE)
		return;

	void** ppv = deviceTarget(dc);
	if (ppv==NULL)
		return;

	if (dd.write>=0 && dt==DEVICETYPE_SWITCH_ACTUATOR) {
		// write value to a specific device. For now, only actuators are relevant targets
		DEBUG_ONLY(logInfoInt(INFO_SETTING_ACTIVATOR_STATE, dd.write!=0));
		((Actuator*)*ppv)->setActive(dd.write!=0);
	} else if (dd.value==1) {		// read values
		if (dt==DEVICETYPE_SWITCH_SENSOR) {
			sprintf(val, STR_FMT_U, (unsigned int) ((SwitchSensor*)*ppv)->sense()!=0); // cheaper than itoa, because it overlaps with vsnprintf
		} else if (dt==DEVICETYPE_TEMP_SENSOR) {
			BasicTempSensor& s = unwrapSensor(dc.deviceFunction, *ppv);
			temperature temp = s.read();
			tempToString(val, temp, 3, 9);
		} else if (dt==DEVICETYPE_SWITCH_ACTUATOR) {
			sprintf(val, STR_FMT_U, (unsigned int) ((Actuator*)*ppv)->isActive()!=0);
		}
	}
}

/**
 * \brief Print list of hardware devices
 * \param doc - JsonDocument to add results to
 */
void DeviceManager::listDevices(JsonDocument& doc) {
	DeviceConfig dc;
	DeviceDisplay dd;

  readJsonIntoDeviceDisplay(dd);
    doc.to<JsonArray>();

	if (dd.id==-2) {
		if (dd.write>=0)
			tempControl.cameraLight.setActive(dd.write!=0);

		return;
	}

	for (device_slot_t idx=0; idx<Config::EepromFormat::MAX_DEVICES; idx++) {
		dc = eepromManager.fetchDevice(idx);
		if (deviceManager.enumDevice(dd, dc, idx))
		{
			char val[10];
			val[0] = 0;
			UpdateDeviceState(dd, dc, val);
			deviceManager.serializeJsonDevice(doc, idx, dc, val);
		}
	}
}


/**
 * \brief Print the raw temp readings from all temp sensors.
 *
 * Allows logging temps that aren't part of the control logic.
 * \param doc - JsonDocument to add results to
 */
void DeviceManager::rawDeviceValues(JsonDocument& doc) {
	EnumerateHardware spec;
	// set up defaults
	spec.unused = 0;			// list all devices
	spec.values = 0;			// don't list values
	spec.pin = -1;				// any pin
	spec.hardware = -1;		// any hardware
	spec.function = 0;		// no function restriction

	enumerateOneWireDevices(spec, outputRawDeviceValue, &doc);
#ifdef HAS_BLUETOOTH
	spec.values = 1;  // Costs nothing for non-Onewire temp sensors
	enumerateInkbirdDevices(spec, outputRawDeviceValue, &doc);
	enumerateTiltDevices(spec, outputRawDeviceValue, &doc);
#endif
}


/**
 * \brief Print the sensor's information & current reading.
 */
void DeviceManager::outputRawDeviceValue(DeviceConfig* config, void* pv, JsonDocument* doc)
{
  if(config->deviceHardware == DeviceHardware::DEVICE_HARDWARE_ONEWIRE_TEMP) {
    // Read the temp
    char str_temp[10];
    DeviceManager::readTempSensorValue(config->deviceHardware, config->hw, str_temp);

    // Pretty-print the address
    char devName[17];
    printBytes(config->hw.address, 8, devName);

    std::string humanName = DeviceNameManager::getDeviceName(devName);

    JsonObject deviceObj = doc->add<JsonObject>();
    deviceObj["device"] = devName;
    deviceObj["value"] = str_temp;
    deviceObj["name"] = humanName;
  }

#ifdef HAS_BLUETOOTH
  if(config->deviceHardware == DeviceHardware::DEVICE_HARDWARE_BLUETOOTH_INKBIRD || 
  	config->deviceHardware == DeviceHardware::DEVICE_HARDWARE_BLUETOOTH_TILT) {
    // Read the temp
    char str_temp[10];

	if(config->deviceHardware == DeviceHardware::DEVICE_HARDWARE_BLUETOOTH_INKBIRD)
		tempToString(str_temp, bt_scanner.get_inkbird(config->hw.btAddress)->getTempFixedPoint(), 3, 9);
	else if(config->deviceHardware == DeviceHardware::DEVICE_HARDWARE_BLUETOOTH_TILT)
		tempToString(str_temp, bt_scanner.get_tilt(config->hw.btAddress)->getTempFixedPoint(), 3, 9);

    // Pretty-print the address
    std::string humanName = DeviceNameManager::getDeviceName(config->hw.btAddress.toString().c_str());

    JsonObject deviceObj = doc->add<JsonObject>();
    deviceObj["device"] = config->hw.btAddress.toString();
    deviceObj["value"] = str_temp;
    deviceObj["name"] = humanName;
  }
#endif
}

/**
 * @brief Scan the filesystem for "pin" actuators, and set their pin mode early.
 * 
 */
void DeviceManager::preloadActuatorPins() {
	// Quick scan for pin-based devices to get their invert state, and set pin mode (with them defaulting to "off")
	for (uint8_t i = 0; i < Config::EepromFormat::MAX_DEVICES; i++) {
		DeviceConfig dev;
		dev = eepromManager.fetchDevice(i);
		if (dev.deviceHardware == DEVICE_HARDWARE_PIN) {
			if (deviceType(dev.deviceFunction) == DEVICETYPE_SWITCH_ACTUATOR) {
				gpio_set_level((gpio_num_t)dev.hw.pinNr, dev.hw.invert ? HIGH : LOW);
				gpio_set_direction((gpio_num_t)dev.hw.pinNr, GPIO_MODE_OUTPUT);
				gpio_set_level((gpio_num_t)dev.hw.pinNr, dev.hw.invert ? HIGH : LOW);
			} else if (deviceType(dev.deviceFunction) == DEVICETYPE_SWITCH_SENSOR) {
				gpio_set_direction((gpio_num_t)dev.hw.pinNr, GPIO_MODE_INPUT);
			} else {
				continue; // only process pin-based actuators
			}
		}
	}
}



/**
 * Determines the class of device for the given DeviceID.
 */
DeviceType deviceType(DeviceFunction id) {
	switch (id) {
	case DEVICE_CHAMBER_DOOR:
		return DEVICETYPE_SWITCH_SENSOR;

	case DEVICE_CHAMBER_HEAT:
	case DEVICE_CHAMBER_COOL:
	case DEVICE_CHAMBER_LIGHT:
	case DEVICE_CHAMBER_FAN:
	case DEVICE_BEER_HEAT:
	case DEVICE_BEER_COOL:
		return DEVICETYPE_SWITCH_ACTUATOR;

	case DEVICE_CHAMBER_TEMP:
	case DEVICE_CHAMBER_ROOM_TEMP:
	case DEVICE_BEER_TEMP:
	case DEVICE_BEER_TEMP2:
		return DEVICETYPE_TEMP_SENSOR;

	default:
		return DEVICETYPE_NONE;
	}
}

DeviceManager deviceManager;
