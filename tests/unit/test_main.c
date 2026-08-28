#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app/clock.h"
#include "app/commands/command_bus.h"
#include "app/commands/zone_command_handler.h"
#include "app/events/event_bus.h"
#include "app/state/state_store.h"
#include "domain/outputs/output.h"
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

static void initialize_store(StateStore *store)
{
    state_store_init(store);
    assert(state_store_configure_zones(store, ZONES, 2U) == IRRIGATION_RESULT_OK);
    assert(state_store_transition_system(store, SYSTEM_STATE_READY) == IRRIGATION_RESULT_OK);
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

int main(void)
{
    test_open_close_and_authority();
    test_invalid_and_forbidden_transitions();
    test_failed_on_safe_close_and_start_lockout();
    test_failed_off_safe_close_retry();
    test_reentrant_open_cannot_actuate_second_zone();
    test_timeout_and_command_flow();
    puts("All host unit tests passed.");
    return 0;
}
