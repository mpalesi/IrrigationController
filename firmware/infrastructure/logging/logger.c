#include "infrastructure/logging/logger.h"

void logger_log(const Logger *logger, LogLevel level, const char *component, const char *message)
{
    if (logger != NULL && logger->sink != NULL) {
        logger->sink(level, component, message, logger->context);
    }
}
