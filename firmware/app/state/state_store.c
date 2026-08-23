#include "app/state/state_store.h"

#include <string.h>

static bool system_transition_is_allowed(SystemState from, SystemState to)
{
    if (from == to) {
        return true;
    }

    return (from == SYSTEM_STATE_BOOT && to == SYSTEM_STATE_READY) ||
           (from == SYSTEM_STATE_READY && to == SYSTEM_STATE_ERROR) ||
           (from == SYSTEM_STATE_ERROR && to == SYSTEM_STATE_BOOT);
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

IrrigationResult state_store_set_output(StateStore *store, size_t output_index, bool enabled)
{
    if (store == NULL) {
        return IRRIGATION_RESULT_INTERNAL_ERROR;
    }
    if (output_index >= IRRIGATION_MAX_OUTPUTS) {
        return IRRIGATION_RESULT_NOT_FOUND;
    }

    store->current.output_states[output_index] = enabled;
    return IRRIGATION_RESULT_OK;
}
