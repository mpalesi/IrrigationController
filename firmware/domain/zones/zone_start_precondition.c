#include "domain/zones/zone_start_precondition.h"

#include <stddef.h>

IrrigationResult zone_start_precondition_check(const ZoneStartPrecondition *precondition)
{
    return (precondition == NULL || precondition->check == NULL)
        ? IRRIGATION_RESULT_REJECTED
        : precondition->check(precondition->context);
}
