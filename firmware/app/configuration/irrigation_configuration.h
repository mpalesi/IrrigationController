#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/result.h"
#include "common/constants.h"
#include "domain/zones/zone.h"

#define IRRIGATION_CONFIGURATION_SCHEMA_VERSION 1U
#define IRRIGATION_CONFIGURATION_ID_SIZE 32U
#define IRRIGATION_CONFIGURATION_NAME_SIZE 32U
#define IRRIGATION_CONFIGURATION_MAX_STEP_DURATION_MS 3600000U

typedef struct {
    char zone_id[IRRIGATION_CONFIGURATION_ID_SIZE];
    char display_name[IRRIGATION_CONFIGURATION_NAME_SIZE];
} IrrigationZoneConfiguration;

typedef struct {
    char zone_id[IRRIGATION_CONFIGURATION_ID_SIZE];
    uint32_t duration_ms;
} IrrigationProgramStepConfiguration;

typedef struct {
    char program_id[IRRIGATION_CONFIGURATION_ID_SIZE];
    char display_name[IRRIGATION_CONFIGURATION_NAME_SIZE];
    IrrigationProgramStepConfiguration steps[IRRIGATION_MAX_PROGRAM_STEPS];
    uint8_t step_count;
} IrrigationProgramConfiguration;

typedef struct {
    char schedule_id[IRRIGATION_CONFIGURATION_ID_SIZE];
    bool enabled;
    uint8_t weekday_mask;
    uint8_t hour;
    uint8_t minute;
    char program_id[IRRIGATION_CONFIGURATION_ID_SIZE];
} IrrigationScheduleConfiguration;

typedef struct {
    uint16_t schema_version;
    IrrigationZoneConfiguration zones[IRRIGATION_MAX_ZONES];
    IrrigationProgramConfiguration programs[IRRIGATION_MAX_PROGRAMS];
    IrrigationScheduleConfiguration schedules[IRRIGATION_MAX_SCHEDULES];
    uint8_t zone_count;
    uint8_t program_count;
    uint8_t schedule_count;
    uint32_t next_program_id;
    uint32_t next_schedule_id;
} IrrigationConfiguration;

IrrigationResult irrigation_configuration_validate(const IrrigationConfiguration *configuration,
                                                    const Zone *board_zones, size_t board_zone_count);
