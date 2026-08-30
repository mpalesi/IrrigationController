#pragma once

#include <stddef.h>

#include "app/result.h"
#include "common/constants.h"

#define DEV_KIT_ZONE_NAME_SIZE 32U

typedef struct {
    char names[IRRIGATION_MAX_ZONES][DEV_KIT_ZONE_NAME_SIZE];
    size_t zone_count;
} DevKitZoneConfiguration;

void dev_kit_zone_configuration_init(DevKitZoneConfiguration *configuration, size_t zone_count);
const char *dev_kit_zone_configuration_name(const DevKitZoneConfiguration *configuration, size_t index);
IrrigationResult dev_kit_zone_configuration_set_name(DevKitZoneConfiguration *configuration, size_t index,
                                                      const char *name);
