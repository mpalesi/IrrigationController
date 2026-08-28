#pragma once

#include "app/result.h"

typedef struct MasterValveDriver MasterValveDriver;

typedef struct {
    /* Each operation returns OK only after the remote valve action is confirmed. */
    IrrigationResult (*open_and_confirm)(MasterValveDriver *driver);
    IrrigationResult (*close_and_confirm)(MasterValveDriver *driver);
} MasterValveDriverVTable;

struct MasterValveDriver {
    const MasterValveDriverVTable *vtable;
};

IrrigationResult master_valve_driver_open_and_confirm(MasterValveDriver *driver);
IrrigationResult master_valve_driver_close_and_confirm(MasterValveDriver *driver);
