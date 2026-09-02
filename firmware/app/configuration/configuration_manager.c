#include "app/configuration/configuration_manager.h"

#include <stdio.h>
#include <string.h>

static void copy_text(char *destination, size_t destination_size, const char *source)
{
    (void)snprintf(destination, destination_size, "%s", source);
}

static int program_index_for_id(const IrrigationConfiguration *configuration, const char *program_id)
{
    for (size_t index = 0U; index < configuration->program_count; ++index) {
        if (strcmp(configuration->programs[index].program_id, program_id) == 0) return (int)index;
    }
    return -1;
}

static int program_slot_for_id(const ConfigurationManager *manager, const char *program_id)
{
    for (size_t index = 0U; index < IRRIGATION_MAX_PROGRAMS; ++index) {
        if (manager->runtime_program_slots[index] && strcmp(manager->runtime_program_ids[index], program_id) == 0) return (int)index;
    }
    return -1;
}

static int schedule_slot_for_id(const ConfigurationManager *manager, const char *schedule_id)
{
    for (size_t index = 0U; index < IRRIGATION_MAX_SCHEDULES; ++index) {
        if (manager->runtime_schedule_slots[index] && strcmp(manager->runtime_schedule_ids[index], schedule_id) == 0) return (int)index;
    }
    return -1;
}

static int unused_slot(const bool *slots, size_t count)
{
    for (size_t index = 0U; index < count; ++index) if (!slots[index]) return (int)index;
    return -1;
}

static bool programs_are_equal(const IrrigationConfiguration *left,
                               const IrrigationConfiguration *right)
{
    if (left->program_count != right->program_count) return false;
    for (size_t index = 0U; index < left->program_count; ++index) {
        const IrrigationProgramConfiguration *a = &left->programs[index];
        const IrrigationProgramConfiguration *b = &right->programs[index];
        if (strcmp(a->program_id, b->program_id) != 0 || strcmp(a->display_name, b->display_name) != 0 ||
            a->step_count != b->step_count) return false;
        for (size_t step = 0U; step < a->step_count; ++step) {
            if (strcmp(a->steps[step].zone_id, b->steps[step].zone_id) != 0 ||
                a->steps[step].duration_ms != b->steps[step].duration_ms) return false;
        }
    }
    return true;
}

static void rebuild_runtime_views(ConfigurationManager *manager,
                                  const IrrigationConfiguration *configuration,
                                  bool rebuild_programs)
{
    bool program_slots_in_use[IRRIGATION_MAX_PROGRAMS] = {false};
    bool schedule_slots_in_use[IRRIGATION_MAX_SCHEDULES] = {false};
    if (rebuild_programs) {
        for (size_t program_index = 0U; program_index < configuration->program_count; ++program_index) {
            const IrrigationProgramConfiguration *configured = &configuration->programs[program_index];
            int slot = program_slot_for_id(manager, configured->program_id);
            if (slot < 0) slot = unused_slot(manager->runtime_program_slots, IRRIGATION_MAX_PROGRAMS);
            if (slot < 0) return;
            program_slots_in_use[slot] = true;
            manager->runtime_program_slots[slot] = true;
            copy_text(manager->runtime_program_ids[slot], sizeof(manager->runtime_program_ids[slot]), configured->program_id);
            copy_text(manager->runtime_program_names[slot], sizeof(manager->runtime_program_names[slot]), configured->display_name);
            for (size_t step_index = 0U; step_index < configured->step_count; ++step_index) {
                copy_text(manager->runtime_step_zone_ids[slot][step_index], sizeof(manager->runtime_step_zone_ids[slot][step_index]), configured->steps[step_index].zone_id);
                manager->runtime_program_steps[slot][step_index] = (ProgramStep){
                    .zone_id = manager->runtime_step_zone_ids[slot][step_index],
                    .duration_ms = configured->steps[step_index].duration_ms,
                };
            }
            manager->runtime_programs[slot] = (Program){
                .id = manager->runtime_program_names[slot], .steps = manager->runtime_program_steps[slot],
                .step_count = configured->step_count,
            };
        }
        for (size_t index = 0U; index < IRRIGATION_MAX_PROGRAMS; ++index) manager->runtime_program_slots[index] = program_slots_in_use[index];
    }

    for (size_t schedule_index = 0U; schedule_index < configuration->schedule_count; ++schedule_index) {
        const IrrigationScheduleConfiguration *configured = &configuration->schedules[schedule_index];
        const int program_index = program_index_for_id(configuration, configured->program_id);
        int slot = schedule_slot_for_id(manager, configured->schedule_id);
        if (slot < 0) slot = unused_slot(manager->runtime_schedule_slots, IRRIGATION_MAX_SCHEDULES);
        if (slot < 0 || program_index < 0) return;
        const int program_slot = program_slot_for_id(manager, configuration->programs[program_index].program_id);
        if (program_slot < 0) return;
        schedule_slots_in_use[slot] = true;
        manager->runtime_schedule_slots[slot] = true;
        copy_text(manager->runtime_schedule_ids[slot], sizeof(manager->runtime_schedule_ids[slot]), configured->schedule_id);
        manager->runtime_schedules[schedule_index] = (SchedulerEntry){
            .id = manager->runtime_schedule_ids[slot], .enabled = configured->enabled,
            .weekday_mask = configured->weekday_mask, .hour = configured->hour, .minute = configured->minute,
            .program = &manager->runtime_programs[program_slot],
        };
    }
    for (size_t index = 0U; index < IRRIGATION_MAX_SCHEDULES; ++index) manager->runtime_schedule_slots[index] = schedule_slots_in_use[index];
}

static IrrigationResult replace_candidate(ConfigurationManager *manager)
{
    IrrigationResult result = configuration_manager_validate_candidate(manager, &manager->candidate);
    if (result != IRRIGATION_RESULT_OK) {
        return result;
    }
    const bool programs_changed = !programs_are_equal(&manager->configuration, &manager->candidate);
    if (manager->scheduler_service != NULL) {
        scheduler_service_lock(manager->scheduler_service);
        rebuild_runtime_views(manager, &manager->candidate, programs_changed);
        result = scheduler_service_configure_locked(manager->scheduler_service, manager->runtime_schedules,
                                                    manager->candidate.schedule_count);
        if (result != IRRIGATION_RESULT_OK) {
            rebuild_runtime_views(manager, &manager->configuration, programs_changed);
            const IrrigationResult rollback = scheduler_service_configure_locked(
                manager->scheduler_service, manager->runtime_schedules,
                manager->configuration.schedule_count);
            scheduler_service_unlock(manager->scheduler_service);
            return rollback == IRRIGATION_RESULT_OK ? result : IRRIGATION_RESULT_INTERNAL_ERROR;
        }
    }
    if (manager->repository == NULL) {
        if (manager->persistence_required) result = IRRIGATION_RESULT_INTERNAL_ERROR;
    } else if (configuration_repository_save(manager->repository, &manager->candidate,
                                             manager->board_zones, manager->board_zone_count) !=
               CONFIGURATION_REPOSITORY_OK) result = IRRIGATION_RESULT_INTERNAL_ERROR;
    if (result != IRRIGATION_RESULT_OK) {
        if (manager->scheduler_service != NULL) {
            rebuild_runtime_views(manager, &manager->configuration, programs_changed);
            const IrrigationResult rollback = scheduler_service_configure_locked(
                manager->scheduler_service, manager->runtime_schedules,
                manager->configuration.schedule_count);
            scheduler_service_unlock(manager->scheduler_service);
            if (rollback != IRRIGATION_RESULT_OK) return IRRIGATION_RESULT_INTERNAL_ERROR;
        }
        return result;
    }
    manager->configuration = manager->candidate;
    if (manager->scheduler_service == NULL) {
        rebuild_runtime_views(manager, &manager->configuration, programs_changed);
    } else {
        scheduler_service_unlock(manager->scheduler_service);
    }
    return IRRIGATION_RESULT_OK;
}

IrrigationResult configuration_manager_init_dev_kit_defaults(ConfigurationManager *manager, const Zone *board_zones, size_t board_zone_count)
{
    if (manager == NULL || board_zones == NULL || board_zone_count < 2U || board_zone_count > IRRIGATION_MAX_ZONES) return IRRIGATION_RESULT_REJECTED;
    *manager = (ConfigurationManager){.board_zones = board_zones, .board_zone_count = board_zone_count};
    IrrigationConfiguration *configuration = &manager->configuration;
    configuration->schema_version = IRRIGATION_CONFIGURATION_SCHEMA_VERSION;
    configuration->zone_count = (uint8_t)board_zone_count;
    configuration->next_program_id = 1U;
    configuration->next_schedule_id = 1U;
    for (size_t index = 0U; index < board_zone_count; ++index) {
        copy_text(configuration->zones[index].zone_id, sizeof(configuration->zones[index].zone_id), board_zones[index].id);
        (void)snprintf(configuration->zones[index].display_name, sizeof(configuration->zones[index].display_name), "Zone %u", (unsigned)(index + 1U));
    }
    configuration->program_count = 1U;
    IrrigationProgramConfiguration *program = &configuration->programs[0];
    copy_text(program->program_id, sizeof(program->program_id), "dev-program");
    copy_text(program->display_name, sizeof(program->display_name), "dev-program");
    program->step_count = 2U;
    for (size_t index = 0U; index < program->step_count; ++index) {
        copy_text(program->steps[index].zone_id, sizeof(program->steps[index].zone_id), board_zones[index].id);
        program->steps[index].duration_ms = 30000U;
    }
    configuration->schedule_count = 1U;
    IrrigationScheduleConfiguration *schedule = &configuration->schedules[0];
    copy_text(schedule->schedule_id, sizeof(schedule->schedule_id), "dev-monday-0600");
    schedule->enabled = false; schedule->weekday_mask = 1U; schedule->hour = 6U; schedule->minute = 0U;
    copy_text(schedule->program_id, sizeof(schedule->program_id), program->program_id);
    IrrigationResult result = irrigation_configuration_validate(configuration, board_zones, board_zone_count);
    if (result == IRRIGATION_RESULT_OK) rebuild_runtime_views(manager, configuration, true);
    return result;
}

const IrrigationConfiguration *configuration_manager_get(const ConfigurationManager *manager) { return manager == NULL ? NULL : &manager->configuration; }

void configuration_manager_set_repository(ConfigurationManager *manager,
                                          ConfigurationRepository *repository,
                                          bool persistence_required)
{
    if (manager == NULL) return;
    manager->repository = repository;
    manager->persistence_required = persistence_required;
}

void configuration_manager_bind_scheduler(ConfigurationManager *manager,
                                          SchedulerService *scheduler_service)
{
    if (manager != NULL) manager->scheduler_service = scheduler_service;
}

ConfigurationManagerBootResult configuration_manager_load_or_persist_defaults(
    ConfigurationManager *manager, ConfigurationRepository *repository)
{
    if (manager == NULL || repository == NULL) {
        return CONFIGURATION_MANAGER_BOOT_DEFAULTS_STORAGE_ERROR;
    }
    uint32_t generation;
    const ConfigurationRepositoryResult result = configuration_repository_load(
        repository, &manager->candidate, &generation, manager->board_zones, manager->board_zone_count);
    if (result == CONFIGURATION_REPOSITORY_OK) {
        manager->configuration = manager->candidate;
        rebuild_runtime_views(manager, &manager->configuration, true);
        return CONFIGURATION_MANAGER_BOOT_LOADED;
    }
    if (result == CONFIGURATION_REPOSITORY_NOT_FOUND) {
        return configuration_repository_save(repository, &manager->configuration, manager->board_zones,
                                             manager->board_zone_count) == CONFIGURATION_REPOSITORY_OK
                   ? CONFIGURATION_MANAGER_BOOT_DEFAULTS_PERSISTED
                   : CONFIGURATION_MANAGER_BOOT_DEFAULTS_STORAGE_ERROR;
    }
    return result == CONFIGURATION_REPOSITORY_INCOMPATIBLE
               ? CONFIGURATION_MANAGER_BOOT_DEFAULTS_INCOMPATIBLE
               : (result == CONFIGURATION_REPOSITORY_STORAGE_ERROR
                      ? CONFIGURATION_MANAGER_BOOT_DEFAULTS_STORAGE_ERROR
                      : CONFIGURATION_MANAGER_BOOT_DEFAULTS_CORRUPT);
}

IrrigationResult configuration_manager_validate_candidate(const ConfigurationManager *manager, const IrrigationConfiguration *candidate)
{
    return manager == NULL ? IRRIGATION_RESULT_REJECTED : irrigation_configuration_validate(candidate, manager->board_zones, manager->board_zone_count);
}

IrrigationResult configuration_manager_replace(ConfigurationManager *manager, const IrrigationConfiguration *candidate)
{
    if (manager == NULL || candidate == NULL) return IRRIGATION_RESULT_REJECTED;
    manager->candidate = *candidate;
    return replace_candidate(manager);
}

IrrigationResult configuration_manager_rename_zone(ConfigurationManager *manager, const char *zone_id, const char *display_name)
{
    if (manager == NULL || zone_id == NULL || display_name == NULL) return IRRIGATION_RESULT_REJECTED;
    manager->candidate = manager->configuration;
    for (size_t index = 0U; index < manager->candidate.zone_count; ++index) {
        if (strcmp(manager->candidate.zones[index].zone_id, zone_id) == 0) {
            copy_text(manager->candidate.zones[index].display_name, sizeof(manager->candidate.zones[index].display_name), display_name);
            return replace_candidate(manager);
        }
    }
    return IRRIGATION_RESULT_NOT_FOUND;
}

static IrrigationResult save_program(ConfigurationManager *manager, size_t program_index, const char *display_name, const IrrigationProgramStepConfiguration *steps, size_t step_count, bool is_new, size_t *created_index)
{
    if (manager == NULL || display_name == NULL || (steps == NULL && step_count != 0U) || step_count > IRRIGATION_MAX_PROGRAM_STEPS) return IRRIGATION_RESULT_REJECTED;
    manager->candidate = manager->configuration;
    if (is_new) {
        if (manager->candidate.program_count >= IRRIGATION_MAX_PROGRAMS) return IRRIGATION_RESULT_REJECTED;
        program_index = manager->candidate.program_count++;
        (void)snprintf(manager->candidate.programs[program_index].program_id, sizeof(manager->candidate.programs[program_index].program_id), "program-%u", (unsigned)manager->candidate.next_program_id++);
    } else if (program_index >= manager->candidate.program_count) return IRRIGATION_RESULT_NOT_FOUND;
    IrrigationProgramConfiguration *program = &manager->candidate.programs[program_index];
    copy_text(program->display_name, sizeof(program->display_name), display_name);
    program->step_count = (uint8_t)step_count;
    for (size_t index = 0U; index < step_count; ++index) program->steps[index] = steps[index];
    IrrigationResult result = replace_candidate(manager);
    if (result == IRRIGATION_RESULT_OK && created_index != NULL) *created_index = program_index;
    return result;
}

IrrigationResult configuration_manager_create_program(ConfigurationManager *manager, const char *display_name, const IrrigationProgramStepConfiguration *steps, size_t step_count, size_t *program_index)
{ return save_program(manager, 0U, display_name, steps, step_count, true, program_index); }
IrrigationResult configuration_manager_update_program(ConfigurationManager *manager, size_t program_index, const char *display_name, const IrrigationProgramStepConfiguration *steps, size_t step_count)
{ return save_program(manager, program_index, display_name, steps, step_count, false, NULL); }

IrrigationResult configuration_manager_delete_program(ConfigurationManager *manager, size_t program_index)
{
    if (manager == NULL || program_index >= manager->configuration.program_count) return IRRIGATION_RESULT_NOT_FOUND;
    const char *program_id = manager->configuration.programs[program_index].program_id;
    for (size_t index = 0U; index < manager->configuration.schedule_count; ++index) if (strcmp(manager->configuration.schedules[index].program_id, program_id) == 0) return IRRIGATION_RESULT_REFERENCED;
    manager->candidate = manager->configuration;
    for (size_t index = program_index + 1U; index < manager->candidate.program_count; ++index) manager->candidate.programs[index - 1U] = manager->candidate.programs[index];
    manager->candidate.program_count--;
    return replace_candidate(manager);
}

static IrrigationResult save_schedule(ConfigurationManager *manager, size_t schedule_index, uint8_t weekday_mask, uint8_t hour, uint8_t minute, const char *program_id, bool is_new, size_t *created_index)
{
    if (manager == NULL || program_id == NULL) return IRRIGATION_RESULT_REJECTED;
    manager->candidate = manager->configuration;
    if (is_new) {
        if (manager->candidate.schedule_count >= IRRIGATION_MAX_SCHEDULES) return IRRIGATION_RESULT_REJECTED;
        schedule_index = manager->candidate.schedule_count++;
        (void)snprintf(manager->candidate.schedules[schedule_index].schedule_id, sizeof(manager->candidate.schedules[schedule_index].schedule_id), "schedule-%u", (unsigned)manager->candidate.next_schedule_id++);
    } else if (schedule_index >= manager->candidate.schedule_count) return IRRIGATION_RESULT_NOT_FOUND;
    IrrigationScheduleConfiguration *schedule = &manager->candidate.schedules[schedule_index];
    schedule->weekday_mask = weekday_mask; schedule->hour = hour; schedule->minute = minute;
    copy_text(schedule->program_id, sizeof(schedule->program_id), program_id);
    IrrigationResult result = replace_candidate(manager);
    if (result == IRRIGATION_RESULT_OK && created_index != NULL) *created_index = schedule_index;
    return result;
}

IrrigationResult configuration_manager_create_schedule(ConfigurationManager *manager, uint8_t weekday_mask, uint8_t hour, uint8_t minute, const char *program_id, size_t *schedule_index)
{ return save_schedule(manager, 0U, weekday_mask, hour, minute, program_id, true, schedule_index); }
IrrigationResult configuration_manager_update_schedule(ConfigurationManager *manager, size_t schedule_index, uint8_t weekday_mask, uint8_t hour, uint8_t minute, const char *program_id)
{ return save_schedule(manager, schedule_index, weekday_mask, hour, minute, program_id, false, NULL); }

IrrigationResult configuration_manager_delete_schedule(ConfigurationManager *manager, size_t schedule_index)
{
    if (manager == NULL || schedule_index >= manager->configuration.schedule_count) return IRRIGATION_RESULT_NOT_FOUND;
    manager->candidate = manager->configuration;
    for (size_t index = schedule_index + 1U; index < manager->candidate.schedule_count; ++index) manager->candidate.schedules[index - 1U] = manager->candidate.schedules[index];
    manager->candidate.schedule_count--;
    return replace_candidate(manager);
}

IrrigationResult configuration_manager_set_schedule_enabled(ConfigurationManager *manager, size_t schedule_index, bool enabled)
{
    if (manager == NULL || schedule_index >= manager->configuration.schedule_count) return IRRIGATION_RESULT_NOT_FOUND;
    manager->candidate = manager->configuration;
    manager->candidate.schedules[schedule_index].enabled = enabled;
    return replace_candidate(manager);
}

const char *configuration_manager_zone_name(const ConfigurationManager *manager, const char *zone_id)
{
    if (manager == NULL || zone_id == NULL) return NULL;
    for (size_t index = 0U; index < manager->configuration.zone_count; ++index) if (strcmp(manager->configuration.zones[index].zone_id, zone_id) == 0) return manager->configuration.zones[index].display_name;
    return NULL;
}

const Program *configuration_manager_program_at(const ConfigurationManager *manager, size_t program_index)
{
    if (manager == NULL || program_index >= manager->configuration.program_count) return NULL;
    const int slot = program_slot_for_id(manager, manager->configuration.programs[program_index].program_id);
    return slot < 0 ? NULL : &manager->runtime_programs[slot];
}

IrrigationResult configuration_manager_configure_scheduler(ConfigurationManager *manager, SchedulerService *scheduler_service)
{
    if (manager == NULL || scheduler_service == NULL) return IRRIGATION_RESULT_REJECTED;
    return scheduler_service_configure(scheduler_service, manager->runtime_schedules, manager->configuration.schedule_count);
}
