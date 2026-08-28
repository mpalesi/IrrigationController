#pragma once

#include <stdatomic.h>

#include "app/clock.h"
#include "app/result.h"
#include "domain/master_valve/master_valve_service.h"
#include "domain/programs/program.h"
#include "domain/zones/zone_service.h"

typedef struct {
    StateStore *state_store;
    MasterValveService *master_valve_service;
    ZoneService *zone_service;
    Clock clock;
    const Program *program;
    atomic_flag operation_lock;
} ProgramService;

void program_service_init(ProgramService *service, StateStore *state_store,
                          MasterValveService *master_valve_service,
                          ZoneService *zone_service, Clock clock);
IrrigationResult program_service_start(ProgramService *service, const Program *program);
IrrigationResult program_service_process(ProgramService *service);
IrrigationResult program_service_abort(ProgramService *service);
bool program_service_is_active(const ProgramService *service);
ProgramState program_service_state(const ProgramService *service);
