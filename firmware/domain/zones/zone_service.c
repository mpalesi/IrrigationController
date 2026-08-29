#include "domain/zones/zone_service.h"

#include <stdio.h>

static void publish_zone_event(ZoneService *service, EventType type, const char *zone_id)
{
    const Event event = {
        .id = "zone-event",
        .timestamp_ms = clock_now_ms(&service->clock),
        .source = "zone_service",
        .type = type,
        .version = 1U,
        .zone_id = zone_id,
    };
    (void)event_bus_publish(service->event_bus, &event);
}

static IrrigationResult fault_zone(ZoneService *service, const char *zone_id, OutputState output_state)
{
    IrrigationResult result = state_store_record_zone_actuation_fault(service->state_store, zone_id, output_state);
    if (result == IRRIGATION_RESULT_OK) {
        publish_zone_event(service, EVENT_TYPE_ZONE_FAULTED, zone_id);
    }
    return result;
}

static bool operation_lock_acquire(ZoneService *service)
{
    return !atomic_flag_test_and_set_explicit(&service->operation_lock, memory_order_acquire);
}

static void operation_lock_release(ZoneService *service)
{
    atomic_flag_clear_explicit(&service->operation_lock, memory_order_release);
}

void zone_service_init(ZoneService *service, StateStore *state_store, OutputDriver *output_driver,
                       EventBus *event_bus, Clock clock, ZoneStartPrecondition start_precondition)
{
    if (service != NULL) {
        *service = (ZoneService){
            .state_store = state_store,
            .output_driver = output_driver,
            .event_bus = event_bus,
            .clock = clock,
            .start_precondition = start_precondition,
        };
        atomic_flag_clear(&service->operation_lock);
    }
}

static IrrigationResult zone_service_open_locked(ZoneService *service, const char *zone_id, uint32_t duration_ms)
{
    if (service == NULL || service->state_store == NULL || service->output_driver == NULL ||
        service->event_bus == NULL || zone_id == NULL) {
        return IRRIGATION_RESULT_REJECTED;
    }
    ZoneRuntimeState zone;
    IrrigationResult result = state_store_get_zone(service->state_store, zone_id, &zone);
    if (result != IRRIGATION_RESULT_OK) {
        return result;
    }
    RuntimeState state = state_store_snapshot(service->state_store);
    printf("zone_service: open requested zone=%s master=%d system=%d\n", zone_id,
           state.master_valve.state, state.system_state);
    if (zone.state != ZONE_STATE_IDLE) {
        printf("zone_service: open rejected zone state=%d\n", zone.state);
        return IRRIGATION_RESULT_INVALID_STATE;
    }
    if (state.master_valve.state != MASTER_VALVE_STATE_OPEN) {
        printf("zone_service: open rejected master state=%d\n", state.master_valve.state);
        return IRRIGATION_RESULT_INVALID_STATE;
    }
    for (size_t index = 0U; index < IRRIGATION_MAX_OUTPUTS; ++index) {
        if (state.output_states[index] != OUTPUT_STATE_OFF) {
            printf("zone_service: open rejected active output=%u\n", (unsigned)index);
            return IRRIGATION_RESULT_INVALID_STATE;
        }
    }
    result = zone_start_precondition_check(&service->start_precondition);
    if (result != IRRIGATION_RESULT_OK) {
        printf("zone_service: open rejected precondition result=%d\n", result);
        return result;
    }
    if (state.system_state == SYSTEM_STATE_READY) {
        IrrigationResult transition = state_store_transition_system(service->state_store, SYSTEM_STATE_MANUAL);
        if (transition != IRRIGATION_RESULT_OK) {
            return transition;
        }
    }

    size_t output_index;
    uint32_t configured_duration;
    result = state_store_prepare_zone_open(service->state_store, zone_id, &output_index, &configured_duration);
    if (result != IRRIGATION_RESULT_OK) {
        printf("zone_service: open rejected state prepare result=%d\n", result);
        return result;
    }
    const uint32_t effective_duration = duration_ms == 0U ? configured_duration : duration_ms;
    result = output_driver_set(service->output_driver, output_index, true);
    printf("zone_service: driver open zone=%s result=%d\n", zone_id, result);
    if (result != IRRIGATION_RESULT_OK) {
        IrrigationResult safe_off = output_driver_set(service->output_driver, output_index, false);
        (void)fault_zone(service, zone_id,
                          safe_off == IRRIGATION_RESULT_OK ? OUTPUT_STATE_OFF : OUTPUT_STATE_UNKNOWN);
        return result;
    }
    result = state_store_open_zone(service->state_store, zone_id, clock_now_ms(&service->clock), effective_duration);
    if (result != IRRIGATION_RESULT_OK) {
        (void)output_driver_set(service->output_driver, output_index, false);
        (void)fault_zone(service, zone_id, OUTPUT_STATE_UNKNOWN);
        return result;
    }
    publish_zone_event(service, EVENT_TYPE_ZONE_OPENED, zone_id);
    printf("zone_service: open complete zone=%s state=OPEN\n", zone_id);
    return IRRIGATION_RESULT_OK;
}

IrrigationResult zone_service_open(ZoneService *service, const char *zone_id, uint32_t duration_ms)
{
    if (service == NULL || !operation_lock_acquire(service)) {
        return IRRIGATION_RESULT_REJECTED;
    }
    IrrigationResult result = zone_service_open_locked(service, zone_id, duration_ms);
    operation_lock_release(service);
    return result;
}

static IrrigationResult zone_service_close_locked(ZoneService *service, const char *zone_id)
{
    ZoneRuntimeState zone;
    if (service == NULL || service->state_store == NULL || service->output_driver == NULL ||
        service->event_bus == NULL || zone_id == NULL) {
        return IRRIGATION_RESULT_REJECTED;
    }
    IrrigationResult result = state_store_get_zone(service->state_store, zone_id, &zone);
    if (result != IRRIGATION_RESULT_OK) {
        return result;
    }
    if (zone.state != ZONE_STATE_OPEN && zone.state != ZONE_STATE_FAULT) {
        printf("zone_service: close rejected zone=%s state=%d\n", zone_id, zone.state);
        return IRRIGATION_RESULT_INVALID_STATE;
    }
    result = output_driver_set(service->output_driver, zone.output_index, false);
    printf("zone_service: driver close zone=%s result=%d\n", zone_id, result);
    if (result != IRRIGATION_RESULT_OK) {
        (void)fault_zone(service, zone_id, OUTPUT_STATE_UNKNOWN);
        return result;
    }
    result = state_store_close_zone(service->state_store, zone_id);
    if (result == IRRIGATION_RESULT_OK) {
        publish_zone_event(service, EVENT_TYPE_ZONE_CLOSED, zone_id);
        printf("zone_service: close complete zone=%s\n", zone_id);
    }
    return result;
}

IrrigationResult zone_service_close(ZoneService *service, const char *zone_id)
{
    if (service == NULL || !operation_lock_acquire(service)) {
        return IRRIGATION_RESULT_REJECTED;
    }
    IrrigationResult result = zone_service_close_locked(service, zone_id);
    operation_lock_release(service);
    return result;
}

IrrigationResult zone_service_safe_close(ZoneService *service)
{
    if (service == NULL || service->state_store == NULL || !operation_lock_acquire(service)) {
        return IRRIGATION_RESULT_REJECTED;
    }
    RuntimeState state = state_store_snapshot(service->state_store);
    IrrigationResult first_failure = IRRIGATION_RESULT_OK;
    for (size_t index = 0U; index < state.zone_count; ++index) {
        if (state.output_states[state.zones[index].output_index] != OUTPUT_STATE_OFF) {
            IrrigationResult result = zone_service_close_locked(service, state.zones[index].zone_id);
            if (first_failure == IRRIGATION_RESULT_OK && result != IRRIGATION_RESULT_OK) {
                first_failure = result;
            }
        }
    }
    operation_lock_release(service);
    return first_failure;
}

IrrigationResult zone_service_process_timeouts(ZoneService *service)
{
    if (service == NULL || service->state_store == NULL || !operation_lock_acquire(service)) {
        return IRRIGATION_RESULT_REJECTED;
    }
    const RuntimeState state = state_store_snapshot(service->state_store);
    const uint64_t now_ms = clock_now_ms(&service->clock);
    for (size_t index = 0U; index < state.zone_count; ++index) {
        const ZoneRuntimeState *zone = &state.zones[index];
        if (zone->state == ZONE_STATE_OPEN && zone->duration_ms != 0U &&
            now_ms - zone->opened_at_ms >= zone->duration_ms) {
            IrrigationResult result = zone_service_close_locked(service, zone->zone_id);
            if (result != IRRIGATION_RESULT_OK) {
                operation_lock_release(service);
                return result;
            }
            publish_zone_event(service, EVENT_TYPE_ZONE_TIMED_OUT, zone->zone_id);
        }
    }
    operation_lock_release(service);
    return IRRIGATION_RESULT_OK;
}
