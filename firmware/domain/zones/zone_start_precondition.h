#pragma once

#include "app/result.h"

/* Future Master Valve Service will implement this confirmation boundary. */
typedef IrrigationResult (*ZoneStartPreconditionCheck)(void *context);

typedef struct {
    ZoneStartPreconditionCheck check;
    void *context;
} ZoneStartPrecondition;

IrrigationResult zone_start_precondition_check(const ZoneStartPrecondition *precondition);
