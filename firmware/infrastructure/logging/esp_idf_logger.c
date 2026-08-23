#include "infrastructure/logging/esp_idf_logger.h"

#include "esp_log.h"

static void esp_idf_log_sink(LogLevel level, const char *component, const char *message, void *context)
{
    (void)context;
    switch (level) {
    case LOG_LEVEL_DEBUG: ESP_LOGD(component, "%s", message); break;
    case LOG_LEVEL_INFO: ESP_LOGI(component, "%s", message); break;
    case LOG_LEVEL_WARNING: ESP_LOGW(component, "%s", message); break;
    case LOG_LEVEL_ERROR: ESP_LOGE(component, "%s", message); break;
    case LOG_LEVEL_CRITICAL: ESP_LOGE(component, "CRITICAL: %s", message); break;
    }
}

Logger esp_idf_logger_create(void)
{
    return (Logger){.sink = esp_idf_log_sink, .context = NULL};
}
