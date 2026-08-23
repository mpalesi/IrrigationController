#include "esp_log.h"

#include "app/events/event_bus.h"
#include "app/result.h"
#include "app/state/state_store.h"
#include "hal/boards/board_composition.h"
#include "hal/outputs/output_driver.h"

static const char *TAG = "irrigation_controller";

static void log_event(const Event *event, void *context)
{
    (void)context;
    ESP_LOGI(TAG, "event type=%d source=%s", event->type, event->source);
}

void app_main(void)
{
    StateStore state_store;
    state_store_init(&state_store);

    OutputDriver *output_driver = board_output_driver();
    if (output_driver_initialize_safe_off(output_driver) != IRRIGATION_RESULT_OK) {
        ESP_LOGE(TAG, "safe output initialization failed; remaining in BOOT");
        return;
    }

    if (state_store_transition_system(&state_store, SYSTEM_STATE_READY) != IRRIGATION_RESULT_OK) {
        ESP_LOGE(TAG, "unable to enter READY state");
        return;
    }

    EventBus event_bus;
    event_bus_init(&event_bus, log_event, NULL);
    const Event ready_event = {
        .id = "system-ready",
        .timestamp_ms = 0U,
        .source = "main",
        .type = EVENT_TYPE_SYSTEM_READY,
        .version = 1U,
    };
    (void)event_bus_publish(&event_bus, &ready_event);
    ESP_LOGI(TAG, "system ready");
}
