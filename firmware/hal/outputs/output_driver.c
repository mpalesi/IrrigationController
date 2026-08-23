#include "hal/outputs/output_driver.h"

IrrigationResult output_driver_initialize_safe_off(OutputDriver *driver)
{
    return (driver == NULL || driver->vtable == NULL || driver->vtable->initialize_safe_off == NULL)
        ? IRRIGATION_RESULT_INTERNAL_ERROR
        : driver->vtable->initialize_safe_off(driver);
}

IrrigationResult output_driver_set(OutputDriver *driver, size_t output_index, bool enabled)
{
    return (driver == NULL || driver->vtable == NULL || driver->vtable->set_output == NULL)
        ? IRRIGATION_RESULT_INTERNAL_ERROR
        : driver->vtable->set_output(driver, output_index, enabled);
}

IrrigationResult output_driver_get(const OutputDriver *driver, size_t output_index, bool *enabled)
{
    return (driver == NULL || driver->vtable == NULL || driver->vtable->get_output == NULL)
        ? IRRIGATION_RESULT_INTERNAL_ERROR
        : driver->vtable->get_output(driver, output_index, enabled);
}
