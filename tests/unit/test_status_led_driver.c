#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "app/status/status_led_service.h"
#include "hal/status_led/irrigation_controller_status_led_driver.h"
#include "driver/gpio.h"

static struct TestTimer {
    esp_timer_create_args_t args;
    bool active;
    uint64_t period_us;
} timer;
static uint32_t gpio_level;
static int gpio_mode;
static void (*after_unlock)(void);
static IrrigationControllerStatusLedDriver driver;

void test_enter_critical(portMUX_TYPE *lock)
{
    assert(*lock == 0);
    *lock = 1;
}

void test_exit_critical(portMUX_TYPE *lock)
{
    assert(*lock == 1);
    *lock = 0;
    if (after_unlock != NULL) {
        void (*hook)(void) = after_unlock;
        after_unlock = NULL;
        hook();
    }
}

esp_err_t gpio_reset_pin(int pin)
{
    assert(pin == GPIO_NUM_25);
    gpio_mode = 0;
    return ESP_OK;
}

esp_err_t gpio_set_direction(int pin, int mode)
{
    assert(pin == GPIO_NUM_25 && mode == GPIO_MODE_OUTPUT);
    gpio_mode = mode;
    return ESP_OK;
}

esp_err_t gpio_set_level(int pin, uint32_t level)
{
    assert(pin == GPIO_NUM_25 && level <= 1U);
    gpio_level = level;
    return ESP_OK;
}

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *handle)
{
    timer = (struct TestTimer){.args = *args};
    *handle = &timer;
    return ESP_OK;
}

esp_err_t esp_timer_stop(esp_timer_handle_t handle)
{
    assert(handle == &timer);
    timer.active = false;
    return ESP_OK;
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t handle, uint64_t period_us)
{
    assert(handle == &timer && !timer.active);
    timer.active = true;
    timer.period_us = period_us;
    return ESP_OK;
}

static void tick(void)
{
    timer.args.callback(timer.args.arg);
}

static void enter_ready(void)
{
    assert(status_led_driver_set_state(&driver.base, STATUS_LED_STATE_READY) == IRRIGATION_RESULT_OK);
    assert(gpio_level == 1U && !timer.active);
}

static void assert_blink(uint64_t period_us)
{
    assert(timer.active && timer.period_us == period_us && gpio_level == 1U);
    tick();
    assert(gpio_level == 0U);
    tick();
    assert(gpio_level == 1U);
}

int main(void)
{
    assert(irrigation_controller_status_led_driver_init(&driver) == IRRIGATION_RESULT_OK);
    assert(gpio_mode == GPIO_MODE_OUTPUT);
    assert_blink(500000U);
    enter_ready();
    tick(); /* A pending callback after READY must preserve HIGH. */
    assert(gpio_level == 1U && !timer.active);
    enter_ready(); /* Repeated READY refresh. */

    const StatusLedState blinking[] = {
        STATUS_LED_STATE_BOOTING, STATUS_LED_STATE_NETWORK_DISCONNECTED, STATUS_LED_STATE_FAULT,
    };
    for (size_t i = 0; i < sizeof(blinking) / sizeof(blinking[0]); ++i) {
        assert(status_led_driver_set_state(&driver.base, blinking[i]) == IRRIGATION_RESULT_OK);
        assert_blink(blinking[i] == STATUS_LED_STATE_FAULT ? 125000U : 500000U);
        /* Simulate READY on another task as an OFF callback releases its lock.
         * A stale GPIO write after unlocking would incorrectly overwrite HIGH. */
        after_unlock = enter_ready;
        tick();
        assert(driver.state == STATUS_LED_STATE_READY && gpio_level == 1U && !timer.active);
    }

    StatusLedService service;
    status_led_service_init(&service, &driver.base);
    assert_blink(500000U);
    status_led_service_set_network_connected(&service, true);
    status_led_service_refresh(&service);
    assert(gpio_level == 1U && !timer.active);
    status_led_service_set_fault(&service, true);
    status_led_service_refresh(&service);
    assert_blink(125000U);
    status_led_service_set_network_connected(&service, false);
    status_led_service_refresh(&service);
    assert(driver.state == STATUS_LED_STATE_FAULT);
    status_led_service_set_fault(&service, false);
    status_led_service_refresh(&service);
    assert(driver.state == STATUS_LED_STATE_NETWORK_DISCONNECTED);
    assert_blink(500000U);
    status_led_service_set_network_connected(&service, true);
    status_led_service_refresh(&service);
    assert(gpio_level == 1U && !timer.active);
    puts("Production status LED tests passed");
    return 0;
}
