/******************************************************************************
 * *
 * References:
 * 1. Stratum Protocol - [link](https://reference.cash/mining/stratum-protocol)
 *****************************************************************************/

#include "stratum_api.h"
#include "cJSON.h"
#include "extranonce.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "utils.h"
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#define BUFFER_SIZE 1024
#define STRATUM_TX_BUFFER_MIN_SIZE (BUFFER_SIZE * 2)
#define STRATUM_RX_LINE_MAX_SIZE (64U * 1024U)
static const char * TAG = "stratum_api";

static char * json_rpc_buffer = NULL;
static size_t json_rpc_buffer_size = 0;
static size_t json_rpc_start = 0;
static size_t json_rpc_end = 0;
static size_t json_rpc_scan = 0;
static char * stratum_tx_buffer = NULL;
static size_t stratum_tx_buffer_size = 0;
static pthread_mutex_t stratum_tx_lock = PTHREAD_MUTEX_INITIALIZER;

static void debug_stratum_tx(const char *);
static void * stratum_buffer_alloc(size_t size);
static bool stratum_tx_buffer_ensure_locked(size_t size);
static int stratum_write_all_locked(int socket, const char *buffer, size_t length);
static int stratum_writef(int socket, const char *format, ...);
static void stratum_message_prepare(StratumApiV1Message *message);
static void stratum_message_set_error(StratumApiV1Message *message, const char *error_str);
static void stratum_message_set_out_of_memory(StratumApiV1Message *message);
static bool stratum_json_array_string(cJSON *array, int index, const char **value,
                                      StratumApiV1Message *message, const char *field_name);
static bool stratum_hex_string_is_valid(const char *value, size_t expected_len);
static bool stratum_parse_hex_u32(cJSON *array, int index, uint32_t *value,
                                  StratumApiV1Message *message, const char *field_name);
static bool stratum_decode_hex_alloc(const char *hex, uint8_t **bin, size_t *bin_len,
                                     StratumApiV1Message *message, const char *field_name);

bool STRATUM_V1_initialize_buffer(void)
{
    if (json_rpc_buffer == NULL) {
        json_rpc_buffer = stratum_buffer_alloc(BUFFER_SIZE);
        json_rpc_buffer_size = BUFFER_SIZE;
        if (json_rpc_buffer == NULL) {
            ESP_LOGE(TAG, "Failed to allocate RX buffer");
            return false;
        }
        memset(json_rpc_buffer, 0, BUFFER_SIZE);
    }

    pthread_mutex_lock(&stratum_tx_lock);
    if (!stratum_tx_buffer_ensure_locked(STRATUM_TX_BUFFER_MIN_SIZE)) {
        pthread_mutex_unlock(&stratum_tx_lock);
        ESP_LOGE(TAG, "Failed to allocate TX buffer");
        return false;
    }
    pthread_mutex_unlock(&stratum_tx_lock);
    return true;
}

void STRATUM_V1_reset_buffer(void)
{
    free(json_rpc_buffer);
    json_rpc_buffer = NULL;
    json_rpc_buffer_size = 0;
    json_rpc_start = json_rpc_end = json_rpc_scan = 0;
}

static bool reserve_json_buffer(size_t extra)
{
    if (json_rpc_buffer_size - json_rpc_end >= extra) return true;
    size_t unread = json_rpc_end - json_rpc_start;
    json_rpc_scan -= json_rpc_start;
    if (json_rpc_start > 0) memmove(json_rpc_buffer, json_rpc_buffer + json_rpc_start, unread);
    json_rpc_start = 0;
    json_rpc_end = unread;
    if (json_rpc_buffer_size - unread >= extra) return true;

    size_t capacity = json_rpc_buffer_size;
    while (capacity < unread + extra) capacity *= 2;
    if (capacity > STRATUM_RX_LINE_MAX_SIZE + BUFFER_SIZE) {
        capacity = STRATUM_RX_LINE_MAX_SIZE + BUFFER_SIZE;
    }
    if (capacity < unread + extra) return false;
    char *next = stratum_buffer_alloc(capacity);
    if (next == NULL) return false;
    memcpy(next, json_rpc_buffer, unread);
    free(json_rpc_buffer);
    json_rpc_buffer = next;
    json_rpc_buffer_size = capacity;
    return true;
}

char *STRATUM_V1_receive_jsonrpc_line(int sockfd)
{
    if (json_rpc_buffer == NULL && !STRATUM_V1_initialize_buffer()) {
        errno = ENOMEM;
        return NULL;
    }
    while (1) {
        char *newline = json_rpc_end > json_rpc_scan ?
            memchr(json_rpc_buffer + json_rpc_scan, '\n', json_rpc_end - json_rpc_scan) : NULL;
        if (newline != NULL) {
            size_t length = (size_t)(newline - (json_rpc_buffer + json_rpc_start));
            if (length > STRATUM_RX_LINE_MAX_SIZE) {
                errno = EMSGSIZE;
                break;
            }
            char *line = malloc(length + 1);
            if (line == NULL) {
                errno = ENOMEM;
                break;
            }
            memcpy(line, json_rpc_buffer + json_rpc_start, length);
            line[length] = '\0';
            json_rpc_start = (size_t)(newline - json_rpc_buffer) + 1;
            json_rpc_scan = json_rpc_start;
            if (json_rpc_start == json_rpc_end) json_rpc_start = json_rpc_end = json_rpc_scan = 0;
            return line;
        }
        if (json_rpc_end - json_rpc_start > STRATUM_RX_LINE_MAX_SIZE) {
            errno = EMSGSIZE;
            break;
        }
        json_rpc_scan = json_rpc_end;
        if (!reserve_json_buffer(BUFFER_SIZE)) {
            errno = ENOMEM;
            break;
        }
        int count = recv(sockfd, json_rpc_buffer + json_rpc_end, BUFFER_SIZE, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            if (count == 0) errno = ECONNRESET;
            break;
        }
        if (memchr(json_rpc_buffer + json_rpc_end, '\0', (size_t)count) != NULL) {
            errno = EINVAL;
            break;
        }
        json_rpc_end += (size_t)count;
    }
    STRATUM_V1_reset_buffer();
    return NULL;
}

void STRATUM_V1_parse(StratumApiV1Message * message, const char * stratum_json)
{
    stratum_message_prepare(message);

    cJSON * json = cJSON_Parse(stratum_json);

    if (json == NULL) {
        ESP_LOGE(TAG, "Error parsing JSON payload: %s", stratum_json);
        message->method = STRATUM_UNKNOWN;
        message->response_success = false;
        return;
    }

    cJSON * id_json = cJSON_GetObjectItem(json, "id");
    int64_t parsed_id = -1;
    if (id_json != NULL && cJSON_IsNumber(id_json)) {
        parsed_id = id_json->valueint;
    }
    message->message_id = parsed_id;

    cJSON * method_json = cJSON_GetObjectItem(json, "method");
    stratum_method result = STRATUM_UNKNOWN;

    if (method_json != NULL && cJSON_IsString(method_json)) {
        if (strcmp("mining.notify", method_json->valuestring) == 0) {
            result = MINING_NOTIFY;
        } else if (strcmp("mining.set_difficulty", method_json->valuestring) == 0) {
            result = MINING_SET_DIFFICULTY;
        } else if (strcmp("mining.set_version_mask", method_json->valuestring) == 0) {
            result = MINING_SET_VERSION_MASK;
        } else if (strcmp("client.reconnect", method_json->valuestring) == 0) {
            result = CLIENT_RECONNECT;
        } else {
            ESP_LOGI(TAG, "unhandled method in stratum message: %s", stratum_json);
        }
    } else {
        cJSON * result_json = cJSON_GetObjectItem(json, "result");
        cJSON * error_json = cJSON_GetObjectItem(json, "error");
        cJSON * reject_reason_json = cJSON_GetObjectItem(json, "reject-reason");

        if (parsed_id == STRATUM_ID_CONFIGURE) {
            cJSON *rolling_enabled = cJSON_IsObject(result_json) ?
                cJSON_GetObjectItem(result_json, "version-rolling") : NULL;
            cJSON *mask = cJSON_IsObject(result_json) ?
                cJSON_GetObjectItem(result_json, "version-rolling.mask") : NULL;

            message->method = STRATUM_RESULT_VERSION_MASK;
            message->version_mask = 0;
            message->response_success = false;

            if ((error_json == NULL || cJSON_IsNull(error_json)) &&
                    cJSON_IsTrue(rolling_enabled) &&
                    cJSON_IsString(mask) && mask->valuestring != NULL &&
                    strlen(mask->valuestring) <= 8 &&
                    stratum_hex_string_is_valid(mask->valuestring,
                                                strlen(mask->valuestring))) {
                message->version_mask = strtoul(mask->valuestring, NULL, 16);
                message->response_success = true;
            }

            if (!message->response_success) {
                stratum_message_set_error(message, "version_rolling_not_supported");
                ESP_LOGW(TAG, "Version rolling negotiation disabled: %s", stratum_json);
            } else {
                ESP_LOGI(TAG, "Negotiated version mask: %08lx", message->version_mask);
            }
            goto done;
        }

        if (result_json == NULL) {
            message->response_success = false;
            stratum_message_set_error(message, "unknown");
        } else if (error_json != NULL && !cJSON_IsNull(error_json)) {
            message->response_success = false;
            stratum_message_set_error(message, "unknown");
            if (parsed_id < 5) {
                result = STRATUM_RESULT_SETUP;
            } else {
                result = STRATUM_RESULT;
            }
            if (cJSON_IsArray(error_json)) {
                int len = cJSON_GetArraySize(error_json);
                if (len >= 2) {
                    cJSON * error_msg = cJSON_GetArrayItem(error_json, 1);
                    if (cJSON_IsString(error_msg)) {
                        stratum_message_set_error(message, cJSON_GetStringValue(error_msg));
                    }
                }
            }
        } else if (cJSON_IsBool(result_json)) {
            if (parsed_id < 5) {
                result = STRATUM_RESULT_SETUP;
            } else {
                result = STRATUM_RESULT;
            }
            if (cJSON_IsTrue(result_json)) {
                message->response_success = true;
            } else {
                message->response_success = false;
                stratum_message_set_error(message, "unknown");
                if (cJSON_IsString(reject_reason_json)) {
                    stratum_message_set_error(message, cJSON_GetStringValue(reject_reason_json));
                }                
            }
        } else if (parsed_id == STRATUM_ID_SUBSCRIBE) {
            result = STRATUM_RESULT_SUBSCRIBE;

            if (!cJSON_IsArray(result_json)) {
                ESP_LOGE(TAG, "Subscribe result is not an array");
                message->response_success = false;
                stratum_message_set_error(message, "malformed_subscribe");
                goto done;
            }

            cJSON * extranonce2_len_json = cJSON_GetArrayItem(result_json, 2);
            if (!cJSON_IsNumber(extranonce2_len_json) ||
                    extranonce2_len_json->valuedouble != (double)extranonce2_len_json->valueint ||
                    extranonce2_len_json->valueint < (int)EXTRANONCE2_MIN_BYTES ||
                    extranonce2_len_json->valueint > (int)EXTRANONCE2_MAX_BYTES) {
                ESP_LOGE(TAG, "Unable to parse extranonce2_len");
                message->response_success = false;
                stratum_message_set_error(message, "malformed_extranonce2_len");
                goto done;
            }
            message->extranonce_2_len = extranonce2_len_json->valueint;

            cJSON * extranonce_json = cJSON_GetArrayItem(result_json, 1);
            if (!cJSON_IsString(extranonce_json) || extranonce_json->valuestring == NULL) {
                ESP_LOGE(TAG, "Unable parse extranonce");
                message->response_success = false;
                stratum_message_set_error(message, "malformed_extranonce");
                goto done;
            }
            if ((strlen(extranonce_json->valuestring) % 2U) != 0U ||
                    !stratum_hex_string_is_valid(extranonce_json->valuestring, strlen(extranonce_json->valuestring))) {
                ESP_LOGE(TAG, "Malformed extranonce hex");
                message->response_success = false;
                stratum_message_set_error(message, "malformed_extranonce");
                goto done;
            }
            
            message->extranonce_str = malloc(strlen(extranonce_json->valuestring) + 1);
            if (message->extranonce_str == NULL) {
                stratum_message_set_out_of_memory(message);
                goto done;
            }
            strcpy(message->extranonce_str, extranonce_json->valuestring);
            
            // [极致优化 2: 全局缓存二进制化]
            message->extranonce_bin_len = strlen(extranonce_json->valuestring) / 2;
            message->extranonce_bin = malloc(message->extranonce_bin_len);
            if (message->extranonce_bin_len > 0 && message->extranonce_bin == NULL) {
                free(message->extranonce_str);
                message->extranonce_str = NULL;
                stratum_message_set_out_of_memory(message);
                goto done;
            }
            if (!hex2bin_exact(extranonce_json->valuestring,
                               message->extranonce_bin,
                               message->extranonce_bin_len)) {
                free(message->extranonce_str);
                free(message->extranonce_bin);
                message->extranonce_str = NULL;
                message->extranonce_bin = NULL;
                message->extranonce_bin_len = 0;
                message->response_success = false;
                stratum_message_set_error(message, "malformed_extranonce");
                goto done;
            }

            message->response_success = true;

            ESP_LOGI(TAG, "extranonce_str: %s", message->extranonce_str);
            ESP_LOGI(TAG, "extranonce_2_len: %d", message->extranonce_2_len);

        } else {
            ESP_LOGI(TAG, "unhandled result in stratum message: %s", stratum_json);
        }
    }

    message->method = result;

    if (message->method == MINING_NOTIFY) {
        cJSON * params = cJSON_GetObjectItem(json, "params");
        
        if (params == NULL || !cJSON_IsArray(params) || cJSON_GetArraySize(params) < 9) {
            ESP_LOGE(TAG, "Malformed mining.notify params received!");
            stratum_message_set_error(message, "malformed_notify");
            goto done;
        }

        mining_notify * new_work = malloc(sizeof(mining_notify));
        if (new_work == NULL) {
            stratum_message_set_out_of_memory(message);
            goto done;
        }

        memset(new_work, 0, sizeof(*new_work));

        const char *job_id = NULL;
        const char *prev_block_hash = NULL;
        const char *coinbase_1 = NULL;
        const char *coinbase_2 = NULL;

        if (!stratum_json_array_string(params, 0, &job_id, message, "job_id") ||
                !stratum_json_array_string(params, 1, &prev_block_hash, message, "prev_block_hash") ||
                !stratum_json_array_string(params, 2, &coinbase_1, message, "coinbase_1") ||
                !stratum_json_array_string(params, 3, &coinbase_2, message, "coinbase_2")) {
            STRATUM_V1_free_mining_notify(new_work);
            goto done;
        }

        new_work->job_id = strdup(job_id);
        if (new_work->job_id == NULL) {
            STRATUM_V1_free_mining_notify(new_work);
            stratum_message_set_out_of_memory(message);
            goto done;
        }
        
        // [极致优化 1: 落地即二进制，消除 strdup]
        if (!hex2bin_exact(prev_block_hash, new_work->prev_block_hash_bin, sizeof(new_work->prev_block_hash_bin))) {
            ESP_LOGE(TAG, "Malformed prev_block_hash in mining.notify");
            STRATUM_V1_free_mining_notify(new_work);
            stratum_message_set_error(message, "malformed_prev_block_hash");
            goto done;
        }
        flip32bytes(new_work->prev_block_hash, new_work->prev_block_hash_bin);
        memcpy(new_work->prev_block_hash_be, new_work->prev_block_hash_bin, sizeof(new_work->prev_block_hash_be));
        reverse_bytes(new_work->prev_block_hash_be, sizeof(new_work->prev_block_hash_be));
        
        if (!stratum_decode_hex_alloc(coinbase_1,
                                      &new_work->coinbase_1_bin,
                                      &new_work->coinbase_1_len,
                                      message,
                                      "coinbase_1") ||
                !stratum_decode_hex_alloc(coinbase_2,
                                          &new_work->coinbase_2_bin,
                                          &new_work->coinbase_2_len,
                                          message,
                                          "coinbase_2")) {
            STRATUM_V1_free_mining_notify(new_work);
            goto done;
        }

        cJSON * merkle_branch = cJSON_GetArrayItem(params, 4);
        if (merkle_branch == NULL || !cJSON_IsArray(merkle_branch)) {
            ESP_LOGE(TAG, "Malformed merkle branches in mining.notify");
            STRATUM_V1_free_mining_notify(new_work);
            stratum_message_set_error(message, "malformed_merkle_branches");
            goto done;
        }
        new_work->n_merkle_branches = cJSON_GetArraySize(merkle_branch);
        if (new_work->n_merkle_branches > MAX_MERKLE_BRANCHES) {
            ESP_LOGE(TAG, "Too many Merkle branches.");
            STRATUM_V1_free_mining_notify(new_work); 
            stratum_message_set_error(message, "too_many_merkle_branches");
            goto done;
        }
        
        if (new_work->n_merkle_branches > 0) {
            new_work->merkle_branches = malloc(HASH_SIZE * new_work->n_merkle_branches);
            if (new_work->merkle_branches == NULL) {
                STRATUM_V1_free_mining_notify(new_work);
                stratum_message_set_out_of_memory(message);
                goto done;
            }
        }
        for (size_t i = 0; i < new_work->n_merkle_branches; i++) {
            cJSON * branch = cJSON_GetArrayItem(merkle_branch, (int)i);
            if (!cJSON_IsString(branch) ||
                    !hex2bin_exact(branch->valuestring, new_work->merkle_branches + HASH_SIZE * i, HASH_SIZE)) {
                ESP_LOGE(TAG, "Malformed merkle branch %u in mining.notify", (unsigned int)i);
                STRATUM_V1_free_mining_notify(new_work);
                stratum_message_set_error(message, "malformed_merkle_branch");
                goto done;
            }
        }

        if (!stratum_parse_hex_u32(params, 5, &new_work->version, message, "version") ||
                !stratum_parse_hex_u32(params, 6, &new_work->target, message, "nbits") ||
                !stratum_parse_hex_u32(params, 7, &new_work->ntime, message, "ntime")) {
            STRATUM_V1_free_mining_notify(new_work);
            goto done;
        }

        message->mining_notification = new_work;

        int paramsLength = cJSON_GetArraySize(params);
        cJSON * clean_jobs = cJSON_GetArrayItem(params, paramsLength - 1);
        if (!cJSON_IsBool(clean_jobs)) {
            ESP_LOGE(TAG, "Malformed clean_jobs flag in mining.notify");
            STRATUM_V1_free_mining_notify(message->mining_notification);
            message->mining_notification = NULL;
            stratum_message_set_error(message, "malformed_clean_jobs");
            goto done;
        }
        message->should_abandon_work = cJSON_IsTrue(clean_jobs);
        
    } else if (message->method == MINING_SET_DIFFICULTY) {
        cJSON * params = cJSON_GetObjectItem(json, "params");
        cJSON * difficulty_json = (params != NULL && cJSON_IsArray(params)) ? cJSON_GetArrayItem(params, 0) : NULL;
        if (!cJSON_IsNumber(difficulty_json)) {
            stratum_message_set_error(message, "malformed_difficulty");
            message->method = STRATUM_UNKNOWN;
            goto done;
        }
        double difficulty_value = difficulty_json->valuedouble;
        uint32_t difficulty = 0;
        if (difficulty_value > 0.0 && isfinite(difficulty_value)) {
            difficulty = difficulty_value > (double)UINT32_MAX
                             ? UINT32_MAX
                             : (uint32_t)difficulty_value;
            message->new_difficulty_value = difficulty_value;
        }
        message->new_difficulty = difficulty;
    } else if (message->method == MINING_SET_VERSION_MASK) {
        cJSON * params = cJSON_GetObjectItem(json, "params");
        uint32_t version_mask = 0;
        if (!stratum_parse_hex_u32(params, 0, &version_mask, message, "version_mask")) {
            message->method = STRATUM_UNKNOWN;
            goto done;
        }
        message->version_mask = version_mask;
    }
    
    done:
    cJSON_Delete(json);
}

// 释放替换为了二进制的指针
void STRATUM_V1_free_mining_notify(mining_notify * params)
{
    if (params == NULL) {
        return;
    }
    free(params->job_id);
    free(params->coinbase_1_bin);
    free(params->coinbase_2_bin);
    free(params->merkle_branches);
    free(params);
}

int STRATUM_V1_subscribe(int socket, int send_uid, char * model)
{
    (void)model;

    return stratum_writef(socket,
                          "{\"id\": %d, \"method\": \"mining.subscribe\", \"params\": [\"NiceHash/1.0.0\"]}\n",
                          send_uid);
}

int STRATUM_V1_suggest_difficulty(int socket, int send_uid, uint32_t difficulty)
{
    return stratum_writef(socket,
                          "{\"id\": %d, \"method\": \"mining.suggest_difficulty\", \"params\": [%lu]}\n",
                          send_uid, (unsigned long)difficulty);
}

static char *json_quote(const char *text)
{
    if (text == NULL) text = "";
    size_t size = 3;
    for (const unsigned char *p = (const unsigned char *)text; *p != 0; p++) {
        size += *p < 0x20 ? 6 : (*p == '"' || *p == '\\' ? 2 : 1);
    }
    char *quoted = malloc(size);
    if (quoted == NULL) return NULL;
    static const char hex[] = "0123456789abcdef";
    char *out = quoted;
    *out++ = '"';
    for (const unsigned char *p = (const unsigned char *)text; *p != 0; p++) {
        if (*p < 0x20) {
            *out++ = '\\'; *out++ = 'u'; *out++ = '0'; *out++ = '0';
            *out++ = hex[*p >> 4]; *out++ = hex[*p & 15];
        } else {
            if (*p == '"' || *p == '\\') *out++ = '\\';
            *out++ = (char)*p;
        }
    }
    *out++ = '"';
    *out = '\0';
    return quoted;
}

int STRATUM_V1_authorize(int socket, int send_uid, const char *username, const char *pass)
{
    char *user_json = json_quote(username);
    char *pass_json = json_quote(pass);
    int result = -1;
    if (user_json != NULL && pass_json != NULL) {
        result = stratum_writef(socket,
            "{\"id\":%d,\"method\":\"mining.authorize\",\"params\":[%s,%s]}\n",
            send_uid, user_json, pass_json);
    } else {
        errno = ENOMEM;
    }
    free(user_json);
    free(pass_json);
    return result;
}

int STRATUM_V1_submit_share(int socket, int send_uid, const char *username, const char *jobid,
                            const char *extranonce_2, uint32_t ntime,
                            uint32_t nonce, uint32_t version, bool version_rolling_negotiated)
{
    char *user_json = json_quote(username);
    char *job_json = json_quote(jobid);
    char *extra_json = json_quote(extranonce_2);
    int result = -1;
    if (user_json != NULL && job_json != NULL && extra_json != NULL) {
        if (version_rolling_negotiated) {
            result = stratum_writef(socket,
                "{\"id\":%d,\"method\":\"mining.submit\",\"params\":[%s,%s,%s,\"%08lx\",\"%08lx\",\"%08lx\"]}\n",
                send_uid, user_json, job_json, extra_json,
                (unsigned long)ntime, (unsigned long)nonce, (unsigned long)version);
        } else {
            result = stratum_writef(socket,
                "{\"id\":%d,\"method\":\"mining.submit\",\"params\":[%s,%s,%s,\"%08lx\",\"%08lx\"]}\n",
                send_uid, user_json, job_json, extra_json,
                (unsigned long)ntime, (unsigned long)nonce);
        }
    } else {
        errno = ENOMEM;
    }
    free(user_json);
    free(job_json);
    free(extra_json);
    return result;
}

int STRATUM_V1_configure_version_rolling(int socket, int send_uid, uint32_t *version_mask,
                                         uint8_t minimum_bit_count)
{
    uint32_t requested_mask = STRATUM_DEFAULT_VERSION_MASK;

    if (version_mask != NULL) {
        requested_mask = *version_mask & STRATUM_DEFAULT_VERSION_MASK;
    }

    return stratum_writef(socket,
                          "{\"id\": %d, \"method\": \"mining.configure\", \"params\": [[\"version-rolling\"], {\"version-rolling.mask\": "
                          "\"%08lx\", \"version-rolling.min-bit-count\": %u}]}\n",
                          send_uid,
                          (unsigned long)requested_mask,
                          (unsigned int)minimum_bit_count);
}

static void debug_stratum_tx(const char * msg)
{
    char * newline = strchr(msg, '\n');
    if (newline != NULL) *newline = '\0';
    ESP_LOGI(TAG, "tx: %s", msg);
    if (newline != NULL) *newline = '\n';
}

static void * stratum_buffer_alloc(size_t size)
{
    void *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = malloc(size);
    }

    return buffer;
}

static bool stratum_tx_buffer_ensure_locked(size_t size)
{
    if (stratum_tx_buffer != NULL && stratum_tx_buffer_size >= size) {
        return true;
    }

    size_t new_size = size;
    if (new_size < STRATUM_TX_BUFFER_MIN_SIZE) {
        new_size = STRATUM_TX_BUFFER_MIN_SIZE;
    }
    if ((new_size % BUFFER_SIZE) != 0) {
        new_size += BUFFER_SIZE - (new_size % BUFFER_SIZE);
    }

    void *new_buffer = stratum_buffer_alloc(new_size);
    if (new_buffer == NULL) {
        return false;
    }

    free(stratum_tx_buffer);
    stratum_tx_buffer = new_buffer;
    stratum_tx_buffer_size = new_size;
    return true;
}

static int stratum_write_all_locked(int socket, const char *buffer, size_t length)
{
    size_t written_total = 0;

    while (written_total < length) {
        int written = write(socket, buffer + written_total, length - written_total);

        if (written > 0) {
            written_total += (size_t)written;
            continue;
        }

        if (written < 0 && errno == EINTR) {
            continue;
        }

        if (written == 0) {
            errno = ECONNRESET;
        }
        return -1;
    }

    return (int)written_total;
}

static int stratum_writef(int socket, const char *format, ...)
{
    int message_len;
    int ret;

    pthread_mutex_lock(&stratum_tx_lock);

    if (!stratum_tx_buffer_ensure_locked(STRATUM_TX_BUFFER_MIN_SIZE)) {
        pthread_mutex_unlock(&stratum_tx_lock);
        errno = ENOMEM;
        return -1;
    }

    while (1) {
        va_list args;
        va_start(args, format);
        message_len = vsnprintf(stratum_tx_buffer, stratum_tx_buffer_size, format, args);
        va_end(args);

        if (message_len < 0) {
            pthread_mutex_unlock(&stratum_tx_lock);
            errno = EINVAL;
            return -1;
        }

        if ((size_t)message_len < stratum_tx_buffer_size) {
            break;
        }

        if (!stratum_tx_buffer_ensure_locked((size_t)message_len + 1)) {
            pthread_mutex_unlock(&stratum_tx_lock);
            errno = ENOMEM;
            return -1;
        }
    }

    if (strstr(format, "mining.authorize") != NULL) {
        ESP_LOGD(TAG, "tx: mining.authorize (credentials omitted)");
    } else {
        debug_stratum_tx(stratum_tx_buffer);
    }
    ret = stratum_write_all_locked(socket, stratum_tx_buffer, (size_t)message_len);

    pthread_mutex_unlock(&stratum_tx_lock);
    return ret;
}

static void stratum_message_prepare(StratumApiV1Message *message)
{
    free(message->error_str);
    free(message->extranonce_str);
    free(message->extranonce_bin);
    STRATUM_V1_free_mining_notify(message->mining_notification);
    message->error_str = NULL;
    message->extranonce_str = NULL;
    message->extranonce_bin = NULL;
    message->extranonce_bin_len = 0;
    message->extranonce_2_len = 0;
    message->message_id = -1;
    message->method = STRATUM_UNKNOWN;
    message->should_abandon_work = 0;
    message->mining_notification = NULL;
    message->new_difficulty = 0;
    message->new_difficulty_value = 0.0;
    message->version_mask = 0;
    message->response_success = false;
}

static void stratum_message_set_error(StratumApiV1Message *message, const char *error_str)
{
    free(message->error_str);
    message->error_str = NULL;

    if (error_str != NULL) {
        message->error_str = strdup(error_str);
    }
}

static void stratum_message_set_out_of_memory(StratumApiV1Message *message)
{
    message->response_success = false;
    stratum_message_set_error(message, "out_of_memory");
}

static bool stratum_json_array_string(cJSON *array, int index, const char **value,
                                      StratumApiV1Message *message, const char *field_name)
{
    cJSON *item = (array != NULL && cJSON_IsArray(array)) ? cJSON_GetArrayItem(array, index) : NULL;

    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        ESP_LOGE(TAG, "Malformed stratum field: %s", field_name);
        stratum_message_set_error(message, field_name);
        return false;
    }

    *value = item->valuestring;
    return true;
}

static bool stratum_hex_string_is_valid(const char *value, size_t expected_len)
{
    size_t len;

    if (value == NULL) {
        return false;
    }

    len = strlen(value);
    if (len != expected_len) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        char c = value[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }

    return true;
}

static bool stratum_parse_hex_u32(cJSON *array, int index, uint32_t *value,
                                  StratumApiV1Message *message, const char *field_name)
{
    const char *text = NULL;
    char *end = NULL;
    size_t len;
    unsigned long parsed;

    if (value == NULL ||
            !stratum_json_array_string(array, index, &text, message, field_name)) {
        return false;
    }

    len = strlen(text);
    if (len == 0 || len > 8 || !stratum_hex_string_is_valid(text, len)) {
        ESP_LOGE(TAG, "Malformed hex u32 field: %s", field_name);
        stratum_message_set_error(message, field_name);
        return false;
    }

    errno = 0;
    parsed = strtoul(text, &end, 16);
    if (errno != 0 || end == NULL || *end != '\0' || parsed > UINT32_MAX) {
        ESP_LOGE(TAG, "Invalid hex u32 field: %s", field_name);
        stratum_message_set_error(message, field_name);
        return false;
    }

    *value = (uint32_t)parsed;
    return true;
}

static bool stratum_decode_hex_alloc(const char *hex, uint8_t **bin, size_t *bin_len,
                                     StratumApiV1Message *message, const char *field_name)
{
    size_t hex_len;
    size_t out_len;

    if (hex == NULL || bin == NULL || bin_len == NULL) {
        stratum_message_set_error(message, field_name);
        return false;
    }

    hex_len = strlen(hex);
    if ((hex_len % 2U) != 0U || !stratum_hex_string_is_valid(hex, hex_len)) {
        ESP_LOGE(TAG, "Malformed hex field: %s", field_name);
        stratum_message_set_error(message, field_name);
        return false;
    }

    out_len = hex_len / 2U;
    *bin = NULL;
    *bin_len = out_len;

    if (out_len == 0) {
        return true;
    }

    *bin = malloc(out_len);
    if (*bin == NULL) {
        stratum_message_set_out_of_memory(message);
        return false;
    }

    if (!hex2bin_exact(hex, *bin, out_len)) {
        free(*bin);
        *bin = NULL;
        *bin_len = 0;
        stratum_message_set_error(message, field_name);
        return false;
    }

    return true;
}
