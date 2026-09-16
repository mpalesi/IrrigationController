#pragma once

#include <stddef.h>
#include <stdint.h>

#include "app/result.h"

typedef struct ShellyHttpTransport ShellyHttpTransport;

typedef struct {
    IrrigationResult (*get)(ShellyHttpTransport *transport, const char *host, uint16_t port,
                            const char *path, uint32_t timeout_ms, char *response,
                            size_t response_capacity, size_t *response_length,
                            int *http_status);
} ShellyHttpTransportVTable;

struct ShellyHttpTransport {
    const ShellyHttpTransportVTable *vtable;
};

IrrigationResult shelly_http_transport_get(ShellyHttpTransport *transport, const char *host,
                                           uint16_t port, const char *path, uint32_t timeout_ms,
                                           char *response, size_t response_capacity,
                                           size_t *response_length, int *http_status);
