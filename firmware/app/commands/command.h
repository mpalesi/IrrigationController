#pragma once

#include <stdint.h>

typedef enum {
    COMMAND_TYPE_SYSTEM_READY = 0,
} CommandType;

typedef struct {
    const char *id;
    uint32_t timestamp_ms;
    const char *origin;
    CommandType type;
    uint16_t version;
} Command;
