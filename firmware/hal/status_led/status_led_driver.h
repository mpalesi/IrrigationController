#pragma once

#include "app/result.h"

typedef enum {
    STATUS_LED_STATE_BOOTING = 0,
    STATUS_LED_STATE_READY,
    STATUS_LED_STATE_NETWORK_DISCONNECTED,
    STATUS_LED_STATE_FAULT,
} StatusLedState;

typedef struct StatusLedDriver StatusLedDriver;

typedef struct {
    IrrigationResult (*set_state)(StatusLedDriver *driver, StatusLedState state);
} StatusLedDriverVTable;

struct StatusLedDriver {
    const StatusLedDriverVTable *vtable;
};

IrrigationResult status_led_driver_set_state(StatusLedDriver *driver, StatusLedState state);
