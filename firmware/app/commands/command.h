#pragma once

#include <stdint.h>

typedef enum {
    COMMAND_TYPE_SYSTEM_READY = 0,
    COMMAND_TYPE_OPEN_ZONE,
    COMMAND_TYPE_CLOSE_ZONE,
} CommandType;

typedef struct {
    const char *id;
    uint64_t timestamp_ms;
    const char *origin;
    CommandType type;
    uint16_t version;
    const char *zone_id;
    uint32_t duration_ms;
} Command;
