#include "app/status/status_led_service.h"

#include <stddef.h>

static StatusLedState select_state(const StatusLedService *service)
{
    if (service == NULL) return STATUS_LED_STATE_BOOTING;
    if (atomic_load_explicit(&service->fault, memory_order_relaxed)) return STATUS_LED_STATE_FAULT;
    if (!atomic_load_explicit(&service->network_state_known, memory_order_relaxed)) return STATUS_LED_STATE_BOOTING;
    return atomic_load_explicit(&service->network_connected, memory_order_relaxed)
        ? STATUS_LED_STATE_READY : STATUS_LED_STATE_NETWORK_DISCONNECTED;
}

static void apply_state(StatusLedService *service)
{
    if (service != NULL) (void)status_led_driver_set_state(service->driver, select_state(service));
}

void status_led_service_init(StatusLedService *service, StatusLedDriver *driver)
{
    if (service == NULL) return;
    *service = (StatusLedService){.driver = driver};
    atomic_init(&service->network_state_known, false);
    atomic_init(&service->network_connected, false);
    atomic_init(&service->fault, false);
    apply_state(service);
}

void status_led_service_set_network_connected(StatusLedService *service, bool connected)
{
    if (service == NULL) return;
    atomic_store_explicit(&service->network_state_known, true, memory_order_relaxed);
    atomic_store_explicit(&service->network_connected, connected, memory_order_relaxed);
}

void status_led_service_set_fault(StatusLedService *service, bool fault)
{
    if (service == NULL) return;
    atomic_store_explicit(&service->fault, fault, memory_order_relaxed);
}

void status_led_service_refresh(StatusLedService *service)
{
    apply_state(service);
}

StatusLedState status_led_service_state(const StatusLedService *service)
{
    return select_state(service);
}
