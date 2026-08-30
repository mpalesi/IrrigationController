#include "interfaces/http/dev_kit_zone_configuration.h"

#include <stdbool.h>
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool name_is_valid(const char *name)
{
    if (name == NULL || *name == '\0' || strlen(name) >= DEV_KIT_ZONE_NAME_SIZE) {
        return false;
    }
    for (const char *character = name; *character != '\0'; ++character) {
        if (!isalnum((unsigned char)*character) && *character != ' ' && *character != '-' &&
            *character != '_') {
            return false;
        }
    }
    return true;
}

void dev_kit_zone_configuration_init(DevKitZoneConfiguration *configuration, size_t zone_count)
{
    if (configuration == NULL || zone_count > IRRIGATION_MAX_ZONES) {
        return;
    }
    *configuration = (DevKitZoneConfiguration){.zone_count = zone_count};
    for (size_t index = 0U; index < zone_count; ++index) {
        (void)snprintf(configuration->names[index], sizeof(configuration->names[index]), "Zone %u",
                       (unsigned)(index + 1U));
    }
}

const char *dev_kit_zone_configuration_name(const DevKitZoneConfiguration *configuration, size_t index)
{
    return configuration != NULL && index < configuration->zone_count ? configuration->names[index] : NULL;
}

IrrigationResult dev_kit_zone_configuration_set_name(DevKitZoneConfiguration *configuration, size_t index,
                                                      const char *name)
{
    if (configuration == NULL || index >= configuration->zone_count || !name_is_valid(name)) {
        return IRRIGATION_RESULT_REJECTED;
    }
    (void)snprintf(configuration->names[index], sizeof(configuration->names[index]), "%s", name);
    return IRRIGATION_RESULT_OK;
}
