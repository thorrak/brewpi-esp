/*
 * Copyright 2012-2013 BrewPi/Elco Jacobs.
 * Copyright 2013 Matthew McGowan.
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
#include "FilterCascaded.h"
#include "TempSensorBasic.h"
#include <stdlib.h>

#define TEMP_SENSOR_DISCONNECTED INVALID_TEMP

#ifndef TEMP_SENSOR_CASCADED_FILTER
#define TEMP_SENSOR_CASCADED_FILTER 1
#endif

#if TEMP_SENSOR_CASCADED_FILTER
typedef CascadedFilter TempSensorFilter;
#else
typedef FixedFilter TempSensorFilter;
#endif


/**
 * \brief Types of temp sensors, defined by their use.
 */
enum TempSensorType {
	TEMP_SENSOR_TYPE_FRIDGE=1,
	TEMP_SENSOR_TYPE_BEER
};


/**
 * \brief General interface for all temperature sensor devices.
 *
 * Wraps up a BasicTempSensor and processes the values through several filters.
 * The filters are either CascadedFilter or FixedFilter implementations,
 * depending on the compile time value of the TEMP_SENSOR_CASCADED_FILTER
 * macro.
 */
class TempSensor {
	public:
	TempSensor(TempSensorType sensorType, BasicTempSensor* sensor =NULL)  {
		updateCounter = 255; // first update for slope filter after (255-4s)
		setSensor(sensor);
	 }

  /**
   * \brief Set the underlying BasicTempSensor
   *
   * @param sensor - Temp sensor to wrap
   */
  void setSensor(BasicTempSensor* sensor) {
   _sensor = sensor;
   failedReadCount = -1;
   rawTemperature = TEMP_SENSOR_DISCONNECTED;
  }

  /**
   * \brief Check if the sensor has a slow filter.
   */
	bool hasSlowFilter() { return true; }

  /**
   * \brief Check if the sensor has a fast filter.
   */
	bool hasFastFilter() { return true; }

  /**
   * \brief Check if the sensor has a slope filter.
   */
	bool hasSlopeFilter() { return true; }

	void init();

  /**
   * \brief Check if the sensor is connected.
   */
	bool isConnected() { return _sensor != NULL && _sensor->isConnected(); }

	void update();

	// Returns the latest reading saved by init() or update(), before smoothing.
	// Reads memory only; does not contact the sensor or request a new measurement.
	// Convert a valid result to Celsius with: value / 512.0 + 48.
	// Returns INVALID_TEMP if no valid reading is cached, including after a
	// failed read or sensor replacement.
	temperature readRawCached() const { return rawTemperature; }

	temperature readFastFiltered();

	temperature readSlowFiltered();

	temperature readSlope();

	temperature detectPosPeak();

	temperature detectNegPeak();

	void setFastFilterCoefficients(uint8_t b);

	void setSlowFilterCoefficients(uint8_t b);

	void setSlopeFilterCoefficients(uint8_t b);

	BasicTempSensor& sensor();

  /**
   * \brief Get the number of consecutive failed reads.
   *
   * @return -1 if uninitialized, 0 if currently connected and reading successfully,
   *         1-120 if sensor is failing/disconnected (capped at 120)
   */
	int8_t getFailedReadCount() { return failedReadCount; }

	private:
	BasicTempSensor* _sensor; //!< Wrapped basic sensor
	temperature rawTemperature;
	TempSensorFilter fastFilter; //!< Fast reacting filter
	TempSensorFilter slowFilter; //!< Slow reacting filter
	TempSensorFilter slopeFilter; //!< Slope filter
	unsigned char updateCounter;
	temperature_precise prevOutputForSlope;
	void initialize_filters(temperature temp);

  /**
   * \brief An indication of how stale the data is in the filters.
   *
   * Each time a read fails, this value is incremented.  It's used to reset the
   * filters after a large enough disconnect delay, and on the first init.
   *
   *  `-1` for uninitialized, `>=0` afterwards.
   */
	int8_t failedReadCount;

	friend class ChamberManager;
	friend class Chamber;
	friend class DeviceManager;
};
