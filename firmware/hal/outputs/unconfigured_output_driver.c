#include "hal/outputs/unconfigured_output_driver.h"

static IrrigationResult unconfigured_operation(OutputDriver *driver)
{
    (void)driver;
    return IRRIGATION_RESULT_NOT_SUPPORTED;
}

static IrrigationResult unconfigured_set(OutputDriver *driver, size_t output_index, bool enabled)
{
    (void)output_index;
    (void)enabled;
    return unconfigured_operation(driver);
}

static IrrigationResult unconfigured_get(const OutputDriver *driver, size_t output_index, bool *enabled)
{
    (void)driver;
    (void)output_index;
    (void)enabled;
    return IRRIGATION_RESULT_NOT_SUPPORTED;
}

static const OutputDriverVTable UNCONFIGURED_OUTPUT_DRIVER_VTABLE = {
    .initialize_safe_off = unconfigured_operation,
    .set_output = unconfigured_set,
    .get_output = unconfigured_get,
};

void unconfigured_output_driver_init(UnconfiguredOutputDriver *driver)
{
    if (driver != NULL) {
        driver->base.vtable = &UNCONFIGURED_OUTPUT_DRIVER_VTABLE;
    }
}
