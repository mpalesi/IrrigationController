#include "infrastructure/persistence/configuration_repository.h"

#include <string.h>

typedef enum { SLOT_EMPTY, SLOT_VALID, SLOT_CORRUPT, SLOT_INCOMPATIBLE, SLOT_STORAGE_ERROR } SlotState;

typedef struct {
    SlotState state;
    uint32_t generation;
} Slot;

static uint32_t crc32(const uint8_t *data, size_t length)
{
    uint32_t crc = 0xffffffffU;
    for (size_t index = 0U; index < length; ++index) {
        crc ^= data[index];
        for (uint8_t bit = 0U; bit < 8U; ++bit) crc = (crc >> 1U) ^ (0xedb88320U & -(crc & 1U));
    }
    return ~crc;
}

static void put_u16(uint8_t **cursor, uint16_t value) { *(*cursor)++ = (uint8_t)value; *(*cursor)++ = (uint8_t)(value >> 8U); }
static void put_u32(uint8_t **cursor, uint32_t value) { for (uint8_t index = 0U; index < 4U; ++index) *(*cursor)++ = (uint8_t)(value >> (8U * index)); }
static uint16_t get_u16(const uint8_t **cursor) { uint16_t value = (uint16_t)(*cursor)[0] | ((uint16_t)(*cursor)[1] << 8U); *cursor += 2U; return value; }
static uint32_t get_u32(const uint8_t **cursor) { uint32_t value = 0U; for (uint8_t index = 0U; index < 4U; ++index) value |= (uint32_t)(*cursor)[index] << (8U * index); *cursor += 4U; return value; }

static bool put_text(uint8_t **cursor, const char *text)
{
    const size_t length = strnlen(text, CONFIGURATION_REPOSITORY_MAX_TEXT_SIZE);
    if (length == 0U || length >= CONFIGURATION_REPOSITORY_MAX_TEXT_SIZE) return false;
    *(*cursor)++ = (uint8_t)length;
    memcpy(*cursor, text, length);
    *cursor += length;
    return true;
}

static bool get_text(const uint8_t **cursor, const uint8_t *end, char *text, size_t capacity)
{
    if (*cursor >= end) return false;
    const uint8_t length = *(*cursor)++;
    if (length == 0U || length >= capacity || (size_t)(end - *cursor) < length) return false;
    memcpy(text, *cursor, length);
    text[length] = '\0';
    *cursor += length;
    return true;
}

void configuration_repository_init(ConfigurationRepository *repository, ConfigurationRepositoryStorage storage)
{
    if (repository != NULL) *repository = (ConfigurationRepository){.storage = storage};
}

ConfigurationRepositoryResult configuration_repository_encode(const IrrigationConfiguration *configuration,
                                                               uint32_t generation, uint8_t *blob,
                                                               size_t capacity, size_t *blob_length)
{
    if (configuration == NULL || blob == NULL || blob_length == NULL || capacity < CONFIGURATION_REPOSITORY_MAX_BLOB_SIZE ||
        configuration->zone_count > IRRIGATION_MAX_ZONES || configuration->program_count > IRRIGATION_MAX_PROGRAMS ||
        configuration->schedule_count > IRRIGATION_MAX_SCHEDULES) return CONFIGURATION_REPOSITORY_INVALID_CONFIGURATION;
    uint8_t *payload = blob + CONFIGURATION_REPOSITORY_HEADER_SIZE;
    uint8_t *cursor = payload;
    const uint8_t *end = blob + capacity;
    if (configuration->schema_version != IRRIGATION_CONFIGURATION_SCHEMA_VERSION) return CONFIGURATION_REPOSITORY_INCOMPATIBLE;
    put_u16(&cursor, configuration->schema_version);
    *cursor++ = configuration->zone_count; *cursor++ = configuration->program_count; *cursor++ = configuration->schedule_count;
    put_u32(&cursor, configuration->next_program_id); put_u32(&cursor, configuration->next_schedule_id);
    for (size_t index = 0U; index < configuration->zone_count; ++index) if (!put_text(&cursor, configuration->zones[index].zone_id) || !put_text(&cursor, configuration->zones[index].display_name)) return CONFIGURATION_REPOSITORY_INVALID_CONFIGURATION;
    for (size_t index = 0U; index < configuration->program_count; ++index) {
        const IrrigationProgramConfiguration *program = &configuration->programs[index];
        if (!put_text(&cursor, program->program_id) || !put_text(&cursor, program->display_name)) return CONFIGURATION_REPOSITORY_INVALID_CONFIGURATION;
        *cursor++ = program->step_count;
        for (size_t step = 0U; step < program->step_count; ++step) { if (!put_text(&cursor, program->steps[step].zone_id)) return CONFIGURATION_REPOSITORY_INVALID_CONFIGURATION; put_u32(&cursor, program->steps[step].duration_ms); }
    }
    for (size_t index = 0U; index < configuration->schedule_count; ++index) {
        const IrrigationScheduleConfiguration *schedule = &configuration->schedules[index];
        if (!put_text(&cursor, schedule->schedule_id)) return CONFIGURATION_REPOSITORY_INVALID_CONFIGURATION;
        *cursor++ = schedule->enabled ? 1U : 0U; *cursor++ = schedule->weekday_mask; *cursor++ = schedule->hour; *cursor++ = schedule->minute;
        if (!put_text(&cursor, schedule->program_id)) return CONFIGURATION_REPOSITORY_INVALID_CONFIGURATION;
    }
    if (cursor > end || (size_t)(cursor - payload) > UINT16_MAX) return CONFIGURATION_REPOSITORY_INVALID_CONFIGURATION;
    const uint16_t payload_length = (uint16_t)(cursor - payload);
    uint8_t *header = blob;
    put_u32(&header, CONFIGURATION_REPOSITORY_MAGIC); put_u16(&header, IRRIGATION_CONFIGURATION_SCHEMA_VERSION);
    put_u32(&header, generation); put_u16(&header, payload_length); put_u32(&header, crc32(payload, payload_length));
    *blob_length = CONFIGURATION_REPOSITORY_HEADER_SIZE + payload_length;
    return CONFIGURATION_REPOSITORY_OK;
}

ConfigurationRepositoryResult configuration_repository_decode(const uint8_t *blob, size_t blob_length,
                                                               IrrigationConfiguration *configuration,
                                                               uint32_t *generation, const Zone *board_zones,
                                                               size_t board_zone_count)
{
    if (blob == NULL || configuration == NULL || generation == NULL || blob_length < CONFIGURATION_REPOSITORY_HEADER_SIZE) return CONFIGURATION_REPOSITORY_CORRUPT;
    const uint8_t *cursor = blob;
    if (get_u32(&cursor) != CONFIGURATION_REPOSITORY_MAGIC) return CONFIGURATION_REPOSITORY_CORRUPT;
    /* Future schemas are handled by a migration path selected here before decoding. */
    if (get_u16(&cursor) != IRRIGATION_CONFIGURATION_SCHEMA_VERSION) return CONFIGURATION_REPOSITORY_INCOMPATIBLE;
    *generation = get_u32(&cursor);
    const uint16_t payload_length = get_u16(&cursor);
    const uint32_t stored_crc = get_u32(&cursor);
    if (payload_length > CONFIGURATION_REPOSITORY_MAX_PAYLOAD_SIZE || blob_length != CONFIGURATION_REPOSITORY_HEADER_SIZE + payload_length || crc32(cursor, payload_length) != stored_crc) return CONFIGURATION_REPOSITORY_CORRUPT;
    const uint8_t *end = cursor + payload_length;
    IrrigationConfiguration *decoded = configuration;
    memset(decoded, 0, sizeof(*decoded));
    if ((size_t)(end - cursor) < 13U) return CONFIGURATION_REPOSITORY_CORRUPT;
    decoded->schema_version = get_u16(&cursor); decoded->zone_count = *cursor++; decoded->program_count = *cursor++; decoded->schedule_count = *cursor++;
    decoded->next_program_id = get_u32(&cursor); decoded->next_schedule_id = get_u32(&cursor);
    if (decoded->schema_version != IRRIGATION_CONFIGURATION_SCHEMA_VERSION || decoded->zone_count > IRRIGATION_MAX_ZONES || decoded->program_count > IRRIGATION_MAX_PROGRAMS || decoded->schedule_count > IRRIGATION_MAX_SCHEDULES) return CONFIGURATION_REPOSITORY_INCOMPATIBLE;
    for (size_t index = 0U; index < decoded->zone_count; ++index) if (!get_text(&cursor, end, decoded->zones[index].zone_id, sizeof(decoded->zones[index].zone_id)) || !get_text(&cursor, end, decoded->zones[index].display_name, sizeof(decoded->zones[index].display_name))) return CONFIGURATION_REPOSITORY_CORRUPT;
    for (size_t index = 0U; index < decoded->program_count; ++index) {
        IrrigationProgramConfiguration *program = &decoded->programs[index];
        if (!get_text(&cursor, end, program->program_id, sizeof(program->program_id)) || !get_text(&cursor, end, program->display_name, sizeof(program->display_name)) || cursor >= end) return CONFIGURATION_REPOSITORY_CORRUPT;
        program->step_count = *cursor++;
        if (program->step_count > IRRIGATION_MAX_PROGRAM_STEPS) return CONFIGURATION_REPOSITORY_CORRUPT;
        for (size_t step = 0U; step < program->step_count; ++step) { if (!get_text(&cursor, end, program->steps[step].zone_id, sizeof(program->steps[step].zone_id)) || (size_t)(end - cursor) < 4U) return CONFIGURATION_REPOSITORY_CORRUPT; program->steps[step].duration_ms = get_u32(&cursor); }
    }
    for (size_t index = 0U; index < decoded->schedule_count; ++index) {
        IrrigationScheduleConfiguration *schedule = &decoded->schedules[index];
        if (!get_text(&cursor, end, schedule->schedule_id, sizeof(schedule->schedule_id)) || (size_t)(end - cursor) < 4U) return CONFIGURATION_REPOSITORY_CORRUPT;
        schedule->enabled = *cursor++ != 0U; schedule->weekday_mask = *cursor++; schedule->hour = *cursor++; schedule->minute = *cursor++;
        if (!get_text(&cursor, end, schedule->program_id, sizeof(schedule->program_id))) return CONFIGURATION_REPOSITORY_CORRUPT;
    }
    if (cursor != end) return CONFIGURATION_REPOSITORY_CORRUPT;
    if (irrigation_configuration_validate(decoded, board_zones, board_zone_count) != IRRIGATION_RESULT_OK) return CONFIGURATION_REPOSITORY_INVALID_CONFIGURATION;
    return CONFIGURATION_REPOSITORY_OK;
}

static Slot read_slot(ConfigurationRepository *repository, size_t index, const char *key, const Zone *zones, size_t zone_count)
{
    Slot slot = {0}; size_t length = 0U;
    ConfigurationRepositoryResult result = repository->storage.read(repository->storage.context, key, repository->slot_buffers[index], sizeof(repository->slot_buffers[index]), &length);
    if (result == CONFIGURATION_REPOSITORY_NOT_FOUND) { slot.state = SLOT_EMPTY; return slot; }
    if (result != CONFIGURATION_REPOSITORY_OK) { slot.state = SLOT_STORAGE_ERROR; return slot; }
    result = configuration_repository_decode(repository->slot_buffers[index], length,
                                             &repository->slot_configurations[index], &slot.generation,
                                             zones, zone_count);
    slot.state = result == CONFIGURATION_REPOSITORY_OK ? SLOT_VALID : (result == CONFIGURATION_REPOSITORY_INCOMPATIBLE ? SLOT_INCOMPATIBLE : SLOT_CORRUPT);
    return slot;
}

ConfigurationRepositoryResult configuration_repository_load(ConfigurationRepository *repository, IrrigationConfiguration *configuration, uint32_t *generation, const Zone *board_zones, size_t board_zone_count)
{
    if (repository == NULL || configuration == NULL || generation == NULL || repository->storage.read == NULL) return CONFIGURATION_REPOSITORY_STORAGE_ERROR;
    Slot a = read_slot(repository, 0U, CONFIGURATION_REPOSITORY_SLOT_A, board_zones, board_zone_count);
    Slot b = read_slot(repository, 1U, CONFIGURATION_REPOSITORY_SLOT_B, board_zones, board_zone_count);
    const Slot *selected = a.state == SLOT_VALID && (b.state != SLOT_VALID || a.generation >= b.generation) ? &a : (b.state == SLOT_VALID ? &b : NULL);
    if (selected != NULL) {
        const size_t selected_index = selected == &a ? 0U : 1U;
        *configuration = repository->slot_configurations[selected_index];
        *generation = selected->generation;
        return CONFIGURATION_REPOSITORY_OK;
    }
    if (a.state == SLOT_EMPTY && b.state == SLOT_EMPTY) return CONFIGURATION_REPOSITORY_NOT_FOUND;
    if (a.state == SLOT_INCOMPATIBLE || b.state == SLOT_INCOMPATIBLE) return CONFIGURATION_REPOSITORY_INCOMPATIBLE;
    if (a.state == SLOT_STORAGE_ERROR || b.state == SLOT_STORAGE_ERROR) return CONFIGURATION_REPOSITORY_STORAGE_ERROR;
    return CONFIGURATION_REPOSITORY_CORRUPT;
}

ConfigurationRepositoryResult configuration_repository_save(ConfigurationRepository *repository, const IrrigationConfiguration *configuration, const Zone *board_zones, size_t board_zone_count)
{
    if (repository == NULL || configuration == NULL || repository->storage.read == NULL || repository->storage.write == NULL || repository->storage.commit == NULL) return CONFIGURATION_REPOSITORY_STORAGE_ERROR;
    if (irrigation_configuration_validate(configuration, board_zones, board_zone_count) != IRRIGATION_RESULT_OK) return CONFIGURATION_REPOSITORY_INVALID_CONFIGURATION;
    Slot a = read_slot(repository, 0U, CONFIGURATION_REPOSITORY_SLOT_A, board_zones, board_zone_count);
    Slot b = read_slot(repository, 1U, CONFIGURATION_REPOSITORY_SLOT_B, board_zones, board_zone_count);
    if (a.state == SLOT_STORAGE_ERROR || b.state == SLOT_STORAGE_ERROR) return CONFIGURATION_REPOSITORY_STORAGE_ERROR;
    const uint32_t generation = a.state == SLOT_VALID && a.generation >= b.generation ? a.generation + 1U : (b.state == SLOT_VALID ? b.generation + 1U : 1U);
    if (generation == 0U) return CONFIGURATION_REPOSITORY_STORAGE_ERROR;
    const size_t target = a.state == SLOT_VALID && (b.state != SLOT_VALID || a.generation >= b.generation) ? 1U : 0U;
    const char *key = target == 0U ? CONFIGURATION_REPOSITORY_SLOT_A : CONFIGURATION_REPOSITORY_SLOT_B;
    size_t length = 0U;
    ConfigurationRepositoryResult result = configuration_repository_encode(configuration, generation, repository->slot_buffers[target], sizeof(repository->slot_buffers[target]), &length);
    if (result != CONFIGURATION_REPOSITORY_OK) return result;
    if (repository->storage.write(repository->storage.context, key, repository->slot_buffers[target], length) != CONFIGURATION_REPOSITORY_OK || repository->storage.commit(repository->storage.context) != CONFIGURATION_REPOSITORY_OK) return CONFIGURATION_REPOSITORY_STORAGE_ERROR;
    size_t verified_length = 0U;
    result = repository->storage.read(repository->storage.context, key, repository->slot_buffers[target], sizeof(repository->slot_buffers[target]), &verified_length);
    if (result != CONFIGURATION_REPOSITORY_OK) return CONFIGURATION_REPOSITORY_STORAGE_ERROR;
    uint32_t verified_generation;
    result = configuration_repository_decode(repository->slot_buffers[target], verified_length,
                                             &repository->slot_configurations[target], &verified_generation,
                                             board_zones, board_zone_count);
    if (result != CONFIGURATION_REPOSITORY_OK || verified_generation != generation) return CONFIGURATION_REPOSITORY_CORRUPT;
    return CONFIGURATION_REPOSITORY_OK;
}
