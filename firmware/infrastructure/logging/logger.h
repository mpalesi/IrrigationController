#pragma once

#include <stddef.h>

typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARNING,
    LOG_LEVEL_ERROR,
    LOG_LEVEL_CRITICAL,
} LogLevel;

typedef void (*LogSink)(LogLevel level, const char *component, const char *message, void *context);

typedef struct {
    LogSink sink;
    void *context;
} Logger;

void logger_log(const Logger *logger, LogLevel level, const char *component, const char *message);
