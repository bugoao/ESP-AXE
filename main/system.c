#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lwip/inet.h"

#include "INA260.h"
#include "adc.h"
#include "asic.h"
#include "connect.h"
#include "display.h"
#include "i2c_bitaxe.h"
#include "input.h"
#include "nvs_config.h"
#include "power.h"
#include "screen.h"
#include "stratum_task.h"
#include "system.h"
#include "thermal.h"
#include "vcore.h"

static const char *TAG = "SystemModule";
static pthread_mutex_t statistics_lock = PTHREAD_MUTEX_INITIALIZER;
static int compare_rejected_reason_stats(const void *a, const void *b);

void SYSTEM_stats_lock(void) { pthread_mutex_lock(&statistics_lock); }
void SYSTEM_stats_unlock(void) { pthread_mutex_unlock(&statistics_lock); }

uint64_t SYSTEM_increment_counter(uint64_t *counter)
{
    SYSTEM_stats_lock();
    uint64_t value = ++*counter;
    SYSTEM_stats_unlock();
    return value;
}

uint64_t SYSTEM_read_counter(const uint64_t *counter)
{
    SYSTEM_stats_lock();
    uint64_t value = *counter;
    SYSTEM_stats_unlock();
    return value;
}

bool SYSTEM_get_is_using_fallback(GlobalState *state)
{
    SYSTEM_stats_lock();
    bool enabled = state->SYSTEM_MODULE.is_using_fallback;
    SYSTEM_stats_unlock();
    return enabled;
}

void SYSTEM_set_is_using_fallback(GlobalState *state, bool enabled)
{
    SYSTEM_stats_lock();
    state->SYSTEM_MODULE.is_using_fallback = enabled;
    SYSTEM_stats_unlock();
}

void SYSTEM_publish_power_snapshot(GlobalState *state)
{
    SYSTEM_stats_lock();
    state->power_snapshot.voltage = state->POWER_MANAGEMENT_MODULE.voltage;
    state->power_snapshot.power = state->POWER_MANAGEMENT_MODULE.power;
    state->power_snapshot.current = state->POWER_MANAGEMENT_MODULE.current;
    state->power_snapshot.core_voltage = state->POWER_MANAGEMENT_MODULE.core_voltage;
    state->power_snapshot.chip_temp_avg = state->POWER_MANAGEMENT_MODULE.chip_temp_avg;
    state->power_snapshot.vr_temp = state->POWER_MANAGEMENT_MODULE.vr_temp;
    state->power_snapshot.actual_frequency = state->POWER_MANAGEMENT_MODULE.actual_frequency;
    state->power_snapshot.fan_perc = state->POWER_MANAGEMENT_MODULE.fan_perc;
    state->power_snapshot.fan_rpm = state->POWER_MANAGEMENT_MODULE.fan_rpm;
    state->overheat_snapshot = state->SYSTEM_MODULE.overheat_mode;
    state->power_fault_snapshot = state->SYSTEM_MODULE.power_fault;
    SYSTEM_stats_unlock();
}

static void rejected_reason_insert_locked(SystemModule *module, const char *error_msg)
{
    for (int i = 0; i < module->rejected_reason_stats_count; i++) {
        if (strncmp(module->rejected_reason_stats[i].message, error_msg,
                     sizeof(module->rejected_reason_stats[i].message) - 1) == 0) {
            module->rejected_reason_stats[i].count++;
            return;
        }
    }
    size_t capacity = sizeof(module->rejected_reason_stats) / sizeof(module->rejected_reason_stats[0]);
    int index = module->rejected_reason_stats_count < (int)capacity ?
        module->rejected_reason_stats_count++ : (int)capacity - 1;
    RejectedReasonStat *entry = &module->rejected_reason_stats[index];
    size_t length = strnlen(error_msg, sizeof(entry->message) - 1);
    memcpy(entry->message, error_msg, length);
    entry->message[length] = '\0';
    entry->count = 1;
    qsort(module->rejected_reason_stats, module->rejected_reason_stats_count,
          sizeof(module->rejected_reason_stats[0]), compare_rejected_reason_stats);
}

void SYSTEM_get_runtime_snapshot(GlobalState *state, SystemRuntimeSnapshot *snapshot)
{
    SYSTEM_stats_lock();
    snapshot->power = state->power_snapshot;
    snapshot->overheat_mode = state->overheat_snapshot;
    snapshot->power_fault = state->power_fault_snapshot;
    snapshot->ap_enabled = state->SYSTEM_MODULE.ap_enabled;
    snapshot->is_using_fallback = state->SYSTEM_MODULE.is_using_fallback;
    memcpy(snapshot->wifi_status, state->SYSTEM_MODULE.wifi_status,
           sizeof(snapshot->wifi_status));
    snapshot->shares_accepted = state->SYSTEM_MODULE.shares_accepted;
    snapshot->shares_rejected = state->SYSTEM_MODULE.shares_rejected;
    snapshot->share_submit_queue_high_water = state->SYSTEM_MODULE.share_submit_queue_high_water;
    snapshot->share_submit_queue_drops = state->SYSTEM_MODULE.share_submit_queue_drops;
    snapshot->block_submit_queue_drops = state->SYSTEM_MODULE.block_submit_queue_drops;
    snapshot->share_submit_last_queue_wait_ms = state->SYSTEM_MODULE.share_submit_last_queue_wait_ms;
    snapshot->share_submit_max_queue_wait_ms = state->SYSTEM_MODULE.share_submit_max_queue_wait_ms;
    snapshot->share_submit_last_send_ms = state->SYSTEM_MODULE.share_submit_last_send_ms;
    snapshot->share_submit_max_send_ms = state->SYSTEM_MODULE.share_submit_max_send_ms;
    snapshot->rejected_reason_stats_count = state->SYSTEM_MODULE.rejected_reason_stats_count;
    memcpy(snapshot->rejected_reason_stats, state->SYSTEM_MODULE.rejected_reason_stats, sizeof(snapshot->rejected_reason_stats));
    memcpy(snapshot->best_diff_string, state->SYSTEM_MODULE.best_diff_string, sizeof(snapshot->best_diff_string));
    memcpy(snapshot->best_session_diff_string, state->SYSTEM_MODULE.best_session_diff_string, sizeof(snapshot->best_session_diff_string));
    snapshot->raw_result_count = state->ASIC_TASK_MODULE.raw_result_count;
    snapshot->duplicate_result_count = state->ASIC_TASK_MODULE.duplicate_result_count;
    snapshot->job_lookup_miss_count = state->ASIC_TASK_MODULE.job_lookup_miss_count;
    snapshot->valid_result_count = state->ASIC_TASK_MODULE.valid_result_count;
    snapshot->invalid_result_count = state->ASIC_TASK_MODULE.invalid_result_count;
    snapshot->retired_job_match_count = state->ASIC_TASK_MODULE.retired_job_match_count;
    // Format the live counters; ASIC drivers only refresh their legacy string every 20 results.
    snprintf(snapshot->chip_submit_srt, sizeof(snapshot->chip_submit_srt),
             "[%" PRIu32 ", %" PRIu32 ", %" PRIu32 ", %" PRIu32 ", %" PRIu32 ", %" PRIu32 "]",
             state->chip_submit[0], state->chip_submit[1], state->chip_submit[2],
             state->chip_submit[3], state->chip_submit[4], state->chip_submit[5]);
    SYSTEM_stats_unlock();

    const char *fault_message = snapshot->power_fault ?
        VCORE_get_fault_string(state) : NULL;
    snprintf(snapshot->power_fault_message, sizeof(snapshot->power_fault_message), "%s",
             fault_message != NULL ? fault_message : "");
}

void SYSTEM_get_stratum_snapshot(GlobalState *state, uint32_t *difficulty)
{
    pthread_mutex_lock(&state->stratum_state_lock);
    *difficulty = state->stratum_difficulty;
    pthread_mutex_unlock(&state->stratum_state_lock);
}


#ifdef CONFIG_SOCKS5_PROXY_ENABLED
#define DEFAULT_SOCKS5_PROXY_ENABLED 1
#else
#define DEFAULT_SOCKS5_PROXY_ENABLED 0
#endif

static esp_netif_t *netif;

static esp_err_t ensure_overheat_mode_config(void);
static void trusted_time_sync_notification_cb(struct timeval *tv);
static bool _check_for_best_diff_locked(GlobalState *GLOBAL_STATE, double diff);
static void _suffix_string(uint64_t val, char *buf, size_t bufsiz, int sigdigits);
static void reset_hashrate_history(SystemModule *module);
static bool hashrate_observation_is_active(GlobalState *GLOBAL_STATE);
static void append_hashrate_sample(GlobalState *GLOBAL_STATE, uint32_t validated_difficulty);
static double calculate_observed_hashrate_ghs(const SystemModule *module, int64_t now_us);

static GlobalState *trusted_time_global_state;

bool SYSTEM_init_hashrate_lock(GlobalState *GLOBAL_STATE)
{
    if (GLOBAL_STATE == NULL) {
        return false;
    }

    return pthread_mutex_init(&GLOBAL_STATE->hashrate_lock, NULL) == 0;
}

void SYSTEM_init_system(GlobalState *GLOBAL_STATE)
{
    SystemModule *module = &GLOBAL_STATE->SYSTEM_MODULE;

    pthread_mutex_lock(&GLOBAL_STATE->hashrate_lock);
    reset_hashrate_history(module);
    module->current_hashrate = 0;
    pthread_mutex_unlock(&GLOBAL_STATE->hashrate_lock);
    module->screen_page = 0;
    module->shares_accepted = 0;
    module->shares_rejected = 0;
    module->best_nonce_diff = nvs_config_get_u64(NVS_CONFIG_BEST_DIFF, 0);
    module->best_target_valid = false;
    module->best_session_nonce_diff = 0;
    module->best_session_target_valid = false;
    module->start_time = esp_timer_get_time();
    module->lastClockSync = 0;
    module->trusted_time_available = false;
    module->FOUND_BLOCK = false;

    module->pool_url = nvs_config_get_string(NVS_CONFIG_STRATUM_URL, CONFIG_STRATUM_URL);
    module->fallback_pool_url = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_URL, CONFIG_FALLBACK_STRATUM_URL);
    module->pool_port = nvs_config_get_u16(NVS_CONFIG_STRATUM_PORT, CONFIG_STRATUM_PORT);
    module->fallback_pool_port = nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_PORT, CONFIG_FALLBACK_STRATUM_PORT);
    module->pool_user = nvs_config_get_string(NVS_CONFIG_STRATUM_USER, CONFIG_STRATUM_USER);
    module->fallback_pool_user = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_USER, CONFIG_FALLBACK_STRATUM_USER);
    module->pool_pass = nvs_config_get_string(NVS_CONFIG_STRATUM_PASS, CONFIG_STRATUM_PW);
    module->fallback_pool_pass = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_PASS, CONFIG_FALLBACK_STRATUM_PW);
    module->pool_protocol = nvs_config_get_u16_clamped(NVS_CONFIG_STRATUM_PROTOCOL,
                                                       CONFIG_STRATUM_PROTOCOL,
                                                       STRATUM_PROTOCOL_V1,
                                                       STRATUM_PROTOCOL_V2);
    module->fallback_pool_protocol = nvs_config_get_u16_clamped(NVS_CONFIG_FALLBACK_STRATUM_PROTOCOL,
                                                                CONFIG_FALLBACK_STRATUM_PROTOCOL,
                                                                STRATUM_PROTOCOL_V1,
                                                                STRATUM_PROTOCOL_V2);
    module->sv2_host = nvs_config_get_string(NVS_CONFIG_SV2_HOST, CONFIG_SV2_HOST);
    module->sv2_port = nvs_config_get_u16(NVS_CONFIG_SV2_PORT, CONFIG_SV2_PORT);
    module->sv2_authority_public_key =
        nvs_config_get_string(NVS_CONFIG_SV2_AUTHORITY_PUBLIC_KEY, CONFIG_SV2_AUTHORITY_PUBLIC_KEY);
    module->fallback_sv2_host = nvs_config_get_string(NVS_CONFIG_FALLBACK_SV2_HOST, CONFIG_FALLBACK_SV2_HOST);
    module->fallback_sv2_port = nvs_config_get_u16(NVS_CONFIG_FALLBACK_SV2_PORT, CONFIG_FALLBACK_SV2_PORT);
    module->fallback_sv2_authority_public_key =
        nvs_config_get_string(NVS_CONFIG_FALLBACK_SV2_AUTHORITY_PUBLIC_KEY,
                              CONFIG_FALLBACK_SV2_AUTHORITY_PUBLIC_KEY);
    module->socks5_proxy_enabled =
        nvs_config_get_u16(NVS_CONFIG_SOCKS5_PROXY_ENABLED, DEFAULT_SOCKS5_PROXY_ENABLED) != 0;
    module->socks5_proxy_host = nvs_config_get_string(NVS_CONFIG_SOCKS5_PROXY_HOST, CONFIG_SOCKS5_PROXY_HOST);
    module->socks5_proxy_port = nvs_config_get_u16(NVS_CONFIG_SOCKS5_PROXY_PORT, CONFIG_SOCKS5_PROXY_PORT);
    module->socks5_proxy_user = nvs_config_get_string(NVS_CONFIG_SOCKS5_PROXY_USER, CONFIG_SOCKS5_PROXY_USER);
    module->socks5_proxy_pass = nvs_config_get_string(NVS_CONFIG_SOCKS5_PROXY_PASS, CONFIG_SOCKS5_PROXY_PASS);
    module->is_using_fallback = false;

    module->overheat_mode = 0;
    ESP_LOGI(TAG, "Runtime overheat mode reset to: %d", module->overheat_mode);
    module->power_fault = 0;

    _suffix_string(module->best_nonce_diff, module->best_diff_string, DIFF_STRING_SIZE, 0);
    _suffix_string(module->best_session_nonce_diff, module->best_session_diff_string, DIFF_STRING_SIZE, 0);

    memset(module->best_target, 0xff, sizeof(module->best_target));
    memset(module->best_session_target, 0xff, sizeof(module->best_session_target));
    if (module->best_nonce_diff > 0) {
        difficulty_to_target_le(module->best_nonce_diff, module->best_target);
        module->best_target_valid = true;
    }

    memset(module->ssid, 0, sizeof(module->ssid));
    memset(module->wifi_status, 0, sizeof(module->wifi_status));
}

static esp_err_t peripheral_startup_failed(GlobalState *state, esp_err_t err, const char *stage)
{
    ESP_LOGE(TAG, "%s failed: %s", stage, esp_err_to_name(err));
    state->SYSTEM_MODULE.asic_status = "Peripheral init failed";
    esp_err_t stop_err = Power_disable(state);
    if (stop_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to disable ASIC power after startup error: %s", esp_err_to_name(stop_err));
    }
    return err;
}

esp_err_t SYSTEM_init_peripherals(GlobalState *GLOBAL_STATE)
{
    uint16_t requested_core_voltage =
        nvs_config_get_u16_clamped(NVS_CONFIG_ASIC_VOLTAGE,
                                   CONFIG_ASIC_VOLTAGE,
                                   NVS_CONFIG_ASIC_VOLTAGE_MIN_MV,
                                   NVS_CONFIG_ASIC_VOLTAGE_MAX_MV);
    uint16_t startup_core_voltage = POWER_MANAGEMENT_get_startup_voltage_mv(
        ASIC_get_asic_count(GLOBAL_STATE),
        requested_core_voltage);

    ESP_RETURN_ON_ERROR(gpio_install_isr_service(0), TAG, "Error installing ISR service");
    esp_err_t err = VCORE_init(GLOBAL_STATE);
    if (err != ESP_OK) {
        return peripheral_startup_failed(GLOBAL_STATE, err, "VCORE init");
    }
    if (startup_core_voltage != requested_core_voltage) {
        ESP_LOGI(TAG,
                 "Applying startup warm-up voltage for multi-ASIC bring-up: requested %umV, using %umV",
                 requested_core_voltage,
                 startup_core_voltage);
    }
    err = VCORE_set_voltage((float)startup_core_voltage / 1000.0f, GLOBAL_STATE);
    if (err != ESP_OK) {
        return peripheral_startup_failed(GLOBAL_STATE, err, "VCORE set voltage");
    }
    err = Thermal_init(GLOBAL_STATE->device_model, nvs_config_get_u16(NVS_CONFIG_INVERT_FAN_POLARITY, 1));
    if (err != ESP_OK) {
        return peripheral_startup_failed(GLOBAL_STATE, err, "Thermal init");
    }

    vTaskDelay(500 / portTICK_PERIOD_MS);
    ESP_RETURN_ON_ERROR(ensure_overheat_mode_config(), TAG, "Failed to ensure overheat_mode config");

    bool ui_ready = false;
    switch (GLOBAL_STATE->device_model) {
        case DEVICE_ULTRA:
        case DEVICE_SUPRA:
        case DEVICE_GAMMA:
        case DEVICE_GAMMATURBO:
        case DEVICE_HEX:
        case DEVICE_SUPRAHEX:
            ui_ready = display_init(GLOBAL_STATE) == ESP_OK;
            if (!ui_ready || !GLOBAL_STATE->SYSTEM_MODULE.is_screen_active) {
                ESP_LOGW(TAG, "OLED init failed!");
            } else {
                ESP_LOGI(TAG, "OLED init success!");
            }
            break;
        default:
            break;
    }

    // Optional UI failures must not stop startup with the ASIC rail enabled.
    // In particular, input uses LVGL and cannot run after display/LVGL init failed.
    if (ui_ready) {
        if (input_init(screen_next, toggle_wifi_softap) != ESP_OK) {
            ESP_LOGW(TAG, "Button initialization failed");
        }
        if (screen_start(GLOBAL_STATE) != ESP_OK) {
            ESP_LOGW(TAG, "Screen initialization failed");
        }
    }

    netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    return ESP_OK;
}

void SYSTEM_notify_accepted_share(GlobalState *GLOBAL_STATE)
{
    SYSTEM_notify_accepted_shares(GLOBAL_STATE, 1);
}

void SYSTEM_notify_accepted_shares(GlobalState *GLOBAL_STATE, uint32_t count)
{
    SYSTEM_stats_lock();
    GLOBAL_STATE->SYSTEM_MODULE.shares_accepted += count;
    SYSTEM_stats_unlock();
}

static int compare_rejected_reason_stats(const void *a, const void *b)
{
    const RejectedReasonStat *ea = a;
    const RejectedReasonStat *eb = b;
    return (eb->count > ea->count) - (ea->count > eb->count);
}

void SYSTEM_notify_rejected_share(GlobalState *GLOBAL_STATE, char *error_msg)
{
    SystemModule *module = &GLOBAL_STATE->SYSTEM_MODULE;
    if (error_msg == NULL) error_msg = "unknown";
    SYSTEM_stats_lock();
    module->shares_rejected++;
    rejected_reason_insert_locked(module, error_msg);
    SYSTEM_stats_unlock();
}

void SYSTEM_notify_mining_started(GlobalState *GLOBAL_STATE)
{
    SYSTEM_reset_hashrate_estimate(GLOBAL_STATE);
    SYSTEM_update_hashrate_estimate(GLOBAL_STATE);
}

void SYSTEM_start_trusted_time_sync(GlobalState *GLOBAL_STATE)
{
    trusted_time_global_state = GLOBAL_STATE;

    if (esp_sntp_enabled()) {
        ESP_LOGD(TAG, "SNTP already initialized");
        return;
    }

    ESP_LOGI(TAG, "Starting SNTP time sync");
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(trusted_time_sync_notification_cb);
    esp_sntp_init();
}

bool SYSTEM_has_trusted_time(GlobalState *GLOBAL_STATE)
{
    return GLOBAL_STATE->SYSTEM_MODULE.trusted_time_available;
}

void SYSTEM_notify_new_ntime(GlobalState *GLOBAL_STATE, uint32_t ntime)
{
    SystemModule *module = &GLOBAL_STATE->SYSTEM_MODULE;

    if (module->trusted_time_available) {
        return;
    }

    if (module->lastClockSync + (60 * 60) > ntime) {
        return;
    }

    ESP_LOGI(TAG, "Syncing clock");
    module->lastClockSync = ntime;

    struct timeval tv;
    tv.tv_sec = ntime;
    tv.tv_usec = 0;
    settimeofday(&tv, NULL);
}

static void trusted_time_sync_notification_cb(struct timeval *tv)
{
    if (trusted_time_global_state == NULL || tv == NULL) {
        return;
    }

    trusted_time_global_state->SYSTEM_MODULE.trusted_time_available = true;
    trusted_time_global_state->SYSTEM_MODULE.lastClockSync = (uint32_t)tv->tv_sec;
    ESP_LOGI(TAG, "Trusted SNTP time synchronized");
}

bool SYSTEM_is_potential_best_nonce(GlobalState *GLOBAL_STATE, const uint8_t hash[32])
{
    SystemModule *module = &GLOBAL_STATE->SYSTEM_MODULE;
    bool potential_best;

    pthread_mutex_lock(&GLOBAL_STATE->stratum_state_lock);
    potential_best = !module->best_session_target_valid ||
                     hash_meets_target(hash, module->best_session_target) ||
                     !module->best_target_valid ||
                     hash_meets_target(hash, module->best_target);
    pthread_mutex_unlock(&GLOBAL_STATE->stratum_state_lock);

    return potential_best;
}

void SYSTEM_notify_found_nonce(GlobalState *GLOBAL_STATE, bool found_block, double found_diff,
                               uint32_t validated_difficulty, uint32_t work_epoch)
{
    SystemModule *module = &GLOBAL_STATE->SYSTEM_MODULE;
    uint8_t asic_count;
    uint16_t small_core_count;
    double frequency_mhz;
    bool observation_active;
    bool persist_best_diff = false;
    uint64_t best_diff_to_persist = 0;

    pthread_mutex_lock(&GLOBAL_STATE->stratum_state_lock);
    if (GLOBAL_STATE->abandon_work != 0 || GLOBAL_STATE->work_epoch != work_epoch ||
            validated_difficulty == 0) {
        pthread_mutex_unlock(&GLOBAL_STATE->stratum_state_lock);
        return;
    }

    asic_count = ASIC_get_asic_count(GLOBAL_STATE);
    small_core_count = ASIC_get_small_core_count(GLOBAL_STATE);
    frequency_mhz = GLOBAL_STATE->POWER_MANAGEMENT_MODULE.actual_frequency > 0.0f ?
        GLOBAL_STATE->POWER_MANAGEMENT_MODULE.actual_frequency :
        GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value;
    observation_active = GLOBAL_STATE->ASIC_initalized &&
                          GLOBAL_STATE->job_queue_initalized &&
                          GLOBAL_STATE->sock >= 0 &&
                         asic_count > 0 &&
                         small_core_count > 0 &&
                          frequency_mhz > 0.0;

    if (observation_active) {
        pthread_mutex_lock(&GLOBAL_STATE->hashrate_lock);
        append_hashrate_sample(GLOBAL_STATE, validated_difficulty);
        module->current_hashrate = calculate_observed_hashrate_ghs(module, esp_timer_get_time());
        pthread_mutex_unlock(&GLOBAL_STATE->hashrate_lock);
    }

    if (found_block) {
        module->FOUND_BLOCK = true;
    }

    if (found_diff >= 0.0) {
        SYSTEM_stats_lock();
        persist_best_diff = _check_for_best_diff_locked(GLOBAL_STATE, found_diff);
        best_diff_to_persist = module->best_nonce_diff;
        SYSTEM_stats_unlock();
    }
    pthread_mutex_unlock(&GLOBAL_STATE->stratum_state_lock);

    if (persist_best_diff) {
        nvs_config_set_u64(NVS_CONFIG_BEST_DIFF, best_diff_to_persist);
    }
}

void SYSTEM_reset_hashrate_estimate(GlobalState *GLOBAL_STATE)
{
    SystemModule *module = &GLOBAL_STATE->SYSTEM_MODULE;

    pthread_mutex_lock(&GLOBAL_STATE->hashrate_lock);
    reset_hashrate_history(module);
    module->current_hashrate = 0.0;
    pthread_mutex_unlock(&GLOBAL_STATE->hashrate_lock);
}

void SYSTEM_update_hashrate_estimate(GlobalState *GLOBAL_STATE)
{
    SystemModule *module = &GLOBAL_STATE->SYSTEM_MODULE;
    bool observation_active = hashrate_observation_is_active(GLOBAL_STATE);

    pthread_mutex_lock(&GLOBAL_STATE->hashrate_lock);
    if (!observation_active) {
        reset_hashrate_history(module);
        module->current_hashrate = 0.0;
    } else {
        module->current_hashrate = calculate_observed_hashrate_ghs(module, esp_timer_get_time());
    }
    pthread_mutex_unlock(&GLOBAL_STATE->hashrate_lock);
}

double SYSTEM_get_current_hashrate(GlobalState *GLOBAL_STATE)
{
    double current_hashrate;

    pthread_mutex_lock(&GLOBAL_STATE->hashrate_lock);
    current_hashrate = GLOBAL_STATE->SYSTEM_MODULE.current_hashrate;
    pthread_mutex_unlock(&GLOBAL_STATE->hashrate_lock);

    return current_hashrate;
}

// The caller holds statistics_lock and stratum_state_lock. The targets and
// their display strings must be published together.
static bool _check_for_best_diff_locked(GlobalState *GLOBAL_STATE, double diff)
{
    SystemModule *module = &GLOBAL_STATE->SYSTEM_MODULE;
    uint64_t diff_u64 = (uint64_t)diff;

    if (diff_u64 == 0) {
        return false;
    }

    if (diff_u64 > module->best_session_nonce_diff) {
        module->best_session_nonce_diff = diff_u64;
        difficulty_to_target_le(diff_u64, module->best_session_target);
        module->best_session_target_valid = true;
        _suffix_string(diff_u64, module->best_session_diff_string, DIFF_STRING_SIZE, 0);
    }

    if (diff_u64 <= module->best_nonce_diff) {
        return false;
    }

    module->best_nonce_diff = diff_u64;
    difficulty_to_target_le(diff_u64, module->best_target);
    module->best_target_valid = true;
    _suffix_string(diff_u64, module->best_diff_string, DIFF_STRING_SIZE, 0);
    return true;
}

static void reset_hashrate_history(SystemModule *module)
{
    module->duration_start = 0.0;
    module->historical_hashrate_rolling_index = 0;
    module->historical_hashrate_init = 0;
    memset(module->historical_hashrate_time_stamps, 0, sizeof(module->historical_hashrate_time_stamps));
    memset(module->historical_hashrate, 0, sizeof(module->historical_hashrate));
}

static bool hashrate_observation_is_active(GlobalState *GLOBAL_STATE)
{
    uint8_t asic_count;
    uint16_t small_core_count;
    double frequency_mhz;

    if (!GLOBAL_STATE->ASIC_initalized || !GLOBAL_STATE->job_queue_initalized) {
        return false;
    }
    if (GLOBAL_STATE->sock < 0 || stratum_is_abandoning_work(GLOBAL_STATE)) {
        return false;
    }

    asic_count = ASIC_get_asic_count(GLOBAL_STATE);
    small_core_count = ASIC_get_small_core_count(GLOBAL_STATE);
    frequency_mhz = GLOBAL_STATE->POWER_MANAGEMENT_MODULE.actual_frequency > 0.0f ?
        GLOBAL_STATE->POWER_MANAGEMENT_MODULE.actual_frequency :
        GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value;

    return asic_count > 0 && small_core_count > 0 && frequency_mhz > 0.0;
}

static void append_hashrate_sample(GlobalState *GLOBAL_STATE, uint32_t validated_difficulty)
{
    SystemModule *module = &GLOBAL_STATE->SYSTEM_MODULE;
    int index = module->historical_hashrate_rolling_index;

    module->historical_hashrate[index] = (double)validated_difficulty;
    module->historical_hashrate_time_stamps[index] = (double)esp_timer_get_time();
    module->historical_hashrate_rolling_index = (index + 1) % HISTORY_LENGTH;

    if (module->historical_hashrate_init < HISTORY_LENGTH) {
        module->historical_hashrate_init++;
    }
}

static double calculate_observed_hashrate_ghs(const SystemModule *module, int64_t now_us)
{
    int sample_count;
    int oldest_index;
    double sum = 0.0;
    double oldest_ts;
    double duration_s;

    if (module->historical_hashrate_init < 2) {
        return 0.0;
    }

    sample_count = module->historical_hashrate_init;
    oldest_index = (sample_count < HISTORY_LENGTH) ? 0 : module->historical_hashrate_rolling_index;
    oldest_ts = module->historical_hashrate_time_stamps[oldest_index];
    if (oldest_ts <= 0.0) {
        return 0.0;
    }

    if (sample_count < HISTORY_LENGTH) {
        for (int i = 1; i < sample_count; i++) {
            sum += module->historical_hashrate[i];
        }
    } else {
        for (int i = 1; i < HISTORY_LENGTH; i++) {
            int idx = (module->historical_hashrate_rolling_index + i) % HISTORY_LENGTH;
            sum += module->historical_hashrate[idx];
        }
    }

    duration_s = ((double)now_us - oldest_ts) / 1000000.0;
    if (duration_s <= 0.0) {
        return 0.0;
    }

    return (sum * 4294967296.0) / (duration_s * 1000000000.0);
}

static void _suffix_string(uint64_t val, char *buf, size_t bufsiz, int sigdigits)
{
    const double dkilo = 1000.0;
    const uint64_t kilo = 1000ull;
    const uint64_t mega = 1000000ull;
    const uint64_t giga = 1000000000ull;
    const uint64_t tera = 1000000000000ull;
    const uint64_t peta = 1000000000000000ull;
    const uint64_t exa = 1000000000000000000ull;
    char suffix[2] = "";
    bool decimal = true;
    double dval;

    if (val >= exa) {
        val /= peta;
        dval = (double)val / dkilo;
        strcpy(suffix, "E");
    } else if (val >= peta) {
        val /= tera;
        dval = (double)val / dkilo;
        strcpy(suffix, "P");
    } else if (val >= tera) {
        val /= giga;
        dval = (double)val / dkilo;
        strcpy(suffix, "T");
    } else if (val >= giga) {
        val /= mega;
        dval = (double)val / dkilo;
        strcpy(suffix, "G");
    } else if (val >= mega) {
        val /= kilo;
        dval = (double)val / dkilo;
        strcpy(suffix, "M");
    } else if (val >= kilo) {
        dval = (double)val / dkilo;
        strcpy(suffix, "k");
    } else {
        dval = (double)val;
        decimal = false;
    }

    if (!sigdigits) {
        if (decimal) {
            snprintf(buf, bufsiz, "%.2f%s", dval, suffix);
        } else {
            snprintf(buf, bufsiz, "%d%s", (unsigned int)dval, suffix);
        }
    } else {
        int ndigits = sigdigits - 1 - (dval > 0.0 ? floor(log10(dval)) : 0);
        snprintf(buf, bufsiz, "%*.*f%s", sigdigits + 1, ndigits, dval, suffix);
    }
}

static esp_err_t ensure_overheat_mode_config(void)
{
    uint16_t overheat_mode = nvs_config_get_u16(NVS_CONFIG_OVERHEAT_MODE, UINT16_MAX);

    if (overheat_mode == UINT16_MAX) {
        nvs_config_set_u16(NVS_CONFIG_OVERHEAT_MODE, 0);
        ESP_LOGI(TAG, "Default value for overheat_mode set to 0");
    } else {
        ESP_LOGI(TAG, "Existing overheat_mode value: %d", overheat_mode);
    }

    return ESP_OK;
}
