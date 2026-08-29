#include "hal/boards/board_composition.h"

#include "hal/master_valve/dev_kit_master_valve_driver.h"
#include "hal/outputs/dev_kit_output_driver.h"
#include "infrastructure/logging/esp_idf_logger.h"

OutputDriver *board_output_driver(void)
{
    static DevKitOutputDriver driver;
    static bool initialized;
    if (!initialized) {
        dev_kit_output_driver_init(&driver, esp_idf_logger_create());
        initialized = true;
    }
    return &driver.base;
}

MasterValveDriver *board_master_valve_driver(void)
{
    static DevKitMasterValveDriver driver;
    static bool initialized;
    if (!initialized) {
        dev_kit_master_valve_driver_init(&driver);
        initialized = true;
    }
    return &driver.base;
}
