#include "infrastructure/persistence/configuration_repository.h"

#include "nvs.h"
#include "nvs_flash.h"

#define IRRIGATION_CONFIGURATION_PARTITION "irrigation_cfg"

typedef struct { nvs_handle_t handle; bool open; } NvsConfigurationStorage;
static NvsConfigurationStorage storage;

static ConfigurationRepositoryResult nvs_read(void *context, const char *key, uint8_t *buffer,
                                              size_t capacity, size_t *length)
{
    NvsConfigurationStorage *nvs_storage = context; size_t size = 0U;
    esp_err_t result = nvs_get_blob(nvs_storage->handle, key, NULL, &size);
    if (result == ESP_ERR_NVS_NOT_FOUND) return CONFIGURATION_REPOSITORY_NOT_FOUND;
    if (result != ESP_OK || size > capacity) return CONFIGURATION_REPOSITORY_STORAGE_ERROR;
    result = nvs_get_blob(nvs_storage->handle, key, buffer, &size);
    if (result != ESP_OK) return CONFIGURATION_REPOSITORY_STORAGE_ERROR;
    *length = size;
    return CONFIGURATION_REPOSITORY_OK;
}

static ConfigurationRepositoryResult nvs_write(void *context, const char *key, const uint8_t *buffer,
                                               size_t length)
{
    NvsConfigurationStorage *nvs_storage = context;
    return nvs_set_blob(nvs_storage->handle, key, buffer, length) == ESP_OK ? CONFIGURATION_REPOSITORY_OK : CONFIGURATION_REPOSITORY_STORAGE_ERROR;
}

static ConfigurationRepositoryResult nvs_storage_commit(void *context)
{
    NvsConfigurationStorage *nvs_storage = context;
    return nvs_commit(nvs_storage->handle) == ESP_OK ? CONFIGURATION_REPOSITORY_OK : CONFIGURATION_REPOSITORY_STORAGE_ERROR;
}

ConfigurationRepositoryResult nvs_configuration_repository_init(ConfigurationRepository *repository)
{
    if (repository == NULL) return CONFIGURATION_REPOSITORY_STORAGE_ERROR;
    if (storage.open) nvs_close(storage.handle);
    storage = (NvsConfigurationStorage){0};
    if (nvs_flash_init_partition(IRRIGATION_CONFIGURATION_PARTITION) != ESP_OK ||
        nvs_open_from_partition(IRRIGATION_CONFIGURATION_PARTITION, "configuration", NVS_READWRITE,
                                &storage.handle) != ESP_OK) return CONFIGURATION_REPOSITORY_STORAGE_ERROR;
    storage.open = true;
    configuration_repository_init(repository, (ConfigurationRepositoryStorage){
        .context = &storage, .read = nvs_read, .write = nvs_write, .commit = nvs_storage_commit,
    });
    return CONFIGURATION_REPOSITORY_OK;
}

void nvs_configuration_repository_deinit(void)
{
    if (storage.open) nvs_close(storage.handle);
    storage = (NvsConfigurationStorage){0};
}
