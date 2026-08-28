#include "domain/scheduler/scheduler_service.h"

#include <string.h>

static bool scheduler_time_is_valid(SchedulerTime time)
{
    return time.weekday < 7U && time.hour < 24U && time.minute < 60U;
}

static bool entry_is_valid(const SchedulerEntry *entry)
{
    return entry != NULL && entry->id != NULL && entry->weekday_mask != 0U &&
           (entry->weekday_mask & ~((1U << 7U) - 1U)) == 0U && entry->hour < 24U &&
           entry->minute < 60U && entry->program != NULL;
}

static uint64_t occurrence_for(SchedulerTime time)
{
    return (uint64_t)time.week_index * 7U + time.weekday;
}

void scheduler_service_init(SchedulerService *service, StateStore *state_store,
                            ProgramService *program_service)
{
    if (service != NULL) {
        *service = (SchedulerService){.state_store = state_store, .program_service = program_service};
    }
}

IrrigationResult scheduler_service_configure(SchedulerService *service, const SchedulerEntry *entries,
                                             size_t entry_count)
{
    if (service == NULL || service->state_store == NULL || (entries == NULL && entry_count != 0U) ||
        entry_count > IRRIGATION_MAX_SCHEDULES) {
        return IRRIGATION_RESULT_REJECTED;
    }
    for (size_t index = 0U; index < entry_count; ++index) {
        if (!entry_is_valid(&entries[index])) {
            return IRRIGATION_RESULT_REJECTED;
        }
    }
    memcpy(service->entries, entries, entry_count * sizeof(*entries));
    service->entry_count = entry_count;
    return state_store_configure_scheduler(service->state_store, entry_count);
}

IrrigationResult scheduler_service_process(SchedulerService *service, SchedulerTime now)
{
    if (service == NULL || service->state_store == NULL || service->program_service == NULL ||
        !scheduler_time_is_valid(now)) {
        return IRRIGATION_RESULT_REJECTED;
    }
    const uint16_t current_minute = (uint16_t)now.hour * 60U + now.minute;
    const uint64_t occurrence = occurrence_for(now);
    RuntimeState state = state_store_snapshot(service->state_store);
    for (size_t index = 0U; index < service->entry_count; ++index) {
        const SchedulerEntry *entry = &service->entries[index];
        const uint16_t scheduled_minute = (uint16_t)entry->hour * 60U + entry->minute;
        if (!entry->enabled || (entry->weekday_mask & SCHEDULER_WEEKDAY_MASK(now.weekday)) == 0U ||
            current_minute < scheduled_minute || state.scheduler.last_handled_occurrence[index] == occurrence) {
            continue;
        }
        ScheduleOccurrenceStatus status;
        if (program_service_is_active(service->program_service)) {
            status = SCHEDULE_OCCURRENCE_SKIPPED_BUSY;
        } else {
            status = program_service_start(service->program_service, entry->program) == IRRIGATION_RESULT_OK
                ? SCHEDULE_OCCURRENCE_STARTED
                : SCHEDULE_OCCURRENCE_REJECTED;
        }
        (void)state_store_record_schedule_occurrence(service->state_store, index, occurrence, status);
        state.scheduler.last_handled_occurrence[index] = occurrence;
    }
    return IRRIGATION_RESULT_OK;
}
