#include "hal/outputs/dev_kit_output_driver.h"

#include "driver/gpio.h"

#include "common/constants.h"

static const gpio_num_t ZONE_PINS[IRRIGATION_LOCAL_OUTPUT_COUNT] = {
    GPIO_NUM_16, GPIO_NUM_17, GPIO_NUM_18, GPIO_NUM_19,
    GPIO_NUM_21, GPIO_NUM_22, GPIO_NUM_23, GPIO_NUM_25,
};

static IrrigationResult configure_output(size_t output_index, bool enabled)
{
    if (output_index >= IRRIGATION_LOCAL_OUTPUT_COUNT) {
        return IRRIGATION_RESULT_NOT_FOUND;
    }
    return gpio_set_level(ZONE_PINS[output_index], enabled ? 1 : 0) == ESP_OK
        ? IRRIGATION_RESULT_OK
        : IRRIGATION_RESULT_INTERNAL_ERROR;
}

static IrrigationResult dev_kit_initialize_safe_off(OutputDriver *base)
{
    DevKitOutputDriver *driver = (DevKitOutputDriver *)base;
    for (size_t index = 0U; index < IRRIGATION_LOCAL_OUTPUT_COUNT; ++index) {
        if (gpio_reset_pin(ZONE_PINS[index]) != ESP_OK ||
            gpio_set_direction(ZONE_PINS[index], GPIO_MODE_OUTPUT) != ESP_OK ||
            configure_output(index, false) != IRRIGATION_RESULT_OK) {
            return IRRIGATION_RESULT_INTERNAL_ERROR;
        }
    }
    logger_log(&driver->logger, LOG_LEVEL_INFO, "dev_kit_output", "zone GPIO outputs set OFF");
    return IRRIGATION_RESULT_OK;
}

static IrrigationResult dev_kit_set_output(OutputDriver *base, size_t output_index, bool enabled)
{
    DevKitOutputDriver *driver = (DevKitOutputDriver *)base;
    IrrigationResult result = configure_output(output_index, enabled);
    if (result == IRRIGATION_RESULT_OK) {
        logger_log(&driver->logger, LOG_LEVEL_INFO, "dev_kit_output", enabled ? "zone GPIO ON" : "zone GPIO OFF");
    }
    return result;
}

static IrrigationResult dev_kit_get_output(const OutputDriver *base, size_t output_index, bool *enabled)
{
    (void)base;
    if (output_index >= IRRIGATION_LOCAL_OUTPUT_COUNT || enabled == NULL) {
        return IRRIGATION_RESULT_NOT_FOUND;
    }
    *enabled = gpio_get_level(ZONE_PINS[output_index]) != 0;
    return IRRIGATION_RESULT_OK;
}

static const OutputDriverVTable DEV_KIT_OUTPUT_DRIVER_VTABLE = {
    .initialize_safe_off = dev_kit_initialize_safe_off,
    .set_output = dev_kit_set_output,
    .get_output = dev_kit_get_output,
};

void dev_kit_output_driver_init(DevKitOutputDriver *driver, Logger logger)
{
    if (driver != NULL) {
        *driver = (DevKitOutputDriver){.base.vtable = &DEV_KIT_OUTPUT_DRIVER_VTABLE, .logger = logger};
    }
}
