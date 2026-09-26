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
#include "TempSensor.h"
#include "PiLink.h"
#include "Ticks.h"

#include <algorithm>
using std::min;


/**
 * \brief Initialize the temperature filters
 */
void TempSensor::initialize_filters(temperature temp){
	rawTemperature = temp;
	fastFilter.init(temp);
	slowFilter.init(temp);
	slopeFilter.init(0);
	prevOutputForSlope = slowFilter.readOutputDoublePrecision();
	failedReadCount = 0;
}


/**
 * \brief Initialize temp sensor
 */
void TempSensor::init()
{
	rawTemperature = TEMP_SENSOR_DISCONNECTED;
	logDebug("tempsensor::init - begin %d", failedReadCount);
	if (_sensor && _sensor->init() && (failedReadCount<0 || failedReadCount>60)) {
		temperature temp = _sensor->read();
		if (temp!=TEMP_SENSOR_DISCONNECTED) {
			logDebug("initializing filters with value %d", temp);
			initialize_filters(temp);
		}		
	}
}

/**
 * \brief Read the sensor and update the filters
 */
void TempSensor::update()
{
	temperature temp;
	if (!_sensor || (temp=_sensor->read())==TEMP_SENSOR_DISCONNECTED) {
		rawTemperature = TEMP_SENSOR_DISCONNECTED;
		if(failedReadCount >= 0)  // Don't increment if we haven't had a successful read yet (meaning failedReadCount is -1, and the filters are uninitialized)
			failedReadCount++;
		failedReadCount = min(failedReadCount,int8_t(120));	// limit
		return;
	}

	rawTemperature = temp;

	// We successfully read the temp. If this is the initial read (-1), initialize the filters.
	// Also reinitialize the filters if we had more than 60 failed reads. 
	if(failedReadCount < 0 || failedReadCount > 60){
		initialize_filters(temp);
		return;
	} else {
		// If we hit here, we have between 0 and 60 failed reads - but the last read was successful. 
		// Reset the fail count.
		failedReadCount = 0;
	}	
		
	fastFilter.add(temp);
	slowFilter.add(temp);
		
	// update slope filter every 3 samples.
	// averaged differences will give the slope. Use the slow filter as input
	updateCounter--;
	// initialize first read for slope filter after (255-4) seconds. This prevents an influence for the startup inaccuracy.
	if(updateCounter == 4){
		// only happens once after startup.
		prevOutputForSlope = slowFilter.readOutputDoublePrecision();
	}
	if(updateCounter == 0){
		temperature_precise slowFilterOutput = slowFilter.readOutputDoublePrecision();
		temperature_precise diff =  slowFilterOutput - prevOutputForSlope;
		temperature diff_upper = diff >> 16;
		if(diff_upper > 27){ // limit to prevent overflow INT_MAX/1200 = 27.14
			diff = (27l << 16);
		}
		else if(diff_upper < -27){
			diff = (-27l << 16);
		}
		slopeFilter.addDoublePrecision(1200*diff); // Multiply by 1200 (1h/4s), shift to single precision
		prevOutputForSlope = slowFilterOutput;
		updateCounter = 3;
	}
}

/**
 * \brief Read the sensor value after processing through the fast filter
 */
temperature TempSensor::readFastFiltered(){
	return fastFilter.readOutput(); //return most recent unfiltered value
}

/**
 * \brief Read the sensor value after processing through the slow filter
 */
temperature TempSensor::readSlowFiltered(){
  return slowFilter.readOutput(); //return most recent unfiltered value
}

/**
 * \brief Read the sensor value after processing through the slope filter
 */
temperature TempSensor::readSlope(){
	// return slope per hour.
	temperature_precise doublePrecision = slopeFilter.readOutputDoublePrecision();
	return doublePrecision>>16; // shift to single precision
}


/**
 * \brief Detect the positive peak
 *
 * Uses the detectPosPeak() method of the slow filter
 */
temperature TempSensor::detectPosPeak(){
	return slowFilter.detectPosPeak();
}


/**
 * \brief Detect the negative peak
 *
 * Uses the detectNegPeak() method of the slow filter
 */
temperature TempSensor::detectNegPeak(){
	return slowFilter.detectNegPeak();
}

/**
 * \brief Set the b coefficients on the fast filter
 *
 * @param b - New coefficient value
 */
void TempSensor::setFastFilterCoefficients(uint8_t b){
	fastFilter.setCoefficients(b);
}


/**
 * \brief Set the b coefficients on the slow filter
 *
 * @param b - New coefficient value
 */
void TempSensor::setSlowFilterCoefficients(uint8_t b){
	slowFilter.setCoefficients(b);
}


/**
 * \brief Set the b coefficients on the slope filter
 *
 * @param b - New coefficient value
 */
void TempSensor::setSlopeFilterCoefficients(uint8_t b){
	slopeFilter.setCoefficients(b);
}

/**
 * \brief Get wrapped sensor
 */
BasicTempSensor& TempSensor::sensor() {
	return *_sensor;
}
