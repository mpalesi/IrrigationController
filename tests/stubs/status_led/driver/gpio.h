#pragma once
#include "esp_timer.h"
#define GPIO_NUM_25 25
#define GPIO_MODE_OUTPUT 2
esp_err_t gpio_reset_pin(int pin);
esp_err_t gpio_set_level(int pin, uint32_t level);
esp_err_t gpio_set_direction(int pin, int mode);
