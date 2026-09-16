#include "hal/master_valve/shelly_http_transport.h"

#include <stddef.h>

IrrigationResult shelly_http_transport_get(ShellyHttpTransport *transport, const char *host,
                                           uint16_t port, const char *path, uint32_t timeout_ms,
                                           char *response, size_t response_capacity,
                                           size_t *response_length, int *http_status)
{
    if (transport == NULL || transport->vtable == NULL || transport->vtable->get == NULL ||
        host == NULL || path == NULL || response == NULL || response_capacity == 0U ||
        response_length == NULL || http_status == NULL || timeout_ms == 0U) {
        return IRRIGATION_RESULT_REJECTED;
    }
    return transport->vtable->get(transport, host, port, path, timeout_ms, response,
                                  response_capacity, response_length, http_status);
}
