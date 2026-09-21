#pragma once

#include <stdbool.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#include "hal/status_led/status_led_driver.h"

typedef struct {
    StatusLedDriver base;
    esp_timer_handle_t timer;
    portMUX_TYPE lock;
    StatusLedState state;
    bool led_on;
    bool initialized;
} IrrigationControllerStatusLedDriver;

IrrigationResult irrigation_controller_status_led_driver_init(IrrigationControllerStatusLedDriver *driver);
