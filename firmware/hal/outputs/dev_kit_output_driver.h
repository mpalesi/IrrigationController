#pragma once

#include "hal/outputs/output_driver.h"
#include "infrastructure/logging/logger.h"

typedef struct {
    OutputDriver base;
    Logger logger;
} DevKitOutputDriver;

void dev_kit_output_driver_init(DevKitOutputDriver *driver, Logger logger);
