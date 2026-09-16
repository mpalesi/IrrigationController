#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "domain/master_valve/master_valve_driver.h"
#include "hal/master_valve/shelly_http_transport.h"

#define SHELLY_MASTER_VALVE_HOST_MAX_LENGTH 64U
#define SHELLY_MASTER_VALVE_HTTP_RESPONSE_CAPACITY 512U

typedef struct {
    const char *host;
    uint16_t port;
    uint8_t switch_id;
    uint32_t operation_timeout_ms;
} ShellyMasterValveConfiguration;

typedef struct {
    MasterValveDriver base;
    ShellyHttpTransport *transport;
    char host[SHELLY_MASTER_VALVE_HOST_MAX_LENGTH];
    uint16_t port;
    uint8_t switch_id;
    uint32_t operation_timeout_ms;
    bool configured;
    char response[SHELLY_MASTER_VALVE_HTTP_RESPONSE_CAPACITY];
} ShellyMasterValveDriver;

void shelly_master_valve_driver_init(ShellyMasterValveDriver *driver, ShellyHttpTransport *transport,
                                     ShellyMasterValveConfiguration configuration);
