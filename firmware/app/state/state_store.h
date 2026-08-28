#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/result.h"
#include "common/constants.h"
#include "domain/zones/zone.h"

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

typedef enum {
    ZONE_STATE_DISABLED = 0,
    ZONE_STATE_IDLE,
    ZONE_STATE_OPENING,
    ZONE_STATE_OPEN,
    ZONE_STATE_CLOSING,
    ZONE_STATE_FAULT,
} ZoneState;

typedef enum {
    OUTPUT_STATE_OFF = 0,
    OUTPUT_STATE_ON,
    OUTPUT_STATE_UNKNOWN,
} OutputState;

typedef struct {
    const char *zone_id;
    const char *output_id;
    size_t output_index;
    ZoneState state;
    uint64_t opened_at_ms;
    uint32_t duration_ms;
} ZoneRuntimeState;

typedef struct {
    SystemState system_state;
    OutputState output_states[IRRIGATION_MAX_OUTPUTS];
    ZoneRuntimeState zones[IRRIGATION_MAX_ZONES];
    size_t zone_count;
} RuntimeState;

typedef struct {
    RuntimeState current;
} StateStore;

void state_store_init(StateStore *store);
IrrigationResult state_store_configure_zones(StateStore *store, const Zone *zones, size_t zone_count);
RuntimeState state_store_snapshot(const StateStore *store);
IrrigationResult state_store_transition_system(StateStore *store, SystemState next_state);
IrrigationResult state_store_get_zone(const StateStore *store, const char *zone_id, ZoneRuntimeState *zone);
IrrigationResult state_store_prepare_zone_open(const StateStore *store, const char *zone_id, size_t *output_index,
                                                uint32_t *duration_ms);
IrrigationResult state_store_open_zone(StateStore *store, const char *zone_id, uint64_t opened_at_ms,
                                       uint32_t duration_ms);
IrrigationResult state_store_close_zone(StateStore *store, const char *zone_id);
IrrigationResult state_store_record_zone_actuation_fault(StateStore *store, const char *zone_id,
                                                          OutputState output_state);
