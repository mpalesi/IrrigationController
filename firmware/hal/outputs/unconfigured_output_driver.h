#pragma once

#include "hal/outputs/output_driver.h"

typedef struct {
    OutputDriver base;
} UnconfiguredOutputDriver;

void unconfigured_output_driver_init(UnconfiguredOutputDriver *driver);
