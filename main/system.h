#ifndef SYSTEM_H_
#define SYSTEM_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "global_state.h"

bool SYSTEM_init_hashrate_lock(GlobalState *GLOBAL_STATE);
void SYSTEM_init_system(GlobalState * GLOBAL_STATE);
esp_err_t SYSTEM_init_peripherals(GlobalState * GLOBAL_STATE);

void SYSTEM_notify_accepted_share(GlobalState * GLOBAL_STATE);
void SYSTEM_notify_accepted_shares(GlobalState *GLOBAL_STATE, uint32_t count);
void SYSTEM_notify_rejected_share(GlobalState * GLOBAL_STATE, char * error_msg);
bool SYSTEM_is_potential_best_nonce(GlobalState * GLOBAL_STATE, const uint8_t hash[32]);
void SYSTEM_notify_found_nonce(GlobalState *GLOBAL_STATE, bool found_block, double found_diff,
                               uint32_t validated_difficulty, uint32_t work_epoch);
void SYSTEM_notify_mining_started(GlobalState * GLOBAL_STATE);
void SYSTEM_notify_new_ntime(GlobalState * GLOBAL_STATE, uint32_t ntime);
void SYSTEM_start_trusted_time_sync(GlobalState *GLOBAL_STATE);
bool SYSTEM_has_trusted_time(GlobalState *GLOBAL_STATE);
void SYSTEM_reset_hashrate_estimate(GlobalState * GLOBAL_STATE);
void SYSTEM_update_hashrate_estimate(GlobalState * GLOBAL_STATE);
double SYSTEM_get_current_hashrate(GlobalState *GLOBAL_STATE);

typedef struct {
    PowerRuntimeSnapshot power;
    uint16_t overheat_mode, power_fault;
    bool ap_enabled, is_using_fallback;
    char wifi_status[20];
    char power_fault_message[256];
    uint64_t shares_accepted, shares_rejected;
    uint32_t share_submit_queue_high_water, share_submit_queue_drops;
    uint32_t block_submit_queue_drops;
    uint32_t share_submit_last_queue_wait_ms, share_submit_max_queue_wait_ms;
    uint32_t share_submit_last_send_ms, share_submit_max_send_ms;
    RejectedReasonStat rejected_reason_stats[10];
    int rejected_reason_stats_count;
    char best_diff_string[DIFF_STRING_SIZE];
    char best_session_diff_string[DIFF_STRING_SIZE];
    uint32_t stratum_difficulty;
    uint64_t raw_result_count, duplicate_result_count, job_lookup_miss_count;
    uint64_t valid_result_count, invalid_result_count, retired_job_match_count;
    char chip_submit_srt[128];
} SystemRuntimeSnapshot;

void SYSTEM_stats_lock(void);
void SYSTEM_stats_unlock(void);
uint64_t SYSTEM_increment_counter(uint64_t *counter);
uint64_t SYSTEM_read_counter(const uint64_t *counter);
bool SYSTEM_get_is_using_fallback(GlobalState *state);
void SYSTEM_set_is_using_fallback(GlobalState *state, bool enabled);
void SYSTEM_publish_power_snapshot(GlobalState *state);
void SYSTEM_get_runtime_snapshot(GlobalState *state, SystemRuntimeSnapshot *snapshot);
void SYSTEM_get_stratum_snapshot(GlobalState *state, uint32_t *difficulty);

#endif /* SYSTEM_H_ */
