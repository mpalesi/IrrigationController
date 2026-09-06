#pragma once

#include <stdint.h>

#include "domain/programs/program_execution.h"

typedef enum {
    EVENT_TYPE_SYSTEM_READY = 0,
    EVENT_TYPE_OUTPUT_SAFE_OFF,
    EVENT_TYPE_ZONE_OPENED,
    EVENT_TYPE_ZONE_CLOSED,
    EVENT_TYPE_ZONE_TIMED_OUT,
    EVENT_TYPE_ZONE_FAULTED,
    EVENT_TYPE_PROGRAM_EXECUTION_STARTED,
    EVENT_TYPE_PROGRAM_EXECUTION_STEP_STARTED,
    EVENT_TYPE_PROGRAM_EXECUTION_STEP_COMPLETED,
    EVENT_TYPE_PROGRAM_EXECUTION_TERMINATED,
} EventType;

typedef struct {
    const char *id;
    uint64_t timestamp_ms;
    const char *source;
    EventType type;
    uint16_t version;
    const char *zone_id;
    const char *program_id;
    ProgramStartContext program_start_context;
    ProgramExecutionResult program_execution_result;
    size_t program_step_index;
    uint32_t program_planned_duration_ms;
} Event;
