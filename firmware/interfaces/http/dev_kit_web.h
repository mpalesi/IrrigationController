#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "app/state/state_store.h"
#include "domain/master_valve/master_valve_service.h"
#include "domain/programs/program_service.h"
#include "domain/scheduler/scheduler_service.h"
#include "domain/zones/zone.h"
#include "domain/zones/zone_service.h"

#define DEV_KIT_WEB_MAX_PROGRAMS 4U
#define DEV_KIT_WEB_MAX_PROGRAM_STEPS 8U
#define DEV_KIT_WEB_PROGRAM_NAME_SIZE 32U
#define DEV_KIT_WEB_ZONE_ID_SIZE 32U

typedef struct {
    Program programs[DEV_KIT_WEB_MAX_PROGRAMS];
    ProgramStep steps[DEV_KIT_WEB_MAX_PROGRAMS][DEV_KIT_WEB_MAX_PROGRAM_STEPS];
    char names[DEV_KIT_WEB_MAX_PROGRAMS][DEV_KIT_WEB_PROGRAM_NAME_SIZE];
    char zone_ids[DEV_KIT_WEB_MAX_PROGRAMS][DEV_KIT_WEB_MAX_PROGRAM_STEPS][DEV_KIT_WEB_ZONE_ID_SIZE];
    bool in_use[DEV_KIT_WEB_MAX_PROGRAMS];
} DevKitProgramConfiguration;

typedef struct {
    StateStore *state_store;
    ZoneService *zone_service;
    MasterValveService *master_valve_service;
    ProgramService *program_service;
    SchedulerService *scheduler_service;
    const Zone *zones;
    size_t zone_count;
    DevKitProgramConfiguration *program_configuration;
} DevKitWebContext;

void dev_kit_web_start(const DevKitWebContext *context);
