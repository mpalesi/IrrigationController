#include "domain/programs/program_execution.h"

#include <stddef.h>

bool program_start_context_is_valid(ProgramStartContext context)
{
    if (context.origin == PROGRAM_EXECUTION_ORIGIN_MANUAL) return context.schedule_id == NULL;
    return context.origin == PROGRAM_EXECUTION_ORIGIN_SCHEDULED && context.schedule_id != NULL &&
           context.schedule_id[0] != '\0';
}

ProgramStartContext program_start_context_manual(void)
{
    return (ProgramStartContext){.origin = PROGRAM_EXECUTION_ORIGIN_MANUAL, .schedule_id = NULL};
}
