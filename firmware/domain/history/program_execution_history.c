#include "domain/history/program_execution_history.h"

#include <string.h>

static void copy_id(char destination[IRRIGATION_PROGRAM_HISTORY_ID_MAX_LENGTH], const char *source)
{
    if (source == NULL) { destination[0] = '\0'; return; }
    (void)strncpy(destination, source, IRRIGATION_PROGRAM_HISTORY_ID_MAX_LENGTH - 1U);
    destination[IRRIGATION_PROGRAM_HISTORY_ID_MAX_LENGTH - 1U] = '\0';
}

static IrrigationResult append_terminal_record(ProgramExecutionHistory *history)
{
    if (history == NULL || !history->in_flight.active ||
        history->record_count >= IRRIGATION_MAX_PROGRAM_HISTORY_RECORDS) return IRRIGATION_RESULT_REJECTED;
    const ProgramExecutionHistoryRecord *record = &history->in_flight.record;
    if (history->repository.append != NULL) {
        const IrrigationResult result = history->repository.append(history->repository.context, record);
        if (result != IRRIGATION_RESULT_OK) return result;
    }
    history->records[history->record_count++] = *record;
    history->in_flight = (ProgramExecutionHistoryInFlight){0};
    return IRRIGATION_RESULT_OK;
}

void program_execution_history_init(ProgramExecutionHistory *history,
                                    ProgramExecutionHistoryRepository repository)
{
    if (history != NULL) *history = (ProgramExecutionHistory){.repository = repository};
}

IrrigationResult program_execution_history_subscribe(ProgramExecutionHistory *history, EventBus *event_bus)
{
    return history == NULL ? IRRIGATION_RESULT_REJECTED :
           event_bus_subscribe(event_bus, program_execution_history_handle_event, history);
}

void program_execution_history_handle_event(const Event *event, void *context)
{
    ProgramExecutionHistory *history = context;
    if (history == NULL || event == NULL) return;
    if (event->type == EVENT_TYPE_PROGRAM_EXECUTION_STARTED) {
        if (history->in_flight.active) return;
        ProgramExecutionHistoryRecord *record = &history->in_flight.record;
        *record = (ProgramExecutionHistoryRecord){.origin = event->program_start_context.origin,
                                                    .started_at = {.value_ms = event->timestamp_ms, .valid = true},
                                                    .planned_duration_ms = event->program_planned_duration_ms};
        copy_id(record->program_id, event->program_id);
        record->schedule_id_valid = event->program_start_context.schedule_id != NULL;
        if (record->schedule_id_valid) copy_id(record->schedule_id, event->program_start_context.schedule_id);
        history->in_flight.active = true;
    } else if (event->type == EVENT_TYPE_PROGRAM_EXECUTION_STEP_STARTED && history->in_flight.active) {
        history->in_flight.record.steps_started++;
        history->in_flight.record.last_active_zone_id_valid = event->zone_id != NULL;
        if (event->zone_id != NULL) copy_id(history->in_flight.record.last_active_zone_id, event->zone_id);
    } else if (event->type == EVENT_TYPE_PROGRAM_EXECUTION_STEP_COMPLETED && history->in_flight.active) {
        history->in_flight.record.steps_completed++;
    } else if (event->type == EVENT_TYPE_PROGRAM_EXECUTION_TERMINATED && history->in_flight.active) {
        ProgramExecutionHistoryRecord *record = &history->in_flight.record;
        record->ended_at = (ProgramExecutionTimestamp){.value_ms = event->timestamp_ms, .valid = true};
        record->actual_duration_ms = event->timestamp_ms - record->started_at.value_ms;
        record->result = event->program_execution_result;
        (void)append_terminal_record(history);
    }
}

IrrigationResult program_execution_history_interrupt_active(ProgramExecutionHistory *history, uint64_t ended_at_ms,
                                                            bool ended_at_valid)
{
    if (history == NULL || !history->in_flight.active) return IRRIGATION_RESULT_NOT_FOUND;
    ProgramExecutionHistoryRecord *record = &history->in_flight.record;
    record->ended_at = (ProgramExecutionTimestamp){.value_ms = ended_at_ms, .valid = ended_at_valid};
    record->actual_duration_ms = ended_at_valid && record->started_at.valid ? ended_at_ms - record->started_at.value_ms : 0U;
    record->result = PROGRAM_EXECUTION_RESULT_INTERRUPTED_REBOOT;
    return append_terminal_record(history);
}

const ProgramExecutionHistoryRecord *program_execution_history_record_at(const ProgramExecutionHistory *history,
                                                                           size_t index)
{
    return history != NULL && index < history->record_count ? &history->records[index] : NULL;
}

size_t program_execution_history_count(const ProgramExecutionHistory *history)
{
    return history == NULL ? 0U : history->record_count;
}
