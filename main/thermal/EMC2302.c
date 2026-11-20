#include "esp_log.h"
#include "esp_check.h"
#include <math.h>
#include <stdio.h>
#include "i2c_bitaxe.h"
#include "EMC2302.h"

static const char *TAG = "EMC2302.c";

// run this first. sets up the config register
static i2c_master_dev_handle_t emc2302_dev_handle;

esp_err_t EMC2302_init(bool invertPolarity) {
    if (i2c_bitaxe_add_device(EMC2302_I2CADDR_DEFAULT, &emc2302_dev_handle,TAG) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add device");
        return ESP_FAIL;
    }

    if (invertPolarity) {
        ESP_RETURN_ON_ERROR(i2c_bitaxe_register_write_byte(emc2302_dev_handle, EMC2302_PWM_POLARITY, 0b00011111), TAG, "Failed to configure fan polarity");
    }

    return ESP_OK;
}

// Sets the fan speed to a given percent
esp_err_t EMC2302_set_fan_speed(uint8_t devicenum, float percent)
{
    uint8_t speed;
    if (devicenum > 1 || !isfinite(percent) || percent < 0.0f || percent > 1.0f) {
        return ESP_ERR_INVALID_ARG;
    }
	uint8_t FAN_SETTING_REG = EMC2302_FAN1_SETTING + (devicenum * 0x10);

    speed = (uint8_t) (255.0 * (1.0f-percent));
    ESP_RETURN_ON_ERROR(i2c_bitaxe_register_write_byte(emc2302_dev_handle, FAN_SETTING_REG, speed), TAG, "Failed to set fan speed");
    return ESP_OK;
}

// Gets the fan speed
uint16_t EMC2302_get_fan_speed(uint8_t devicenum)
{
    uint8_t tach_lsb, tach_msb;
    uint8_t TACH_LSB_REG = EMC2302_TACH1_LSB + (devicenum * 0x10);
    uint8_t TACH_MSB_REG = EMC2302_TACH1_MSB + (devicenum * 0x10);

    // Tach reading registers are high-byte first (unlike the target registers).
    if (i2c_bitaxe_register_read(emc2302_dev_handle, TACH_MSB_REG, &tach_msb, 1) != ESP_OK ||
            i2c_bitaxe_register_read(emc2302_dev_handle, TACH_LSB_REG, &tach_lsb, 1) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read fan %u tachometer", devicenum);
        return 0;
    }

    //ESP_LOGI(TAG, "Raw Fan Speed[%d] = %02X %02X", devicenum, tach_msb, tach_lsb);
    uint16_t tach_count = (tach_msb << 5) + ((tach_lsb >> 3) & 0x1F);
    if (tach_count == 0 || tach_count == 0x1fffU) {
        return 0;
    }
    uint32_t rpm = EMC2302_FAN_RPM_NUMERATOR / tach_count;
    return (uint16_t)(rpm > UINT16_MAX ? UINT16_MAX : rpm);
}
