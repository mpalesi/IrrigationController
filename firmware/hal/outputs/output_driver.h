#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app/result.h"

typedef struct OutputDriver OutputDriver;

typedef struct {
    IrrigationResult (*initialize_safe_off)(OutputDriver *driver);
    IrrigationResult (*set_output)(OutputDriver *driver, size_t output_index, bool enabled);
    IrrigationResult (*get_output)(const OutputDriver *driver, size_t output_index, bool *enabled);
} OutputDriverVTable;

struct OutputDriver {
    const OutputDriverVTable *vtable;
};

IrrigationResult output_driver_initialize_safe_off(OutputDriver *driver);
IrrigationResult output_driver_set(OutputDriver *driver, size_t output_index, bool enabled);
IrrigationResult output_driver_get(const OutputDriver *driver, size_t output_index, bool *enabled);
