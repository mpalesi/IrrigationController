#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "domain/outputs/output.h"

typedef struct {
    const char *id;
    const Output *output;
    bool enabled;
    uint32_t default_duration_ms;
} Zone;
