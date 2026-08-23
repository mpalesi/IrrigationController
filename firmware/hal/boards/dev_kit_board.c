#include "hal/boards/board_composition.h"

#include "hal/outputs/virtual_output_driver.h"
#include "infrastructure/logging/esp_idf_logger.h"

OutputDriver *board_output_driver(void)
{
    static VirtualOutputDriver driver;
    static bool initialized;
    if (!initialized) {
        virtual_output_driver_init(&driver, esp_idf_logger_create());
        initialized = true;
    }
    return &driver.base;
}
