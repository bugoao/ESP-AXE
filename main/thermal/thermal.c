#include "thermal.h"
#include <math.h>

#define INTERNAL_OFFSET 5 //degrees C


esp_err_t Thermal_init(DeviceModel device_model, bool polarity) {
    esp_err_t err = ESP_OK;
    switch (device_model) {
        case DEVICE_ULTRA:
        case DEVICE_SUPRA:
            err = EMC2101_init(polarity);
            break;
        case DEVICE_GAMMA:
            err = EMC2101_init(polarity);
            if (err == ESP_OK) {
                err = EMC2101_set_ideality_factor(EMC2101_IDEALITY_1_0319);
                if (err == ESP_OK) {
                    err = EMC2101_set_beta_compensation(EMC2101_BETA_11);
                }
            }
            break;
        case DEVICE_GAMMATURBO:
            err = EMC2103_init(polarity);
            break;
        case DEVICE_HEX:
        case DEVICE_SUPRAHEX:
            err = TMP1075_init();
            if (err == ESP_OK) {
                err = EMC2302_init(true);
            }
            break;
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
    return err;
    
}

//percent is a float between 0.0 and 1.0
esp_err_t Thermal_set_fan_percent(DeviceModel device_model, float percent) {
    if (!isfinite(percent) || percent < 0.0f || percent > 1.0f) {
        return ESP_ERR_INVALID_ARG;
    }

    switch (device_model) {
        case DEVICE_ULTRA:
        case DEVICE_SUPRA:
        case DEVICE_GAMMA:
            return EMC2101_set_fan_speed(percent);
        case DEVICE_GAMMATURBO:
            return EMC2103_set_fan_speed(percent);
        case DEVICE_HEX:
        case DEVICE_SUPRAHEX:
        {
            // Attempt both fans even if one write fails.
            esp_err_t first = EMC2302_set_fan_speed(0,percent);
            esp_err_t second = EMC2302_set_fan_speed(1,percent);
            return first != ESP_OK ? first : second;
        }
        default:
            return ESP_ERR_NOT_SUPPORTED;
    }
}

uint16_t Thermal_get_fan_speed(DeviceModel device_model) {
    switch (device_model) {
        case DEVICE_ULTRA:
        case DEVICE_SUPRA:
        case DEVICE_GAMMA:
            return EMC2101_get_fan_speed();
        case DEVICE_GAMMATURBO:
            return EMC2103_get_fan_speed();
        case DEVICE_HEX:
        case DEVICE_SUPRAHEX:
            return EMC2302_get_fan_speed(0);    // May not be the best way to only get the first fan speed;
        default:
    }
    return 0;
}

float Thermal_get_chip_temp(GlobalState * GLOBAL_STATE) {
    if (!GLOBAL_STATE->ASIC_initalized) {
        return -1;
    }

    switch (GLOBAL_STATE->device_model) {
        case DEVICE_ULTRA:
        case DEVICE_SUPRA:
            if (GLOBAL_STATE->board_version >= 402 && GLOBAL_STATE->board_version <= 499) {
                return EMC2101_get_external_temp();
            } else {
                return EMC2101_get_internal_temp() + INTERNAL_OFFSET;
            }
        case DEVICE_GAMMA:
            return EMC2101_get_external_temp();
        case DEVICE_GAMMATURBO:
            return EMC2103_get_external_temp();
        case DEVICE_HEX:
        case DEVICE_SUPRAHEX:
            return TMP1075_read_temperature_weighted();
        default:
    }

    return -1;
}
