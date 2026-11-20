#include <pthread.h>

#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "main.h"

#include "asic_result_task.h"
#include "asic_task.h"
#include "create_jobs_task.h"
#include "esp_netif.h"
#include "system.h"
#include "http_server.h"
#include "nvs_config.h"
#include "serial.h"
#include "stratum_task.h"
#include "i2c_bitaxe.h"
#include "adc.h"
#include "nvs_device.h"
#include "self_test.h"
#include "asic.h"
#include "power.h"

#define WIFI_INITIAL_CONNECT_TIMEOUT_MS 15000
#define ASIC_INIT_RETRY_DELAY_MS 5000
#define ASIC_DEGRADED_RETRY_DELAY_MS 1000
#define ASIC_DEGRADED_RETRY_COUNT 2

static GlobalState GLOBAL_STATE = {
    .extranonce_str = NULL, 
    .extranonce_2_len = 0, 
    .abandon_work = 0, 
    .stratum_difficulty = (CONFIG_STRATUM_DIFFICULTY > 0 ? CONFIG_STRATUM_DIFFICULTY : 1),
    .version_mask = 0,
    .pending_version_mask = 0,
    .ASIC_initalized = false,
    .sock = -1,
    .send_uid = 1
};

static const char * TAG = "bitaxe";

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
        case ESP_RST_POWERON:
            return "POWERON";
        case ESP_RST_EXT:
            return "EXT";
        case ESP_RST_SW:
            return "SW";
        case ESP_RST_PANIC:
            return "PANIC";
        case ESP_RST_INT_WDT:
            return "INT_WDT";
        case ESP_RST_TASK_WDT:
            return "TASK_WDT";
        case ESP_RST_WDT:
            return "WDT";
        case ESP_RST_DEEPSLEEP:
            return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:
            return "BROWNOUT";
        case ESP_RST_SDIO:
            return "SDIO";
        default:
            return "UNKNOWN";
    }
}

static bool log_task_create_result(const char *task_name, BaseType_t rc)
{
    unsigned long free_heap = (unsigned long)esp_get_free_heap_size();
    unsigned long free_internal = (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    unsigned long largest_internal_block = (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    if (rc == pdPASS) {
        ESP_LOGD(TAG, "%s rc=%d heap=%lu internal=%lu largest_internal=%lu",
                 task_name, (int)rc, free_heap, free_internal, largest_internal_block);
        return true;
    }

    ESP_LOGE(TAG, "%s rc=%d heap=%lu internal=%lu largest_internal=%lu",
             task_name, (int)rc, free_heap, free_internal, largest_internal_block);
    ESP_LOGE(TAG, "Aborting miner startup after %s creation failed. Internal RAM is exhausted.", task_name);
    return false;
}

static void mark_running_app_valid_after_startup(void)
{
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state;

    if (running == NULL) {
        return;
    }

    if (esp_ota_get_state_partition(running, &ota_state) != ESP_OK ||
            ota_state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Marked running OTA image as valid after miner startup");
    } else {
        ESP_LOGE(TAG, "Failed to mark running OTA image valid: %s", esp_err_to_name(err));
    }
#endif
}

static bool wait_for_initial_wifi_connection(GlobalState *global_state, const char *wifi_ssid)
{
    if (wifi_ssid == NULL || wifi_ssid[0] == '\0') {
        SYSTEM_stats_lock();
        snprintf(global_state->SYSTEM_MODULE.wifi_status,
                 sizeof(global_state->SYSTEM_MODULE.wifi_status),
                 "AP mode");
        SYSTEM_stats_unlock();
        return false;
    }

    EventBits_t result_bits = wifi_connect(pdMS_TO_TICKS(WIFI_INITIAL_CONNECT_TIMEOUT_MS));
    if (result_bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to SSID: %s", wifi_ssid);
        SYSTEM_stats_lock();
        strncpy(global_state->SYSTEM_MODULE.wifi_status, "Connected!", 20);
        SYSTEM_stats_unlock();
        return true;
    }

    ESP_LOGW(TAG,
             "Initial Wi-Fi connection to SSID: %s did not complete within %u ms. Continuing startup with AP enabled and background retries.",
             wifi_ssid,
             WIFI_INITIAL_CONNECT_TIMEOUT_MS);
    SYSTEM_stats_lock();
    strncpy(global_state->SYSTEM_MODULE.wifi_status, "AP mode / retry", 20);
    SYSTEM_stats_unlock();
    return false;
}

static bool wait_for_asic_ready(GlobalState *global_state)
{
    esp_err_t serial_err = SERIAL_init();
    uint8_t degraded_retries = 0;
    if (serial_err != ESP_OK) {
        ESP_LOGE(TAG, "UART initialization failed: %s", esp_err_to_name(serial_err));
        return false;
    }

    while (1) {
        uint8_t expected_chips;
        uint8_t detected_chips;

        SERIAL_clear_buffer();

        detected_chips = ASIC_init(global_state);
        if (detected_chips != 0) {
            expected_chips = ASIC_get_expected_asic_count(global_state);
            if (detected_chips != expected_chips) {
                if (degraded_retries < ASIC_DEGRADED_RETRY_COUNT) {
                    degraded_retries++;
                    ESP_LOGW(TAG,
                             "ASIC chain probe detected %u of %u chips; retrying full initialization (%u/%u)",
                             detected_chips,
                             expected_chips,
                             degraded_retries,
                             ASIC_DEGRADED_RETRY_COUNT);
                    vTaskDelay(pdMS_TO_TICKS(ASIC_DEGRADED_RETRY_DELAY_MS));
                    continue;
                }
                global_state->SYSTEM_MODULE.asic_status = "Degraded chip count";
                ESP_LOGE(TAG,
                         "ASIC chain degraded: detected %u of %u configured chips",
                         detected_chips,
                         expected_chips);
            } else {
                global_state->SYSTEM_MODULE.asic_status = NULL;
            }
            return true;
        }

        degraded_retries = 0;
        global_state->SYSTEM_MODULE.asic_status = "Chip count 0";
        ESP_LOGE(TAG, "ASIC init failed, retrying in %u ms", ASIC_INIT_RETRY_DELAY_MS);
        vTaskDelay(pdMS_TO_TICKS(ASIC_INIT_RETRY_DELAY_MS));
    }
}

static bool boot_button_pressed(void)
{
    const gpio_num_t pin = CONFIG_GPIO_BUTTON_BOOT;
    if (!GPIO_IS_VALID_GPIO(pin)) {
        ESP_LOGW(TAG, "Invalid BOOT button GPIO: %d", pin);
        return false;
    }

    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not configure BOOT button: %s", esp_err_to_name(err));
        return false;
    }

    // Allow the pad to settle after enabling the pull-up.
    vTaskDelay(pdMS_TO_TICKS(20));
    return gpio_get_level(pin) == 0;
}

void app_main(void)
{
    BaseType_t rc;

    ESP_LOGI(TAG, "Welcome to the bitaxe - FOSS || GTFO!");
    esp_reset_reason_t reset_reason = esp_reset_reason();
    ESP_LOGW(TAG, "Reset reason: %d (%s)", reset_reason, reset_reason_name(reset_reason));

    if (!esp_psram_is_initialized()) {
        ESP_LOGE(TAG, "No PSRAM available on ESP32 device!");
        GLOBAL_STATE.psram_is_available = false;
    } else {
        GLOBAL_STATE.psram_is_available = true;
    }

    // Init I2C
    ESP_ERROR_CHECK(i2c_bitaxe_init());
    ESP_LOGI(TAG, "I2C initialized successfully");

    //wait for I2C to init
    vTaskDelay(100 / portTICK_PERIOD_MS);

    //Init ADC
    ADC_init();

    //initialize the ESP32 NVS
    if (NVSDevice_init() != ESP_OK){
        ESP_LOGE(TAG, "Failed to init NVS");
        return;
    }

    //parse the NVS config into GLOBAL_STATE
    if (NVSDevice_parse_config(&GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to parse NVS config");
        return;
    }

    if (!SYSTEM_init_hashrate_lock(&GLOBAL_STATE)) {
        ESP_LOGE(TAG, "Failed to initialize hashrate lock");
        return;
    }

    // The Web server reads stratum state even before ASIC job resources exist.
    if (pthread_mutex_init(&GLOBAL_STATE.stratum_state_lock, NULL) != 0) {
        ESP_LOGE(TAG, "Failed to initialize stratum state lock");
        return;
    }

    if (ASIC_set_device_model(&GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Error setting ASIC model");
        return;
    }

    bool pressed = boot_button_pressed();
    bool self_test_requested = should_test(&GLOBAL_STATE);
    if (self_test_requested || pressed) {
        ESP_LOGI(TAG, "Entering self-test: BOOT button=%d, NVS request=%d",
                 pressed, self_test_requested);
        self_test((void *) &GLOBAL_STATE);
        return;
    }

    SYSTEM_init_system(&GLOBAL_STATE);

    // pull the wifi credentials and hostname out of NVS
    char * wifi_ssid = nvs_config_get_string(NVS_CONFIG_WIFI_SSID, WIFI_SSID);
    char * wifi_pass = nvs_config_get_string(NVS_CONFIG_WIFI_PASS, WIFI_PASS);
    char * hostname  = nvs_config_get_string(NVS_CONFIG_HOSTNAME, HOSTNAME);
    const char *active_ssid = wifi_ssid != NULL && wifi_pass != NULL ? wifi_ssid : "";
    if (wifi_ssid == NULL || wifi_pass == NULL || hostname == NULL) {
        ESP_LOGE(TAG, "Not enough memory to load Wi-Fi settings; keeping recovery AP available");
    }

    // copy the wifi ssid to the global state
    strncpy(GLOBAL_STATE.SYSTEM_MODULE.ssid, active_ssid, sizeof(GLOBAL_STATE.SYSTEM_MODULE.ssid));
    GLOBAL_STATE.SYSTEM_MODULE.ssid[sizeof(GLOBAL_STATE.SYSTEM_MODULE.ssid)-1] = 0;

    // init AP and connect to wifi
    wifi_init(active_ssid, wifi_pass != NULL ? wifi_pass : "",
              hostname != NULL ? hostname : HOSTNAME, GLOBAL_STATE.SYSTEM_MODULE.ip_addr_str);

    generate_ssid(GLOBAL_STATE.SYSTEM_MODULE.ap_ssid);

    // Keep configuration and recovery reachable if a peripheral or ASIC
    // cannot start. Status reads use the last sampled values.
    if (start_rest_server((void *)&GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Web server");
        free(wifi_ssid);
        free(wifi_pass);
        free(hostname);
        return;
    }

    if (SYSTEM_init_peripherals(&GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Peripheral initialization failed");
        free(wifi_ssid);
        free(wifi_pass);
        free(hostname);
        return;
    }

    bool initial_wifi_connected = wait_for_initial_wifi_connection(&GLOBAL_STATE, active_ssid);
    if (initial_wifi_connected) {
        SYSTEM_start_trusted_time_sync(&GLOBAL_STATE);
    }

    free(wifi_ssid);
    free(wifi_pass);
    free(hostname);

    if (initial_wifi_connected) {
        wifi_softap_off();
    }

    queue_init(&GLOBAL_STATE.stratum_queue);
    queue_init(&GLOBAL_STATE.ASIC_jobs_queue);
    GLOBAL_STATE.stratum_submit_queue = xQueueCreate(
        STRATUM_SUBMIT_QUEUE_LENGTH, sizeof(stratum_share_submission));
    if (GLOBAL_STATE.stratum_submit_queue == NULL) {
        ESP_LOGE(TAG, "Failed to initialize stratum submit queue");
        Power_disable(&GLOBAL_STATE);
        return;
    }
    GLOBAL_STATE.stratum_block_submit_queue = xQueueCreate(
        STRATUM_BLOCK_SUBMIT_QUEUE_LENGTH, sizeof(stratum_share_submission));
    if (GLOBAL_STATE.stratum_block_submit_queue == NULL) {
        ESP_LOGE(TAG, "Failed to initialize block submit queue");
        Power_disable(&GLOBAL_STATE);
        return;
    }
    if (!ASIC_init_job_resources(&GLOBAL_STATE)) {
        ESP_LOGE(TAG, "Failed to initialize ASIC job resources");
        Power_disable(&GLOBAL_STATE);
        return;
    }

    if (!wait_for_asic_ready(&GLOBAL_STATE)) {
        Power_disable(&GLOBAL_STATE);
        return;
    }

    int max_baud = ASIC_set_max_baud(&GLOBAL_STATE);
    if (max_baud <= 0 || SERIAL_set_baud(max_baud) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to switch ASIC UART to mining baud rate");
        Power_disable(&GLOBAL_STATE);
        return;
    }
    SERIAL_clear_buffer();

    if (!ASIC_set_job_difficulty_mask(&GLOBAL_STATE, GLOBAL_STATE.stratum_difficulty)) {
        ESP_LOGE(TAG, "Failed to initialize ASIC difficulty mask");
        Power_disable(&GLOBAL_STATE);
        return;
    }
    GLOBAL_STATE.ASIC_initalized = true;

    rc = xTaskCreatePinnedToCore(
        POWER_MANAGEMENT_task, "power management", 4096, (void *)&GLOBAL_STATE, 10, NULL, 0);
    if (!log_task_create_result("POWER_MANAGEMENT_task", rc)) {
        GLOBAL_STATE.ASIC_initalized = false;
        Power_disable(&GLOBAL_STATE);
        return;
    }

    ESP_LOGD(TAG, "before miner tasks: heap=%lu internal=%lu largest_internal=%lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));

    // Pin mining work to Core 1 and network/admin work to Core 0.
    rc = xTaskCreatePinnedToCore(create_jobs_task, "stratum miner", 5120, (void *)&GLOBAL_STATE, 6, NULL, 0);
    if (!log_task_create_result("create_jobs_task", rc)) {
        return;
    }

    rc = xTaskCreatePinnedToCore(stratum_task, "stratum admin", 12288, (void *) &GLOBAL_STATE, 8, NULL, 0);
    if (!log_task_create_result("stratum_task", rc)) {
        return;
    }

    rc = xTaskCreatePinnedToCore(stratum_submit_task, "stratum submit", 4096, (void *)&GLOBAL_STATE, 9, NULL, 0);
    if (!log_task_create_result("stratum_submit_task", rc)) {
        return;
    }

    rc = xTaskCreatePinnedToCore(stratum_block_submit_task, "block submit", 4096,
                                 (void *)&GLOBAL_STATE, 11, NULL, 0);
    if (!log_task_create_result("stratum_block_submit_task", rc)) {
        return;
    }

    rc = xTaskCreatePinnedToCore(ASIC_task, "asic", 4096, (void *)&GLOBAL_STATE, 14, NULL, 1);
    if (!log_task_create_result("ASIC_task", rc)) {
        return;
    }

    // RX must preempt sub-millisecond TX pacing so the UART result buffer drains.
    rc = xTaskCreatePinnedToCore(ASIC_result_task, "asic result", 8192, (void *)&GLOBAL_STATE, 15, NULL, 1);
    if (!log_task_create_result("ASIC_result_task", rc)) {
        return;
    }

    mark_running_app_valid_after_startup();
}

void MINER_set_wifi_status(wifi_status_t status, int retry_count, int reason)
{
    SYSTEM_stats_lock();
    switch(status) {
        case WIFI_CONNECTING:
            snprintf(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, 20, "Connecting...");
            SYSTEM_stats_unlock();
            return;
        case WIFI_CONNECTED:
            snprintf(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, 20, "Connected!");
            SYSTEM_stats_unlock();
            return;
        case WIFI_RETRYING:
            // See https://github.com/espressif/esp-idf/blob/master/components/esp_wifi/include/esp_wifi_types_generic.h for codes
            switch(reason) {
                case 201:
                    snprintf(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, 20, "No AP found (%d)", retry_count);
                    SYSTEM_stats_unlock();
                    return;
                case 15:
                case 205:
                    snprintf(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, 20, "Password error (%d)", retry_count);
                    SYSTEM_stats_unlock();
                    return;
                default:
                    snprintf(GLOBAL_STATE.SYSTEM_MODULE.wifi_status, 20, "Error %d (%d)", reason, retry_count);
                    SYSTEM_stats_unlock();
                    return;
            }
    }
    SYSTEM_stats_unlock();
    ESP_LOGW(TAG, "Unknown status: %d", status);
}

void MINER_set_ap_status(bool enabled) {
    SYSTEM_stats_lock();
    GLOBAL_STATE.SYSTEM_MODULE.ap_enabled = enabled;
    SYSTEM_stats_unlock();
}
