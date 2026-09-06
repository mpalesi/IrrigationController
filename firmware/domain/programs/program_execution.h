#pragma once

#include <stdbool.h>

typedef enum {
    PROGRAM_EXECUTION_ORIGIN_MANUAL = 0,
    PROGRAM_EXECUTION_ORIGIN_SCHEDULED,
} ProgramExecutionOrigin;

typedef struct {
    ProgramExecutionOrigin origin;
    const char *schedule_id;
} ProgramStartContext;

typedef enum {
    PROGRAM_EXECUTION_RESULT_COMPLETED = 0,
    PROGRAM_EXECUTION_RESULT_ABORTED,
    PROGRAM_EXECUTION_RESULT_FAULT,
    PROGRAM_EXECUTION_RESULT_INTERRUPTED_REBOOT,
} ProgramExecutionResult;

bool program_start_context_is_valid(ProgramStartContext context);
ProgramStartContext program_start_context_manual(void);
