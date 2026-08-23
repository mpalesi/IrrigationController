#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/result.h"
#include "common/constants.h"

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

typedef struct {
    SystemState system_state;
    bool output_states[IRRIGATION_MAX_OUTPUTS];
} RuntimeState;

typedef struct {
    RuntimeState current;
} StateStore;

void state_store_init(StateStore *store);
RuntimeState state_store_snapshot(const StateStore *store);
IrrigationResult state_store_transition_system(StateStore *store, SystemState next_state);
IrrigationResult state_store_set_output(StateStore *store, size_t output_index, bool enabled);
