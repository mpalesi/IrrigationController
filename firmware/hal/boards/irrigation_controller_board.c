#include "hal/boards/board_composition.h"

#include "hal/outputs/irrigation_controller_output_driver.h"
#include "infrastructure/logging/esp_idf_logger.h"

OutputDriver *board_output_driver(void)
{
    static IrrigationControllerOutputDriver driver;
    static bool initialized;
    if (!initialized) {
        irrigation_controller_output_driver_init(&driver, esp_idf_logger_create());
        initialized = true;
    }
    return &driver.base;
}

MasterValveDriver *board_master_valve_driver(void)
{
    return NULL;
}
