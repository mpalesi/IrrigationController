#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app/configuration/configuration_manager.h"
#include "app/state/state_store.h"
#include "domain/master_valve/master_valve_service.h"
#include "domain/programs/program_service.h"
#include "domain/scheduler/scheduler_service.h"
#include "domain/zones/zone.h"
#include "domain/zones/zone_service.h"

#define DEV_KIT_WEB_MAX_PROGRAMS IRRIGATION_MAX_PROGRAMS
#define DEV_KIT_WEB_MAX_PROGRAM_STEPS IRRIGATION_MAX_PROGRAM_STEPS
#define DEV_KIT_WEB_PROGRAM_NAME_SIZE IRRIGATION_CONFIGURATION_NAME_SIZE
#define DEV_KIT_WEB_ZONE_ID_SIZE IRRIGATION_CONFIGURATION_ID_SIZE

typedef struct {
    StateStore *state_store;
    ZoneService *zone_service;
    MasterValveService *master_valve_service;
    ProgramService *program_service;
    SchedulerService *scheduler_service;
    const Zone *zones;
    size_t zone_count;
    ConfigurationManager *configuration_manager;
} DevKitWebContext;

void dev_kit_web_start(const DevKitWebContext *context);
