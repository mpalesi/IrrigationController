#include "hal/status_led/status_led_driver.h"

#include <stddef.h>

IrrigationResult status_led_driver_set_state(StatusLedDriver *driver, StatusLedState state)
{
    return driver == NULL || driver->vtable == NULL || driver->vtable->set_state == NULL
        ? IRRIGATION_RESULT_NOT_SUPPORTED
        : driver->vtable->set_state(driver, state);
}
