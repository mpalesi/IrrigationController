#pragma once

#include "hal/outputs/output_driver.h"
#include "domain/master_valve/master_valve_driver.h"
#include "hal/status_led/status_led_driver.h"

OutputDriver *board_output_driver(void);
MasterValveDriver *board_master_valve_driver(void);
StatusLedDriver *board_status_led_driver(void);
