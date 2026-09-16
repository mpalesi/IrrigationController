#include "domain/master_valve/master_valve_service.h"

#include <stdio.h>

static bool operation_lock_acquire(MasterValveService *service)
{
    return !atomic_flag_test_and_set_explicit(&service->operation_lock, memory_order_acquire);
}

static void operation_lock_release(MasterValveService *service)
{
    atomic_flag_clear_explicit(&service->operation_lock, memory_order_release);
}

void master_valve_service_init(MasterValveService *service, StateStore *state_store,
                               MasterValveDriver *driver, Clock clock,
                               MasterValveConfiguration configuration)
{
    if (service == NULL) {
        return;
    }
    *service = (MasterValveService){.state_store = state_store, .driver = driver, .clock = clock};
    atomic_flag_clear(&service->operation_lock);
    if (state_store != NULL) {
        (void)state_store_configure_master_valve(state_store, configuration.pre_open_delay_ms);
    }
}

IrrigationResult master_valve_service_open(MasterValveService *service)
{
    if (service == NULL || service->state_store == NULL || service->driver == NULL ||
        !operation_lock_acquire(service)) {
        return IRRIGATION_RESULT_REJECTED;
    }
    MasterValveState state = state_store_snapshot(service->state_store).master_valve.state;
    printf("master_valve: open requested state=%d\n", state);
    if (state != MASTER_VALVE_STATE_CLOSED && state != MASTER_VALVE_STATE_FAULT) {
        operation_lock_release(service);
        return IRRIGATION_RESULT_INVALID_STATE;
    }
    IrrigationResult result = master_valve_driver_open_and_confirm(service->driver);
    printf("master_valve: driver open result=%d\n", result);
    if (result == IRRIGATION_RESULT_OK) {
        result = state_store_begin_master_valve_open(service->state_store, clock_now_ms(&service->clock));
        printf("master_valve: open state=%d result=%d\n",
               state_store_snapshot(service->state_store).master_valve.state, result);
    }
    if (result != IRRIGATION_RESULT_OK) {
        (void)state_store_record_master_valve_fault(service->state_store);
    }
    operation_lock_release(service);
    return result;
}

IrrigationResult master_valve_service_process_time(MasterValveService *service)
{
    if (service == NULL || service->state_store == NULL || !operation_lock_acquire(service)) {
        return IRRIGATION_RESULT_REJECTED;
    }
    RuntimeState state = state_store_snapshot(service->state_store);
    IrrigationResult result = IRRIGATION_RESULT_OK;
    if (state.master_valve.state == MASTER_VALVE_STATE_OPENING) {
        result = state_store_complete_master_valve_pre_open_delay(service->state_store,
                                                                    clock_now_ms(&service->clock));
        if (result == IRRIGATION_RESULT_TIMEOUT) {
            result = IRRIGATION_RESULT_OK;
        } else if (result == IRRIGATION_RESULT_OK) {
            printf("master_valve: state OPENING -> OPEN\n");
        }
    }
    operation_lock_release(service);
    return result;
}

IrrigationResult master_valve_service_close(MasterValveService *service)
{
    if (service == NULL || service->state_store == NULL || service->driver == NULL ||
        !operation_lock_acquire(service)) {
        return IRRIGATION_RESULT_REJECTED;
    }
    printf("master_valve: close requested state=%d\n", state_store_snapshot(service->state_store).master_valve.state);
    IrrigationResult result = state_store_begin_master_valve_close(service->state_store);
    if (result != IRRIGATION_RESULT_OK) {
        operation_lock_release(service);
        return result;
    }
    result = master_valve_driver_close_and_confirm(service->driver);
    printf("master_valve: driver close result=%d\n", result);
    if (result == IRRIGATION_RESULT_OK) {
        result = state_store_complete_master_valve_close(service->state_store);
        printf("master_valve: close state=%d result=%d\n",
               state_store_snapshot(service->state_store).master_valve.state, result);
    }
    if (result != IRRIGATION_RESULT_OK) {
        (void)state_store_record_master_valve_fault(service->state_store);
    }
    operation_lock_release(service);
    return result;
}

IrrigationResult master_valve_service_check_zone_start(void *context)
{
    MasterValveService *service = context;
    if (service == NULL || service->state_store == NULL) {
        return IRRIGATION_RESULT_REJECTED;
    }
    return state_store_snapshot(service->state_store).master_valve.state == MASTER_VALVE_STATE_OPEN
        ? IRRIGATION_RESULT_OK
        : IRRIGATION_RESULT_INVALID_STATE;
}

ZoneStartPrecondition master_valve_service_zone_start_precondition(MasterValveService *service)
{
    return (ZoneStartPrecondition){.check = master_valve_service_check_zone_start, .context = service};
}

bool master_valve_service_close_is_eligible(const MasterValveService *service)
{
    return service != NULL && state_store_master_valve_close_is_eligible(service->state_store);
}

MasterValveState master_valve_service_state(const MasterValveService *service)
{
    return service == NULL || service->state_store == NULL
        ? MASTER_VALVE_STATE_FAULT
        : state_store_snapshot(service->state_store).master_valve.state;
}
