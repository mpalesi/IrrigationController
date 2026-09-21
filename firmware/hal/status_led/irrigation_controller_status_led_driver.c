#include "hal/status_led/irrigation_controller_status_led_driver.h"

#include "driver/gpio.h"
#include "esp_log.h"

#define IRRIGATION_CONTROLLER_STATUS_LED_GPIO GPIO_NUM_25
#define STATUS_LED_SLOW_HALF_PERIOD_US 500000U
#define STATUS_LED_FAST_HALF_PERIOD_US 125000U

static const char *TAG = "production_status_led";

static void set_led_level(bool on)
{
    (void)gpio_set_level(IRRIGATION_CONTROLLER_STATUS_LED_GPIO, on ? 1 : 0);
}

static uint64_t blink_half_period_us(StatusLedState state)
{
    return state == STATUS_LED_STATE_FAULT ? STATUS_LED_FAST_HALF_PERIOD_US
                                           : STATUS_LED_SLOW_HALF_PERIOD_US;
}

static void blink_timer_callback(void *context)
{
    IrrigationControllerStatusLedDriver *driver = context;
    portENTER_CRITICAL(&driver->lock);
    driver->led_on = driver->state == STATUS_LED_STATE_READY || !driver->led_on;
    /* Keep the GPIO write serialized with state transitions, including READY. */
    set_led_level(driver->led_on);
    portEXIT_CRITICAL(&driver->lock);
}

static IrrigationResult set_state(StatusLedDriver *base, StatusLedState state)
{
    IrrigationControllerStatusLedDriver *driver = (IrrigationControllerStatusLedDriver *)base;
    if (driver == NULL || !driver->initialized) return IRRIGATION_RESULT_INVALID_STATE;

    portENTER_CRITICAL(&driver->lock);
    const bool unchanged = driver->state == state;
    portEXIT_CRITICAL(&driver->lock);
    if (unchanged) return IRRIGATION_RESULT_OK;

    (void)esp_timer_stop(driver->timer);
    portENTER_CRITICAL(&driver->lock);
    driver->state = state;
    /* READY stays ON; blinking states start with their ON half-period. */
    driver->led_on = true;
    set_led_level(driver->led_on);
    portEXIT_CRITICAL(&driver->lock);

    if (state != STATUS_LED_STATE_READY &&
        esp_timer_start_periodic(driver->timer, blink_half_period_us(state)) != ESP_OK) {
        portENTER_CRITICAL(&driver->lock);
        driver->led_on = driver->state == STATUS_LED_STATE_READY;
        set_led_level(driver->led_on);
        portEXIT_CRITICAL(&driver->lock);
        return IRRIGATION_RESULT_INTERNAL_ERROR;
    }
    return IRRIGATION_RESULT_OK;
}

static const StatusLedDriverVTable IRRIGATION_CONTROLLER_STATUS_LED_DRIVER_VTABLE = {
    .set_state = set_state,
};

IrrigationResult irrigation_controller_status_led_driver_init(IrrigationControllerStatusLedDriver *driver)
{
    if (driver == NULL) return IRRIGATION_RESULT_REJECTED;
    *driver = (IrrigationControllerStatusLedDriver){
        .base.vtable = &IRRIGATION_CONTROLLER_STATUS_LED_DRIVER_VTABLE,
        .lock = portMUX_INITIALIZER_UNLOCKED,
        .state = STATUS_LED_STATE_READY,
    };
    if (gpio_reset_pin(IRRIGATION_CONTROLLER_STATUS_LED_GPIO) != ESP_OK ||
        gpio_set_level(IRRIGATION_CONTROLLER_STATUS_LED_GPIO, 0) != ESP_OK ||
        gpio_set_direction(IRRIGATION_CONTROLLER_STATUS_LED_GPIO, GPIO_MODE_OUTPUT) != ESP_OK) {
        ESP_LOGE(TAG, "GPIO25 safe initialization failed");
        return IRRIGATION_RESULT_INTERNAL_ERROR;
    }
    const esp_timer_create_args_t timer_arguments = {
        .callback = blink_timer_callback,
        .arg = driver,
        .name = "status_led",
    };
    if (esp_timer_create(&timer_arguments, &driver->timer) != ESP_OK) {
        ESP_LOGE(TAG, "blink timer creation failed");
        return IRRIGATION_RESULT_INTERNAL_ERROR;
    }
    driver->initialized = true;
    return set_state(&driver->base, STATUS_LED_STATE_BOOTING);
}
