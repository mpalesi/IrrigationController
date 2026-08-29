#pragma once

#include <stddef.h>

#include "app/state/state_store.h"
#include "domain/master_valve/master_valve_service.h"
#include "domain/programs/program_service.h"
#include "domain/scheduler/scheduler_service.h"
#include "domain/zones/zone.h"
#include "domain/zones/zone_service.h"

typedef struct {
    StateStore *state_store;
    ZoneService *zone_service;
    MasterValveService *master_valve_service;
    ProgramService *program_service;
    SchedulerService *scheduler_service;
    const Zone *zones;
    size_t zone_count;
    const Program *programs;
    size_t program_count;
} DevKitWebContext;

void dev_kit_web_start(const DevKitWebContext *context);
