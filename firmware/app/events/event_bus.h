#pragma once

#include <stddef.h>

#include "app/events/event.h"
#include "app/result.h"

typedef void (*EventSubscriber)(const Event *event, void *context);

typedef struct {
    EventSubscriber subscriber;
    void *context;
} EventBus;

void event_bus_init(EventBus *bus, EventSubscriber subscriber, void *context);
IrrigationResult event_bus_publish(const EventBus *bus, const Event *event);
