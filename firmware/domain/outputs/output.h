#pragma once

#include <stddef.h>

/* A logical output is stable application configuration, not a GPIO or relay mapping. */
typedef struct {
    const char *id;
    size_t driver_output_index;
} Output;
