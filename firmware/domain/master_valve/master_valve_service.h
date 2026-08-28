#pragma once

#include <stdatomic.h>

#include "app/clock.h"
#include "app/result.h"
#include "app/state/state_store.h"
#include "domain/master_valve/master_valve_driver.h"
#include "domain/zones/zone_start_precondition.h"

typedef struct {
    uint32_t pre_open_delay_ms;
} MasterValveConfiguration;

typedef struct {
    StateStore *state_store;
    MasterValveDriver *driver;
    Clock clock;
    atomic_flag operation_lock;
} MasterValveService;

void master_valve_service_init(MasterValveService *service, StateStore *state_store,
                               MasterValveDriver *driver, Clock clock,
                               MasterValveConfiguration configuration);
IrrigationResult master_valve_service_open(MasterValveService *service);
IrrigationResult master_valve_service_close(MasterValveService *service);
IrrigationResult master_valve_service_process_time(MasterValveService *service);
IrrigationResult master_valve_service_check_zone_start(void *context);
ZoneStartPrecondition master_valve_service_zone_start_precondition(MasterValveService *service);
bool master_valve_service_close_is_eligible(const MasterValveService *service);
MasterValveState master_valve_service_state(const MasterValveService *service);
