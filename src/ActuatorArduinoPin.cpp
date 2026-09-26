
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "Brewpi.h"
#include "Pins.h"
#include "ActuatorArduinoPin.h"
#include "Display.h"
#include "EepromManager.h"  // for extendedSettings
#include <driver/gpio.h>

#ifndef HIGH
#define HIGH 1
#endif
#ifndef LOW
#define LOW 0
#endif


void DigitalPinActuator::setActive(bool active_setting) {

    bool oldActive = active;
    this->active = active_setting;
    gpio_set_level((gpio_num_t)pin, active_setting^invert ? HIGH : LOW);

    if (oldActive != active && extendedSettings.resetScreenOnPin) {
        // We toggled one of the pins, which can cause issues for the display. 
        // Delay slightly to let everything settle down, then reinit the display
        vTaskDelay(pdMS_TO_TICKS(100));
        display.reset();
        display.printAll();
    }

}

