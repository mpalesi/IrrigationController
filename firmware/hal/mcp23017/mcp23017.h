#pragma once

#include <stdint.h>

#include "driver/i2c.h"
#include "esp_err.h"

typedef struct {
    i2c_port_t port;
    uint8_t address;
} Mcp23017;

typedef struct {
    i2c_port_t port;
    gpio_num_t sda_gpio;
    gpio_num_t scl_gpio;
    uint8_t address;
    uint32_t clock_hz;
} Mcp23017Configuration;

esp_err_t mcp23017_init(Mcp23017 *device, const Mcp23017Configuration *configuration);
esp_err_t mcp23017_write_port_a(Mcp23017 *device, uint8_t values);
esp_err_t mcp23017_configure_port_a_outputs(Mcp23017 *device, uint8_t output_mask);
