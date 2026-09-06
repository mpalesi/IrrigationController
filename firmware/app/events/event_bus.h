#pragma once

#include <stddef.h>

#include "app/events/event.h"
#include "app/result.h"

typedef void (*EventSubscriber)(const Event *event, void *context);

#define EVENT_BUS_MAX_SUBSCRIBERS 4U

typedef struct {
    EventSubscriber subscriber;
    void *context;
} EventBusSubscription;

typedef struct {
    EventBusSubscription subscriptions[EVENT_BUS_MAX_SUBSCRIBERS];
} EventBus;

void event_bus_init(EventBus *bus, EventSubscriber subscriber, void *context);
IrrigationResult event_bus_subscribe(EventBus *bus, EventSubscriber subscriber, void *context);
IrrigationResult event_bus_publish(const EventBus *bus, const Event *event);
