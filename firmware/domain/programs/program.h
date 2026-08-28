#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    const char *zone_id;
    uint32_t duration_ms;
} ProgramStep;

typedef struct {
    const char *id;
    const ProgramStep *steps;
    size_t step_count;
    uint32_t post_program_delay_ms;
} Program;
