#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/result.h"
#include "common/constants.h"
#include "domain/zones/zone.h"

#define IRRIGATION_MAX_OUTPUTS IRRIGATION_LOCAL_OUTPUT_COUNT

typedef enum {
    SYSTEM_STATE_BOOT = 0,
    SYSTEM_STATE_READY,
    SYSTEM_STATE_RUNNING,
    SYSTEM_STATE_MANUAL,
    SYSTEM_STATE_OTA,
    SYSTEM_STATE_MAINTENANCE,
    SYSTEM_STATE_ERROR,
} SystemState;

typedef enum {
    ZONE_STATE_DISABLED = 0,
    ZONE_STATE_IDLE,
    ZONE_STATE_OPENING,
    ZONE_STATE_OPEN,
    ZONE_STATE_CLOSING,
    ZONE_STATE_FAULT,
} ZoneState;

typedef enum {
    OUTPUT_STATE_OFF = 0,
    OUTPUT_STATE_ON,
    OUTPUT_STATE_UNKNOWN,
} OutputState;

typedef enum {
    MASTER_VALVE_STATE_CLOSED = 0,
    MASTER_VALVE_STATE_OPENING,
    MASTER_VALVE_STATE_OPEN,
    MASTER_VALVE_STATE_CLOSING,
    MASTER_VALVE_STATE_FAULT,
} MasterValveState;

typedef enum {
    PROGRAM_STATE_IDLE = 0,
    PROGRAM_STATE_WAITING_MASTER,
    PROGRAM_STATE_RUNNING_ZONE,
    PROGRAM_STATE_WAITING_POST_PROGRAM_DELAY,
    PROGRAM_STATE_COMPLETED,
    PROGRAM_STATE_ABORTED,
    PROGRAM_STATE_FAULT,
} ProgramState;

typedef enum {
    SCHEDULE_OCCURRENCE_NONE = 0,
    SCHEDULE_OCCURRENCE_STARTED,
    SCHEDULE_OCCURRENCE_SKIPPED_BUSY,
    SCHEDULE_OCCURRENCE_REJECTED,
} ScheduleOccurrenceStatus;

typedef struct {
    MasterValveState state;
    uint64_t opening_confirmed_at_ms;
    uint32_t pre_open_delay_ms;
} MasterValveRuntimeState;

typedef struct {
    ProgramState state;
    size_t current_step_index;
    uint64_t step_started_at_ms;
} ProgramRuntimeState;

typedef struct {
    uint64_t last_handled_occurrence[IRRIGATION_MAX_SCHEDULES];
    ScheduleOccurrenceStatus last_occurrence_status[IRRIGATION_MAX_SCHEDULES];
    size_t entry_count;
} SchedulerRuntimeState;

typedef struct {
    const char *zone_id;
    const char *output_id;
    size_t output_index;
    ZoneState state;
    uint64_t opened_at_ms;
    uint32_t duration_ms;
} ZoneRuntimeState;

typedef struct {
    SystemState system_state;
    OutputState output_states[IRRIGATION_MAX_OUTPUTS];
    MasterValveRuntimeState master_valve;
    ProgramRuntimeState program;
    SchedulerRuntimeState scheduler;
    ZoneRuntimeState zones[IRRIGATION_MAX_ZONES];
    size_t zone_count;
} RuntimeState;

typedef struct {
    RuntimeState current;
} StateStore;

void state_store_init(StateStore *store);
IrrigationResult state_store_configure_zones(StateStore *store, const Zone *zones, size_t zone_count);
RuntimeState state_store_snapshot(const StateStore *store);
IrrigationResult state_store_transition_system(StateStore *store, SystemState next_state);
IrrigationResult state_store_get_zone(const StateStore *store, const char *zone_id, ZoneRuntimeState *zone);
IrrigationResult state_store_prepare_zone_open(const StateStore *store, const char *zone_id, size_t *output_index,
                                                uint32_t *duration_ms);
IrrigationResult state_store_open_zone(StateStore *store, const char *zone_id, uint64_t opened_at_ms,
                                       uint32_t duration_ms);
IrrigationResult state_store_close_zone(StateStore *store, const char *zone_id);
IrrigationResult state_store_record_zone_actuation_fault(StateStore *store, const char *zone_id,
                                                          OutputState output_state);
IrrigationResult state_store_configure_master_valve(StateStore *store, uint32_t pre_open_delay_ms);
IrrigationResult state_store_begin_master_valve_open(StateStore *store, uint64_t confirmed_at_ms);
IrrigationResult state_store_complete_master_valve_pre_open_delay(StateStore *store, uint64_t now_ms);
IrrigationResult state_store_begin_master_valve_close(StateStore *store);
IrrigationResult state_store_complete_master_valve_close(StateStore *store);
IrrigationResult state_store_record_master_valve_fault(StateStore *store);
bool state_store_master_valve_close_is_eligible(const StateStore *store);
IrrigationResult state_store_start_program(StateStore *store);
IrrigationResult state_store_set_program_running_zone(StateStore *store, size_t step_index,
                                                       uint64_t started_at_ms);
IrrigationResult state_store_set_program_waiting_post_delay(StateStore *store, uint64_t started_at_ms);
IrrigationResult state_store_set_program_terminal(StateStore *store, ProgramState state);
IrrigationResult state_store_configure_scheduler(StateStore *store, size_t entry_count);
IrrigationResult state_store_record_schedule_occurrence(StateStore *store, size_t entry_index,
                                                         uint64_t occurrence,
                                                         ScheduleOccurrenceStatus status);
