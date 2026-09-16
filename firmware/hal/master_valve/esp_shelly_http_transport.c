#include "hal/master_valve/esp_shelly_http_transport.h"

#include <stdbool.h>
#include <string.h>

#include "esp_err.h"
#include "esp_http_client.h"

typedef struct {
    char *response;
    size_t capacity;
    size_t length;
    bool overflow;
} ResponseBuffer;

static esp_err_t response_event_handler(esp_http_client_event_t *event)
{
    ResponseBuffer *buffer = event->user_data;
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        const size_t available = buffer->capacity > buffer->length ? buffer->capacity - buffer->length : 0U;
        if ((size_t)event->data_len > available) {
            buffer->overflow = true;
        } else {
            memcpy(buffer->response + buffer->length, event->data, (size_t)event->data_len);
            buffer->length += (size_t)event->data_len;
        }
    }
    return ESP_OK;
}

static IrrigationResult esp_get(ShellyHttpTransport *base, const char *host, uint16_t port,
                                const char *path, uint32_t timeout_ms, char *response,
                                size_t response_capacity, size_t *response_length, int *http_status)
{
    (void)base;
    ResponseBuffer buffer = {.response = response, .capacity = response_capacity - 1U};
    esp_http_client_config_t config = {
        .host = host,
        .port = port,
        .path = path,
        .method = HTTP_METHOD_GET,
        .timeout_ms = (int)timeout_ms,
        .event_handler = response_event_handler,
        .user_data = &buffer,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) return IRRIGATION_RESULT_INTERNAL_ERROR;
    const esp_err_t result = esp_http_client_perform(client);
    if (result == ESP_OK && !buffer.overflow) {
        *http_status = esp_http_client_get_status_code(client);
        *response_length = buffer.length;
    }
    esp_http_client_cleanup(client);
    if (result == ESP_ERR_TIMEOUT) return IRRIGATION_RESULT_TIMEOUT;
    return result == ESP_OK && !buffer.overflow ? IRRIGATION_RESULT_OK : IRRIGATION_RESULT_INTERNAL_ERROR;
}

static const ShellyHttpTransportVTable ESP_SHELLY_HTTP_TRANSPORT_VTABLE = {
    .get = esp_get,
};

void esp_shelly_http_transport_init(EspShellyHttpTransport *transport)
{
    if (transport != NULL) *transport = (EspShellyHttpTransport){.base.vtable = &ESP_SHELLY_HTTP_TRANSPORT_VTABLE};
}
