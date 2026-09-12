#include "hal/outputs/irrigation_controller_output_driver.h"

#include "common/constants.h"

#define MCP23017_ADDRESS 0x20U
#define MCP23017_SDA_GPIO GPIO_NUM_21
#define MCP23017_SCL_GPIO GPIO_NUM_22
#define MCP23017_I2C_CLOCK_HZ 100000U
#define RELAY_OUTPUT_MASK ((uint8_t)((1U << IRRIGATION_LOCAL_OUTPUT_COUNT) - 1U))

static IrrigationResult ensure_safe_off(IrrigationControllerOutputDriver *driver)
{
    driver->output_mask = 0U;
    return mcp23017_write_port_a(&driver->mcp23017, 0U) == ESP_OK
        ? IRRIGATION_RESULT_OK
        : IRRIGATION_RESULT_INTERNAL_ERROR;
}

static IrrigationResult irrigation_controller_initialize_safe_off(OutputDriver *base)
{
    IrrigationControllerOutputDriver *driver = (IrrigationControllerOutputDriver *)base;
    driver->initialized = false;
    driver->output_mask = 0U;

    const Mcp23017Configuration configuration = {
        .port = I2C_NUM_0,
        .sda_gpio = MCP23017_SDA_GPIO,
        .scl_gpio = MCP23017_SCL_GPIO,
        .address = MCP23017_ADDRESS,
        .clock_hz = MCP23017_I2C_CLOCK_HZ,
    };
    if (mcp23017_init(&driver->mcp23017, &configuration) != ESP_OK) {
        logger_log(&driver->logger, LOG_LEVEL_ERROR, "production_output", "MCP23017 initialization failed");
        return IRRIGATION_RESULT_INTERNAL_ERROR;
    }
    if (mcp23017_configure_port_a_outputs(&driver->mcp23017, RELAY_OUTPUT_MASK) != ESP_OK ||
        ensure_safe_off(driver) != IRRIGATION_RESULT_OK) {
        (void)ensure_safe_off(driver);
        logger_log(&driver->logger, LOG_LEVEL_ERROR, "production_output", "MCP23017 safe output initialization failed");
        return IRRIGATION_RESULT_INTERNAL_ERROR;
    }

    driver->initialized = true;
    logger_log(&driver->logger, LOG_LEVEL_INFO, "production_output", "all relay outputs set OFF");
    return IRRIGATION_RESULT_OK;
}

static IrrigationResult irrigation_controller_set_output(OutputDriver *base, size_t output_index, bool enabled)
{
    IrrigationControllerOutputDriver *driver = (IrrigationControllerOutputDriver *)base;
    if (!driver->initialized) {
        return IRRIGATION_RESULT_INVALID_STATE;
    }
    if (output_index >= IRRIGATION_LOCAL_OUTPUT_COUNT) {
        return IRRIGATION_RESULT_NOT_FOUND;
    }

    const uint8_t output_bit = (uint8_t)(1U << output_index);
    const uint8_t requested_mask = enabled ? (uint8_t)(driver->output_mask | output_bit)
                                           : (uint8_t)(driver->output_mask & (uint8_t)~output_bit);
    if (mcp23017_write_port_a(&driver->mcp23017, requested_mask) != ESP_OK) {
        (void)ensure_safe_off(driver);
        logger_log(&driver->logger, LOG_LEVEL_ERROR, "production_output", "MCP23017 output write failed; requested safe OFF");
        return IRRIGATION_RESULT_INTERNAL_ERROR;
    }

    driver->output_mask = requested_mask;
    logger_log(&driver->logger, LOG_LEVEL_INFO, "production_output", enabled ? "relay output ON" : "relay output OFF");
    return IRRIGATION_RESULT_OK;
}

static IrrigationResult irrigation_controller_get_output(const OutputDriver *base, size_t output_index, bool *enabled)
{
    const IrrigationControllerOutputDriver *driver = (const IrrigationControllerOutputDriver *)base;
    if (!driver->initialized) {
        return IRRIGATION_RESULT_INVALID_STATE;
    }
    if (output_index >= IRRIGATION_LOCAL_OUTPUT_COUNT || enabled == NULL) {
        return IRRIGATION_RESULT_NOT_FOUND;
    }
    *enabled = (driver->output_mask & (uint8_t)(1U << output_index)) != 0U;
    return IRRIGATION_RESULT_OK;
}

static const OutputDriverVTable IRRIGATION_CONTROLLER_OUTPUT_DRIVER_VTABLE = {
    .initialize_safe_off = irrigation_controller_initialize_safe_off,
    .set_output = irrigation_controller_set_output,
    .get_output = irrigation_controller_get_output,
};

void irrigation_controller_output_driver_init(IrrigationControllerOutputDriver *driver, Logger logger)
{
    if (driver != NULL) {
        *driver = (IrrigationControllerOutputDriver){
            .base.vtable = &IRRIGATION_CONTROLLER_OUTPUT_DRIVER_VTABLE,
            .logger = logger,
        };
    }
}
