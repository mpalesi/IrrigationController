#include "app/state/state_store.h"

#include <string.h>

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
        has_active_or_unknown_output(store)) {
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
