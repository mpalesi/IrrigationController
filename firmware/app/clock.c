#include "app/clock.h"

#include <stddef.h>

uint64_t clock_now_ms(const Clock *clock)
{
    return (clock == NULL || clock->now_ms == NULL) ? 0U : clock->now_ms(clock->context);
}
