#include <math.h>
#include <stdio.h>
#include "esp_log.h"
#include "i2c_bitaxe.h"

#include "TMP1075.h"

static const char *TAG = "TMP1075";

static i2c_master_dev_handle_t tmp1075_u7_dev_handle;
static i2c_master_dev_handle_t tmp1075_u8_dev_handle;   //for Hex and SupraHex

/**
 * @brief Initialize the TMP1075 sensor.
 *
 * @return esp_err_t ESP_OK on success, or an error code on failure.
 */
esp_err_t TMP1075_init(void) {
    esp_err_t err = i2c_bitaxe_add_device(TMP1075_I2CADDR_U7, &tmp1075_u7_dev_handle, TAG);
    if (err != ESP_OK) {
        return err;
    }
    return i2c_bitaxe_add_device(TMP1075_I2CADDR_U8, &tmp1075_u8_dev_handle, TAG);
}

static float read_temperature(int device_index)
{
    uint8_t data[2];
    i2c_master_dev_handle_t handle = device_index == 0 ? tmp1075_u7_dev_handle : tmp1075_u8_dev_handle;

    if (i2c_bitaxe_register_read(handle, TMP1075_TEMP_REG, data, 2) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read TMP1075 sensor %d", device_index);
        return NAN;
    }
    int16_t raw = (int16_t)(((uint16_t)data[0] << 8) | data[1]);
    return (float)raw / 256.0f;
}

float TMP1075_read_temperature_weighted(void)
{
    float u7 = read_temperature(0);
    float u8 = read_temperature(1);
    if (!isfinite(u7) || !isfinite(u8)) {
        return NAN;
    }
    // Protect against a local hotspot on one side of a multi-ASIC board.
    return fmaxf(u7, u8);
}
