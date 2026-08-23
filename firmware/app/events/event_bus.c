#include "app/events/event_bus.h"

void event_bus_init(EventBus *bus, EventSubscriber subscriber, void *context)
{
    if (bus == NULL) {
        return;
    }
    bus->subscriber = subscriber;
    bus->context = context;
}

IrrigationResult event_bus_publish(const EventBus *bus, const Event *event)
{
    if (bus == NULL || event == NULL || bus->subscriber == NULL) {
        return IRRIGATION_RESULT_REJECTED;
    }
    bus->subscriber(event, bus->context);
    return IRRIGATION_RESULT_OK;
}
