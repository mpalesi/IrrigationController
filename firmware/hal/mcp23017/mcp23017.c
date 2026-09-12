#include "hal/mcp23017/mcp23017.h"

#include "freertos/FreeRTOS.h"

#define MCP23017_IODIRA_REGISTER 0x00U
#define MCP23017_OLATA_REGISTER 0x14U
#define MCP23017_I2C_TIMEOUT_MS 1000U

static esp_err_t write_register(const Mcp23017 *device, uint8_t register_address, uint8_t value)
{
    const uint8_t write_data[] = {register_address, value};
    return i2c_master_write_to_device(device->port, device->address, write_data, sizeof(write_data),
                                      pdMS_TO_TICKS(MCP23017_I2C_TIMEOUT_MS));
}

esp_err_t mcp23017_init(Mcp23017 *device, const Mcp23017Configuration *configuration)
{
    if (device == NULL || configuration == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const i2c_config_t i2c_configuration = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = configuration->sda_gpio,
        .scl_io_num = configuration->scl_gpio,
        .sda_pullup_en = GPIO_PULLUP_DISABLE,
        .scl_pullup_en = GPIO_PULLUP_DISABLE,
        .master.clk_speed = configuration->clock_hz,
        .clk_flags = 0,
    };

    esp_err_t result = i2c_param_config(configuration->port, &i2c_configuration);
    if (result != ESP_OK) {
        return result;
    }
    result = i2c_driver_install(configuration->port, i2c_configuration.mode, 0, 0, 0);
    if (result != ESP_OK) {
        return result;
    }

    *device = (Mcp23017){.port = configuration->port, .address = configuration->address};
    /* Keep the output latch low before any GPA pin is configured as an output. */
    return mcp23017_write_port_a(device, 0U);
}

esp_err_t mcp23017_write_port_a(Mcp23017 *device, uint8_t values)
{
    return device == NULL ? ESP_ERR_INVALID_ARG : write_register(device, MCP23017_OLATA_REGISTER, values);
}

esp_err_t mcp23017_configure_port_a_outputs(Mcp23017 *device, uint8_t output_mask)
{
    return device == NULL ? ESP_ERR_INVALID_ARG
                          : write_register(device, MCP23017_IODIRA_REGISTER, (uint8_t)~output_mask);
}
