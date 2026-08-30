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

static bool entries_are_unchanged(const SchedulerEntry *left, const SchedulerEntry *right)
{
    return strcmp(left->id, right->id) == 0 && left->enabled == right->enabled &&
           left->weekday_mask == right->weekday_mask && left->hour == right->hour &&
           left->minute == right->minute && left->program == right->program;
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
    uint64_t preserved_occurrences[IRRIGATION_MAX_SCHEDULES] = {0};
    ScheduleOccurrenceStatus preserved_statuses[IRRIGATION_MAX_SCHEDULES] = {0};
    if (service == NULL || service->state_store == NULL || (entries == NULL && entry_count != 0U) ||
        entry_count > IRRIGATION_MAX_SCHEDULES) {
        return IRRIGATION_RESULT_REJECTED;
    }
    for (size_t index = 0U; index < entry_count; ++index) {
        if (!entry_is_valid(&entries[index])) {
            return IRRIGATION_RESULT_REJECTED;
        }
        for (size_t previous = 0U; previous < index; ++previous) {
            if (strcmp(entries[previous].id, entries[index].id) == 0) {
                return IRRIGATION_RESULT_REJECTED;
            }
        }
        for (size_t previous = 0U; previous < service->entry_count; ++previous) {
            if (entries_are_unchanged(&entries[index], &service->entries[previous])) {
                (void)state_store_get_schedule_occurrence(service->state_store, previous,
                                                          &preserved_occurrences[index],
                                                          &preserved_statuses[index]);
                break;
            }
        }
    }
    IrrigationResult result = state_store_configure_scheduler(service->state_store, entry_count);
    if (result != IRRIGATION_RESULT_OK) {
        return result;
    }
    for (size_t index = 0U; index < entry_count; ++index) {
        if (preserved_statuses[index] == SCHEDULE_OCCURRENCE_NONE) {
            result = state_store_reset_schedule_occurrence(service->state_store, index);
        } else {
            result = state_store_record_schedule_occurrence(service->state_store, index,
                                                            preserved_occurrences[index],
                                                            preserved_statuses[index]);
        }
        if (result != IRRIGATION_RESULT_OK) {
            return result;
        }
    }
    memcpy(service->entries, entries, entry_count * sizeof(*entries));
    service->entry_count = entry_count;
    return IRRIGATION_RESULT_OK;
}

IrrigationResult scheduler_service_set_enabled(SchedulerService *service, size_t entry_index, bool enabled)
{
    if (service == NULL || entry_index >= service->entry_count) {
        return IRRIGATION_RESULT_NOT_FOUND;
    }
    IrrigationResult result = state_store_reset_schedule_occurrence(service->state_store, entry_index);
    if (result == IRRIGATION_RESULT_OK) {
        service->entries[entry_index].enabled = enabled;
    }
    return result;
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
            current_minute != scheduled_minute || state.scheduler.last_handled_occurrence[index] == occurrence) {
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
