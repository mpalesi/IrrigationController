#include "domain/master_valve/master_valve_driver.h"

#include <stddef.h>

IrrigationResult master_valve_driver_open_and_confirm(MasterValveDriver *driver)
{
    return (driver == NULL || driver->vtable == NULL || driver->vtable->open_and_confirm == NULL)
        ? IRRIGATION_RESULT_INTERNAL_ERROR
        : driver->vtable->open_and_confirm(driver);
}

IrrigationResult master_valve_driver_close_and_confirm(MasterValveDriver *driver)
{
    return (driver == NULL || driver->vtable == NULL || driver->vtable->close_and_confirm == NULL)
        ? IRRIGATION_RESULT_INTERNAL_ERROR
        : driver->vtable->close_and_confirm(driver);
}
