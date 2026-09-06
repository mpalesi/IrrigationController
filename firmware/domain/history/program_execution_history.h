#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/events/event_bus.h"
#include "app/result.h"
#include "common/constants.h"
#include "domain/programs/program_execution.h"

typedef struct {
    uint64_t value_ms;
    bool valid;
} ProgramExecutionTimestamp;

typedef struct {
    char program_id[IRRIGATION_PROGRAM_HISTORY_ID_MAX_LENGTH];
    ProgramExecutionOrigin origin;
    char schedule_id[IRRIGATION_PROGRAM_HISTORY_ID_MAX_LENGTH];
    bool schedule_id_valid;
    ProgramExecutionTimestamp started_at;
    ProgramExecutionTimestamp ended_at;
    uint32_t planned_duration_ms;
    uint64_t actual_duration_ms;
    ProgramExecutionResult result;
    size_t steps_started;
    size_t steps_completed;
    char last_active_zone_id[IRRIGATION_PROGRAM_HISTORY_ID_MAX_LENGTH];
    bool last_active_zone_id_valid;
} ProgramExecutionHistoryRecord;

typedef struct {
    void *context;
    IrrigationResult (*append)(void *context, const ProgramExecutionHistoryRecord *record);
} ProgramExecutionHistoryRepository;

typedef struct {
    bool active;
    ProgramExecutionHistoryRecord record;
} ProgramExecutionHistoryInFlight;

typedef struct {
    ProgramExecutionHistoryRepository repository;
    ProgramExecutionHistoryInFlight in_flight;
    ProgramExecutionHistoryRecord records[IRRIGATION_MAX_PROGRAM_HISTORY_RECORDS];
    size_t record_count;
} ProgramExecutionHistory;

void program_execution_history_init(ProgramExecutionHistory *history,
                                    ProgramExecutionHistoryRepository repository);
IrrigationResult program_execution_history_subscribe(ProgramExecutionHistory *history, EventBus *event_bus);
void program_execution_history_handle_event(const Event *event, void *context);
IrrigationResult program_execution_history_interrupt_active(ProgramExecutionHistory *history, uint64_t ended_at_ms,
                                                            bool ended_at_valid);
const ProgramExecutionHistoryRecord *program_execution_history_record_at(const ProgramExecutionHistory *history,
                                                                           size_t index);
size_t program_execution_history_count(const ProgramExecutionHistory *history);
