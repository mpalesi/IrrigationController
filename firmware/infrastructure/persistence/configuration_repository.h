#pragma once

#include <stddef.h>
#include <stdint.h>

#include "app/configuration/irrigation_configuration.h"

#define CONFIGURATION_REPOSITORY_SLOT_A "cfg_a"
#define CONFIGURATION_REPOSITORY_SLOT_B "cfg_b"
#define CONFIGURATION_REPOSITORY_MAGIC 0x47464349U
#define CONFIGURATION_REPOSITORY_HEADER_SIZE 16U
#define CONFIGURATION_REPOSITORY_MAX_TEXT_SIZE IRRIGATION_CONFIGURATION_ID_SIZE
#define CONFIGURATION_REPOSITORY_MAX_PAYLOAD_SIZE \
    (13U + IRRIGATION_MAX_ZONES * (2U * CONFIGURATION_REPOSITORY_MAX_TEXT_SIZE) + \
     IRRIGATION_MAX_PROGRAMS * (2U * CONFIGURATION_REPOSITORY_MAX_TEXT_SIZE + 1U + \
                                IRRIGATION_MAX_PROGRAM_STEPS * (CONFIGURATION_REPOSITORY_MAX_TEXT_SIZE + 4U)) + \
     IRRIGATION_MAX_SCHEDULES * (2U * CONFIGURATION_REPOSITORY_MAX_TEXT_SIZE + 4U))
#define CONFIGURATION_REPOSITORY_MAX_BLOB_SIZE \
    (CONFIGURATION_REPOSITORY_HEADER_SIZE + CONFIGURATION_REPOSITORY_MAX_PAYLOAD_SIZE)

typedef enum {
    CONFIGURATION_REPOSITORY_OK = 0,
    CONFIGURATION_REPOSITORY_NOT_FOUND,
    CONFIGURATION_REPOSITORY_CORRUPT,
    CONFIGURATION_REPOSITORY_INCOMPATIBLE,
    CONFIGURATION_REPOSITORY_INVALID_CONFIGURATION,
    CONFIGURATION_REPOSITORY_STORAGE_ERROR,
} ConfigurationRepositoryResult;

typedef ConfigurationRepositoryResult (*ConfigurationRepositoryRead)(void *context, const char *key,
                                                                      uint8_t *buffer, size_t capacity,
                                                                      size_t *length);
typedef ConfigurationRepositoryResult (*ConfigurationRepositoryWrite)(void *context, const char *key,
                                                                       const uint8_t *buffer, size_t length);
typedef ConfigurationRepositoryResult (*ConfigurationRepositoryCommit)(void *context);

typedef struct {
    void *context;
    ConfigurationRepositoryRead read;
    ConfigurationRepositoryWrite write;
    ConfigurationRepositoryCommit commit;
} ConfigurationRepositoryStorage;

typedef struct {
    ConfigurationRepositoryStorage storage;
    uint8_t slot_buffers[2][CONFIGURATION_REPOSITORY_MAX_BLOB_SIZE];
    IrrigationConfiguration slot_configurations[2];
} ConfigurationRepository;

void configuration_repository_init(ConfigurationRepository *repository,
                                   ConfigurationRepositoryStorage storage);
ConfigurationRepositoryResult configuration_repository_encode(
    const IrrigationConfiguration *configuration, uint32_t generation, uint8_t *blob, size_t capacity,
    size_t *blob_length);
ConfigurationRepositoryResult configuration_repository_decode(
    const uint8_t *blob, size_t blob_length, IrrigationConfiguration *configuration, uint32_t *generation,
    const Zone *board_zones, size_t board_zone_count);
ConfigurationRepositoryResult configuration_repository_load(ConfigurationRepository *repository,
                                                             IrrigationConfiguration *configuration,
                                                             uint32_t *generation, const Zone *board_zones,
                                                             size_t board_zone_count);
ConfigurationRepositoryResult configuration_repository_save(ConfigurationRepository *repository,
                                                             const IrrigationConfiguration *configuration,
                                                             const Zone *board_zones, size_t board_zone_count);

ConfigurationRepositoryResult nvs_configuration_repository_init(ConfigurationRepository *repository);
void nvs_configuration_repository_deinit(void);
