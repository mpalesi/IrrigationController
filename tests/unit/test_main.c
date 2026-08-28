#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app/clock.h"
#include "app/commands/command_bus.h"
#include "app/commands/zone_command_handler.h"
#include "app/events/event_bus.h"
#include "app/state/state_store.h"
#include "domain/master_valve/master_valve_service.h"
#include "domain/outputs/output.h"
#include "domain/programs/program_service.h"
#include "domain/scheduler/scheduler_service.h"
#include "domain/zones/zone.h"
#include "domain/zones/zone_service.h"
#include "hal/outputs/virtual_output_driver.h"

typedef struct {
    uint64_t now_ms;
} FakeClock;

typedef struct {
    Event events[8];
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

static const Output OUTPUT_ONE = {.id = "output-1", .driver_output_index = 0U};
static const Output OUTPUT_TWO = {.id = "output-2", .driver_output_index = 1U};
static const Zone ZONES[] = {
    {.id = "zone-1", .output = &OUTPUT_ONE, .enabled = true, .default_duration_ms = 100U},
    {.id = "zone-2", .output = &OUTPUT_TWO, .enabled = true, .default_duration_ms = 200U},
};

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

static void test_program_faults_on_master_close_failure_and_aborts_safely(void)
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
    assert(program_service_state(&program_service) == PROGRAM_STATE_FAULT);

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

static void complete_zero_duration_program(ProgramService *service)
{
    assert(program_service_process(service) == IRRIGATION_RESULT_OK);
    assert(program_service_process(service) == IRRIGATION_RESULT_OK);
    assert(program_service_process(service) == IRRIGATION_RESULT_OK);
    assert(program_service_state(service) == PROGRAM_STATE_COMPLETED);
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
    test_program_faults_on_master_close_failure_and_aborts_safely();
    test_scheduler_recurs_without_duplicate_starts();
    test_scheduler_skips_busy_and_rejected_occurrences();
    test_scheduler_handles_midnight_rollover();
    puts("All host unit tests passed.");
    return 0;
}
