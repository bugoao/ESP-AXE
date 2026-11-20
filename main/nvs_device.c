#include <string.h>
#include <errno.h>
#include <limits.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "nvs_flash.h"
#include "nvs_config.h"
#include "nvs_device.h"

#include "global_state.h"

static const char * TAG = "nvs_device";

esp_err_t NVSDevice_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

esp_err_t NVSDevice_parse_config(GlobalState * GLOBAL_STATE) {

    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value =
        nvs_config_get_u16_clamped(NVS_CONFIG_ASIC_FREQ,
                                   CONFIG_ASIC_FREQUENCY,
                                   NVS_CONFIG_ASIC_FREQUENCY_MIN_MHZ,
                                   NVS_CONFIG_ASIC_FREQUENCY_MAX_MHZ);
    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.actual_frequency =
        GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value;
    ESP_LOGI(TAG, "NVS_CONFIG_ASIC_FREQ %f", (float)GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value);

    GLOBAL_STATE->asic_model_str = nvs_config_get_string(NVS_CONFIG_ASIC_MODEL, "");
    GLOBAL_STATE->device_model_str = nvs_config_get_string(NVS_CONFIG_DEVICE_MODEL, "invalid");
    if (GLOBAL_STATE->asic_model_str == NULL || GLOBAL_STATE->device_model_str == NULL) {
        free(GLOBAL_STATE->asic_model_str);
        free(GLOBAL_STATE->device_model_str);
        GLOBAL_STATE->asic_model_str = NULL;
        GLOBAL_STATE->device_model_str = NULL;
        return ESP_ERR_NO_MEM;
    }
    char * board_version = nvs_config_get_string(NVS_CONFIG_BOARD_VERSION, "000");
    if (board_version == NULL) {
        free(GLOBAL_STATE->asic_model_str);
        free(GLOBAL_STATE->device_model_str);
        GLOBAL_STATE->asic_model_str = NULL;
        GLOBAL_STATE->device_model_str = NULL;
        return ESP_ERR_NO_MEM;
    }
    errno = 0;
    char *end = NULL;
    long parsed_board_version = strtol(board_version, &end, 10);
    if (end == board_version) {
        // Older releases used atoi(), so deployed display labels such as
        // "Antminer S23" select the legacy board profile (revision 0).
        // Keep that behavior for every device model.
        ESP_LOGW(TAG, "Using legacy board revision 0 for label '%s'", board_version);
        parsed_board_version = 0;
    } else if (errno != 0 || parsed_board_version < 0 || parsed_board_version > INT_MAX) {
        ESP_LOGE(TAG, "Invalid board revision for device model %s", GLOBAL_STATE->device_model_str);
        free(GLOBAL_STATE->asic_model_str);
        free(GLOBAL_STATE->device_model_str);
        GLOBAL_STATE->asic_model_str = NULL;
        GLOBAL_STATE->device_model_str = NULL;
        free(board_version);
        return ESP_ERR_INVALID_ARG;
    } else if (*end != '\0') {
        // Preserve atoi() behavior for older values that start with a numeric
        // board revision followed by a descriptive suffix.
        ESP_LOGW(TAG, "Using board revision %ld from label '%s'", parsed_board_version, board_version);
    }
    GLOBAL_STATE->board_version = (int)parsed_board_version;
    free(board_version);
    ESP_LOGI(TAG, "Found Device Model: %s", GLOBAL_STATE->device_model_str);
    ESP_LOGI(TAG, "Found Board Version: %d", GLOBAL_STATE->board_version);
    
    return ESP_OK;
}
