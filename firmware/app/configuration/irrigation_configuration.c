#include "app/configuration/irrigation_configuration.h"

#include <ctype.h>
#include <string.h>

static bool text_is_valid(const char *text, size_t maximum_length)
{
    if (text == NULL || *text == '\0' || strnlen(text, maximum_length) >= maximum_length) {
        return false;
    }
    for (const char *character = text; *character != '\0'; ++character) {
        if (!isalnum((unsigned char)*character) && *character != ' ' && *character != '-' &&
            *character != '_') {
            return false;
        }
    }
    return true;
}

static bool board_zone_exists(const Zone *board_zones, size_t board_zone_count, const char *zone_id)
{
    for (size_t index = 0U; index < board_zone_count; ++index) {
        if (strcmp(board_zones[index].id, zone_id) == 0) {
            return true;
        }
    }
    return false;
}

static bool program_exists(const IrrigationConfiguration *configuration, const char *program_id)
{
    for (size_t index = 0U; index < configuration->program_count; ++index) {
        if (strcmp(configuration->programs[index].program_id, program_id) == 0) {
            return true;
        }
    }
    return false;
}

IrrigationResult irrigation_configuration_validate(const IrrigationConfiguration *configuration,
                                                    const Zone *board_zones, size_t board_zone_count)
{
    if (configuration == NULL || board_zones == NULL || configuration->schema_version != IRRIGATION_CONFIGURATION_SCHEMA_VERSION ||
        board_zone_count > IRRIGATION_MAX_ZONES || configuration->zone_count != board_zone_count ||
        configuration->program_count > IRRIGATION_MAX_PROGRAMS ||
        configuration->schedule_count > IRRIGATION_MAX_SCHEDULES) {
        return IRRIGATION_RESULT_REJECTED;
    }
    for (size_t index = 0U; index < configuration->zone_count; ++index) {
        const IrrigationZoneConfiguration *zone = &configuration->zones[index];
        if (!text_is_valid(zone->zone_id, sizeof(zone->zone_id)) ||
            !text_is_valid(zone->display_name, sizeof(zone->display_name)) ||
            !board_zone_exists(board_zones, board_zone_count, zone->zone_id)) {
            return IRRIGATION_RESULT_REJECTED;
        }
        for (size_t previous = 0U; previous < index; ++previous) {
            if (strcmp(configuration->zones[previous].zone_id, zone->zone_id) == 0) {
                return IRRIGATION_RESULT_REJECTED;
            }
        }
    }
    for (size_t index = 0U; index < configuration->program_count; ++index) {
        const IrrigationProgramConfiguration *program = &configuration->programs[index];
        if (!text_is_valid(program->program_id, sizeof(program->program_id)) ||
            !text_is_valid(program->display_name, sizeof(program->display_name)) ||
            program->step_count > IRRIGATION_MAX_PROGRAM_STEPS) {
            return IRRIGATION_RESULT_REJECTED;
        }
        for (size_t previous = 0U; previous < index; ++previous) {
            if (strcmp(configuration->programs[previous].program_id, program->program_id) == 0) {
                return IRRIGATION_RESULT_REJECTED;
            }
        }
        for (size_t step_index = 0U; step_index < program->step_count; ++step_index) {
            const IrrigationProgramStepConfiguration *step = &program->steps[step_index];
            if (!text_is_valid(step->zone_id, sizeof(step->zone_id)) ||
                !board_zone_exists(board_zones, board_zone_count, step->zone_id) ||
                step->duration_ms == 0U || step->duration_ms > IRRIGATION_CONFIGURATION_MAX_STEP_DURATION_MS) {
                return IRRIGATION_RESULT_REJECTED;
            }
        }
    }
    for (size_t index = 0U; index < configuration->schedule_count; ++index) {
        const IrrigationScheduleConfiguration *schedule = &configuration->schedules[index];
        if (!text_is_valid(schedule->schedule_id, sizeof(schedule->schedule_id)) ||
            !text_is_valid(schedule->program_id, sizeof(schedule->program_id)) ||
            !program_exists(configuration, schedule->program_id) || schedule->weekday_mask == 0U ||
            (schedule->weekday_mask & ~((1U << 7U) - 1U)) != 0U || schedule->hour >= 24U ||
            schedule->minute >= 60U) {
            return IRRIGATION_RESULT_REJECTED;
        }
        for (size_t previous = 0U; previous < index; ++previous) {
            if (strcmp(configuration->schedules[previous].schedule_id, schedule->schedule_id) == 0) {
                return IRRIGATION_RESULT_REJECTED;
            }
        }
    }
    return IRRIGATION_RESULT_OK;
}
