#pragma once

#include <stddef.h>

#include "app/configuration/irrigation_configuration.h"
#include "domain/scheduler/scheduler_service.h"
#include "infrastructure/persistence/configuration_repository.h"

typedef enum {
    CONFIGURATION_MANAGER_BOOT_LOADED = 0,
    CONFIGURATION_MANAGER_BOOT_DEFAULTS_PERSISTED,
    CONFIGURATION_MANAGER_BOOT_DEFAULTS_CORRUPT,
    CONFIGURATION_MANAGER_BOOT_DEFAULTS_INCOMPATIBLE,
    CONFIGURATION_MANAGER_BOOT_DEFAULTS_STORAGE_ERROR,
} ConfigurationManagerBootResult;

typedef struct {
    IrrigationConfiguration configuration;
    IrrigationConfiguration candidate;
    const Zone *board_zones;
    size_t board_zone_count;
    ConfigurationRepository *repository;
    bool persistence_required;
    SchedulerService *scheduler_service;
    Program runtime_programs[IRRIGATION_MAX_PROGRAMS];
    ProgramStep runtime_program_steps[IRRIGATION_MAX_PROGRAMS][IRRIGATION_MAX_PROGRAM_STEPS];
    char runtime_program_ids[IRRIGATION_MAX_PROGRAMS][IRRIGATION_CONFIGURATION_ID_SIZE];
    char runtime_program_names[IRRIGATION_MAX_PROGRAMS][IRRIGATION_CONFIGURATION_NAME_SIZE];
    char runtime_step_zone_ids[IRRIGATION_MAX_PROGRAMS][IRRIGATION_MAX_PROGRAM_STEPS]
                              [IRRIGATION_CONFIGURATION_ID_SIZE];
    bool runtime_program_slots[IRRIGATION_MAX_PROGRAMS];
    SchedulerEntry runtime_schedules[IRRIGATION_MAX_SCHEDULES];
    char runtime_schedule_ids[IRRIGATION_MAX_SCHEDULES][IRRIGATION_CONFIGURATION_ID_SIZE];
    bool runtime_schedule_slots[IRRIGATION_MAX_SCHEDULES];
} ConfigurationManager;

IrrigationResult configuration_manager_init_dev_kit_defaults(ConfigurationManager *manager,
                                                              const Zone *board_zones,
                                                              size_t board_zone_count);
void configuration_manager_set_repository(ConfigurationManager *manager,
                                          ConfigurationRepository *repository,
                                          bool persistence_required);
void configuration_manager_bind_scheduler(ConfigurationManager *manager,
                                          SchedulerService *scheduler_service);
ConfigurationManagerBootResult configuration_manager_load_or_persist_defaults(
    ConfigurationManager *manager, ConfigurationRepository *repository);
const IrrigationConfiguration *configuration_manager_get(const ConfigurationManager *manager);
IrrigationResult configuration_manager_validate_candidate(const ConfigurationManager *manager,
                                                          const IrrigationConfiguration *candidate);
IrrigationResult configuration_manager_replace(ConfigurationManager *manager,
                                               const IrrigationConfiguration *candidate);
IrrigationResult configuration_manager_rename_zone(ConfigurationManager *manager, const char *zone_id,
                                                    const char *display_name);
IrrigationResult configuration_manager_create_program(ConfigurationManager *manager,
                                                       const char *display_name,
                                                       const IrrigationProgramStepConfiguration *steps,
                                                       size_t step_count, size_t *program_index);
IrrigationResult configuration_manager_update_program(ConfigurationManager *manager, size_t program_index,
                                                       const char *display_name,
                                                       const IrrigationProgramStepConfiguration *steps,
                                                       size_t step_count);
IrrigationResult configuration_manager_delete_program(ConfigurationManager *manager, size_t program_index);
IrrigationResult configuration_manager_create_schedule(ConfigurationManager *manager, uint8_t weekday_mask,
                                                        uint8_t hour, uint8_t minute, const char *program_id,
                                                        size_t *schedule_index);
IrrigationResult configuration_manager_update_schedule(ConfigurationManager *manager, size_t schedule_index,
                                                        uint8_t weekday_mask, uint8_t hour, uint8_t minute,
                                                        const char *program_id);
IrrigationResult configuration_manager_delete_schedule(ConfigurationManager *manager, size_t schedule_index);
IrrigationResult configuration_manager_set_schedule_enabled(ConfigurationManager *manager,
                                                             size_t schedule_index, bool enabled);
const char *configuration_manager_zone_name(const ConfigurationManager *manager, const char *zone_id);
const Program *configuration_manager_program_at(const ConfigurationManager *manager, size_t program_index);
IrrigationResult configuration_manager_configure_scheduler(ConfigurationManager *manager,
                                                            SchedulerService *scheduler_service);
