#include "hal/boards/board_composition.h"

#include "hal/outputs/unconfigured_output_driver.h"

OutputDriver *board_output_driver(void)
{
    static UnconfiguredOutputDriver driver;
    static bool initialized;
    if (!initialized) {
        unconfigured_output_driver_init(&driver);
        initialized = true;
    }
    return &driver.base;
}

MasterValveDriver *board_master_valve_driver(void)
{
    return NULL;
}
