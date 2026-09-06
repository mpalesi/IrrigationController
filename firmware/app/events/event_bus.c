#include "app/events/event_bus.h"

#include <string.h>

void event_bus_init(EventBus *bus, EventSubscriber subscriber, void *context)
{
    if (bus == NULL) {
        return;
    }
    memset(bus, 0, sizeof(*bus));
    if (subscriber != NULL) {
        bus->subscriptions[0] = (EventBusSubscription){.subscriber = subscriber, .context = context};
    }
}

IrrigationResult event_bus_subscribe(EventBus *bus, EventSubscriber subscriber, void *context)
{
    if (bus == NULL || subscriber == NULL) return IRRIGATION_RESULT_REJECTED;
    for (size_t index = 0U; index < EVENT_BUS_MAX_SUBSCRIBERS; ++index) {
        if (bus->subscriptions[index].subscriber == NULL) {
            bus->subscriptions[index] = (EventBusSubscription){.subscriber = subscriber, .context = context};
            return IRRIGATION_RESULT_OK;
        }
    }
    return IRRIGATION_RESULT_REJECTED;
}

IrrigationResult event_bus_publish(const EventBus *bus, const Event *event)
{
    if (bus == NULL || event == NULL) {
        return IRRIGATION_RESULT_REJECTED;
    }
    bool delivered = false;
    for (size_t index = 0U; index < EVENT_BUS_MAX_SUBSCRIBERS; ++index) {
        if (bus->subscriptions[index].subscriber != NULL) {
            bus->subscriptions[index].subscriber(event, bus->subscriptions[index].context);
            delivered = true;
        }
    }
    if (!delivered) return IRRIGATION_RESULT_REJECTED;
    return IRRIGATION_RESULT_OK;
}
