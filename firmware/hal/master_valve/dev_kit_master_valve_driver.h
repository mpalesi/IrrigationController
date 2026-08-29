#pragma once

#include "domain/master_valve/master_valve_driver.h"

typedef struct {
    MasterValveDriver base;
} DevKitMasterValveDriver;

void dev_kit_master_valve_driver_init(DevKitMasterValveDriver *driver);
