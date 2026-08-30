#include "app/state/state_store.h"

#include <string.h>

#define SCHEDULE_OCCURRENCE_UNHANDLED UINT64_MAX

static ZoneRuntimeState *find_zone(StateStore *store, const char *zone_id)
{
    if (store == NULL || zone_id == NULL) {
        return NULL;
    }
    for (size_t index = 0U; index < store->current.zone_count; ++index) {
        if (strcmp(store->current.zones[index].zone_id, zone_id) == 0) {
            return &store->current.zones[index];
        }
    }
    return NULL;
}

static const ZoneRuntimeState *find_zone_const(const StateStore *store, const char *zone_id)
{
    return find_zone((StateStore *)store, zone_id);
}

static bool has_active_or_unknown_output(const StateStore *store)
{
    for (size_t index = 0U; index < IRRIGATION_MAX_OUTPUTS; ++index) {
        if (store->current.output_states[index] != OUTPUT_STATE_OFF) {
            return true;
        }
    }
    return false;
}

bool state_store_master_valve_close_is_eligible(const StateStore *store)
{
    return store != NULL && (store->current.master_valve.state == MASTER_VALVE_STATE_OPEN ||
                             store->current.master_valve.state == MASTER_VALVE_STATE_OPENING) &&
           !has_active_or_unknown_output(store);
}

static bool system_transition_is_allowed(SystemState from, SystemState to)
{
    if (from == to) {
        return true;
    }

    return (from == SYSTEM_STATE_BOOT && to == SYSTEM_STATE_READY) ||
           (from == SYSTEM_STATE_READY && to == SYSTEM_STATE_MANUAL) ||
           (from == SYSTEM_STATE_MANUAL && to == SYSTEM_STATE_READY) ||
           (from == SYSTEM_STATE_READY && to == SYSTEM_STATE_ERROR) ||
           (from == SYSTEM_STATE_ERROR && to == SYSTEM_STATE_BOOT);
}

IrrigationResult state_store_configure_zones(StateStore *store, const Zone *zones, size_t zone_count)
{
    if (store == NULL || (zones == NULL && zone_count != 0U) || zone_count > IRRIGATION_MAX_ZONES) {
        return IRRIGATION_RESULT_REJECTED;
    }

    RuntimeState configured = store->current;
    memset(configured.zones, 0, sizeof(configured.zones));
    configured.zone_count = 0U;
    for (size_t index = 0U; index < zone_count; ++index) {
        const Zone *zone = &zones[index];
        if (zone->id == NULL || zone->output == NULL || zone->output->id == NULL ||
            zone->output->driver_output_index >= IRRIGATION_MAX_OUTPUTS) {
            return IRRIGATION_RESULT_REJECTED;
        }
        for (size_t previous = 0U; previous < index; ++previous) {
            if (strcmp(zones[previous].id, zone->id) == 0 ||
                zones[previous].output->driver_output_index == zone->output->driver_output_index) {
                return IRRIGATION_RESULT_REJECTED;
            }
        }
        configured.zones[index] = (ZoneRuntimeState){
            .zone_id = zone->id,
            .output_id = zone->output->id,
            .output_index = zone->output->driver_output_index,
            .state = zone->enabled ? ZONE_STATE_IDLE : ZONE_STATE_DISABLED,
            .duration_ms = zone->default_duration_ms,
        };
        configured.zone_count++;
    }
    store->current = configured;
    return IRRIGATION_RESULT_OK;
}

void state_store_init(StateStore *store)
{
    if (store == NULL) {
        return;
    }

    memset(store, 0, sizeof(*store));
    store->current.system_state = SYSTEM_STATE_BOOT;
    store->current.master_valve.state = MASTER_VALVE_STATE_CLOSED;
    store->current.program.state = PROGRAM_STATE_IDLE;
    for (size_t index = 0U; index < IRRIGATION_MAX_SCHEDULES; ++index) {
        store->current.scheduler.last_handled_occurrence[index] = SCHEDULE_OCCURRENCE_UNHANDLED;
    }
}

RuntimeState state_store_snapshot(const StateStore *store)
{
    RuntimeState snapshot = {0};
    if (store != NULL) {
        snapshot = store->current;
    }
    return snapshot;
}

IrrigationResult state_store_transition_system(StateStore *store, SystemState next_state)
{
    if (store == NULL) {
        return IRRIGATION_RESULT_INTERNAL_ERROR;
    }
    if (!system_transition_is_allowed(store->current.system_state, next_state)) {
        return IRRIGATION_RESULT_INVALID_STATE;
    }

    store->current.system_state = next_state;
    return IRRIGATION_RESULT_OK;
}

IrrigationResult state_store_get_zone(const StateStore *store, const char *zone_id, ZoneRuntimeState *zone)
{
    const ZoneRuntimeState *found = find_zone_const(store, zone_id);
    if (found == NULL || zone == NULL) {
        return IRRIGATION_RESULT_NOT_FOUND;
    }
    *zone = *found;
    return IRRIGATION_RESULT_OK;
}

IrrigationResult state_store_prepare_zone_open(const StateStore *store, const char *zone_id, size_t *output_index,
                                                uint32_t *duration_ms)
{
    const ZoneRuntimeState *zone = find_zone_const(store, zone_id);
    if (zone == NULL) {
        return IRRIGATION_RESULT_NOT_FOUND;
    }
    if (store->current.system_state != SYSTEM_STATE_MANUAL || zone->state != ZONE_STATE_IDLE ||
        store->current.master_valve.state != MASTER_VALVE_STATE_OPEN || has_active_or_unknown_output(store)) {
        return IRRIGATION_RESULT_INVALID_STATE;
    }
    if (output_index == NULL || duration_ms == NULL) {
        return IRRIGATION_RESULT_INTERNAL_ERROR;
    }
    *output_index = zone->output_index;
    *duration_ms = zone->duration_ms;
    return IRRIGATION_RESULT_OK;
}

IrrigationResult state_store_open_zone(StateStore *store, const char *zone_id, uint64_t opened_at_ms,
                                       uint32_t duration_ms)
{
    size_t output_index;
    uint32_t ignored_duration;
    IrrigationResult result = state_store_prepare_zone_open(store, zone_id, &output_index, &ignored_duration);
    if (result != IRRIGATION_RESULT_OK) {
        return result;
    }
    ZoneRuntimeState *zone = find_zone(store, zone_id);
    zone->state = ZONE_STATE_OPEN;
    zone->opened_at_ms = opened_at_ms;
    zone->duration_ms = duration_ms;
    store->current.output_states[output_index] = OUTPUT_STATE_ON;
    return IRRIGATION_RESULT_OK;
}

IrrigationResult state_store_close_zone(StateStore *store, const char *zone_id)
{
    ZoneRuntimeState *zone = find_zone(store, zone_id);
    if (zone == NULL) {
        return IRRIGATION_RESULT_NOT_FOUND;
    }
    if (zone->state != ZONE_STATE_OPEN && zone->state != ZONE_STATE_FAULT) {
        return IRRIGATION_RESULT_INVALID_STATE;
    }
    zone->state = ZONE_STATE_IDLE;
    zone->opened_at_ms = 0U;
    store->current.output_states[zone->output_index] = OUTPUT_STATE_OFF;
    if (!has_active_or_unknown_output(store) && store->current.system_state == SYSTEM_STATE_MANUAL) {
        store->current.system_state = SYSTEM_STATE_READY;
    }
    return IRRIGATION_RESULT_OK;
}

IrrigationResult state_store_record_zone_actuation_fault(StateStore *store, const char *zone_id,
                                                          OutputState output_state)
{
    ZoneRuntimeState *zone = find_zone(store, zone_id);
    if (zone == NULL) {
        return IRRIGATION_RESULT_NOT_FOUND;
    }
    if (zone->state == ZONE_STATE_DISABLED) {
        return IRRIGATION_RESULT_INVALID_STATE;
    }
    zone->state = ZONE_STATE_FAULT;
    store->current.output_states[zone->output_index] = output_state;
    store->current.system_state = SYSTEM_STATE_ERROR;
    return IRRIGATION_RESULT_OK;
}

IrrigationResult state_store_configure_master_valve(StateStore *store, uint32_t pre_open_delay_ms)
{
    if (store == NULL || store->current.master_valve.state != MASTER_VALVE_STATE_CLOSED) {
        return IRRIGATION_RESULT_INVALID_STATE;
    }
    store->current.master_valve.pre_open_delay_ms = pre_open_delay_ms;
    return IRRIGATION_RESULT_OK;
}

IrrigationResult state_store_begin_master_valve_open(StateStore *store, uint64_t confirmed_at_ms)
{
    if (store == NULL || store->current.master_valve.state != MASTER_VALVE_STATE_CLOSED) {
        return IRRIGATION_RESULT_INVALID_STATE;
    }
    store->current.master_valve.state = MASTER_VALVE_STATE_OPENING;
    store->current.master_valve.opening_confirmed_at_ms = confirmed_at_ms;
    return IRRIGATION_RESULT_OK;
}

IrrigationResult state_store_complete_master_valve_pre_open_delay(StateStore *store, uint64_t now_ms)
{
    if (store == NULL || store->current.master_valve.state != MASTER_VALVE_STATE_OPENING) {
        return IRRIGATION_RESULT_INVALID_STATE;
    }
    if (now_ms - store->current.master_valve.opening_confirmed_at_ms <
        store->current.master_valve.pre_open_delay_ms) {
        return IRRIGATION_RESULT_TIMEOUT;
    }
    store->current.master_valve.state = MASTER_VALVE_STATE_OPEN;
    return IRRIGATION_RESULT_OK;
}

IrrigationResult state_store_begin_master_valve_close(StateStore *store)
{
    if (!state_store_master_valve_close_is_eligible(store)) {
        return IRRIGATION_RESULT_INVALID_STATE;
    }
    store->current.master_valve.state = MASTER_VALVE_STATE_CLOSING;
    return IRRIGATION_RESULT_OK;
}

IrrigationResult state_store_complete_master_valve_close(StateStore *store)
{
    if (store == NULL || store->current.master_valve.state != MASTER_VALVE_STATE_CLOSING) {
        return IRRIGATION_RESULT_INVALID_STATE;
    }
    store->current.master_valve.state = MASTER_VALVE_STATE_CLOSED;
    store->current.master_valve.opening_confirmed_at_ms = 0U;
    return IRRIGATION_RESULT_OK;
}

IrrigationResult state_store_record_master_valve_fault(StateStore *store)
{
    if (store == NULL) {
        return IRRIGATION_RESULT_INTERNAL_ERROR;
    }
    store->current.master_valve.state = MASTER_VALVE_STATE_FAULT;
    return IRRIGATION_RESULT_OK;
}

IrrigationResult state_store_start_program(StateStore *store)
{
    if (store == NULL || (store->current.program.state != PROGRAM_STATE_IDLE &&
                          store->current.program.state != PROGRAM_STATE_COMPLETED &&
                          store->current.program.state != PROGRAM_STATE_ABORTED)) {
        return IRRIGATION_RESULT_INVALID_STATE;
    }
    store->current.program = (ProgramRuntimeState){.state = PROGRAM_STATE_WAITING_MASTER};
    return IRRIGATION_RESULT_OK;
}

IrrigationResult state_store_set_program_running_zone(StateStore *store, size_t step_index,
                                                       uint64_t started_at_ms)
{
    if (store == NULL || (store->current.program.state != PROGRAM_STATE_WAITING_MASTER &&
                          store->current.program.state != PROGRAM_STATE_RUNNING_ZONE)) {
        return IRRIGATION_RESULT_INVALID_STATE;
    }
    store->current.program = (ProgramRuntimeState){
        .state = PROGRAM_STATE_RUNNING_ZONE, .current_step_index = step_index, .step_started_at_ms = started_at_ms};
    return IRRIGATION_RESULT_OK;
}

IrrigationResult state_store_set_program_waiting_post_delay(StateStore *store, uint64_t started_at_ms)
{
    if (store == NULL || store->current.program.state != PROGRAM_STATE_RUNNING_ZONE) {
        return IRRIGATION_RESULT_INVALID_STATE;
    }
    store->current.program.state = PROGRAM_STATE_WAITING_POST_PROGRAM_DELAY;
    store->current.program.step_started_at_ms = started_at_ms;
    return IRRIGATION_RESULT_OK;
}

IrrigationResult state_store_set_program_terminal(StateStore *store, ProgramState state)
{
    if (store == NULL || (state != PROGRAM_STATE_COMPLETED && state != PROGRAM_STATE_ABORTED &&
                          state != PROGRAM_STATE_FAULT)) {
        return IRRIGATION_RESULT_INVALID_STATE;
    }
    store->current.program.state = state;
    return IRRIGATION_RESULT_OK;
}

IrrigationResult state_store_configure_scheduler(StateStore *store, size_t entry_count)
{
    if (store == NULL || entry_count > IRRIGATION_MAX_SCHEDULES) {
        return IRRIGATION_RESULT_REJECTED;
    }
    store->current.scheduler.entry_count = entry_count;
    return IRRIGATION_RESULT_OK;
}

IrrigationResult state_store_get_schedule_occurrence(const StateStore *store, size_t entry_index,
                                                      uint64_t *occurrence,
                                                      ScheduleOccurrenceStatus *status)
{
    if (store == NULL || occurrence == NULL || status == NULL ||
        entry_index >= store->current.scheduler.entry_count) {
        return IRRIGATION_RESULT_INVALID_STATE;
    }
    *occurrence = store->current.scheduler.last_handled_occurrence[entry_index];
    *status = store->current.scheduler.last_occurrence_status[entry_index];
    return IRRIGATION_RESULT_OK;
}

IrrigationResult state_store_reset_schedule_occurrence(StateStore *store, size_t entry_index)
{
    if (store == NULL || entry_index >= store->current.scheduler.entry_count) {
        return IRRIGATION_RESULT_INVALID_STATE;
    }
    store->current.scheduler.last_handled_occurrence[entry_index] = SCHEDULE_OCCURRENCE_UNHANDLED;
    store->current.scheduler.last_occurrence_status[entry_index] = SCHEDULE_OCCURRENCE_NONE;
    return IRRIGATION_RESULT_OK;
}

IrrigationResult state_store_record_schedule_occurrence(StateStore *store, size_t entry_index,
                                                         uint64_t occurrence,
                                                         ScheduleOccurrenceStatus status)
{
    if (store == NULL || entry_index >= store->current.scheduler.entry_count ||
        status == SCHEDULE_OCCURRENCE_NONE) {
        return IRRIGATION_RESULT_INVALID_STATE;
    }
    store->current.scheduler.last_handled_occurrence[entry_index] = occurrence;
    store->current.scheduler.last_occurrence_status[entry_index] = status;
    return IRRIGATION_RESULT_OK;
}
