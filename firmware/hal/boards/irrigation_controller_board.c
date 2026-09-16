#include "hal/boards/board_composition.h"

#include "hal/master_valve/esp_shelly_http_transport.h"
#include "hal/master_valve/shelly_master_valve_driver.h"
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
    static EspShellyHttpTransport transport;
    static ShellyMasterValveDriver driver;
    static bool initialized;
    if (!initialized) {
        esp_shelly_http_transport_init(&transport);
        shelly_master_valve_driver_init(&driver, &transport.base, (ShellyMasterValveConfiguration){
            .host = CONFIG_IRRIGATION_SHELLY_MASTER_VALVE_HOST,
            .port = CONFIG_IRRIGATION_SHELLY_MASTER_VALVE_PORT,
            .switch_id = CONFIG_IRRIGATION_SHELLY_MASTER_VALVE_SWITCH_ID,
            .operation_timeout_ms = CONFIG_IRRIGATION_SHELLY_MASTER_VALVE_OPERATION_TIMEOUT_MS,
        });
        initialized = true;
    }
    return &driver.base;
}
