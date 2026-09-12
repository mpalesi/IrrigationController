#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "hal/mcp23017/mcp23017.h"
#include "hal/outputs/output_driver.h"
#include "infrastructure/logging/logger.h"

typedef struct {
    OutputDriver base;
    Mcp23017 mcp23017;
    Logger logger;
    uint8_t output_mask;
    bool initialized;
} IrrigationControllerOutputDriver;

void irrigation_controller_output_driver_init(IrrigationControllerOutputDriver *driver, Logger logger);
