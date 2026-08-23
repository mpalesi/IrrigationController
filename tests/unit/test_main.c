#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "app/commands/command_bus.h"
#include "app/events/event_bus.h"
#include "app/state/state_store.h"
#include "hal/outputs/virtual_output_driver.h"

static unsigned int log_count;
static unsigned int event_count;
static unsigned int command_count;

static void test_log_sink(LogLevel level, const char *component, const char *message, void *context)
{
    (void)level;
    (void)component;
    (void)message;
    (void)context;
    log_count++;
}

static void test_event_subscriber(const Event *event, void *context)
{
    (void)event;
    (void)context;
    event_count++;
}

static IrrigationResult test_command_handler(const Command *command, void *context)
{
    (void)command;
    (void)context;
    command_count++;
    return IRRIGATION_RESULT_OK;
}

static void test_state_store(void)
{
    StateStore store;
    state_store_init(&store);
    assert(state_store_snapshot(&store).system_state == SYSTEM_STATE_BOOT);
    assert(state_store_transition_system(&store, SYSTEM_STATE_READY) == IRRIGATION_RESULT_OK);
    assert(state_store_transition_system(&store, SYSTEM_STATE_RUNNING) == IRRIGATION_RESULT_INVALID_STATE);
    assert(state_store_set_output(&store, 0U, true) == IRRIGATION_RESULT_OK);
    assert(state_store_snapshot(&store).output_states[0]);
    assert(state_store_set_output(&store, IRRIGATION_MAX_OUTPUTS, true) == IRRIGATION_RESULT_NOT_FOUND);
}

static void test_virtual_output_driver(void)
{
    Logger logger = {.sink = test_log_sink, .context = NULL};
    VirtualOutputDriver driver;
    virtual_output_driver_init(&driver, logger);
    bool enabled = true;
    assert(output_driver_get(&driver.base, 0U, &enabled) == IRRIGATION_RESULT_OK);
    assert(!enabled);
    assert(output_driver_set(&driver.base, 2U, true) == IRRIGATION_RESULT_OK);
    enabled = false;
    assert(output_driver_get(&driver.base, 2U, &enabled) == IRRIGATION_RESULT_OK);
    assert(enabled);
    assert(output_driver_initialize_safe_off(&driver.base) == IRRIGATION_RESULT_OK);
    assert(output_driver_get(&driver.base, 2U, &enabled) == IRRIGATION_RESULT_OK);
    assert(!enabled);
    assert(output_driver_set(&driver.base, IRRIGATION_MAX_OUTPUTS, true) == IRRIGATION_RESULT_NOT_FOUND);
    assert(log_count >= 2U);
}

static void test_buses(void)
{
    CommandBus command_bus;
    command_bus_init(&command_bus, test_command_handler, NULL);
    const Command command = {.id = "test", .timestamp_ms = 0U, .origin = "unit", .type = COMMAND_TYPE_SYSTEM_READY, .version = 1U};
    assert(command_bus_dispatch(&command_bus, &command) == IRRIGATION_RESULT_OK);
    assert(command_count == 1U);

    EventBus event_bus;
    event_bus_init(&event_bus, test_event_subscriber, NULL);
    const Event event = {.id = "test", .timestamp_ms = 0U, .source = "unit", .type = EVENT_TYPE_SYSTEM_READY, .version = 1U};
    assert(event_bus_publish(&event_bus, &event) == IRRIGATION_RESULT_OK);
    assert(event_count == 1U);
}

int main(void)
{
    test_state_store();
    test_virtual_output_driver();
    test_buses();
    puts("All host unit tests passed.");
    return 0;
}
