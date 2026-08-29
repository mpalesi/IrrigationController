#include "domain/programs/program_service.h"

#include <stdio.h>

static bool operation_lock_acquire(ProgramService *service)
{
    return !atomic_flag_test_and_set_explicit(&service->operation_lock, memory_order_acquire);
}

static void operation_lock_release(ProgramService *service)
{
    atomic_flag_clear_explicit(&service->operation_lock, memory_order_release);
}

static bool program_is_valid(const Program *program)
{
    if (program == NULL || program->id == NULL || program->steps == NULL || program->step_count == 0U) {
        return false;
    }
    for (size_t index = 0U; index < program->step_count; ++index) {
        if (program->steps[index].zone_id == NULL) {
            return false;
        }
    }
    return true;
}

static IrrigationResult fault(ProgramService *service, IrrigationResult result)
{
    printf("program_service: fault result=%d\n", result);
    if (master_valve_service_close_is_eligible(service->master_valve_service)) {
        (void)master_valve_service_close(service->master_valve_service);
    }
    (void)state_store_set_program_terminal(service->state_store, PROGRAM_STATE_FAULT);
    return result;
}

static IrrigationResult open_current_zone(ProgramService *service)
{
    RuntimeState state = state_store_snapshot(service->state_store);
    const ProgramStep *step = &service->program->steps[state.program.current_step_index];
    printf("program_service: open zone step=%u zone=%s\n", (unsigned)state.program.current_step_index, step->zone_id);
    IrrigationResult result = zone_service_open(service->zone_service, step->zone_id, step->duration_ms);
    if (result != IRRIGATION_RESULT_OK) {
        return fault(service, result);
    }
    result = state_store_set_program_running_zone(service->state_store, state.program.current_step_index,
                                                  clock_now_ms(&service->clock));
    printf("program_service: state RUNNING_ZONE step=%u result=%d\n",
           (unsigned)state.program.current_step_index, result);
    return result;
}

void program_service_init(ProgramService *service, StateStore *state_store,
                          MasterValveService *master_valve_service,
                          ZoneService *zone_service, Clock clock)
{
    if (service == NULL) {
        return;
    }
    *service = (ProgramService){
        .state_store = state_store, .master_valve_service = master_valve_service,
        .zone_service = zone_service,
        .clock = clock,
    };
    atomic_flag_clear(&service->operation_lock);
}

bool program_service_is_active(const ProgramService *service)
{
    if (service == NULL || service->state_store == NULL) {
        return false;
    }
    ProgramState state = state_store_snapshot(service->state_store).program.state;
    return state == PROGRAM_STATE_WAITING_MASTER || state == PROGRAM_STATE_RUNNING_ZONE ||
           state == PROGRAM_STATE_WAITING_POST_PROGRAM_DELAY;
}

ProgramState program_service_state(const ProgramService *service)
{
    return service == NULL || service->state_store == NULL
        ? PROGRAM_STATE_FAULT
        : state_store_snapshot(service->state_store).program.state;
}

IrrigationResult program_service_start(ProgramService *service, const Program *program)
{
    if (service == NULL || service->state_store == NULL || service->master_valve_service == NULL || service->zone_service == NULL ||
        !program_is_valid(program) || program_service_is_active(service) || !operation_lock_acquire(service)) {
        return IRRIGATION_RESULT_REJECTED;
    }
    service->program = program;
    printf("program_service: start program=%s\n", program->id);
    IrrigationResult result = state_store_start_program(service->state_store);
    if (result != IRRIGATION_RESULT_OK) {
        operation_lock_release(service);
        return result;
    }
    printf("program_service: state WAITING_MASTER\n");
    printf("program_service: request master open\n");
    result = master_valve_service_open(service->master_valve_service);
    printf("program_service: master open result=%d\n", result);
    if (result != IRRIGATION_RESULT_OK) {
        (void)state_store_set_program_terminal(service->state_store, PROGRAM_STATE_FAULT);
    }
    operation_lock_release(service);
    return result;
}

IrrigationResult program_service_process(ProgramService *service)
{
    if (service == NULL || !program_service_is_active(service) || !operation_lock_acquire(service)) {
        return IRRIGATION_RESULT_REJECTED;
    }
    IrrigationResult result = IRRIGATION_RESULT_OK;
    RuntimeState state = state_store_snapshot(service->state_store);
    if (state.program.state == PROGRAM_STATE_WAITING_MASTER) {
        result = master_valve_service_process_time(service->master_valve_service);
        if (result != IRRIGATION_RESULT_OK) {
            result = fault(service, result);
        } else if (master_valve_service_state(service->master_valve_service) == MASTER_VALVE_STATE_OPEN) {
            result = open_current_zone(service);
        } else if (master_valve_service_state(service->master_valve_service) == MASTER_VALVE_STATE_FAULT) {
            result = fault(service, IRRIGATION_RESULT_INVALID_STATE);
        }
    } else if (state.program.state == PROGRAM_STATE_RUNNING_ZONE) {
        const ProgramStep *step = &service->program->steps[state.program.current_step_index];
        if (clock_now_ms(&service->clock) - state.program.step_started_at_ms >= step->duration_ms) {
            printf("program_service: close zone step=%u zone=%s\n",
                   (unsigned)state.program.current_step_index, step->zone_id);
            result = zone_service_close(service->zone_service, step->zone_id);
            if (result != IRRIGATION_RESULT_OK) {
                result = fault(service, result);
            } else if (state.program.current_step_index + 1U < service->program->step_count) {
                (void)state_store_set_program_running_zone(service->state_store,
                                                            state.program.current_step_index + 1U,
                                                            clock_now_ms(&service->clock));
                result = open_current_zone(service);
            } else {
                result = state_store_set_program_waiting_post_delay(service->state_store,
                                                                      clock_now_ms(&service->clock));
                printf("program_service: state WAITING_POST_PROGRAM_DELAY result=%d\n", result);
            }
        }
    } else if (state.program.state == PROGRAM_STATE_WAITING_POST_PROGRAM_DELAY &&
               clock_now_ms(&service->clock) - state.program.step_started_at_ms >=
                   service->program->post_program_delay_ms) {
        printf("program_service: request master close\n");
        result = master_valve_service_close(service->master_valve_service);
        (void)state_store_set_program_terminal(service->state_store,
                                               result == IRRIGATION_RESULT_OK ? PROGRAM_STATE_COMPLETED : PROGRAM_STATE_FAULT);
        printf("program_service: state %s result=%d\n",
               result == IRRIGATION_RESULT_OK ? "COMPLETED" : "FAULT", result);
    }
    operation_lock_release(service);
    return result;
}

IrrigationResult program_service_abort(ProgramService *service)
{
    if (service == NULL || !program_service_is_active(service) || !operation_lock_acquire(service)) {
        return IRRIGATION_RESULT_REJECTED;
    }
    IrrigationResult result = IRRIGATION_RESULT_OK;
    RuntimeState state = state_store_snapshot(service->state_store);
    printf("program_service: abort requested state=%d\n", state.program.state);
    if (state.program.state == PROGRAM_STATE_RUNNING_ZONE) {
        result = zone_service_close(service->zone_service, service->program->steps[state.program.current_step_index].zone_id);
    }
    if (result == IRRIGATION_RESULT_OK && master_valve_service_state(service->master_valve_service) !=
                                        MASTER_VALVE_STATE_CLOSED) {
        result = master_valve_service_close(service->master_valve_service);
    }
    (void)state_store_set_program_terminal(service->state_store,
                                           result == IRRIGATION_RESULT_OK ? PROGRAM_STATE_ABORTED : PROGRAM_STATE_FAULT);
    operation_lock_release(service);
    return result;
}
