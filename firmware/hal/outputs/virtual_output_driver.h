#pragma once

#include <stdbool.h>

#include "common/constants.h"
#include "hal/outputs/output_driver.h"
#include "infrastructure/logging/logger.h"

typedef struct {
    OutputDriver base;
    /* Last hardware state reported by this test double; State Store owns logical state. */
    bool reported_outputs[IRRIGATION_LOCAL_OUTPUT_COUNT];
    Logger logger;
} VirtualOutputDriver;

void virtual_output_driver_init(VirtualOutputDriver *driver, Logger logger);
