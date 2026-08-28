#pragma once

#include <stdint.h>

typedef enum {
    EVENT_TYPE_SYSTEM_READY = 0,
    EVENT_TYPE_OUTPUT_SAFE_OFF,
    EVENT_TYPE_ZONE_OPENED,
    EVENT_TYPE_ZONE_CLOSED,
    EVENT_TYPE_ZONE_TIMED_OUT,
    EVENT_TYPE_ZONE_FAULTED,
} EventType;

typedef struct {
    const char *id;
    uint64_t timestamp_ms;
    const char *source;
    EventType type;
    uint16_t version;
    const char *zone_id;
} Event;
