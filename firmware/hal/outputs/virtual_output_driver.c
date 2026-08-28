#include "hal/outputs/virtual_output_driver.h"

#include <string.h>

static IrrigationResult virtual_initialize_safe_off(OutputDriver *base)
{
    VirtualOutputDriver *driver = (VirtualOutputDriver *)base;
    memset(driver->reported_outputs, 0, sizeof(driver->reported_outputs));
    logger_log(&driver->logger, LOG_LEVEL_INFO, "virtual_output", "all logical outputs set OFF");
    return IRRIGATION_RESULT_OK;
}

static IrrigationResult virtual_set_output(OutputDriver *base, size_t output_index, bool enabled)
{
    VirtualOutputDriver *driver = (VirtualOutputDriver *)base;
    if (output_index >= IRRIGATION_LOCAL_OUTPUT_COUNT) {
        return IRRIGATION_RESULT_NOT_FOUND;
    }
    driver->reported_outputs[output_index] = enabled;
    logger_log(&driver->logger, LOG_LEVEL_INFO, "virtual_output", enabled ? "logical output ON" : "logical output OFF");
    return IRRIGATION_RESULT_OK;
}

static IrrigationResult virtual_get_output(const OutputDriver *base, size_t output_index, bool *enabled)
{
    const VirtualOutputDriver *driver = (const VirtualOutputDriver *)base;
    if (output_index >= IRRIGATION_LOCAL_OUTPUT_COUNT || enabled == NULL) {
        return IRRIGATION_RESULT_NOT_FOUND;
    }
    *enabled = driver->reported_outputs[output_index];
    return IRRIGATION_RESULT_OK;
}

static const OutputDriverVTable VIRTUAL_OUTPUT_DRIVER_VTABLE = {
    .initialize_safe_off = virtual_initialize_safe_off,
    .set_output = virtual_set_output,
    .get_output = virtual_get_output,
};

void virtual_output_driver_init(VirtualOutputDriver *driver, Logger logger)
{
    if (driver == NULL) {
        return;
    }
    memset(driver, 0, sizeof(*driver));
    driver->base.vtable = &VIRTUAL_OUTPUT_DRIVER_VTABLE;
    driver->logger = logger;
}
