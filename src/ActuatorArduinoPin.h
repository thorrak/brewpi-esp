/* 
 * File:   ArduinoActuator.h
 * Author: mat
 *
 * Created on 19 August 2013, 20:32
 */

#pragma once

#include "Actuator.h"
#include <driver/gpio.h>

//template<uint8_t pin, bool invert>
//class DigitalConstantPinActuator ACTUATOR_BASE_CLASS_DECL
//{
//	private:
//	bool active;
//
//	public:
//	DigitalConstantPinActuator() : active(false)
//	{
//		setActive(false);
//		fastPinMode(pin, OUTPUT);
//	}
//
//	inline ACTUATOR_METHOD void setActive(bool active) {
//		this->active = active;
//		fastDigitalWrite(pin, active^invert ? HIGH : LOW);
//	}
//
//	bool isActive() { return active; }
//
//};

class DigitalPinActuator ACTUATOR_BASE_CLASS_DECL
{
	private:
	bool invert;
	uint8_t pin;
	bool active = false;
	public:
	DigitalPinActuator(uint8_t pin, bool invert) {
		this->invert = invert;
		this->pin = pin;
#ifdef BREWPI_CHILLSIM_TEST
        if (pin == 25 || pin == 26) this->invert = true;
#endif
		setActive(false);
		gpio_set_direction((gpio_num_t)pin, GPIO_MODE_OUTPUT);
	}
	
	void setActive(bool active_setting);
	
	bool isActive() { return active; }
};
