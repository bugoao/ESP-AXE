#include "nvs_config.h"
#include "esp_log.h"
#include "nvs.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>

#define NVS_CONFIG_NAMESPACE "main"

static const char * TAG = "nvs_config";
static pthread_mutex_t config_write_lock = PTHREAD_MUTEX_INITIALIZER;

static char *copy_default_string(const char *value)
{
    size_t size = strlen(value) + 1;
    char *copy = malloc(size);
    if (copy != NULL) {
        memcpy(copy, value, size);
    }
    return copy;
}

char * nvs_config_get_string(const char * key, const char * default_value)
{
    nvs_handle handle;
    esp_err_t err;
    err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return copy_default_string(default_value);
    }

    size_t size = 0;
    err = nvs_get_str(handle, key, NULL, &size);

    if (err != ESP_OK) {
        nvs_close(handle);
        return copy_default_string(default_value);
    }

    char * out = malloc(size);
    if (out == NULL) {
        nvs_close(handle);
        return NULL;
    }
    err = nvs_get_str(handle, key, out, &size);

    if (err != ESP_OK) {
        free(out);
        nvs_close(handle);
        return copy_default_string(default_value);
    }

    nvs_close(handle);
    return out;
}

esp_err_t nvs_config_set_string(const char *key, const char *value)
{
    nvs_config_value_t entry = {.key = key, .type = NVS_VALUE_STRING, .value.string = value};
    return nvs_config_set_values(&entry, 1);
}

uint16_t nvs_config_get_u16(const char * key, const uint16_t default_value)
{
    nvs_handle handle;
    esp_err_t err;
    err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return default_value;
    }

    uint16_t out;
    err = nvs_get_u16(handle, key, &out);
    nvs_close(handle);

    if (err != ESP_OK) {
        return default_value;
    }
    return out;
}

uint16_t nvs_config_get_u16_clamped(const char *key, const uint16_t default_value,
                                    const uint16_t min_value, const uint16_t max_value)
{
    uint16_t value = nvs_config_get_u16(key, default_value);

    if (value < min_value) {
        ESP_LOGW(TAG, "Clamping NVS key %s from %u to %u",
                 key, (unsigned int)value, (unsigned int)min_value);
        return min_value;
    }
    if (value > max_value) {
        ESP_LOGW(TAG, "Clamping NVS key %s from %u to %u",
                 key, (unsigned int)value, (unsigned int)max_value);
        return max_value;
    }

    return value;
}

esp_err_t nvs_config_set_u16(const char *key, uint16_t value)
{
    nvs_config_value_t entry = {.key = key, .type = NVS_VALUE_U16, .value.u16 = value};
    return nvs_config_set_values(&entry, 1);
}

uint64_t nvs_config_get_u64(const char * key, const uint64_t default_value)
{
    nvs_handle handle;
    esp_err_t err;
    err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return default_value;
    }

    uint64_t out;
    err = nvs_get_u64(handle, key, &out);

    if (err != ESP_OK) {
        nvs_close(handle);
        return default_value;
    }

    nvs_close(handle);
    return out;
}

esp_err_t nvs_config_set_u64(const char *key, uint64_t value)
{
    nvs_config_value_t entry = {.key = key, .type = NVS_VALUE_U64, .value.u64 = value};
    return nvs_config_set_values(&entry, 1);
}

static esp_err_t value_matches(nvs_handle handle, const nvs_config_value_t *entry, bool *equal)
{
    *equal = false;
    esp_err_t err;
    if (entry->type == NVS_VALUE_U16) {
        uint16_t old;
        err = nvs_get_u16(handle, entry->key, &old);
        if (err == ESP_OK) *equal = old == entry->value.u16;
    } else if (entry->type == NVS_VALUE_U64) {
        uint64_t old;
        err = nvs_get_u64(handle, entry->key, &old);
        if (err == ESP_OK) *equal = old == entry->value.u64;
    } else {
        size_t length = 0;
        err = nvs_get_str(handle, entry->key, NULL, &length);
        if (err == ESP_OK && length == strlen(entry->value.string) + 1) {
            char *old = malloc(length);
            if (old == NULL) return ESP_ERR_NO_MEM;
            err = nvs_get_str(handle, entry->key, old, &length);
            if (err == ESP_OK) *equal = strcmp(old, entry->value.string) == 0;
            free(old);
        }
    }
    return err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_TYPE_MISMATCH ? ESP_OK : err;
}

esp_err_t nvs_config_set_values(const nvs_config_value_t *values, size_t count)
{
    if (count == 0) return ESP_OK;
    if (values == NULL) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < count; i++) {
        if (values[i].key == NULL || values[i].key[0] == '\0' || strlen(values[i].key) > 15 ||
                values[i].type < NVS_VALUE_STRING || values[i].type > NVS_VALUE_U64 ||
                (values[i].type == NVS_VALUE_STRING && values[i].value.string == NULL)) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    pthread_mutex_lock(&config_write_lock);
    nvs_handle handle;
    esp_err_t err = nvs_open(NVS_CONFIG_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        pthread_mutex_unlock(&config_write_lock);
        return err;
    }
    bool changed = false;
    for (size_t i = 0; i < count; i++) {
        bool equal;
        err = value_matches(handle, &values[i], &equal);
        if (err != ESP_OK) break;
        if (equal) continue;
        switch (values[i].type) {
            case NVS_VALUE_STRING: err = nvs_set_str(handle, values[i].key, values[i].value.string); break;
            case NVS_VALUE_U16: err = nvs_set_u16(handle, values[i].key, values[i].value.u16); break;
            case NVS_VALUE_U64: err = nvs_set_u64(handle, values[i].key, values[i].value.u64); break;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Could not write NVS key: %s", values[i].key);
            break;
        }
        changed = true;
    }
    if (err == ESP_OK && changed) err = nvs_commit(handle);
    nvs_close(handle);
    pthread_mutex_unlock(&config_write_lock);
    return err;
}
