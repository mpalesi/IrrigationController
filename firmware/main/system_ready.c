#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app/clock.h"
#include "app/events/event_bus.h"
#include "app/result.h"
#include "app/state/state_store.h"
#include "domain/programs/program_service.h"
#include "domain/scheduler/scheduler_service.h"
#include "hal/boards/board_composition.h"
#include "hal/outputs/output_driver.h"

static const char *TAG = "irrigation_controller";
static StateStore state_store;
static ProgramService program_service;
static SchedulerService scheduler_service;

static bool scheduler_time_from_system(SchedulerTime *scheduler_time)
{
    time_t now;
    time(&now);
    if (now < 1700000000) {
        return false;
    }
    struct tm local_time;
    localtime_r(&now, &local_time);
    char iso_week[3] = {0};
    if (strftime(iso_week, sizeof(iso_week), "%V", &local_time) == 0U) {
        return false;
    }
    *scheduler_time = (SchedulerTime){
        .week_index = (uint32_t)((local_time.tm_year + 1900) * 54 + strtoul(iso_week, NULL, 10)),
        .weekday = (uint8_t)((local_time.tm_wday + 6) % 7),
        .hour = (uint8_t)local_time.tm_hour,
        .minute = (uint8_t)local_time.tm_min,
    };
    return true;
}

static void scheduler_task(void *context)
{
    SchedulerService *service = context;
    for (;;) {
        SchedulerTime now;
        if (scheduler_time_from_system(&now)) {
            (void)scheduler_service_process(service, now);
        }
        vTaskDelay(pdMS_TO_TICKS(30000U));
    }
}

static void initialize_local_time(void)
{
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

static void log_event(const Event *event, void *context)
{
    (void)context;
    ESP_LOGI(TAG, "event type=%d source=%s", event->type, event->source);
}

void app_main(void)
{
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

    program_service_init(&program_service, &state_store, NULL, NULL, (Clock){0});
    scheduler_service_init(&scheduler_service, &state_store, &program_service);
    (void)scheduler_service_configure(&scheduler_service, NULL, 0U);
    initialize_local_time();
    (void)xTaskCreate(scheduler_task, "scheduler", 3072U, &scheduler_service, 4U, NULL);
    ESP_LOGI(TAG, "system ready");
}
