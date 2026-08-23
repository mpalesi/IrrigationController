#pragma once

#include <stdint.h>

typedef enum {
    EVENT_TYPE_SYSTEM_READY = 0,
    EVENT_TYPE_OUTPUT_SAFE_OFF,
} EventType;

typedef struct {
    const char *id;
    uint32_t timestamp_ms;
    const char *source;
    EventType type;
    uint16_t version;
} Event;
