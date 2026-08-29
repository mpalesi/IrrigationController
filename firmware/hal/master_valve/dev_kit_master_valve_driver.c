#include "hal/master_valve/dev_kit_master_valve_driver.h"

#include "driver/gpio.h"

#define DEV_KIT_MASTER_VALVE_GPIO GPIO_NUM_26

static IrrigationResult set_master_valve(bool enabled)
{
    return gpio_set_level(DEV_KIT_MASTER_VALVE_GPIO, enabled ? 1 : 0) == ESP_OK &&
           (gpio_get_level(DEV_KIT_MASTER_VALVE_GPIO) != 0) == enabled
        ? IRRIGATION_RESULT_OK
        : IRRIGATION_RESULT_INTERNAL_ERROR;
}

static IrrigationResult dev_kit_open_and_confirm(MasterValveDriver *base)
{
    (void)base;
    return set_master_valve(true);
}

static IrrigationResult dev_kit_close_and_confirm(MasterValveDriver *base)
{
    (void)base;
    return set_master_valve(false);
}

static const MasterValveDriverVTable DEV_KIT_MASTER_VALVE_DRIVER_VTABLE = {
    .open_and_confirm = dev_kit_open_and_confirm,
    .close_and_confirm = dev_kit_close_and_confirm,
};

void dev_kit_master_valve_driver_init(DevKitMasterValveDriver *driver)
{
    if (driver != NULL) {
        *driver = (DevKitMasterValveDriver){.base.vtable = &DEV_KIT_MASTER_VALVE_DRIVER_VTABLE};
        (void)gpio_reset_pin(DEV_KIT_MASTER_VALVE_GPIO);
        (void)gpio_set_direction(DEV_KIT_MASTER_VALVE_GPIO, GPIO_MODE_INPUT_OUTPUT);
        (void)set_master_valve(false);
    }
}
