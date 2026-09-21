#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app/clock.h"
#include "app/commands/command_bus.h"
#include "app/commands/zone_command_handler.h"
#include "app/configuration/configuration_manager.h"
#include "app/events/event_bus.h"
#include "app/state/state_store.h"
#include "app/status/status_led_service.h"
#include "domain/master_valve/master_valve_service.h"
#include "domain/history/program_execution_history.h"
#include "domain/outputs/output.h"
#include "domain/programs/program_service.h"
#include "domain/scheduler/scheduler_service.h"
#include "domain/zones/zone.h"
#include "domain/zones/zone_service.h"
#include "hal/master_valve/shelly_master_valve_driver.h"
#include "hal/outputs/virtual_output_driver.h"
#include "interfaces/http/dev_kit_zone_configuration.h"
#include "infrastructure/persistence/configuration_repository.h"

typedef struct {
    uint64_t now_ms;
} FakeClock;

typedef struct {
    Event events[64];
    size_t count;
} EventRecorder;

typedef struct {
    OutputDriver base;
    bool state[IRRIGATION_LOCAL_OUTPUT_COUNT];
    bool fail_after_on;
    unsigned int fail_after_off_count;
} FailingOutputDriver;

typedef struct {
    OutputDriver base;
    bool state[IRRIGATION_LOCAL_OUTPUT_COUNT];
    ZoneService *service;
    bool nested_open_attempted;
    IrrigationResult nested_open_result;
} ReentrantOutputDriver;

typedef struct {
    MasterValveDriver base;
    bool is_open;
    bool fail_open;
    bool fail_close;
    unsigned int open_calls;
    unsigned int close_calls;
} FakeMasterValveDriver;

typedef struct {
    StatusLedDriver base;
    StatusLedState state;
    size_t set_calls;
} FakeStatusLedDriver;

typedef struct {
    IrrigationResult result;
    int http_status;
    const char *body;
} FakeShellyHttpResponse;

typedef struct {
    ShellyHttpTransport base;
    FakeShellyHttpResponse responses[2];
    char paths[2][80];
    size_t response_count;
    size_t request_count;
} FakeShellyHttpTransport;

static const Output OUTPUT_ONE = {.id = "output-1", .driver_output_index = 0U};
static const Output OUTPUT_TWO = {.id = "output-2", .driver_output_index = 1U};
static const Zone ZONES[] = {
    {.id = "zone-1", .output = &OUTPUT_ONE, .enabled = true, .default_duration_ms = 100U},
    {.id = "zone-2", .output = &OUTPUT_TWO, .enabled = true, .default_duration_ms = 200U},
};

static const Zone CONFIGURATION_ZONES[] = {
    {.id = "zone-1", .output = &OUTPUT_ONE, .enabled = true, .default_duration_ms = 100U},
    {.id = "zone-2", .output = &OUTPUT_TWO, .enabled = true, .default_duration_ms = 200U},
    {.id = "zone-3", .output = &OUTPUT_ONE, .enabled = true, .default_duration_ms = 100U},
    {.id = "zone-4", .output = &OUTPUT_TWO, .enabled = true, .default_duration_ms = 200U},
};

typedef struct {
    uint8_t blobs[2][CONFIGURATION_REPOSITORY_MAX_BLOB_SIZE];
    size_t lengths[2];
    uint8_t pending_blobs[2][CONFIGURATION_REPOSITORY_MAX_BLOB_SIZE];
    size_t pending_lengths[2];
    bool pending[2];
    bool fail_write;
    bool fail_commit;
    bool corrupt_after_commit;
    int last_written_slot;
} MockConfigurationStorage;

typedef struct {
    unsigned int lock_calls;
    unsigned int unlock_calls;
    bool locked;
} SchedulerLockRecorder;

static void record_scheduler_lock(void *context)
{
    SchedulerLockRecorder *recorder = context;
    assert(!recorder->locked);
    recorder->locked = true;
    recorder->lock_calls++;
}

static void record_scheduler_unlock(void *context)
{
    SchedulerLockRecorder *recorder = context;
    assert(recorder->locked);
    recorder->locked = false;
    recorder->unlock_calls++;
}

static int mock_slot_for_key(const char *key)
{
    return strcmp(key, CONFIGURATION_REPOSITORY_SLOT_A) == 0 ? 0 :
           (strcmp(key, CONFIGURATION_REPOSITORY_SLOT_B) == 0 ? 1 : -1);
}

static ConfigurationRepositoryResult mock_configuration_read(void *context, const char *key,
                                                             uint8_t *buffer, size_t capacity, size_t *length)
{
    MockConfigurationStorage *storage = context;
    const int slot = mock_slot_for_key(key);
    if (slot < 0 || storage->lengths[slot] == 0U) return CONFIGURATION_REPOSITORY_NOT_FOUND;
    if (storage->lengths[slot] > capacity) return CONFIGURATION_REPOSITORY_STORAGE_ERROR;
    memcpy(buffer, storage->blobs[slot], storage->lengths[slot]);
    *length = storage->lengths[slot];
    return CONFIGURATION_REPOSITORY_OK;
}

static ConfigurationRepositoryResult mock_configuration_write(void *context, const char *key,
                                                              const uint8_t *buffer, size_t length)
{
    MockConfigurationStorage *storage = context;
    const int slot = mock_slot_for_key(key);
    if (storage->fail_write || slot < 0 || length > sizeof(storage->pending_blobs[slot])) {
        return CONFIGURATION_REPOSITORY_STORAGE_ERROR;
    }
    memcpy(storage->pending_blobs[slot], buffer, length);
    storage->pending_lengths[slot] = length;
    storage->pending[slot] = true;
    storage->last_written_slot = slot;
    return CONFIGURATION_REPOSITORY_OK;
}

static ConfigurationRepositoryResult mock_configuration_commit(void *context)
{
    MockConfigurationStorage *storage = context;
    if (storage->fail_commit) {
        memset(storage->pending, 0, sizeof(storage->pending));
        return CONFIGURATION_REPOSITORY_STORAGE_ERROR;
    }
    for (size_t slot = 0U; slot < 2U; ++slot) {
        if (!storage->pending[slot]) continue;
        memcpy(storage->blobs[slot], storage->pending_blobs[slot], storage->pending_lengths[slot]);
        storage->lengths[slot] = storage->pending_lengths[slot];
        storage->pending[slot] = false;
        if (storage->corrupt_after_commit) {
            storage->blobs[slot][CONFIGURATION_REPOSITORY_HEADER_SIZE] ^= 1U;
        }
    }
    return CONFIGURATION_REPOSITORY_OK;
}

static void initialize_mock_repository(ConfigurationRepository *repository,
                                       MockConfigurationStorage *storage)
{
    *storage = (MockConfigurationStorage){0};
    storage->last_written_slot = -1;
    configuration_repository_init(repository, (ConfigurationRepositoryStorage){
        .context = storage, .read = mock_configuration_read, .write = mock_configuration_write,
        .commit = mock_configuration_commit,
    });
}

static void test_irrigation_configuration_validation(void)
{
    ConfigurationManager manager;
    assert(configuration_manager_init_dev_kit_defaults(&manager, CONFIGURATION_ZONES,
                                                       sizeof(CONFIGURATION_ZONES) / sizeof(CONFIGURATION_ZONES[0])) ==
           IRRIGATION_RESULT_OK);
    const IrrigationConfiguration *defaults = configuration_manager_get(&manager);
    assert(defaults != NULL);
    assert(defaults->zone_count == 4U);
    assert(defaults->program_count == 1U);
    assert(defaults->schedule_count == 1U);
    assert(strcmp(defaults->programs[0].program_id, "dev-program") == 0);
    assert(strcmp(defaults->programs[0].display_name, "dev-program") == 0);
    assert(irrigation_configuration_validate(defaults, CONFIGURATION_ZONES,
                                             sizeof(CONFIGURATION_ZONES) / sizeof(CONFIGURATION_ZONES[0])) ==
           IRRIGATION_RESULT_OK);

    IrrigationConfiguration candidate = *defaults;
    candidate.program_count = 2U;
    candidate.programs[1] = candidate.programs[0];
    assert(configuration_manager_validate_candidate(&manager, &candidate) == IRRIGATION_RESULT_REJECTED);

    candidate = *defaults;
    candidate.schedule_count = 2U;
    candidate.schedules[1] = candidate.schedules[0];
    assert(configuration_manager_validate_candidate(&manager, &candidate) == IRRIGATION_RESULT_REJECTED);

    candidate = *defaults;
    (void)snprintf(candidate.programs[0].steps[0].zone_id,
                   sizeof(candidate.programs[0].steps[0].zone_id), "%s", "missing-zone");
    assert(configuration_manager_validate_candidate(&manager, &candidate) == IRRIGATION_RESULT_REJECTED);

    candidate = *defaults;
    (void)snprintf(candidate.schedules[0].program_id, sizeof(candidate.schedules[0].program_id), "%s",
                   "missing-program");
    assert(configuration_manager_validate_candidate(&manager, &candidate) == IRRIGATION_RESULT_REJECTED);

    candidate = *defaults;
    candidate.schedules[0].weekday_mask = 0U;
    assert(configuration_manager_validate_candidate(&manager, &candidate) == IRRIGATION_RESULT_REJECTED);

    candidate = *defaults;
    candidate.schedules[0].weekday_mask = 1U << 7U;
    assert(configuration_manager_validate_candidate(&manager, &candidate) == IRRIGATION_RESULT_REJECTED);

    candidate = *defaults;
    candidate.schedules[0].hour = 24U;
    assert(configuration_manager_validate_candidate(&manager, &candidate) == IRRIGATION_RESULT_REJECTED);

    candidate = *defaults;
    candidate.schedules[0].minute = 60U;
    assert(configuration_manager_validate_candidate(&manager, &candidate) == IRRIGATION_RESULT_REJECTED);

    candidate = *defaults;
    candidate.program_count = IRRIGATION_MAX_PROGRAMS + 1U;
    assert(configuration_manager_validate_candidate(&manager, &candidate) == IRRIGATION_RESULT_REJECTED);

    candidate = *defaults;
    candidate.schedule_count = IRRIGATION_MAX_SCHEDULES + 1U;
    assert(configuration_manager_validate_candidate(&manager, &candidate) == IRRIGATION_RESULT_REJECTED);
}

static size_t configuration_zone_count(void)
{
    return sizeof(CONFIGURATION_ZONES) / sizeof(CONFIGURATION_ZONES[0]);
}

static void assert_configuration_equal(const IrrigationConfiguration *actual,
                                       const IrrigationConfiguration *expected)
{
    assert(actual->schema_version == expected->schema_version);
    assert(actual->zone_count == expected->zone_count);
    assert(actual->program_count == expected->program_count);
    assert(actual->schedule_count == expected->schedule_count);
    assert(actual->next_program_id == expected->next_program_id);
    assert(actual->next_schedule_id == expected->next_schedule_id);
    for (size_t index = 0U; index < expected->zone_count; ++index) {
        assert(strcmp(actual->zones[index].zone_id, expected->zones[index].zone_id) == 0);
        assert(strcmp(actual->zones[index].display_name, expected->zones[index].display_name) == 0);
    }
    for (size_t index = 0U; index < expected->program_count; ++index) {
        const IrrigationProgramConfiguration *actual_program = &actual->programs[index];
        const IrrigationProgramConfiguration *expected_program = &expected->programs[index];
        assert(strcmp(actual_program->program_id, expected_program->program_id) == 0);
        assert(strcmp(actual_program->display_name, expected_program->display_name) == 0);
        assert(actual_program->step_count == expected_program->step_count);
        for (size_t step = 0U; step < expected_program->step_count; ++step) {
            assert(strcmp(actual_program->steps[step].zone_id,
                          expected_program->steps[step].zone_id) == 0);
            assert(actual_program->steps[step].duration_ms ==
                   expected_program->steps[step].duration_ms);
        }
    }
    for (size_t index = 0U; index < expected->schedule_count; ++index) {
        const IrrigationScheduleConfiguration *actual_schedule = &actual->schedules[index];
        const IrrigationScheduleConfiguration *expected_schedule = &expected->schedules[index];
        assert(strcmp(actual_schedule->schedule_id, expected_schedule->schedule_id) == 0);
        assert(actual_schedule->enabled == expected_schedule->enabled);
        assert(actual_schedule->weekday_mask == expected_schedule->weekday_mask);
        assert(actual_schedule->hour == expected_schedule->hour);
        assert(actual_schedule->minute == expected_schedule->minute);
        assert(strcmp(actual_schedule->program_id, expected_schedule->program_id) == 0);
    }
}

static IrrigationConfiguration make_repository_configuration(void)
{
    ConfigurationManager manager;
    assert(configuration_manager_init_dev_kit_defaults(&manager, CONFIGURATION_ZONES,
                                                       configuration_zone_count()) == IRRIGATION_RESULT_OK);
    IrrigationConfiguration configuration = *configuration_manager_get(&manager);
    (void)snprintf(configuration.zones[0].display_name, sizeof(configuration.zones[0].display_name), "%s",
                   "Front lawn");
    (void)snprintf(configuration.zones[1].display_name, sizeof(configuration.zones[1].display_name), "%s",
                   "Back lawn");
    (void)snprintf(configuration.zones[2].display_name, sizeof(configuration.zones[2].display_name), "%s",
                   "Vegetable beds");
    (void)snprintf(configuration.zones[3].display_name, sizeof(configuration.zones[3].display_name), "%s",
                   "Orchard");
    configuration.program_count = 2U;
    configuration.programs[0] = (IrrigationProgramConfiguration){
        .program_id = "program-morning", .display_name = "Morning garden",
        .steps = {{.zone_id = "zone-1", .duration_ms = 11000U},
                  {.zone_id = "zone-2", .duration_ms = 22000U}}, .step_count = 2U,
    };
    configuration.programs[1] = (IrrigationProgramConfiguration){
        .program_id = "program-evening", .display_name = "Evening garden",
        .steps = {{.zone_id = "zone-4", .duration_ms = 44000U},
                  {.zone_id = "zone-3", .duration_ms = 33000U},
                  {.zone_id = "zone-1", .duration_ms = 55000U}}, .step_count = 3U,
    };
    configuration.schedule_count = 2U;
    configuration.schedules[0] = (IrrigationScheduleConfiguration){
        .schedule_id = "schedule-morning", .enabled = true,
        .weekday_mask = SCHEDULER_WEEKDAY_MASK(0U) | SCHEDULER_WEEKDAY_MASK(2U) |
                        SCHEDULER_WEEKDAY_MASK(4U),
        .hour = 6U, .minute = 5U, .program_id = "program-morning",
    };
    configuration.schedules[1] = (IrrigationScheduleConfiguration){
        .schedule_id = "schedule-evening", .enabled = false,
        .weekday_mask = SCHEDULER_WEEKDAY_MASK(1U) | SCHEDULER_WEEKDAY_MASK(3U) |
                        SCHEDULER_WEEKDAY_MASK(6U),
        .hour = 19U, .minute = 30U, .program_id = "program-evening",
    };
    configuration.next_program_id = 37U;
    configuration.next_schedule_id = 41U;
    assert(irrigation_configuration_validate(&configuration, CONFIGURATION_ZONES,
                                             configuration_zone_count()) == IRRIGATION_RESULT_OK);
    return configuration;
}

static void test_configuration_repository(void)
{
    ConfigurationRepository repository;
    MockConfigurationStorage storage;
    IrrigationConfiguration configuration = make_repository_configuration();
    IrrigationConfiguration loaded;
    uint32_t generation;
    uint8_t blob[CONFIGURATION_REPOSITORY_MAX_BLOB_SIZE];
    size_t blob_length;

    assert(configuration_repository_encode(&configuration, 7U, blob, sizeof(blob), &blob_length) ==
           CONFIGURATION_REPOSITORY_OK);
    assert(configuration_repository_decode(blob, blob_length, &loaded, &generation, CONFIGURATION_ZONES,
                                            configuration_zone_count()) == CONFIGURATION_REPOSITORY_OK);
    assert(generation == 7U);
    assert_configuration_equal(&loaded, &configuration);

    initialize_mock_repository(&repository, &storage);
    assert(configuration_repository_load(&repository, &loaded, &generation, CONFIGURATION_ZONES,
                                         configuration_zone_count()) == CONFIGURATION_REPOSITORY_NOT_FOUND);
    assert(configuration_repository_save(&repository, &configuration, CONFIGURATION_ZONES,
                                         configuration_zone_count()) == CONFIGURATION_REPOSITORY_OK);
    assert(storage.last_written_slot == 0);
    assert(storage.lengths[0] != 0U && storage.lengths[1] == 0U);
    assert(configuration_repository_load(&repository, &loaded, &generation, CONFIGURATION_ZONES,
                                         configuration_zone_count()) == CONFIGURATION_REPOSITORY_OK);
    assert(generation == 1U);
    assert_configuration_equal(&loaded, &configuration);

    IrrigationConfiguration second = configuration;
    (void)snprintf(second.zones[0].display_name, sizeof(second.zones[0].display_name), "%s", "Updated lawn");
    assert(configuration_repository_save(&repository, &second, CONFIGURATION_ZONES,
                                         configuration_zone_count()) == CONFIGURATION_REPOSITORY_OK);
    assert(storage.last_written_slot == 1);
    IrrigationConfiguration third = second;
    (void)snprintf(third.programs[0].display_name, sizeof(third.programs[0].display_name), "%s", "Updated morning");
    assert(configuration_repository_save(&repository, &third, CONFIGURATION_ZONES,
                                         configuration_zone_count()) == CONFIGURATION_REPOSITORY_OK);
    assert(storage.last_written_slot == 0);
    assert(configuration_repository_load(&repository, &loaded, &generation, CONFIGURATION_ZONES,
                                         configuration_zone_count()) == CONFIGURATION_REPOSITORY_OK);
    assert(generation == 3U);
    assert_configuration_equal(&loaded, &third);

    storage.blobs[0][CONFIGURATION_REPOSITORY_HEADER_SIZE] ^= 1U;
    assert(configuration_repository_load(&repository, &loaded, &generation, CONFIGURATION_ZONES,
                                         configuration_zone_count()) == CONFIGURATION_REPOSITORY_OK);
    assert(generation == 2U);
    assert_configuration_equal(&loaded, &second);
    storage.blobs[1][CONFIGURATION_REPOSITORY_HEADER_SIZE] ^= 1U;
    assert(configuration_repository_load(&repository, &loaded, &generation, CONFIGURATION_ZONES,
                                         configuration_zone_count()) == CONFIGURATION_REPOSITORY_CORRUPT);

    assert(configuration_repository_encode(&configuration, 3U, blob, sizeof(blob), &blob_length) ==
           CONFIGURATION_REPOSITORY_OK);
    blob[CONFIGURATION_REPOSITORY_HEADER_SIZE] ^= 1U;
    assert(configuration_repository_decode(blob, blob_length, &loaded, &generation, CONFIGURATION_ZONES,
                                            configuration_zone_count()) == CONFIGURATION_REPOSITORY_CORRUPT);
    assert(configuration_repository_encode(&configuration, 3U, blob, sizeof(blob), &blob_length) ==
           CONFIGURATION_REPOSITORY_OK);
    assert(configuration_repository_decode(blob, blob_length - 1U, &loaded, &generation, CONFIGURATION_ZONES,
                                            configuration_zone_count()) == CONFIGURATION_REPOSITORY_CORRUPT);
    blob[0] ^= 1U;
    assert(configuration_repository_decode(blob, blob_length, &loaded, &generation, CONFIGURATION_ZONES,
                                            configuration_zone_count()) == CONFIGURATION_REPOSITORY_CORRUPT);
    assert(configuration_repository_encode(&configuration, 3U, blob, sizeof(blob), &blob_length) ==
           CONFIGURATION_REPOSITORY_OK);
    blob[4] = 2U;
    assert(configuration_repository_decode(blob, blob_length, &loaded, &generation, CONFIGURATION_ZONES,
                                            configuration_zone_count()) == CONFIGURATION_REPOSITORY_INCOMPATIBLE);
    assert(configuration_repository_encode(&configuration, 3U, blob, sizeof(blob), &blob_length) ==
           CONFIGURATION_REPOSITORY_OK);
    blob[10] = 0xffU;
    blob[11] = 0xffU;
    assert(configuration_repository_decode(blob, blob_length, &loaded, &generation, CONFIGURATION_ZONES,
                                            configuration_zone_count()) == CONFIGURATION_REPOSITORY_CORRUPT);

    IrrigationConfiguration invalid = configuration;
    (void)snprintf(invalid.programs[0].steps[0].zone_id,
                   sizeof(invalid.programs[0].steps[0].zone_id), "%s", "missing-zone");
    assert(configuration_repository_encode(&invalid, 4U, blob, sizeof(blob), &blob_length) ==
           CONFIGURATION_REPOSITORY_OK);
    assert(configuration_repository_decode(blob, blob_length, &loaded, &generation, CONFIGURATION_ZONES,
                                            configuration_zone_count()) == CONFIGURATION_REPOSITORY_INVALID_CONFIGURATION);

    initialize_mock_repository(&repository, &storage);
    assert(configuration_repository_save(&repository, &configuration, CONFIGURATION_ZONES,
                                         configuration_zone_count()) == CONFIGURATION_REPOSITORY_OK);
    storage.fail_write = true;
    assert(configuration_repository_save(&repository, &second, CONFIGURATION_ZONES,
                                         configuration_zone_count()) == CONFIGURATION_REPOSITORY_STORAGE_ERROR);
    assert(configuration_repository_load(&repository, &loaded, &generation, CONFIGURATION_ZONES,
                                         configuration_zone_count()) == CONFIGURATION_REPOSITORY_OK);
    assert(generation == 1U);
    assert_configuration_equal(&loaded, &configuration);
    storage.fail_write = false;
    storage.fail_commit = true;
    assert(configuration_repository_save(&repository, &second, CONFIGURATION_ZONES,
                                         configuration_zone_count()) == CONFIGURATION_REPOSITORY_STORAGE_ERROR);
    assert(configuration_repository_load(&repository, &loaded, &generation, CONFIGURATION_ZONES,
                                         configuration_zone_count()) == CONFIGURATION_REPOSITORY_OK);
    assert(generation == 1U);
    assert_configuration_equal(&loaded, &configuration);
    storage.fail_commit = false;
    storage.corrupt_after_commit = true;
    assert(configuration_repository_save(&repository, &second, CONFIGURATION_ZONES,
                                         configuration_zone_count()) == CONFIGURATION_REPOSITORY_CORRUPT);
    assert(configuration_repository_load(&repository, &loaded, &generation, CONFIGURATION_ZONES,
                                         configuration_zone_count()) == CONFIGURATION_REPOSITORY_OK);
    assert(generation == 1U);
    assert_configuration_equal(&loaded, &configuration);

    IrrigationConfiguration boundary = configuration;
    while (boundary.program_count < IRRIGATION_MAX_PROGRAMS) {
        const size_t index = boundary.program_count++;
        boundary.programs[index] = boundary.programs[0];
        (void)snprintf(boundary.programs[index].program_id, sizeof(boundary.programs[index].program_id),
                       "program-boundary-%u", (unsigned)index);
        (void)snprintf(boundary.programs[index].display_name,
                       sizeof(boundary.programs[index].display_name), "Program %u", (unsigned)index);
        boundary.programs[index].step_count = IRRIGATION_MAX_PROGRAM_STEPS;
        for (size_t step = 0U; step < IRRIGATION_MAX_PROGRAM_STEPS; ++step) {
            (void)snprintf(boundary.programs[index].steps[step].zone_id,
                           sizeof(boundary.programs[index].steps[step].zone_id), "%s", "zone-1");
            boundary.programs[index].steps[step].duration_ms = 1U;
        }
    }
    while (boundary.schedule_count < IRRIGATION_MAX_SCHEDULES) {
        const size_t index = boundary.schedule_count++;
        boundary.schedules[index] = boundary.schedules[0];
        (void)snprintf(boundary.schedules[index].schedule_id,
                       sizeof(boundary.schedules[index].schedule_id), "schedule-boundary-%u",
                       (unsigned)index);
    }
    assert(irrigation_configuration_validate(&boundary, CONFIGURATION_ZONES,
                                             sizeof(CONFIGURATION_ZONES) / sizeof(CONFIGURATION_ZONES[0])) ==
           IRRIGATION_RESULT_OK);
    assert(configuration_repository_encode(&boundary, 11U, blob, sizeof(blob), &blob_length) ==
           CONFIGURATION_REPOSITORY_OK);
    assert(blob_length <= CONFIGURATION_REPOSITORY_MAX_BLOB_SIZE);
    assert(configuration_repository_decode(blob, blob_length, &loaded, &generation, CONFIGURATION_ZONES,
                                            sizeof(CONFIGURATION_ZONES) / sizeof(CONFIGURATION_ZONES[0])) ==
           CONFIGURATION_REPOSITORY_OK);
    assert(generation == 11U);
    assert_configuration_equal(&loaded, &boundary);
}

static void initialize_configuration_manager(ConfigurationManager *manager)
{
    assert(configuration_manager_init_dev_kit_defaults(manager, CONFIGURATION_ZONES,
                                                       configuration_zone_count()) == IRRIGATION_RESULT_OK);
}

static void reboot_configuration_manager(ConfigurationManager *manager, ConfigurationRepository *repository)
{
    initialize_configuration_manager(manager);
    assert(configuration_manager_load_or_persist_defaults(manager, repository) ==
           CONFIGURATION_MANAGER_BOOT_LOADED);
    configuration_manager_set_repository(manager, repository, true);
}

static void test_configuration_manager_persistence(void)
{
    const IrrigationProgramStepConfiguration steps[] = {
        {.zone_id = "zone-3", .duration_ms = 3000U},
        {.zone_id = "zone-4", .duration_ms = 4000U},
    };
    ConfigurationRepository repository;
    MockConfigurationStorage storage;
    ConfigurationManager manager;
    ConfigurationManager restarted;
    initialize_mock_repository(&repository, &storage);
    initialize_configuration_manager(&manager);

    assert(configuration_manager_load_or_persist_defaults(&manager, &repository) ==
           CONFIGURATION_MANAGER_BOOT_DEFAULTS_PERSISTED);
    assert(storage.lengths[0] != 0U);
    configuration_manager_set_repository(&manager, &repository, true);

    assert(configuration_manager_rename_zone(&manager, "zone-1", "Persisted lawn") ==
           IRRIGATION_RESULT_OK);
    reboot_configuration_manager(&restarted, &repository);
    assert(strcmp(configuration_manager_zone_name(&restarted, "zone-1"), "Persisted lawn") == 0);

    size_t program_index;
    assert(configuration_manager_create_program(&restarted, "Persistent evening", steps, 2U,
                                                &program_index) == IRRIGATION_RESULT_OK);
    assert(strcmp(configuration_manager_get(&restarted)->programs[program_index].program_id, "program-1") == 0);
    reboot_configuration_manager(&manager, &repository);
    assert(manager.configuration.program_count == 2U);
    assert(strcmp(manager.configuration.programs[1].display_name, "Persistent evening") == 0);
    assert(manager.configuration.next_program_id == 2U);
    assert(configuration_manager_update_program(&manager, 1U, "Persistent evening updated", steps, 2U) ==
           IRRIGATION_RESULT_OK);
    reboot_configuration_manager(&restarted, &repository);
    assert(strcmp(restarted.configuration.programs[1].program_id, "program-1") == 0);
    assert(strcmp(restarted.configuration.programs[1].display_name, "Persistent evening updated") == 0);

    size_t schedule_index;
    assert(configuration_manager_create_schedule(&restarted,
                                                 SCHEDULER_WEEKDAY_MASK(1U) | SCHEDULER_WEEKDAY_MASK(3U),
                                                 18U, 15U, "program-1", &schedule_index) == IRRIGATION_RESULT_OK);
    assert(strcmp(restarted.configuration.schedules[schedule_index].schedule_id, "schedule-1") == 0);
    reboot_configuration_manager(&manager, &repository);
    assert(manager.configuration.schedule_count == 2U);
    assert(manager.configuration.next_schedule_id == 2U);
    assert(configuration_manager_update_schedule(&manager, 1U, SCHEDULER_WEEKDAY_MASK(5U), 19U, 30U,
                                                 "program-1") == IRRIGATION_RESULT_OK);
    assert(configuration_manager_set_schedule_enabled(&manager, 1U, true) == IRRIGATION_RESULT_OK);
    reboot_configuration_manager(&restarted, &repository);
    assert(strcmp(restarted.configuration.schedules[1].schedule_id, "schedule-1") == 0);
    assert(restarted.configuration.schedules[1].enabled);
    assert(restarted.configuration.schedules[1].weekday_mask == SCHEDULER_WEEKDAY_MASK(5U));
    assert(restarted.configuration.schedules[1].hour == 19U && restarted.configuration.schedules[1].minute == 30U);
    assert(configuration_manager_set_schedule_enabled(&restarted, 1U, false) == IRRIGATION_RESULT_OK);
    reboot_configuration_manager(&manager, &repository);
    assert(!manager.configuration.schedules[1].enabled);
    assert(configuration_manager_delete_schedule(&manager, 1U) == IRRIGATION_RESULT_OK);
    assert(configuration_manager_delete_program(&manager, 1U) == IRRIGATION_RESULT_OK);
    reboot_configuration_manager(&restarted, &repository);
    assert(restarted.configuration.program_count == 1U && restarted.configuration.schedule_count == 1U);
    assert(restarted.configuration.next_program_id == 2U && restarted.configuration.next_schedule_id == 2U);
    assert(configuration_manager_create_program(&restarted, "No reused ID", steps, 2U, &program_index) ==
           IRRIGATION_RESULT_OK);
    assert(strcmp(restarted.configuration.programs[program_index].program_id, "program-2") == 0);

    const IrrigationConfiguration before_failed_save = *configuration_manager_get(&restarted);
    storage.fail_write = true;
    assert(configuration_manager_rename_zone(&restarted, "zone-2", "Should not apply") ==
           IRRIGATION_RESULT_INTERNAL_ERROR);
    assert_configuration_equal(configuration_manager_get(&restarted), &before_failed_save);
    storage.fail_write = false;
    storage.fail_commit = true;
    assert(configuration_manager_rename_zone(&restarted, "zone-2", "Still should not apply") ==
           IRRIGATION_RESULT_INTERNAL_ERROR);
    assert_configuration_equal(configuration_manager_get(&restarted), &before_failed_save);
}

static void test_configuration_manager_persistence_boot_fallbacks(void)
{
    ConfigurationRepository repository;
    MockConfigurationStorage storage;
    ConfigurationManager manager;
    IrrigationConfiguration persisted = make_repository_configuration();
    initialize_mock_repository(&repository, &storage);
    assert(configuration_repository_save(&repository, &persisted, CONFIGURATION_ZONES,
                                         configuration_zone_count()) == CONFIGURATION_REPOSITORY_OK);
    initialize_configuration_manager(&manager);
    assert(configuration_manager_load_or_persist_defaults(&manager, &repository) ==
           CONFIGURATION_MANAGER_BOOT_LOADED);
    assert_configuration_equal(configuration_manager_get(&manager), &persisted);

    storage.blobs[0][CONFIGURATION_REPOSITORY_HEADER_SIZE] ^= 1U;
    const size_t corrupted_length = storage.lengths[0];
    uint8_t corrupted_blob[CONFIGURATION_REPOSITORY_MAX_BLOB_SIZE];
    memcpy(corrupted_blob, storage.blobs[0], corrupted_length);
    initialize_configuration_manager(&manager);
    assert(configuration_manager_load_or_persist_defaults(&manager, &repository) ==
           CONFIGURATION_MANAGER_BOOT_DEFAULTS_CORRUPT);
    assert(strcmp(configuration_manager_zone_name(&manager, "zone-1"), "Zone 1") == 0);
    assert(storage.lengths[0] == corrupted_length);
    assert(memcmp(storage.blobs[0], corrupted_blob, corrupted_length) == 0);

    initialize_mock_repository(&repository, &storage);
    assert(configuration_repository_encode(&persisted, 1U, storage.blobs[0], sizeof(storage.blobs[0]),
                                           &storage.lengths[0]) == CONFIGURATION_REPOSITORY_OK);
    storage.blobs[0][4] = 2U;
    const size_t incompatible_length = storage.lengths[0];
    uint8_t incompatible_blob[CONFIGURATION_REPOSITORY_MAX_BLOB_SIZE];
    memcpy(incompatible_blob, storage.blobs[0], incompatible_length);
    initialize_configuration_manager(&manager);
    assert(configuration_manager_load_or_persist_defaults(&manager, &repository) ==
           CONFIGURATION_MANAGER_BOOT_DEFAULTS_INCOMPATIBLE);
    assert(storage.lengths[0] == incompatible_length);
    assert(memcmp(storage.blobs[0], incompatible_blob, incompatible_length) == 0);
}

static void test_configuration_manager_mutations(void)
{
    ConfigurationManager manager;
    const IrrigationProgramStepConfiguration steps[] = {
        {.zone_id = "zone-1", .duration_ms = 1000U},
        {.zone_id = "zone-2", .duration_ms = 2000U},
    };
    assert(configuration_manager_init_dev_kit_defaults(&manager, CONFIGURATION_ZONES,
                                                       sizeof(CONFIGURATION_ZONES) / sizeof(CONFIGURATION_ZONES[0])) ==
           IRRIGATION_RESULT_OK);
    assert(configuration_manager_rename_zone(&manager, "zone-1", "Front lawn") == IRRIGATION_RESULT_OK);
    assert(strcmp(configuration_manager_zone_name(&manager, "zone-1"), "Front lawn") == 0);

    size_t program_index;
    assert(configuration_manager_create_program(&manager, "Evening", steps, 2U, &program_index) == IRRIGATION_RESULT_OK);
    assert(program_index == 1U);
    const IrrigationConfiguration *configuration = configuration_manager_get(&manager);
    assert(strcmp(configuration->programs[program_index].program_id, "program-1") == 0);
    assert(configuration_manager_update_program(&manager, program_index, "Evening revised", steps, 2U) == IRRIGATION_RESULT_OK);
    assert(strcmp(configuration_manager_get(&manager)->programs[program_index].program_id, "program-1") == 0);

    const IrrigationConfiguration before_invalid_update = *configuration_manager_get(&manager);
    const IrrigationProgramStepConfiguration invalid_steps[] = {{.zone_id = "missing", .duration_ms = 1000U}};
    assert(configuration_manager_update_program(&manager, program_index, "Invalid", invalid_steps, 1U) == IRRIGATION_RESULT_REJECTED);
    assert(memcmp(&before_invalid_update, configuration_manager_get(&manager), sizeof(before_invalid_update)) == 0);

    size_t schedule_index;
    assert(configuration_manager_create_schedule(&manager, SCHEDULER_WEEKDAY_MASK(0U), 19U, 30U,
                                                 "program-1", &schedule_index) == IRRIGATION_RESULT_OK);
    assert(schedule_index == 1U);
    assert(strcmp(configuration_manager_get(&manager)->schedules[schedule_index].schedule_id, "schedule-1") == 0);
    assert(configuration_manager_update_schedule(&manager, schedule_index, SCHEDULER_WEEKDAY_MASK(2U),
                                                 20U, 15U, "program-1") == IRRIGATION_RESULT_OK);
    assert(strcmp(configuration_manager_get(&manager)->schedules[schedule_index].schedule_id, "schedule-1") == 0);
    assert(configuration_manager_set_schedule_enabled(&manager, schedule_index, true) == IRRIGATION_RESULT_OK);
    assert(configuration_manager_get(&manager)->schedules[schedule_index].enabled);
    assert(configuration_manager_delete_program(&manager, program_index) == IRRIGATION_RESULT_REFERENCED);
    assert(configuration_manager_delete_schedule(&manager, schedule_index) == IRRIGATION_RESULT_OK);
    assert(configuration_manager_delete_program(&manager, program_index) == IRRIGATION_RESULT_OK);

    const IrrigationConfiguration before_invalid_schedule = *configuration_manager_get(&manager);
    assert(configuration_manager_create_schedule(&manager, SCHEDULER_WEEKDAY_MASK(0U), 6U, 0U,
                                                 "missing", NULL) == IRRIGATION_RESULT_REJECTED);
    assert(memcmp(&before_invalid_schedule, configuration_manager_get(&manager), sizeof(before_invalid_schedule)) == 0);
}

static void test_dev_kit_zone_names(void)
{
    DevKitZoneConfiguration configuration;
    const ProgramStep step = {.zone_id = "zone-1", .duration_ms = 100U};
    char too_long[DEV_KIT_ZONE_NAME_SIZE + 1U];
    memset(too_long, 'a', DEV_KIT_ZONE_NAME_SIZE);
    too_long[DEV_KIT_ZONE_NAME_SIZE] = '\0';

    dev_kit_zone_configuration_init(&configuration, 4U);
    assert(strcmp(dev_kit_zone_configuration_name(&configuration, 0U), "Zone 1") == 0);
    assert(strcmp(dev_kit_zone_configuration_name(&configuration, 3U), "Zone 4") == 0);
    assert(dev_kit_zone_configuration_set_name(&configuration, 0U, "Prato") == IRRIGATION_RESULT_OK);
    assert(strcmp(dev_kit_zone_configuration_name(&configuration, 0U), "Prato") == 0);
    assert(strcmp(step.zone_id, "zone-1") == 0);
    assert(dev_kit_zone_configuration_set_name(&configuration, 0U, "") == IRRIGATION_RESULT_REJECTED);
    assert(dev_kit_zone_configuration_set_name(&configuration, 0U, too_long) == IRRIGATION_RESULT_REJECTED);
}

static IrrigationResult fake_status_led_set_state(StatusLedDriver *base, StatusLedState state)
{
    FakeStatusLedDriver *driver = (FakeStatusLedDriver *)base;
    driver->state = state;
    driver->set_calls++;
    return IRRIGATION_RESULT_OK;
}

static const StatusLedDriverVTable FAKE_STATUS_LED_DRIVER_VTABLE = {
    .set_state = fake_status_led_set_state,
};

static void test_status_led_service_priority_and_transitions(void)
{
    FakeStatusLedDriver driver = {.base.vtable = &FAKE_STATUS_LED_DRIVER_VTABLE};
    StatusLedService service;

    status_led_service_init(&service, &driver.base);
    assert(driver.state == STATUS_LED_STATE_BOOTING && driver.set_calls == 1U);

    status_led_service_set_network_connected(&service, true);
    assert(status_led_service_state(&service) == STATUS_LED_STATE_READY);
    status_led_service_refresh(&service);
    assert(driver.state == STATUS_LED_STATE_READY);

    status_led_service_set_network_connected(&service, false);
    assert(status_led_service_state(&service) == STATUS_LED_STATE_NETWORK_DISCONNECTED);
    status_led_service_refresh(&service);
    assert(driver.state == STATUS_LED_STATE_NETWORK_DISCONNECTED);

    status_led_service_set_fault(&service, true);
    status_led_service_set_network_connected(&service, true);
    assert(status_led_service_state(&service) == STATUS_LED_STATE_FAULT);
    status_led_service_refresh(&service);
    assert(driver.state == STATUS_LED_STATE_FAULT);

    status_led_service_set_fault(&service, false);
    assert(status_led_service_state(&service) == STATUS_LED_STATE_READY);
    status_led_service_refresh(&service);
    assert(driver.state == STATUS_LED_STATE_READY);
}

static IrrigationResult fake_shelly_http_get(ShellyHttpTransport *base, const char *host, uint16_t port,
                                             const char *path, uint32_t timeout_ms, char *response,
                                             size_t response_capacity, size_t *response_length,
                                             int *http_status)
{
    FakeShellyHttpTransport *transport = (FakeShellyHttpTransport *)base;
    assert(strcmp(host, "shelly-master") == 0);
    assert(port == 80U && timeout_ms == 1000U);
    assert(transport->request_count < transport->response_count);
    const size_t index = transport->request_count++;
    assert(snprintf(transport->paths[index], sizeof(transport->paths[index]), "%s", path) <
           (int)sizeof(transport->paths[index]));
    const FakeShellyHttpResponse *configured = &transport->responses[index];
    if (configured->result != IRRIGATION_RESULT_OK) return configured->result;
    const size_t length = strlen(configured->body);
    assert(length < response_capacity);
    memcpy(response, configured->body, length);
    *response_length = length;
    *http_status = configured->http_status;
    return IRRIGATION_RESULT_OK;
}

static const ShellyHttpTransportVTable FAKE_SHELLY_HTTP_TRANSPORT_VTABLE = {
    .get = fake_shelly_http_get,
};

static void fake_shelly_http_transport_init(FakeShellyHttpTransport *transport,
                                            FakeShellyHttpResponse first,
                                            FakeShellyHttpResponse second)
{
    *transport = (FakeShellyHttpTransport){
        .base.vtable = &FAKE_SHELLY_HTTP_TRANSPORT_VTABLE,
        .responses = {first, second},
        .response_count = 2U,
    };
}

static void test_shelly_master_valve_driver(void)
{
    const FakeShellyHttpResponse set_ok = {
        .result = IRRIGATION_RESULT_OK, .http_status = 200, .body = "{\"was_on\":false}"};
    FakeShellyHttpTransport transport;
    ShellyMasterValveDriver driver;

    fake_shelly_http_transport_init(&transport, set_ok,
                                    (FakeShellyHttpResponse){.result = IRRIGATION_RESULT_OK,
                                                              .http_status = 200,
                                                              .body = "{\"output\":true}"});
    shelly_master_valve_driver_init(&driver, &transport.base, (ShellyMasterValveConfiguration){
        .host = "shelly-master", .port = 80U, .switch_id = 0U, .operation_timeout_ms = 1000U});
    assert(master_valve_driver_open_and_confirm(&driver.base) == IRRIGATION_RESULT_OK);
    assert(transport.request_count == 2U);
    assert(strcmp(transport.paths[0], "/rpc/Switch.Set?id=0&on=true") == 0);
    assert(strcmp(transport.paths[1], "/rpc/Switch.GetStatus?id=0") == 0);

    fake_shelly_http_transport_init(&transport, set_ok,
                                    (FakeShellyHttpResponse){.result = IRRIGATION_RESULT_OK,
                                                              .http_status = 200,
                                                              .body = "{\"output\":false}"});
    shelly_master_valve_driver_init(&driver, &transport.base, (ShellyMasterValveConfiguration){
        .host = "shelly-master", .port = 80U, .switch_id = 0U, .operation_timeout_ms = 1000U});
    assert(master_valve_driver_close_and_confirm(&driver.base) == IRRIGATION_RESULT_OK);
    assert(strcmp(transport.paths[0], "/rpc/Switch.Set?id=0&on=false") == 0);

    fake_shelly_http_transport_init(&transport,
                                    (FakeShellyHttpResponse){.result = IRRIGATION_RESULT_OK,
                                                              .http_status = 500,
                                                              .body = "{\"error\":\"failed\"}"},
                                    set_ok);
    shelly_master_valve_driver_init(&driver, &transport.base, (ShellyMasterValveConfiguration){
        .host = "shelly-master", .port = 80U, .switch_id = 0U, .operation_timeout_ms = 1000U});
    assert(master_valve_driver_open_and_confirm(&driver.base) == IRRIGATION_RESULT_INTERNAL_ERROR);
    assert(transport.request_count == 1U);

    fake_shelly_http_transport_init(&transport, set_ok,
                                    (FakeShellyHttpResponse){.result = IRRIGATION_RESULT_INTERNAL_ERROR});
    shelly_master_valve_driver_init(&driver, &transport.base, (ShellyMasterValveConfiguration){
        .host = "shelly-master", .port = 80U, .switch_id = 0U, .operation_timeout_ms = 1000U});
    assert(master_valve_driver_open_and_confirm(&driver.base) == IRRIGATION_RESULT_INTERNAL_ERROR);

    fake_shelly_http_transport_init(&transport, set_ok,
                                    (FakeShellyHttpResponse){.result = IRRIGATION_RESULT_OK,
                                                              .http_status = 200,
                                                              .body = "{\"output\":true"});
    shelly_master_valve_driver_init(&driver, &transport.base, (ShellyMasterValveConfiguration){
        .host = "shelly-master", .port = 80U, .switch_id = 0U, .operation_timeout_ms = 1000U});
    assert(master_valve_driver_open_and_confirm(&driver.base) == IRRIGATION_RESULT_INTERNAL_ERROR);

    fake_shelly_http_transport_init(&transport, set_ok,
                                    (FakeShellyHttpResponse){.result = IRRIGATION_RESULT_OK,
                                                              .http_status = 200,
                                                              .body = "{\"output\":false}"});
    shelly_master_valve_driver_init(&driver, &transport.base, (ShellyMasterValveConfiguration){
        .host = "shelly-master", .port = 80U, .switch_id = 0U, .operation_timeout_ms = 1000U});
    assert(master_valve_driver_open_and_confirm(&driver.base) == IRRIGATION_RESULT_INTERNAL_ERROR);

    fake_shelly_http_transport_init(&transport,
                                    (FakeShellyHttpResponse){.result = IRRIGATION_RESULT_TIMEOUT}, set_ok);
    shelly_master_valve_driver_init(&driver, &transport.base, (ShellyMasterValveConfiguration){
        .host = "shelly-master", .port = 80U, .switch_id = 0U, .operation_timeout_ms = 1000U});
    assert(master_valve_driver_open_and_confirm(&driver.base) == IRRIGATION_RESULT_TIMEOUT);
}

static uint64_t fake_now_ms(void *context)
{
    return ((FakeClock *)context)->now_ms;
}

static IrrigationResult allow_zone_start(void *context)
{
    (void)context;
    return IRRIGATION_RESULT_OK;
}

static IrrigationResult fake_master_valve_open(MasterValveDriver *base)
{
    FakeMasterValveDriver *driver = (FakeMasterValveDriver *)base;
    driver->open_calls++;
    if (driver->fail_open) {
        return IRRIGATION_RESULT_INTERNAL_ERROR;
    }
    driver->is_open = true;
    return IRRIGATION_RESULT_OK;
}

static IrrigationResult fake_master_valve_close(MasterValveDriver *base)
{
    FakeMasterValveDriver *driver = (FakeMasterValveDriver *)base;
    driver->close_calls++;
    if (driver->fail_close) {
        return IRRIGATION_RESULT_INTERNAL_ERROR;
    }
    driver->is_open = false;
    return IRRIGATION_RESULT_OK;
}

static const MasterValveDriverVTable FAKE_MASTER_VALVE_VTABLE = {
    .open_and_confirm = fake_master_valve_open,
    .close_and_confirm = fake_master_valve_close,
};

static void fake_master_valve_driver_init(FakeMasterValveDriver *driver)
{
    memset(driver, 0, sizeof(*driver));
    driver->base.vtable = &FAKE_MASTER_VALVE_VTABLE;
}

static void record_event(const Event *event, void *context)
{
    EventRecorder *recorder = context;
    assert(recorder->count < sizeof(recorder->events) / sizeof(recorder->events[0]));
    recorder->events[recorder->count++] = *event;
}

static IrrigationResult failing_initialize_safe_off(OutputDriver *base)
{
    FailingOutputDriver *driver = (FailingOutputDriver *)base;
    memset(driver->state, 0, sizeof(driver->state));
    return IRRIGATION_RESULT_OK;
}

static IrrigationResult failing_set(OutputDriver *base, size_t output_index, bool enabled)
{
    FailingOutputDriver *driver = (FailingOutputDriver *)base;
    if (output_index >= IRRIGATION_LOCAL_OUTPUT_COUNT) {
        return IRRIGATION_RESULT_NOT_FOUND;
    }
    driver->state[output_index] = enabled;
    if (enabled && driver->fail_after_on) {
        driver->fail_after_on = false;
        return IRRIGATION_RESULT_INTERNAL_ERROR;
    }
    if (!enabled && driver->fail_after_off_count != 0U) {
        driver->fail_after_off_count--;
        return IRRIGATION_RESULT_INTERNAL_ERROR;
    }
    return IRRIGATION_RESULT_OK;
}

static IrrigationResult reentrant_initialize_safe_off(OutputDriver *base)
{
    ReentrantOutputDriver *driver = (ReentrantOutputDriver *)base;
    memset(driver->state, 0, sizeof(driver->state));
    return IRRIGATION_RESULT_OK;
}

static IrrigationResult reentrant_set(OutputDriver *base, size_t output_index, bool enabled)
{
    ReentrantOutputDriver *driver = (ReentrantOutputDriver *)base;
    if (output_index >= IRRIGATION_LOCAL_OUTPUT_COUNT) {
        return IRRIGATION_RESULT_NOT_FOUND;
    }
    driver->state[output_index] = enabled;
    if (enabled && output_index == 0U && !driver->nested_open_attempted) {
        driver->nested_open_attempted = true;
        driver->nested_open_result = zone_service_open(driver->service, "zone-2", 0U);
    }
    return IRRIGATION_RESULT_OK;
}

static IrrigationResult reentrant_get(const OutputDriver *base, size_t output_index, bool *enabled)
{
    const ReentrantOutputDriver *driver = (const ReentrantOutputDriver *)base;
    if (output_index >= IRRIGATION_LOCAL_OUTPUT_COUNT || enabled == NULL) {
        return IRRIGATION_RESULT_NOT_FOUND;
    }
    *enabled = driver->state[output_index];
    return IRRIGATION_RESULT_OK;
}

static const OutputDriverVTable REENTRANT_VTABLE = {
    .initialize_safe_off = reentrant_initialize_safe_off,
    .set_output = reentrant_set,
    .get_output = reentrant_get,
};

static void reentrant_output_driver_init(ReentrantOutputDriver *driver, ZoneService *service)
{
    memset(driver, 0, sizeof(*driver));
    driver->base.vtable = &REENTRANT_VTABLE;
    driver->service = service;
}

static IrrigationResult failing_get(const OutputDriver *base, size_t output_index, bool *enabled)
{
    const FailingOutputDriver *driver = (const FailingOutputDriver *)base;
    if (output_index >= IRRIGATION_LOCAL_OUTPUT_COUNT || enabled == NULL) {
        return IRRIGATION_RESULT_NOT_FOUND;
    }
    *enabled = driver->state[output_index];
    return IRRIGATION_RESULT_OK;
}

static const OutputDriverVTable FAILING_VTABLE = {
    .initialize_safe_off = failing_initialize_safe_off,
    .set_output = failing_set,
    .get_output = failing_get,
};

static void failing_output_driver_init(FailingOutputDriver *driver)
{
    memset(driver, 0, sizeof(*driver));
    driver->base.vtable = &FAILING_VTABLE;
}

static ZoneService make_service(StateStore *store, OutputDriver *driver, EventRecorder *recorder,
                                FakeClock *clock, EventBus *bus)
{
    event_bus_init(bus, record_event, recorder);
    ZoneService service;
    zone_service_init(&service, store, driver, bus, (Clock){.now_ms = fake_now_ms, .context = clock},
                      (ZoneStartPrecondition){.check = allow_zone_start, .context = NULL});
    return service;
}

static ZoneService make_service_with_precondition(StateStore *store, OutputDriver *driver,
                                                  EventRecorder *recorder, FakeClock *clock, EventBus *bus,
                                                  ZoneStartPrecondition precondition)
{
    event_bus_init(bus, record_event, recorder);
    ZoneService service;
    zone_service_init(&service, store, driver, bus, (Clock){.now_ms = fake_now_ms, .context = clock}, precondition);
    return service;
}

static void initialize_store_with_closed_master_valve(StateStore *store)
{
    state_store_init(store);
    assert(state_store_configure_zones(store, ZONES, 2U) == IRRIGATION_RESULT_OK);
    assert(state_store_transition_system(store, SYSTEM_STATE_READY) == IRRIGATION_RESULT_OK);
}

static void initialize_store(StateStore *store)
{
    initialize_store_with_closed_master_valve(store);
    assert(state_store_begin_master_valve_open(store, 0U) == IRRIGATION_RESULT_OK);
    assert(state_store_complete_master_valve_pre_open_delay(store, 0U) == IRRIGATION_RESULT_OK);
}

static void test_configuration_manager_preserves_scheduler_occurrence(void)
{
    ConfigurationManager manager;
    StateStore store;
    FakeClock clock = {0};
    EventRecorder recorder = {0};
    EventBus bus;
    VirtualOutputDriver zone_driver;
    FakeMasterValveDriver master_driver;
    MasterValveService master_service;
    ProgramService program_service;
    SchedulerService scheduler;
    SchedulerLockRecorder lock_recorder = {0};
    virtual_output_driver_init(&zone_driver, (Logger){0});
    fake_master_valve_driver_init(&master_driver);
    initialize_store_with_closed_master_valve(&store);
    master_valve_service_init(&master_service, &store, &master_driver.base,
                              (Clock){.now_ms = fake_now_ms, .context = &clock},
                              (MasterValveConfiguration){0});
    ZoneService zone_service = make_service(&store, &zone_driver.base, &recorder, &clock, &bus);
    program_service_init(&program_service, &store, &master_service, &zone_service,
                         (Clock){.now_ms = fake_now_ms, .context = &clock});
    scheduler_service_init(&scheduler, &store, &program_service);
    scheduler_service_set_synchronization(&scheduler, (SchedulerServiceSynchronization){
        .context = &lock_recorder, .lock = record_scheduler_lock, .unlock = record_scheduler_unlock,
    });
    assert(configuration_manager_init_dev_kit_defaults(&manager, CONFIGURATION_ZONES,
                                                       sizeof(CONFIGURATION_ZONES) / sizeof(CONFIGURATION_ZONES[0])) ==
           IRRIGATION_RESULT_OK);
    assert(configuration_manager_update_schedule(&manager, 0U, SCHEDULER_WEEKDAY_MASK(0U), 6U, 0U,
                                                 "dev-program") == IRRIGATION_RESULT_OK);
    assert(configuration_manager_set_schedule_enabled(&manager, 0U, true) == IRRIGATION_RESULT_OK);
    assert(configuration_manager_configure_scheduler(&manager, &scheduler) == IRRIGATION_RESULT_OK);
    configuration_manager_bind_scheduler(&manager, &scheduler);
    assert(scheduler_service_process(&scheduler, (SchedulerTime){.week_index = 3U, .weekday = 0U,
                                                                  .hour = 6U}) == IRRIGATION_RESULT_OK);
    assert(master_driver.open_calls == 1U);
    const RuntimeState before = state_store_snapshot(&store);
    assert(before.scheduler.last_occurrence_status[0] == SCHEDULE_OCCURRENCE_STARTED);
    assert(configuration_manager_create_schedule(&manager, SCHEDULER_WEEKDAY_MASK(1U), 7U, 0U,
                                                 "dev-program", NULL) == IRRIGATION_RESULT_OK);
    assert(scheduler.entry_count == 2U);
    const RuntimeState after = state_store_snapshot(&store);
    assert(after.scheduler.last_handled_occurrence[0] == before.scheduler.last_handled_occurrence[0]);
    assert(after.scheduler.last_occurrence_status[0] == SCHEDULE_OCCURRENCE_STARTED);
    assert(scheduler_service_process(&scheduler, (SchedulerTime){.week_index = 3U, .weekday = 0U,
                                                                  .hour = 6U}) == IRRIGATION_RESULT_OK);
    assert(master_driver.open_calls == 1U);
    assert(configuration_manager_set_schedule_enabled(&manager, 1U, true) == IRRIGATION_RESULT_OK);
    assert(scheduler.entries[1].enabled);
    assert(configuration_manager_delete_schedule(&manager, 1U) == IRRIGATION_RESULT_OK);
    assert(scheduler.entry_count == 1U);
    assert(lock_recorder.lock_calls == lock_recorder.unlock_calls && !lock_recorder.locked);
}

static void test_configuration_manager_scheduler_reconfiguration_failure_rolls_back(void)
{
    ConfigurationManager manager;
    ConfigurationRepository repository;
    MockConfigurationStorage storage;
    SchedulerService failing_scheduler = {0};
    IrrigationConfiguration loaded;
    uint32_t generation;
    initialize_mock_repository(&repository, &storage);
    assert(configuration_manager_init_dev_kit_defaults(&manager, CONFIGURATION_ZONES,
                                                       configuration_zone_count()) == IRRIGATION_RESULT_OK);
    assert(configuration_manager_load_or_persist_defaults(&manager, &repository) ==
           CONFIGURATION_MANAGER_BOOT_DEFAULTS_PERSISTED);
    configuration_manager_set_repository(&manager, &repository, true);
    configuration_manager_bind_scheduler(&manager, &failing_scheduler);
    const IrrigationConfiguration before = *configuration_manager_get(&manager);
    assert(configuration_manager_set_schedule_enabled(&manager, 0U, true) == IRRIGATION_RESULT_INTERNAL_ERROR);
    assert_configuration_equal(configuration_manager_get(&manager), &before);
    assert(configuration_repository_load(&repository, &loaded, &generation, CONFIGURATION_ZONES,
                                         configuration_zone_count()) == CONFIGURATION_REPOSITORY_OK);
    assert(generation == 1U);
    assert_configuration_equal(&loaded, &before);
}

static void test_open_close_and_authority(void)
{
    StateStore store;
    FakeClock clock = {.now_ms = 10U};
    EventRecorder recorder = {0};
    EventBus bus;
    VirtualOutputDriver driver;
    virtual_output_driver_init(&driver, (Logger){0});
    initialize_store(&store);
    ZoneService service = make_service(&store, &driver.base, &recorder, &clock, &bus);

    assert(zone_service_open(&service, "zone-1", 0U) == IRRIGATION_RESULT_OK);
    RuntimeState state = state_store_snapshot(&store);
    assert(state.system_state == SYSTEM_STATE_MANUAL && state.output_states[0] == OUTPUT_STATE_ON);
    assert(state.zones[0].state == ZONE_STATE_OPEN && state.zones[0].duration_ms == 100U);
    assert(recorder.count == 1U && recorder.events[0].type == EVENT_TYPE_ZONE_OPENED);
    assert(strcmp(recorder.events[0].zone_id, "zone-1") == 0);

    assert(zone_service_close(&service, "zone-1") == IRRIGATION_RESULT_OK);
    state = state_store_snapshot(&store);
    assert(state.system_state == SYSTEM_STATE_READY && state.output_states[0] == OUTPUT_STATE_OFF);
    assert(state.zones[0].state == ZONE_STATE_IDLE);
    assert(recorder.count == 2U && recorder.events[1].type == EVENT_TYPE_ZONE_CLOSED);
}

static void test_invalid_and_forbidden_transitions(void)
{
    StateStore store;
    FakeClock clock = {0};
    EventRecorder recorder = {0};
    EventBus bus;
    VirtualOutputDriver driver;
    virtual_output_driver_init(&driver, (Logger){0});
    initialize_store(&store);
    ZoneService service = make_service(&store, &driver.base, &recorder, &clock, &bus);

    assert(zone_service_open(&service, "unknown", 0U) == IRRIGATION_RESULT_NOT_FOUND);
    assert(zone_service_open(&service, "zone-1", 0U) == IRRIGATION_RESULT_OK);
    assert(zone_service_open(&service, "zone-2", 0U) == IRRIGATION_RESULT_INVALID_STATE);
    assert(zone_service_close(&service, "zone-2") == IRRIGATION_RESULT_INVALID_STATE);
}

static void test_failed_on_safe_close_and_start_lockout(void)
{
    StateStore store;
    FakeClock clock = {0};
    EventRecorder recorder = {0};
    EventBus bus;
    FailingOutputDriver driver;
    failing_output_driver_init(&driver);
    initialize_store(&store);
    ZoneService service = make_service(&store, &driver.base, &recorder, &clock, &bus);

    driver.fail_after_on = true;
    driver.fail_after_off_count = 1U;
    assert(zone_service_open(&service, "zone-1", 0U) == IRRIGATION_RESULT_INTERNAL_ERROR);
    RuntimeState state = state_store_snapshot(&store);
    assert(state.output_states[0] == OUTPUT_STATE_UNKNOWN && state.zones[0].state == ZONE_STATE_FAULT);
    assert(state.system_state == SYSTEM_STATE_ERROR);
    assert(recorder.count == 1U && recorder.events[0].type == EVENT_TYPE_ZONE_FAULTED);
    assert(zone_service_open(&service, "zone-2", 0U) == IRRIGATION_RESULT_INVALID_STATE);
    assert(zone_service_safe_close(&service) == IRRIGATION_RESULT_OK);
    state = state_store_snapshot(&store);
    assert(state.output_states[0] == OUTPUT_STATE_OFF && state.zones[0].state == ZONE_STATE_IDLE);

    initialize_store(&store);
    recorder.count = 0U;
    driver.fail_after_on = true;
    assert(zone_service_open(&service, "zone-1", 0U) == IRRIGATION_RESULT_INTERNAL_ERROR);
    state = state_store_snapshot(&store);
    assert(state.output_states[0] == OUTPUT_STATE_OFF && state.zones[0].state == ZONE_STATE_FAULT);
    assert(!driver.state[0]);
}

static void test_failed_off_safe_close_retry(void)
{
    StateStore store;
    FakeClock clock = {0};
    EventRecorder recorder = {0};
    EventBus bus;
    FailingOutputDriver driver;
    failing_output_driver_init(&driver);
    initialize_store(&store);
    ZoneService service = make_service(&store, &driver.base, &recorder, &clock, &bus);

    assert(zone_service_open(&service, "zone-1", 0U) == IRRIGATION_RESULT_OK);
    driver.fail_after_off_count = 1U;
    assert(zone_service_close(&service, "zone-1") == IRRIGATION_RESULT_INTERNAL_ERROR);
    RuntimeState state = state_store_snapshot(&store);
    assert(state.output_states[0] == OUTPUT_STATE_UNKNOWN && state.zones[0].state == ZONE_STATE_FAULT);
    assert(zone_service_safe_close(&service) == IRRIGATION_RESULT_OK);
    state = state_store_snapshot(&store);
    assert(state.output_states[0] == OUTPUT_STATE_OFF && state.zones[0].state == ZONE_STATE_IDLE);
}

static void test_reentrant_open_cannot_actuate_second_zone(void)
{
    StateStore store;
    FakeClock clock = {0};
    EventRecorder recorder = {0};
    EventBus bus;
    ZoneService service;
    ReentrantOutputDriver driver;
    initialize_store(&store);
    zone_service_init(&service, &store, &driver.base, &bus, (Clock){.now_ms = fake_now_ms, .context = &clock},
                      (ZoneStartPrecondition){.check = allow_zone_start, .context = NULL});
    event_bus_init(&bus, record_event, &recorder);
    reentrant_output_driver_init(&driver, &service);

    assert(zone_service_open(&service, "zone-1", 0U) == IRRIGATION_RESULT_OK);
    assert(driver.nested_open_attempted);
    assert(driver.nested_open_result == IRRIGATION_RESULT_REJECTED);
    assert(driver.state[0] && !driver.state[1]);
    RuntimeState state = state_store_snapshot(&store);
    assert(state.output_states[0] == OUTPUT_STATE_ON && state.output_states[1] == OUTPUT_STATE_OFF);
}

static void test_timeout_and_command_flow(void)
{
    StateStore store;
    FakeClock clock = {.now_ms = 100U};
    EventRecorder recorder = {0};
    EventBus bus;
    VirtualOutputDriver driver;
    virtual_output_driver_init(&driver, (Logger){0});
    initialize_store(&store);
    ZoneService service = make_service(&store, &driver.base, &recorder, &clock, &bus);
    CommandBus command_bus;
    command_bus_init(&command_bus, zone_command_handler_dispatch, &service);
    const Command open = {.id = "open-1", .timestamp_ms = 100U, .origin = "unit",
                          .type = COMMAND_TYPE_OPEN_ZONE, .version = 1U, .zone_id = "zone-1"};
    assert(command_bus_dispatch(&command_bus, &open) == IRRIGATION_RESULT_OK);
    clock.now_ms = 200U;
    assert(zone_service_process_timeouts(&service) == IRRIGATION_RESULT_OK);
    RuntimeState state = state_store_snapshot(&store);
    assert(state.zones[0].state == ZONE_STATE_IDLE && state.output_states[0] == OUTPUT_STATE_OFF);
    assert(recorder.count == 3U);
    assert(recorder.events[0].type == EVENT_TYPE_ZONE_OPENED);
    assert(recorder.events[1].type == EVENT_TYPE_ZONE_CLOSED);
    assert(recorder.events[2].type == EVENT_TYPE_ZONE_TIMED_OUT);
}

static void test_master_valve_zone_invariants_and_pre_open_delay(void)
{
    StateStore store;
    FakeClock clock = {0};
    EventRecorder recorder = {0};
    EventBus bus;
    VirtualOutputDriver zone_driver;
    FakeMasterValveDriver master_driver;
    MasterValveService master_service;
    virtual_output_driver_init(&zone_driver, (Logger){0});
    fake_master_valve_driver_init(&master_driver);
    initialize_store_with_closed_master_valve(&store);
    master_valve_service_init(&master_service, &store, &master_driver.base,
                              (Clock){.now_ms = fake_now_ms, .context = &clock},
                              (MasterValveConfiguration){.pre_open_delay_ms = 50U});
    ZoneService zone_service = make_service_with_precondition(
        &store, &zone_driver.base, &recorder, &clock, &bus,
        master_valve_service_zone_start_precondition(&master_service));

    assert(zone_service_open(&zone_service, "zone-1", 0U) == IRRIGATION_RESULT_INVALID_STATE);
    assert(!zone_driver.reported_outputs[0]);
    assert(master_valve_service_open(&master_service) == IRRIGATION_RESULT_OK);
    assert(master_driver.is_open && master_driver.open_calls == 1U);
    assert(state_store_snapshot(&store).master_valve.state == MASTER_VALVE_STATE_OPENING);
    assert(zone_service_open(&zone_service, "zone-1", 0U) == IRRIGATION_RESULT_INVALID_STATE);

    clock.now_ms = 49U;
    assert(master_valve_service_process_time(&master_service) == IRRIGATION_RESULT_OK);
    assert(state_store_snapshot(&store).master_valve.state == MASTER_VALVE_STATE_OPENING);
    clock.now_ms = 50U;
    assert(master_valve_service_process_time(&master_service) == IRRIGATION_RESULT_OK);
    assert(state_store_snapshot(&store).master_valve.state == MASTER_VALVE_STATE_OPEN);

    assert(zone_service_open(&zone_service, "zone-1", 0U) == IRRIGATION_RESULT_OK);
    assert(master_valve_service_close(&master_service) == IRRIGATION_RESULT_INVALID_STATE);
    assert(state_store_snapshot(&store).master_valve.state == MASTER_VALVE_STATE_OPEN);
    assert(zone_service_open(&zone_service, "zone-2", 0U) == IRRIGATION_RESULT_INVALID_STATE);
    assert(master_driver.is_open && master_driver.close_calls == 0U);

    assert(zone_service_close(&zone_service, "zone-1") == IRRIGATION_RESULT_OK);
    assert(master_valve_service_close_is_eligible(&master_service));
    assert(master_valve_service_close(&master_service) == IRRIGATION_RESULT_OK);
    assert(!master_driver.is_open && master_driver.close_calls == 1U);
    assert(state_store_snapshot(&store).master_valve.state == MASTER_VALVE_STATE_CLOSED);
}

static void test_master_valve_confirmation_failure_blocks_zone_actuation(void)
{
    StateStore store;
    FakeClock clock = {0};
    EventRecorder recorder = {0};
    EventBus bus;
    VirtualOutputDriver zone_driver;
    FakeMasterValveDriver master_driver;
    MasterValveService master_service;
    virtual_output_driver_init(&zone_driver, (Logger){0});
    fake_master_valve_driver_init(&master_driver);
    master_driver.fail_open = true;
    initialize_store_with_closed_master_valve(&store);
    master_valve_service_init(&master_service, &store, &master_driver.base,
                              (Clock){.now_ms = fake_now_ms, .context = &clock},
                              (MasterValveConfiguration){0});
    ZoneService zone_service = make_service_with_precondition(
        &store, &zone_driver.base, &recorder, &clock, &bus,
        master_valve_service_zone_start_precondition(&master_service));

    assert(master_valve_service_open(&master_service) == IRRIGATION_RESULT_INTERNAL_ERROR);
    assert(state_store_snapshot(&store).master_valve.state == MASTER_VALVE_STATE_FAULT);
    assert(zone_service_open(&zone_service, "zone-1", 0U) == IRRIGATION_RESULT_INVALID_STATE);
    assert(!zone_driver.reported_outputs[0]);
}

static void test_master_valve_close_failure_blocks_zone_actuation(void)
{
    StateStore store;
    FakeClock clock = {0};
    EventRecorder recorder = {0};
    EventBus bus;
    VirtualOutputDriver zone_driver;
    FakeMasterValveDriver master_driver;
    MasterValveService master_service;
    virtual_output_driver_init(&zone_driver, (Logger){0});
    fake_master_valve_driver_init(&master_driver);
    initialize_store_with_closed_master_valve(&store);
    master_valve_service_init(&master_service, &store, &master_driver.base,
                              (Clock){.now_ms = fake_now_ms, .context = &clock},
                              (MasterValveConfiguration){0});
    ZoneService zone_service = make_service_with_precondition(
        &store, &zone_driver.base, &recorder, &clock, &bus,
        master_valve_service_zone_start_precondition(&master_service));

    assert(master_valve_service_open(&master_service) == IRRIGATION_RESULT_OK);
    assert(state_store_snapshot(&store).master_valve.state == MASTER_VALVE_STATE_OPENING);
    assert(master_valve_service_process_time(&master_service) == IRRIGATION_RESULT_OK);
    assert(state_store_snapshot(&store).master_valve.state == MASTER_VALVE_STATE_OPEN);
    master_driver.fail_close = true;
    assert(master_valve_service_close(&master_service) == IRRIGATION_RESULT_INTERNAL_ERROR);
    assert(state_store_snapshot(&store).master_valve.state == MASTER_VALVE_STATE_FAULT);
    assert(zone_service_open(&zone_service, "zone-1", 0U) == IRRIGATION_RESULT_INVALID_STATE);
    assert(!zone_driver.reported_outputs[0]);
}

static void test_unknown_output_blocks_master_valve_close_and_zone_start(void)
{
    StateStore store;
    FakeClock clock = {0};
    EventRecorder recorder = {0};
    EventBus bus;
    VirtualOutputDriver zone_driver;
    FakeMasterValveDriver master_driver;
    MasterValveService master_service;
    virtual_output_driver_init(&zone_driver, (Logger){0});
    fake_master_valve_driver_init(&master_driver);
    initialize_store_with_closed_master_valve(&store);
    master_valve_service_init(&master_service, &store, &master_driver.base,
                              (Clock){.now_ms = fake_now_ms, .context = &clock},
                              (MasterValveConfiguration){0});
    ZoneService zone_service = make_service_with_precondition(
        &store, &zone_driver.base, &recorder, &clock, &bus,
        master_valve_service_zone_start_precondition(&master_service));

    assert(master_valve_service_open(&master_service) == IRRIGATION_RESULT_OK);
    assert(master_valve_service_process_time(&master_service) == IRRIGATION_RESULT_OK);
    assert(state_store_record_zone_actuation_fault(&store, "zone-1", OUTPUT_STATE_UNKNOWN) ==
           IRRIGATION_RESULT_OK);
    assert(master_valve_service_close(&master_service) == IRRIGATION_RESULT_INVALID_STATE);
    assert(master_driver.close_calls == 0U);
    assert(zone_service_open(&zone_service, "zone-2", 0U) == IRRIGATION_RESULT_INVALID_STATE);
    assert(!zone_driver.reported_outputs[1]);
}

static void test_program_rejects_empty_program(void)
{
    StateStore store;
    FakeClock clock = {0};
    EventRecorder recorder = {0};
    EventBus bus;
    VirtualOutputDriver zone_driver;
    FakeMasterValveDriver master_driver;
    MasterValveService master_service;
    ProgramService program_service;
    virtual_output_driver_init(&zone_driver, (Logger){0});
    fake_master_valve_driver_init(&master_driver);
    initialize_store_with_closed_master_valve(&store);
    master_valve_service_init(&master_service, &store, &master_driver.base,
                              (Clock){.now_ms = fake_now_ms, .context = &clock},
                              (MasterValveConfiguration){0});
    ZoneService zone_service = make_service(&store, &zone_driver.base, &recorder, &clock, &bus);
    program_service_init(&program_service, &store, &master_service, &zone_service,
                         (Clock){.now_ms = fake_now_ms, .context = &clock});
    const Program empty = {.id = "empty", .steps = NULL, .step_count = 0U};

    assert(program_service_start(&program_service, &empty) == IRRIGATION_RESULT_REJECTED);
    assert(program_service_state(&program_service) == PROGRAM_STATE_IDLE);
}

static void test_program_single_and_multi_zone_sequences(void)
{
    const ProgramStep steps[] = {{.zone_id = "zone-1", .duration_ms = 10U},
                                 {.zone_id = "zone-2", .duration_ms = 20U}};
    const Program program = {
        .id = "two-zones", .steps = steps, .step_count = 2U, .post_program_delay_ms = 5U};
    StateStore store;
    FakeClock clock = {0};
    EventRecorder recorder = {0};
    EventBus bus;
    VirtualOutputDriver zone_driver;
    FakeMasterValveDriver master_driver;
    MasterValveService master_service;
    ProgramService program_service;
    virtual_output_driver_init(&zone_driver, (Logger){0});
    fake_master_valve_driver_init(&master_driver);
    initialize_store_with_closed_master_valve(&store);
    master_valve_service_init(&master_service, &store, &master_driver.base,
                              (Clock){.now_ms = fake_now_ms, .context = &clock},
                              (MasterValveConfiguration){.pre_open_delay_ms = 5U});
    ZoneService zone_service = make_service(&store, &zone_driver.base, &recorder, &clock, &bus);
    program_service_init(&program_service, &store, &master_service, &zone_service,
                         (Clock){.now_ms = fake_now_ms, .context = &clock});

    assert(program_service_start(&program_service, &program) == IRRIGATION_RESULT_OK);
    assert(program_service_is_active(&program_service) && master_driver.open_calls == 1U);
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    assert(!zone_driver.reported_outputs[0] && !zone_driver.reported_outputs[1]);
    clock.now_ms = 5U;
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    assert(zone_driver.reported_outputs[0] && !zone_driver.reported_outputs[1]);
    clock.now_ms = 15U;
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    assert(!zone_driver.reported_outputs[0] && zone_driver.reported_outputs[1]);
    clock.now_ms = 35U;
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    assert(!zone_driver.reported_outputs[0] && !zone_driver.reported_outputs[1]);
    assert(master_driver.close_calls == 0U);
    clock.now_ms = 39U;
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    assert(master_driver.close_calls == 0U);
    clock.now_ms = 40U;
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    assert(program_service_state(&program_service) == PROGRAM_STATE_COMPLETED);
    assert(!program_service_is_active(&program_service));
    assert(master_driver.open_calls == 1U && master_driver.close_calls == 1U && !master_driver.is_open);
}

static void test_program_zero_durations_complete_deterministically(void)
{
    const ProgramStep steps[] = {{.zone_id = "zone-1", .duration_ms = 0U}};
    const Program program = {.id = "zero", .steps = steps, .step_count = 1U};
    StateStore store;
    FakeClock clock = {0};
    EventRecorder recorder = {0};
    EventBus bus;
    VirtualOutputDriver zone_driver;
    FakeMasterValveDriver master_driver;
    MasterValveService master_service;
    ProgramService program_service;
    virtual_output_driver_init(&zone_driver, (Logger){0});
    fake_master_valve_driver_init(&master_driver);
    initialize_store_with_closed_master_valve(&store);
    master_valve_service_init(&master_service, &store, &master_driver.base,
                              (Clock){.now_ms = fake_now_ms, .context = &clock},
                              (MasterValveConfiguration){0});
    ZoneService zone_service = make_service(&store, &zone_driver.base, &recorder, &clock, &bus);
    program_service_init(&program_service, &store, &master_service, &zone_service,
                         (Clock){.now_ms = fake_now_ms, .context = &clock});

    assert(program_service_start(&program_service, &program) == IRRIGATION_RESULT_OK);
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    assert(zone_driver.reported_outputs[0]);
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    assert(!zone_driver.reported_outputs[0]);
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    assert(program_service_state(&program_service) == PROGRAM_STATE_COMPLETED);
}

static void test_program_faults_on_master_or_zone_failures(void)
{
    const ProgramStep steps[] = {{.zone_id = "zone-1", .duration_ms = 10U},
                                 {.zone_id = "zone-2", .duration_ms = 10U}};
    const Program program = {.id = "failures", .steps = steps, .step_count = 2U};
    StateStore store;
    FakeClock clock = {0};
    EventRecorder recorder = {0};
    EventBus bus;
    FailingOutputDriver zone_driver;
    FakeMasterValveDriver master_driver;
    MasterValveService master_service;
    ProgramService program_service;
    failing_output_driver_init(&zone_driver);
    fake_master_valve_driver_init(&master_driver);
    master_driver.fail_open = true;
    initialize_store_with_closed_master_valve(&store);
    master_valve_service_init(&master_service, &store, &master_driver.base,
                              (Clock){.now_ms = fake_now_ms, .context = &clock},
                              (MasterValveConfiguration){0});
    ZoneService zone_service = make_service(&store, &zone_driver.base, &recorder, &clock, &bus);
    program_service_init(&program_service, &store, &master_service, &zone_service,
                         (Clock){.now_ms = fake_now_ms, .context = &clock});

    assert(program_service_start(&program_service, &program) == IRRIGATION_RESULT_INTERNAL_ERROR);
    assert(program_service_state(&program_service) == PROGRAM_STATE_FAULT);
    assert(!zone_driver.state[0] && !zone_driver.state[1]);

    fake_master_valve_driver_init(&master_driver);
    failing_output_driver_init(&zone_driver);
    initialize_store_with_closed_master_valve(&store);
    master_valve_service_init(&master_service, &store, &master_driver.base,
                              (Clock){.now_ms = fake_now_ms, .context = &clock},
                              (MasterValveConfiguration){0});
    zone_service = make_service(&store, &zone_driver.base, &recorder, &clock, &bus);
    program_service_init(&program_service, &store, &master_service, &zone_service,
                         (Clock){.now_ms = fake_now_ms, .context = &clock});
    zone_driver.fail_after_on = true;
    assert(program_service_start(&program_service, &program) == IRRIGATION_RESULT_OK);
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_INTERNAL_ERROR);
    assert(program_service_state(&program_service) == PROGRAM_STATE_FAULT);
    assert(!zone_driver.state[1]);
    assert(master_driver.close_calls == 1U && !master_driver.is_open);
    assert(master_valve_service_state(&master_service) == MASTER_VALVE_STATE_CLOSED);
    assert(program_service_start(&program_service, &program) == IRRIGATION_RESULT_INVALID_STATE);

    fake_master_valve_driver_init(&master_driver);
    failing_output_driver_init(&zone_driver);
    initialize_store_with_closed_master_valve(&store);
    master_valve_service_init(&master_service, &store, &master_driver.base,
                              (Clock){.now_ms = fake_now_ms, .context = &clock},
                              (MasterValveConfiguration){0});
    zone_service = make_service(&store, &zone_driver.base, &recorder, &clock, &bus);
    program_service_init(&program_service, &store, &master_service, &zone_service,
                         (Clock){.now_ms = fake_now_ms, .context = &clock});
    assert(program_service_start(&program_service, &program) == IRRIGATION_RESULT_OK);
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    zone_driver.fail_after_off_count = 1U;
    clock.now_ms = 10U;
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_INTERNAL_ERROR);
    assert(program_service_state(&program_service) == PROGRAM_STATE_FAULT);
    assert(!zone_driver.state[1]);
}

static void test_program_completes_on_master_close_failure_and_aborts_safely(void)
{
    const ProgramStep steps[] = {{.zone_id = "zone-1", .duration_ms = 10U},
                                 {.zone_id = "zone-2", .duration_ms = 10U}};
    const Program program = {.id = "abort", .steps = steps, .step_count = 2U};
    StateStore store;
    FakeClock clock = {0};
    EventRecorder recorder = {0};
    EventBus bus;
    VirtualOutputDriver zone_driver;
    FakeMasterValveDriver master_driver;
    MasterValveService master_service;
    ProgramService program_service;
    virtual_output_driver_init(&zone_driver, (Logger){0});
    fake_master_valve_driver_init(&master_driver);
    initialize_store_with_closed_master_valve(&store);
    master_valve_service_init(&master_service, &store, &master_driver.base,
                              (Clock){.now_ms = fake_now_ms, .context = &clock},
                              (MasterValveConfiguration){0});
    ZoneService zone_service = make_service(&store, &zone_driver.base, &recorder, &clock, &bus);
    program_service_init(&program_service, &store, &master_service, &zone_service,
                         (Clock){.now_ms = fake_now_ms, .context = &clock});

    assert(program_service_start(&program_service, &program) == IRRIGATION_RESULT_OK);
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    clock.now_ms = 10U;
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    master_driver.fail_close = true;
    clock.now_ms = 20U;
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_INTERNAL_ERROR);
    assert(program_service_state(&program_service) == PROGRAM_STATE_COMPLETED);
    assert(!program_service_is_active(&program_service));
    assert(master_valve_service_state(&master_service) == MASTER_VALVE_STATE_FAULT);

    master_driver.fail_close = false;
    assert(program_service_start(&program_service, &program) == IRRIGATION_RESULT_OK);
    assert(master_driver.open_calls == 2U);
    assert(master_valve_service_state(&master_service) == MASTER_VALVE_STATE_OPENING);
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    assert(zone_driver.reported_outputs[0]);

    virtual_output_driver_init(&zone_driver, (Logger){0});
    fake_master_valve_driver_init(&master_driver);
    initialize_store_with_closed_master_valve(&store);
    master_valve_service_init(&master_service, &store, &master_driver.base,
                              (Clock){.now_ms = fake_now_ms, .context = &clock},
                              (MasterValveConfiguration){0});
    zone_service = make_service(&store, &zone_driver.base, &recorder, &clock, &bus);
    program_service_init(&program_service, &store, &master_service, &zone_service,
                         (Clock){.now_ms = fake_now_ms, .context = &clock});
    clock.now_ms = 0U;
    assert(program_service_start(&program_service, &program) == IRRIGATION_RESULT_OK);
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    assert(zone_driver.reported_outputs[0]);
    assert(program_service_abort(&program_service) == IRRIGATION_RESULT_OK);
    assert(program_service_state(&program_service) == PROGRAM_STATE_ABORTED);
    assert(!zone_driver.reported_outputs[0] && !zone_driver.reported_outputs[1]);
    assert(master_driver.close_calls == 1U && !master_driver.is_open);

    virtual_output_driver_init(&zone_driver, (Logger){0});
    fake_master_valve_driver_init(&master_driver);
    initialize_store_with_closed_master_valve(&store);
    master_valve_service_init(&master_service, &store, &master_driver.base,
                              (Clock){.now_ms = fake_now_ms, .context = &clock},
                              (MasterValveConfiguration){.pre_open_delay_ms = 20U});
    zone_service = make_service(&store, &zone_driver.base, &recorder, &clock, &bus);
    program_service_init(&program_service, &store, &master_service, &zone_service,
                         (Clock){.now_ms = fake_now_ms, .context = &clock});
    assert(program_service_start(&program_service, &program) == IRRIGATION_RESULT_OK);
    assert(program_service_abort(&program_service) == IRRIGATION_RESULT_OK);
    assert(program_service_state(&program_service) == PROGRAM_STATE_ABORTED);
    assert(!zone_driver.reported_outputs[0] && !zone_driver.reported_outputs[1]);
    assert(master_driver.close_calls == 1U && !master_driver.is_open);
}

static void test_program_execution_history_lifecycle(void)
{
    const ProgramStep steps[] = {{.zone_id = "zone-1", .duration_ms = 10U}};
    const Program program = {.id = "history-program", .steps = steps, .step_count = 1U,
                             .post_program_delay_ms = 5U};
    StateStore store;
    FakeClock clock = {.now_ms = 100U};
    EventRecorder recorder = {0};
    EventBus bus;
    VirtualOutputDriver zone_driver;
    FakeMasterValveDriver master_driver;
    MasterValveService master_service;
    ProgramService program_service;
    ProgramExecutionHistory history;

    virtual_output_driver_init(&zone_driver, (Logger){0});
    fake_master_valve_driver_init(&master_driver);
    initialize_store_with_closed_master_valve(&store);
    master_valve_service_init(&master_service, &store, &master_driver.base,
                              (Clock){.now_ms = fake_now_ms, .context = &clock},
                              (MasterValveConfiguration){0});
    ZoneService zone_service = make_service(&store, &zone_driver.base, &recorder, &clock, &bus);
    program_service_init(&program_service, &store, &master_service, &zone_service,
                         (Clock){.now_ms = fake_now_ms, .context = &clock});
    program_execution_history_init(&history, (ProgramExecutionHistoryRepository){0});
    assert(program_execution_history_subscribe(&history, &bus) == IRRIGATION_RESULT_OK);
    program_service_set_event_bus(&program_service, &bus);

    assert(program_service_start(&program_service, &program) == IRRIGATION_RESULT_OK);
    assert(program_execution_history_count(&history) == 0U);
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    clock.now_ms = 110U;
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    assert(program_execution_history_count(&history) == 0U);
    clock.now_ms = 115U;
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    assert(master_driver.close_calls == 1U && program_execution_history_count(&history) == 1U);
    const ProgramExecutionHistoryRecord *record = program_execution_history_record_at(&history, 0U);
    assert(record != NULL && strcmp(record->program_id, "history-program") == 0);
    assert(record->origin == PROGRAM_EXECUTION_ORIGIN_MANUAL && !record->schedule_id_valid);
    assert(record->started_at.valid && record->started_at.value_ms == 100U);
    assert(record->ended_at.valid && record->ended_at.value_ms == 115U);
    assert(record->planned_duration_ms == 15U && record->actual_duration_ms == 15U);
    assert(record->result == PROGRAM_EXECUTION_RESULT_COMPLETED);
    assert(record->steps_started == 1U && record->steps_completed == 1U);
    assert(record->last_active_zone_id_valid && strcmp(record->last_active_zone_id, "zone-1") == 0);

    /* A scheduled start preserves the occurrence flow while carrying its schedule identity. */
    state_store_init(&store);
    assert(state_store_configure_zones(&store, ZONES, sizeof(ZONES) / sizeof(ZONES[0])) == IRRIGATION_RESULT_OK);
    assert(state_store_transition_system(&store, SYSTEM_STATE_READY) == IRRIGATION_RESULT_OK);
    fake_master_valve_driver_init(&master_driver);
    master_valve_service_init(&master_service, &store, &master_driver.base,
                              (Clock){.now_ms = fake_now_ms, .context = &clock}, (MasterValveConfiguration){0});
    zone_service = make_service(&store, &zone_driver.base, &recorder, &clock, &bus);
    assert(program_execution_history_subscribe(&history, &bus) == IRRIGATION_RESULT_OK);
    program_service_init(&program_service, &store, &master_service, &zone_service,
                         (Clock){.now_ms = fake_now_ms, .context = &clock});
    program_service_set_event_bus(&program_service, &bus);
    SchedulerService scheduler;
    scheduler_service_init(&scheduler, &store, &program_service);
    const SchedulerEntry entry = {.id = "history-schedule", .enabled = true,
                                  .weekday_mask = SCHEDULER_WEEKDAY_MASK(1U), .hour = 6U,
                                  .minute = 0U, .program = &program};
    assert(scheduler_service_configure(&scheduler, &entry, 1U) == IRRIGATION_RESULT_OK);
    clock.now_ms = 200U;
    assert(scheduler_service_process(&scheduler, (SchedulerTime){.week_index = 3U, .weekday = 1U, .hour = 6U}) ==
           IRRIGATION_RESULT_OK);
    assert(state_store_snapshot(&store).scheduler.last_occurrence_status[0] == SCHEDULE_OCCURRENCE_STARTED);
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    clock.now_ms = 210U;
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    clock.now_ms = 215U;
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    record = program_execution_history_record_at(&history, 1U);
    assert(record != NULL && record->origin == PROGRAM_EXECUTION_ORIGIN_SCHEDULED && record->schedule_id_valid);
    assert(strcmp(record->schedule_id, "history-schedule") == 0 &&
           record->result == PROGRAM_EXECUTION_RESULT_COMPLETED);

    assert(program_service_start(&program_service, &program) == IRRIGATION_RESULT_OK);
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    assert(program_service_abort(&program_service) == IRRIGATION_RESULT_OK);
    record = program_execution_history_record_at(&history, 2U);
    assert(record != NULL && record->result == PROGRAM_EXECUTION_RESULT_ABORTED &&
           record->steps_started == 1U && record->steps_completed == 0U);

    state_store_init(&store);
    assert(state_store_configure_zones(&store, ZONES, sizeof(ZONES) / sizeof(ZONES[0])) == IRRIGATION_RESULT_OK);
    assert(state_store_transition_system(&store, SYSTEM_STATE_READY) == IRRIGATION_RESULT_OK);
    fake_master_valve_driver_init(&master_driver);
    master_driver.fail_open = true;
    master_valve_service_init(&master_service, &store, &master_driver.base,
                              (Clock){.now_ms = fake_now_ms, .context = &clock}, (MasterValveConfiguration){0});
    zone_service = make_service(&store, &zone_driver.base, &recorder, &clock, &bus);
    assert(program_execution_history_subscribe(&history, &bus) == IRRIGATION_RESULT_OK);
    program_service_init(&program_service, &store, &master_service, &zone_service,
                         (Clock){.now_ms = fake_now_ms, .context = &clock});
    program_service_set_event_bus(&program_service, &bus);
    assert(program_service_start(&program_service, &program) == IRRIGATION_RESULT_INTERNAL_ERROR);
    record = program_execution_history_record_at(&history, 3U);
    assert(record != NULL && record->result == PROGRAM_EXECUTION_RESULT_FAULT);

    state_store_init(&store);
    assert(state_store_configure_zones(&store, ZONES, sizeof(ZONES) / sizeof(ZONES[0])) == IRRIGATION_RESULT_OK);
    assert(state_store_transition_system(&store, SYSTEM_STATE_READY) == IRRIGATION_RESULT_OK);
    fake_master_valve_driver_init(&master_driver);
    master_valve_service_init(&master_service, &store, &master_driver.base,
                              (Clock){.now_ms = fake_now_ms, .context = &clock}, (MasterValveConfiguration){0});
    zone_service = make_service(&store, &zone_driver.base, &recorder, &clock, &bus);
    assert(program_execution_history_subscribe(&history, &bus) == IRRIGATION_RESULT_OK);
    program_service_init(&program_service, &store, &master_service, &zone_service,
                         (Clock){.now_ms = fake_now_ms, .context = &clock});
    program_service_set_event_bus(&program_service, &bus);
    clock.now_ms = 300U;
    assert(program_service_start(&program_service, &program) == IRRIGATION_RESULT_OK);
    assert(program_execution_history_interrupt_active(&history, 320U, true) == IRRIGATION_RESULT_OK);
    record = program_execution_history_record_at(&history, 4U);
    assert(record != NULL && record->result == PROGRAM_EXECUTION_RESULT_INTERRUPTED_REBOOT &&
           record->actual_duration_ms == 20U);
    state_store_init(&store);
    assert(state_store_snapshot(&store).program.state == PROGRAM_STATE_IDLE);
}

static void complete_zero_duration_program(ProgramService *service)
{
    assert(program_service_process(service) == IRRIGATION_RESULT_OK);
    assert(program_service_process(service) == IRRIGATION_RESULT_OK);
    assert(program_service_process(service) == IRRIGATION_RESULT_OK);
    assert(program_service_state(service) == PROGRAM_STATE_COMPLETED);
}

static void test_scheduler_does_not_catch_up_or_replay_unchanged_entries(void)
{
    const ProgramStep steps[] = {{.zone_id = "zone-1", .duration_ms = 0U}};
    const Program program = {.id = "program", .steps = steps, .step_count = 1U};
    const SchedulerEntry entry_a[] = {{.id = "schedule-a", .enabled = true,
                                        .weekday_mask = SCHEDULER_WEEKDAY_MASK(2U), .hour = 6U,
                                        .minute = 0U, .program = &program}};
    const SchedulerEntry entries_ab[] = {
        {.id = "schedule-a", .enabled = true, .weekday_mask = SCHEDULER_WEEKDAY_MASK(2U), .hour = 6U,
         .minute = 0U, .program = &program},
        {.id = "schedule-b", .enabled = false, .weekday_mask = SCHEDULER_WEEKDAY_MASK(3U), .hour = 7U,
         .minute = 0U, .program = &program},
    };
    StateStore store;
    FakeClock clock = {0};
    EventRecorder recorder = {0};
    EventBus bus;
    VirtualOutputDriver zone_driver;
    FakeMasterValveDriver master_driver;
    MasterValveService master_service;
    ProgramService program_service;
    SchedulerService scheduler;
    virtual_output_driver_init(&zone_driver, (Logger){0});
    fake_master_valve_driver_init(&master_driver);
    initialize_store_with_closed_master_valve(&store);
    master_valve_service_init(&master_service, &store, &master_driver.base,
                              (Clock){.now_ms = fake_now_ms, .context = &clock},
                              (MasterValveConfiguration){0});
    ZoneService zone_service = make_service(&store, &zone_driver.base, &recorder, &clock, &bus);
    program_service_init(&program_service, &store, &master_service, &zone_service,
                         (Clock){.now_ms = fake_now_ms, .context = &clock});
    scheduler_service_init(&scheduler, &store, &program_service);

    assert(scheduler_service_configure(&scheduler, entry_a, 1U) == IRRIGATION_RESULT_OK);
    assert(scheduler_service_process(&scheduler, (SchedulerTime){.week_index = 1U, .weekday = 2U,
                                                                  .hour = 16U, .minute = 5U}) ==
           IRRIGATION_RESULT_OK);
    assert(master_driver.open_calls == 0U);
    assert(scheduler_service_process(&scheduler, (SchedulerTime){.week_index = 2U, .weekday = 2U,
                                                                  .hour = 6U}) == IRRIGATION_RESULT_OK);
    assert(master_driver.open_calls == 1U);
    complete_zero_duration_program(&program_service);

    assert(scheduler_service_configure(&scheduler, entries_ab, 2U) == IRRIGATION_RESULT_OK);
    assert(scheduler_service_process(&scheduler, (SchedulerTime){.week_index = 2U, .weekday = 2U,
                                                                  .hour = 6U}) == IRRIGATION_RESULT_OK);
    assert(master_driver.open_calls == 1U);
    assert(scheduler_service_set_enabled(&scheduler, 1U, true) == IRRIGATION_RESULT_OK);
    assert(scheduler_service_process(&scheduler, (SchedulerTime){.week_index = 2U, .weekday = 2U,
                                                                  .hour = 6U}) == IRRIGATION_RESULT_OK);
    assert(master_driver.open_calls == 1U);
    assert(scheduler_service_configure(&scheduler, entry_a, 1U) == IRRIGATION_RESULT_OK);
    assert(scheduler_service_process(&scheduler, (SchedulerTime){.week_index = 2U, .weekday = 2U,
                                                                  .hour = 6U}) == IRRIGATION_RESULT_OK);
    assert(master_driver.open_calls == 1U);
}

static void test_scheduler_recurs_without_duplicate_starts(void)
{
    const ProgramStep steps_a[] = {{.zone_id = "zone-1", .duration_ms = 0U}};
    const ProgramStep steps_b[] = {{.zone_id = "zone-2", .duration_ms = 0U}};
    const Program program_a = {.id = "program-a", .steps = steps_a, .step_count = 1U};
    const Program program_b = {.id = "program-b", .steps = steps_b, .step_count = 1U};
    const SchedulerEntry disabled[] = {{.id = "disabled", .enabled = false,
                                        .weekday_mask = SCHEDULER_WEEKDAY_MASK(1U), .hour = 6U,
                                        .minute = 0U, .program = &program_a}};
    const SchedulerEntry entries[] = {
        {.id = "morning", .enabled = true, .weekday_mask = SCHEDULER_WEEKDAY_MASK(1U), .hour = 6U,
         .minute = 0U, .program = &program_a},
        {.id = "evening", .enabled = true, .weekday_mask = SCHEDULER_WEEKDAY_MASK(1U), .hour = 7U,
         .minute = 0U, .program = &program_b},
    };
    StateStore store;
    FakeClock clock = {0};
    EventRecorder recorder = {0};
    EventBus bus;
    VirtualOutputDriver zone_driver;
    FakeMasterValveDriver master_driver;
    MasterValveService master_service;
    ProgramService program_service;
    SchedulerService scheduler;
    virtual_output_driver_init(&zone_driver, (Logger){0});
    fake_master_valve_driver_init(&master_driver);
    initialize_store_with_closed_master_valve(&store);
    master_valve_service_init(&master_service, &store, &master_driver.base,
                              (Clock){.now_ms = fake_now_ms, .context = &clock},
                              (MasterValveConfiguration){0});
    ZoneService zone_service = make_service(&store, &zone_driver.base, &recorder, &clock, &bus);
    program_service_init(&program_service, &store, &master_service, &zone_service,
                         (Clock){.now_ms = fake_now_ms, .context = &clock});
    scheduler_service_init(&scheduler, &store, &program_service);

    assert(scheduler_service_configure(&scheduler, disabled, 1U) == IRRIGATION_RESULT_OK);
    assert(scheduler_service_process(&scheduler, (SchedulerTime){.weekday = 1U, .hour = 6U}) ==
           IRRIGATION_RESULT_OK);
    assert(master_driver.open_calls == 0U);

    assert(scheduler_service_configure(&scheduler, entries, 2U) == IRRIGATION_RESULT_OK);
    assert(scheduler_service_process(&scheduler, (SchedulerTime){.weekday = 1U, .hour = 6U}) ==
           IRRIGATION_RESULT_OK);
    assert(master_driver.open_calls == 1U);
    assert(state_store_snapshot(&store).scheduler.last_occurrence_status[0] == SCHEDULE_OCCURRENCE_STARTED);
    assert(scheduler_service_process(&scheduler, (SchedulerTime){.weekday = 1U, .hour = 6U, .minute = 30U}) ==
           IRRIGATION_RESULT_OK);
    assert(master_driver.open_calls == 1U);
    complete_zero_duration_program(&program_service);

    assert(scheduler_service_process(&scheduler, (SchedulerTime){.weekday = 1U, .hour = 7U}) ==
           IRRIGATION_RESULT_OK);
    assert(master_driver.open_calls == 2U);
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    assert(!zone_driver.reported_outputs[0] && zone_driver.reported_outputs[1]);
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    assert(program_service_process(&program_service) == IRRIGATION_RESULT_OK);
    assert(program_service_state(&program_service) == PROGRAM_STATE_COMPLETED);

    assert(scheduler_service_process(&scheduler, (SchedulerTime){.week_index = 1U, .weekday = 1U, .hour = 6U}) ==
           IRRIGATION_RESULT_OK);
    assert(master_driver.open_calls == 3U);
}

static void test_scheduler_skips_busy_and_rejected_occurrences(void)
{
    const ProgramStep long_steps[] = {{.zone_id = "zone-1", .duration_ms = 100U}};
    const Program long_program = {.id = "long", .steps = long_steps, .step_count = 1U};
    const Program invalid_program = {.id = "invalid", .steps = NULL, .step_count = 0U};
    const SchedulerEntry entries[] = {
        {.id = "first", .enabled = true, .weekday_mask = SCHEDULER_WEEKDAY_MASK(2U), .hour = 6U,
         .program = &long_program},
        {.id = "busy", .enabled = true, .weekday_mask = SCHEDULER_WEEKDAY_MASK(2U), .hour = 7U,
         .program = &long_program},
    };
    const SchedulerEntry invalid_entry[] = {{.id = "invalid", .enabled = true,
                                              .weekday_mask = SCHEDULER_WEEKDAY_MASK(2U), .hour = 8U,
                                              .program = &invalid_program}};
    StateStore store;
    FakeClock clock = {0};
    EventRecorder recorder = {0};
    EventBus bus;
    VirtualOutputDriver zone_driver;
    FakeMasterValveDriver master_driver;
    MasterValveService master_service;
    ProgramService program_service;
    SchedulerService scheduler;
    virtual_output_driver_init(&zone_driver, (Logger){0});
    fake_master_valve_driver_init(&master_driver);
    initialize_store_with_closed_master_valve(&store);
    master_valve_service_init(&master_service, &store, &master_driver.base,
                              (Clock){.now_ms = fake_now_ms, .context = &clock},
                              (MasterValveConfiguration){0});
    ZoneService zone_service = make_service(&store, &zone_driver.base, &recorder, &clock, &bus);
    program_service_init(&program_service, &store, &master_service, &zone_service,
                         (Clock){.now_ms = fake_now_ms, .context = &clock});
    scheduler_service_init(&scheduler, &store, &program_service);

    assert(scheduler_service_configure(&scheduler, entries, 2U) == IRRIGATION_RESULT_OK);
    assert(scheduler_service_process(&scheduler, (SchedulerTime){.weekday = 2U, .hour = 6U}) ==
           IRRIGATION_RESULT_OK);
    assert(scheduler_service_process(&scheduler, (SchedulerTime){.weekday = 2U, .hour = 7U}) ==
           IRRIGATION_RESULT_OK);
    assert(state_store_snapshot(&store).scheduler.last_occurrence_status[1] == SCHEDULE_OCCURRENCE_SKIPPED_BUSY);
    assert(master_driver.open_calls == 1U);

    assert(program_service_abort(&program_service) == IRRIGATION_RESULT_OK);
    assert(scheduler_service_configure(&scheduler, invalid_entry, 1U) == IRRIGATION_RESULT_OK);
    assert(scheduler_service_process(&scheduler, (SchedulerTime){.weekday = 2U, .hour = 8U}) ==
           IRRIGATION_RESULT_OK);
    assert(state_store_snapshot(&store).scheduler.last_occurrence_status[0] == SCHEDULE_OCCURRENCE_REJECTED);
    assert(scheduler_service_process(&scheduler, (SchedulerTime){.weekday = 2U, .hour = 8U, .minute = 30U}) ==
           IRRIGATION_RESULT_OK);
    assert(master_driver.open_calls == 1U);
}

static void test_scheduler_handles_midnight_rollover(void)
{
    const ProgramStep steps[] = {{.zone_id = "zone-1", .duration_ms = 0U}};
    const Program program = {.id = "midnight", .steps = steps, .step_count = 1U};
    const SchedulerEntry entry[] = {{.id = "midnight", .enabled = true,
                                      .weekday_mask = SCHEDULER_WEEKDAY_MASK(3U), .hour = 0U,
                                      .minute = 0U, .program = &program}};
    StateStore store;
    FakeClock clock = {0};
    EventRecorder recorder = {0};
    EventBus bus;
    VirtualOutputDriver zone_driver;
    FakeMasterValveDriver master_driver;
    MasterValveService master_service;
    ProgramService program_service;
    SchedulerService scheduler;
    virtual_output_driver_init(&zone_driver, (Logger){0});
    fake_master_valve_driver_init(&master_driver);
    initialize_store_with_closed_master_valve(&store);
    master_valve_service_init(&master_service, &store, &master_driver.base,
                              (Clock){.now_ms = fake_now_ms, .context = &clock},
                              (MasterValveConfiguration){0});
    ZoneService zone_service = make_service(&store, &zone_driver.base, &recorder, &clock, &bus);
    program_service_init(&program_service, &store, &master_service, &zone_service,
                         (Clock){.now_ms = fake_now_ms, .context = &clock});
    scheduler_service_init(&scheduler, &store, &program_service);
    assert(scheduler_service_configure(&scheduler, entry, 1U) == IRRIGATION_RESULT_OK);

    assert(scheduler_service_process(&scheduler, (SchedulerTime){.weekday = 2U, .hour = 23U, .minute = 59U}) ==
           IRRIGATION_RESULT_OK);
    assert(master_driver.open_calls == 0U);
    assert(scheduler_service_process(&scheduler, (SchedulerTime){.weekday = 3U}) == IRRIGATION_RESULT_OK);
    assert(master_driver.open_calls == 1U);
}

int main(void)
{
    test_irrigation_configuration_validation();
    test_configuration_repository();
    test_configuration_manager_persistence();
    test_configuration_manager_persistence_boot_fallbacks();
    test_configuration_manager_mutations();
    test_dev_kit_zone_names();
    test_status_led_service_priority_and_transitions();
    test_shelly_master_valve_driver();
    test_configuration_manager_preserves_scheduler_occurrence();
    test_configuration_manager_scheduler_reconfiguration_failure_rolls_back();
    test_open_close_and_authority();
    test_invalid_and_forbidden_transitions();
    test_failed_on_safe_close_and_start_lockout();
    test_failed_off_safe_close_retry();
    test_reentrant_open_cannot_actuate_second_zone();
    test_timeout_and_command_flow();
    test_master_valve_zone_invariants_and_pre_open_delay();
    test_master_valve_confirmation_failure_blocks_zone_actuation();
    test_master_valve_close_failure_blocks_zone_actuation();
    test_unknown_output_blocks_master_valve_close_and_zone_start();
    test_program_rejects_empty_program();
    test_program_single_and_multi_zone_sequences();
    test_program_zero_durations_complete_deterministically();
    test_program_faults_on_master_or_zone_failures();
    test_program_completes_on_master_close_failure_and_aborts_safely();
    test_program_execution_history_lifecycle();
    test_scheduler_does_not_catch_up_or_replay_unchanged_entries();
    test_scheduler_recurs_without_duplicate_starts();
    test_scheduler_skips_busy_and_rejected_occurrences();
    test_scheduler_handles_midnight_rollover();
    puts("All host unit tests passed.");
    return 0;
}
