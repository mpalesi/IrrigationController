#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/result.h"
#include "app/state/state_store.h"
#include "common/constants.h"
#include "domain/programs/program_service.h"

#define SCHEDULER_WEEKDAY_MASK(day) (1U << (day))

typedef struct {
    uint32_t week_index;
    uint8_t weekday;
    uint8_t hour;
    uint8_t minute;
} SchedulerTime;

typedef struct {
    const char *id;
    bool enabled;
    uint8_t weekday_mask;
    uint8_t hour;
    uint8_t minute;
    const Program *program;
} SchedulerEntry;

typedef struct {
    StateStore *state_store;
    ProgramService *program_service;
    SchedulerEntry entries[IRRIGATION_MAX_SCHEDULES];
    size_t entry_count;
} SchedulerService;

void scheduler_service_init(SchedulerService *service, StateStore *state_store,
                            ProgramService *program_service);
IrrigationResult scheduler_service_configure(SchedulerService *service, const SchedulerEntry *entries,
                                             size_t entry_count);
IrrigationResult scheduler_service_set_enabled(SchedulerService *service, size_t entry_index, bool enabled);
IrrigationResult scheduler_service_process(SchedulerService *service, SchedulerTime now);
