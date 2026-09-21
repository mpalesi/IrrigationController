#pragma once

#include <stdbool.h>
#include <stdatomic.h>

#include "hal/status_led/status_led_driver.h"

typedef struct {
    StatusLedDriver *driver;
    atomic_bool network_state_known;
    atomic_bool network_connected;
    atomic_bool fault;
} StatusLedService;

void status_led_service_init(StatusLedService *service, StatusLedDriver *driver);
void status_led_service_set_network_connected(StatusLedService *service, bool connected);
void status_led_service_set_fault(StatusLedService *service, bool fault);
void status_led_service_refresh(StatusLedService *service);
StatusLedState status_led_service_state(const StatusLedService *service);
