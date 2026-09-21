#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "app/clock.h"
#include "app/configuration/configuration_manager.h"
#include "app/events/event_bus.h"
#include "app/result.h"
#include "app/state/state_store.h"
#include "app/status/status_led_service.h"
#include "domain/master_valve/master_valve_service.h"
#include "domain/history/program_execution_history.h"
#include "domain/outputs/output.h"
#include "domain/programs/program_service.h"
#include "domain/scheduler/scheduler_service.h"
#include "domain/zones/zone_service.h"
#include "hal/boards/board_composition.h"
#include "hal/outputs/output_driver.h"
#include "interfaces/http/dev_kit_web.h"
#include "infrastructure/persistence/configuration_repository.h"

static const char *TAG = "irrigation_controller";
static StateStore state_store;
static EventBus event_bus;
static ZoneService zone_service;
static MasterValveService master_valve_service;
static ProgramService program_service;
static ProgramExecutionHistory program_execution_history;
static SchedulerService scheduler_service;
static SemaphoreHandle_t scheduler_configuration_mutex;
static DevKitWebContext web_context;
static ConfigurationManager configuration_manager;
static ConfigurationRepository configuration_repository;
static StatusLedService status_led_service;

static const Output DEV_OUTPUTS[] = {
    {.id = "dev-output-1", .driver_output_index = 0U},
    {.id = "dev-output-2", .driver_output_index = 1U},
    {.id = "dev-output-3", .driver_output_index = 2U},
    {.id = "dev-output-4", .driver_output_index = 3U},
};
static const Zone DEV_ZONES[] = {
    {.id = "zone-1", .output = &DEV_OUTPUTS[0], .enabled = true, .default_duration_ms = 30000U},
    {.id = "zone-2", .output = &DEV_OUTPUTS[1], .enabled = true, .default_duration_ms = 30000U},
    {.id = "zone-3", .output = &DEV_OUTPUTS[2], .enabled = true, .default_duration_ms = 30000U},
    {.id = "zone-4", .output = &DEV_OUTPUTS[3], .enabled = true, .default_duration_ms = 30000U},
};
static void initialize_local_time(void);

static void indicate_status_fault(void)
{
    status_led_service_set_fault(&status_led_service, true);
    status_led_service_refresh(&status_led_service);
}

static void scheduler_lock(void *context)
{
    (void)xSemaphoreTake((SemaphoreHandle_t)context, portMAX_DELAY);
}

static void scheduler_unlock(void *context)
{
    (void)xSemaphoreGive((SemaphoreHandle_t)context);
}

static uint64_t esp_clock_now_ms(void *context)
{
    (void)context;
    return (uint64_t)esp_timer_get_time() / 1000U;
}

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
    uint16_t tick_count = 0U;
    for (;;) {
        (void)master_valve_service_process_time(&master_valve_service);
        if (program_service_is_active(&program_service)) {
            (void)program_service_process(&program_service);
        }
        const RuntimeState status_state = state_store_snapshot(&state_store);
        status_led_service_set_fault(&status_led_service,
                                     status_state.system_state == SYSTEM_STATE_ERROR ||
                                     status_state.master_valve.state == MASTER_VALVE_STATE_FAULT);
        status_led_service_refresh(&status_led_service);
        if (++tick_count >= 150U) {
            SchedulerTime now;
            if (scheduler_time_from_system(&now)) {
                (void)scheduler_service_process(service, now);
            }
            tick_count = 0U;
        }
        vTaskDelay(pdMS_TO_TICKS(200U));
    }
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)argument;
    (void)event_data;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        status_led_service_set_network_connected(&status_led_service, false);
        (void)esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting");
        status_led_service_set_network_connected(&status_led_service, false);
        (void)esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = event_data;
        ESP_LOGI(TAG, "Wi-Fi connected, IP=" IPSTR, IP2STR(&event->ip_info.ip));
        status_led_service_set_network_connected(&status_led_service, true);
        dev_kit_web_start(&web_context);
    }
}

static void initialize_wifi(void)
{
    if (CONFIG_IRRIGATION_WIFI_SSID[0] == '\0') {
        ESP_LOGW(TAG, "Wi-Fi SSID is not configured; web UI is disabled");
        status_led_service_set_network_connected(&status_led_service, false);
        return;
    }
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    initialize_local_time();
    (void)esp_netif_create_default_wifi_sta();
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_config));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));
    wifi_config_t wifi_config = {0};
    (void)snprintf((char *)wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid), "%s", CONFIG_IRRIGATION_WIFI_SSID);
    (void)snprintf((char *)wifi_config.sta.password, sizeof(wifi_config.sta.password), "%s", CONFIG_IRRIGATION_WIFI_PASSWORD);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
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

static bool initialize_configuration(void)
{
    if (configuration_manager_init_dev_kit_defaults(&configuration_manager, DEV_ZONES,
                                                     sizeof(DEV_ZONES) / sizeof(DEV_ZONES[0])) !=
        IRRIGATION_RESULT_OK) {
        ESP_LOGE(TAG, "unable to initialize DEV_KIT configuration defaults");
        return false;
    }
    if (nvs_configuration_repository_init(&configuration_repository) != CONFIGURATION_REPOSITORY_OK) {
        ESP_LOGE(TAG, "unable to initialize irrigation_cfg; configuration mutations are disabled");
        configuration_manager_set_repository(&configuration_manager, NULL, true);
        return true;
    }
    const ConfigurationManagerBootResult boot_result =
        configuration_manager_load_or_persist_defaults(&configuration_manager, &configuration_repository);
    switch (boot_result) {
        case CONFIGURATION_MANAGER_BOOT_LOADED:
            ESP_LOGI(TAG, "loaded persistent irrigation configuration");
            break;
        case CONFIGURATION_MANAGER_BOOT_DEFAULTS_PERSISTED:
            ESP_LOGI(TAG, "irrigation_cfg is empty; persisted DEV_KIT defaults");
            break;
        case CONFIGURATION_MANAGER_BOOT_DEFAULTS_CORRUPT:
            ESP_LOGW(TAG, "irrigation_cfg is corrupt; using compiled DEV_KIT defaults without overwriting it");
            break;
        case CONFIGURATION_MANAGER_BOOT_DEFAULTS_INCOMPATIBLE:
            ESP_LOGW(TAG, "irrigation_cfg schema is incompatible; using compiled DEV_KIT defaults without overwriting it");
            break;
        case CONFIGURATION_MANAGER_BOOT_DEFAULTS_STORAGE_ERROR:
            ESP_LOGE(TAG, "unable to load/save irrigation_cfg; using defaults and disabling configuration mutations");
            configuration_manager_set_repository(&configuration_manager, NULL, true);
            return true;
    }
    configuration_manager_set_repository(&configuration_manager, &configuration_repository, true);
    return true;
}

void app_main(void)
{
    state_store_init(&state_store);
    status_led_service_init(&status_led_service, board_status_led_driver());

    OutputDriver *output_driver = board_output_driver();
    if (output_driver_initialize_safe_off(output_driver) != IRRIGATION_RESULT_OK) {
        ESP_LOGE(TAG, "safe output initialization failed; remaining in BOOT");
        indicate_status_fault();
        return;
    }

    if (state_store_transition_system(&state_store, SYSTEM_STATE_READY) != IRRIGATION_RESULT_OK) {
        ESP_LOGE(TAG, "unable to enter READY state");
        indicate_status_fault();
        return;
    }

    event_bus_init(&event_bus, log_event, NULL);
    const Event ready_event = {
        .id = "system-ready",
        .timestamp_ms = 0U,
        .source = "main",
        .type = EVENT_TYPE_SYSTEM_READY,
        .version = 1U,
    };
    (void)event_bus_publish(&event_bus, &ready_event);

    Clock clock = {.now_ms = esp_clock_now_ms, .context = NULL};
    if (!initialize_configuration()) {
        indicate_status_fault();
        return;
    }
    if (state_store_configure_zones(&state_store, DEV_ZONES,
                                    sizeof(DEV_ZONES) / sizeof(DEV_ZONES[0])) != IRRIGATION_RESULT_OK) {
        ESP_LOGE(TAG, "unable to configure DEV_KIT zones");
        indicate_status_fault();
        return;
    }
    master_valve_service_init(&master_valve_service, &state_store, board_master_valve_driver(), clock,
                              (MasterValveConfiguration){.pre_open_delay_ms = 500U});
    zone_service_init(&zone_service, &state_store, output_driver, &event_bus, clock,
                      master_valve_service_zone_start_precondition(&master_valve_service));
    program_service_init(&program_service, &state_store, &master_valve_service, &zone_service, clock);
    program_service_set_event_bus(&program_service, &event_bus);
    program_execution_history_init(&program_execution_history, (ProgramExecutionHistoryRepository){0});
    if (program_execution_history_subscribe(&program_execution_history, &event_bus) != IRRIGATION_RESULT_OK) {
        ESP_LOGE(TAG, "unable to subscribe program execution history");
        indicate_status_fault();
        return;
    }
    scheduler_service_init(&scheduler_service, &state_store, &program_service);
    scheduler_configuration_mutex = xSemaphoreCreateMutex();
    if (scheduler_configuration_mutex == NULL) {
        ESP_LOGE(TAG, "unable to create scheduler configuration mutex");
        indicate_status_fault();
        return;
    }
    scheduler_service_set_synchronization(&scheduler_service, (SchedulerServiceSynchronization){
        .context = scheduler_configuration_mutex, .lock = scheduler_lock, .unlock = scheduler_unlock,
    });
    if (configuration_manager_configure_scheduler(&configuration_manager, &scheduler_service) !=
        IRRIGATION_RESULT_OK) {
        ESP_LOGE(TAG, "unable to configure scheduler");
        indicate_status_fault();
        return;
    }
    configuration_manager_bind_scheduler(&configuration_manager, &scheduler_service);
    web_context = (DevKitWebContext){
        .state_store = &state_store, .zone_service = &zone_service,
        .master_valve_service = &master_valve_service, .program_service = &program_service,
        .scheduler_service = &scheduler_service, .zones = DEV_ZONES,
        .zone_count = sizeof(DEV_ZONES) / sizeof(DEV_ZONES[0]),
        .configuration_manager = &configuration_manager,
    };
    (void)xTaskCreate(scheduler_task, "scheduler", 6144U, &scheduler_service, 4U, NULL);
    initialize_wifi();
    ESP_LOGI(TAG, "system ready");
}
