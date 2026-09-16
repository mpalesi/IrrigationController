#pragma once

#include "hal/master_valve/shelly_http_transport.h"

typedef struct {
    ShellyHttpTransport base;
} EspShellyHttpTransport;

void esp_shelly_http_transport_init(EspShellyHttpTransport *transport);
