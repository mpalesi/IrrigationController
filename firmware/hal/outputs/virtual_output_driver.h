#pragma once

#include <stdbool.h>

#include "common/constants.h"
#include "hal/outputs/output_driver.h"
#include "infrastructure/logging/logger.h"

typedef struct {
    OutputDriver base;
    bool outputs[IRRIGATION_LOCAL_OUTPUT_COUNT];
    Logger logger;
} VirtualOutputDriver;

void virtual_output_driver_init(VirtualOutputDriver *driver, Logger logger);
