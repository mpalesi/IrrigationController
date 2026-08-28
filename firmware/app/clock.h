#pragma once

#include <stdint.h>

typedef uint64_t (*ClockNowMs)(void *context);

typedef struct {
    ClockNowMs now_ms;
    void *context;
} Clock;

uint64_t clock_now_ms(const Clock *clock);
