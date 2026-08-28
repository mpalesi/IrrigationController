#pragma once

#include <stdatomic.h>

#include "app/clock.h"
#include "app/events/event_bus.h"
#include "app/result.h"
#include "app/state/state_store.h"
#include "domain/zones/zone_start_precondition.h"
#include "hal/outputs/output_driver.h"

typedef struct {
    StateStore *state_store;
    OutputDriver *output_driver;
    EventBus *event_bus;
    Clock clock;
    ZoneStartPrecondition start_precondition;
    atomic_flag operation_lock;
} ZoneService;

void zone_service_init(ZoneService *service, StateStore *state_store, OutputDriver *output_driver,
                       EventBus *event_bus, Clock clock, ZoneStartPrecondition start_precondition);
IrrigationResult zone_service_open(ZoneService *service, const char *zone_id, uint32_t duration_ms);
IrrigationResult zone_service_close(ZoneService *service, const char *zone_id);
IrrigationResult zone_service_safe_close(ZoneService *service);
IrrigationResult zone_service_process_timeouts(ZoneService *service);
