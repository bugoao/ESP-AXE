#include <pthread.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_vfs.h"

#include "dns_server.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "lwip/err.h"
#include "lwip/inet.h"
#include "lwip/lwip_napt.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#include "cJSON.h"
#include "global_state.h"
#include "nvs_config.h"
#include "vcore.h"
#include "power.h"
#include "connect.h"
#include "asic.h"
#include "system.h"
#include "TPS546.h"
#include "theme_api.h"  // Add theme API include
#include "http_server.h"
#include "http_update.h"
#include "http_json.h"

static const char * TAG = "http_server";
static const char * CORS_TAG = "CORS";

#ifdef CONFIG_SOCKS5_PROXY_ENABLED
#define DEFAULT_SOCKS5_PROXY_ENABLED 1
#else
#define DEFAULT_SOCKS5_PROXY_ENABLED 0
#endif

/* Handler for WiFi scan endpoint */
static esp_err_t GET_wifi_scan(httpd_req_t *req)
{
    if (http_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }
    httpd_resp_set_type(req, "application/json");
    
    // Give some time for the connected flag to take effect
    vTaskDelay(100 / portTICK_PERIOD_MS);
    
    wifi_ap_record_simple_t ap_records[20];
    uint16_t ap_count = 0;

    esp_err_t err = wifi_scan(ap_records, &ap_count);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "WiFi scan failed");
        return ESP_OK;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON *networks = cJSON_CreateArray();
    if (root == NULL || networks == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(networks);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Out of memory");
    }

    for (int i = 0; i < ap_count; i++) {
        cJSON *network = cJSON_CreateObject();
        if (network == NULL ||
                cJSON_AddStringToObject(network, "ssid", ap_records[i].ssid) == NULL ||
                cJSON_AddNumberToObject(network, "rssi", ap_records[i].rssi) == NULL ||
                cJSON_AddNumberToObject(network, "authmode", ap_records[i].authmode) == NULL ||
                !cJSON_AddItemToArray(networks, network)) {
            cJSON_Delete(network);
            cJSON_Delete(networks);
            cJSON_Delete(root);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "Out of memory");
        }
    }

    if (!cJSON_AddItemToObject(root, "networks", networks)) {
        cJSON_Delete(networks);
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Out of memory");
    }

    char *response = cJSON_PrintUnformatted(root);
    if (response == NULL) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Failed to serialize Wi-Fi scan");
    }
    esp_err_t send_result = httpd_resp_sendstr(req, response);

    free(response);
    cJSON_Delete(root);
    return send_result;
}

static GlobalState * GLOBAL_STATE;
static httpd_handle_t server = NULL;
static QueueHandle_t log_queue = NULL;

static int fd = -1;

#define REST_CHECK(a, str, goto_tag, ...)                                                                                          \
    do {                                                                                                                           \
        if (!(a)) {                                                                                                                \
            ESP_LOGE(TAG, "%s(%d): " str, __FUNCTION__, __LINE__, ##__VA_ARGS__);                                                  \
            goto goto_tag;                                                                                                         \
        }                                                                                                                          \
    } while (0)

#define FILE_PATH_MAX (ESP_VFS_PATH_MAX + 128)
#define SCRATCH_BUFSIZE (10240)
#define MESSAGE_QUEUE_SIZE (128)

typedef struct rest_server_context
{
    char base_path[ESP_VFS_PATH_MAX + 1];
    char scratch[SCRATCH_BUFSIZE];
} rest_server_context_t;

static bool has_file_extension(const char *filename, const char *extension)
{
    size_t length = strlen(filename);
    size_t extension_length = strlen(extension);
    return length >= extension_length &&
           strcasecmp(filename + length - extension_length, extension) == 0;
}

#define CHECK_FILE_EXTENSION(filename, ext) has_file_extension(filename, ext)

static bool json_u16_in_range(cJSON *item, const char *label,
                              uint16_t min_value, uint16_t max_value,
                              uint16_t *out)
{
    double raw_value;
    uint16_t clamped_value;
    bool value_was_clamped = false;

    if (out == NULL) {
        return false;
    }

    if (cJSON_IsBool(item)) {
        if (min_value != 0U || max_value != 1U) {
            return false;
        }
        raw_value = cJSON_IsTrue(item) ? 1.0 : 0.0;
    } else if (cJSON_IsNumber(item)) {
        raw_value = item->valuedouble;
    } else {
        return false;
    }

    if (raw_value != raw_value) {
        ESP_LOGW(TAG, "Ignoring NaN value for %s", label);
        return false;
    }

    if (raw_value < (double)min_value) {
        clamped_value = min_value;
        value_was_clamped = true;
    } else if (raw_value > (double)max_value) {
        clamped_value = max_value;
        value_was_clamped = true;
    } else {
        clamped_value = (uint16_t)raw_value;
        value_was_clamped = ((double)clamped_value != raw_value);
    }

    if (value_was_clamped) {
        ESP_LOGW(TAG, "Clamped %s from %.0f to %u", label, raw_value, (unsigned int)clamped_value);
    }

    *out = clamped_value;
    return true;
}

static esp_err_t ip_in_private_range(uint32_t address) {
    uint32_t ip_address = ntohl(address);

    // 10.0.0.0 - 10.255.255.255 (Class A)
    if ((ip_address >= 0x0A000000) && (ip_address <= 0x0AFFFFFF)) {
        return ESP_OK;
    }

    // 172.16.0.0 - 172.31.255.255 (Class B)
    if ((ip_address >= 0xAC100000) && (ip_address <= 0xAC1FFFFF)) {
        return ESP_OK;
    }

    // 192.168.0.0 - 192.168.255.255 (Class C)
    if ((ip_address >= 0xC0A80000) && (ip_address <= 0xC0A8FFFF)) {
        return ESP_OK;
    }

    return ESP_FAIL;
}

static uint32_t extract_origin_ip_addr(char *origin)
{
    char ip_str[16];
    uint32_t origin_ip_addr = 0;

    // Find the start of the IP address in the Origin header
    const char *prefix = "http://";
    char *ip_start = strstr(origin, prefix);
    if (ip_start) {
        ip_start += strlen(prefix); // Move past "http://"

        // Extract the IP address portion (up to the next '/')
        char *ip_end = strchr(ip_start, '/');
        size_t ip_len = ip_end ? (size_t)(ip_end - ip_start) : strlen(ip_start);
        if (ip_len < sizeof(ip_str)) {
            strncpy(ip_str, ip_start, ip_len);
            ip_str[ip_len] = '\0'; // Null-terminate the string

            // Convert the IP address string to uint32_t
            origin_ip_addr = inet_addr(ip_str);
            if (origin_ip_addr == INADDR_NONE) {
                ESP_LOGW(CORS_TAG, "Invalid IP address: %s", ip_str);
            } else {
                ESP_LOGD(CORS_TAG, "Extracted IP address %lu", origin_ip_addr);
            }
        } else {
            ESP_LOGW(CORS_TAG, "IP address string is too long: %s", ip_start);
        }
    }

    return origin_ip_addr;
}

esp_err_t http_network_allowed(httpd_req_t * req)
{
    if (GLOBAL_STATE->SYSTEM_MODULE.ap_enabled == true) {
        ESP_LOGI(CORS_TAG, "Device in AP mode. Allowing CORS.");
        return ESP_OK;
    }

    int sockfd = httpd_req_to_sockfd(req);
    char ipstr[INET6_ADDRSTRLEN];
    struct sockaddr_storage addr = {0};
    socklen_t addr_size = sizeof(addr);

    if (getpeername(sockfd, (struct sockaddr *)&addr, &addr_size) < 0) {
        ESP_LOGE(CORS_TAG, "Error getting client IP");
        return ESP_FAIL;
    }

    uint32_t request_ip_addr;
    if (addr.ss_family == AF_INET) {
        request_ip_addr = ((struct sockaddr_in *)&addr)->sin_addr.s_addr;
    } else if (addr.ss_family == AF_INET6) {
        static const uint8_t mapped_prefix[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        const struct sockaddr_in6 *addr6 = (const struct sockaddr_in6 *)&addr;
        if (memcmp(&addr6->sin6_addr, mapped_prefix, sizeof(mapped_prefix)) != 0) {
            return ESP_FAIL;
        }
        memcpy(&request_ip_addr, (const uint8_t *)&addr6->sin6_addr + 12,
               sizeof(request_ip_addr));
    } else {
        return ESP_FAIL;
    }

    // // Convert to IPv6 string
    // inet_ntop(AF_INET, &addr.sin6_addr, ipstr, sizeof(ipstr));

    // Convert to IPv4 string
    inet_ntop(AF_INET, &request_ip_addr, ipstr, sizeof(ipstr));

    // Attempt to get the Origin header.
    char origin[128];
    uint32_t origin_ip_addr;
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin)) == ESP_OK) {
        ESP_LOGD(CORS_TAG, "Origin header: %s", origin);
        char host[96];
        char same_origin[sizeof(host) + sizeof("http://")];
        if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) == ESP_OK) {
            snprintf(same_origin, sizeof(same_origin), "http://%s", host);
            if (strcasecmp(origin, same_origin) == 0) {
                return ip_in_private_range(request_ip_addr);
            }
        }
        origin_ip_addr = extract_origin_ip_addr(origin);
    } else {
        ESP_LOGD(CORS_TAG, "No origin header found.");
        origin_ip_addr = request_ip_addr;
    }

    if (ip_in_private_range(origin_ip_addr) == ESP_OK && ip_in_private_range(request_ip_addr) == ESP_OK) {
        return ESP_OK;
    }

    ESP_LOGI(CORS_TAG, "Client is NOT in the private ip ranges or same range as server.");
    return ESP_FAIL;
}

static esp_err_t init_fs(void)
{
    esp_err_t ret = http_mount_www();

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return ESP_FAIL;
    }

    size_t total = 0, used = 0;
    ret = esp_spiffs_info("www", &total, &used);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
    }
    return ESP_OK;
}

/* Set HTTP response content type according to file extension */
static esp_err_t set_content_type_from_file(httpd_req_t * req, const char * filepath)
{
    const char * type = "text/plain";
    if (CHECK_FILE_EXTENSION(filepath, ".html")) {
        type = "text/html";
    } else if (CHECK_FILE_EXTENSION(filepath, ".js")) {
        type = "application/javascript";
    } else if (CHECK_FILE_EXTENSION(filepath, ".css")) {
        type = "text/css";
    } else if (CHECK_FILE_EXTENSION(filepath, ".png")) {
        type = "image/png";
    } else if (CHECK_FILE_EXTENSION(filepath, ".ico")) {
        type = "image/x-icon";
    } else if (CHECK_FILE_EXTENSION(filepath, ".svg")) {
        type = "text/xml";
    } else if (CHECK_FILE_EXTENSION(filepath, ".pdf")) {
        type = "application/pdf";
    }
    return httpd_resp_set_type(req, type);
}

static esp_err_t set_cors_headers(httpd_req_t * req)
{

    esp_err_t err;

    err = httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    if (err != ESP_OK) {
        return ESP_FAIL;
    }

    err = httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, PUT, PATCH, DELETE, OPTIONS");
    if (err != ESP_OK) {
        return ESP_FAIL;
    }

    err = httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    if (err != ESP_OK) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

/* Recovery handler */
static esp_err_t rest_recovery_handler(httpd_req_t * req)
{
    if (http_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    extern const unsigned char recovery_page_start[] asm("_binary_recovery_page_html_start");
    extern const unsigned char recovery_page_end[] asm("_binary_recovery_page_html_end");
    const size_t recovery_page_size = (recovery_page_end - recovery_page_start);
    httpd_resp_send_chunk(req, (const char*)recovery_page_start, recovery_page_size);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* Send HTTP response with the contents of the requested file */
static esp_err_t rest_common_get_handler(httpd_req_t * req)
{
    if (!esp_spiffs_mounted("www")) {
        return rest_recovery_handler(req);
    }
    char filepath[FILE_PATH_MAX];
    rest_server_context_t * rest_context = (rest_server_context_t *) req->user_ctx;
    size_t uri_length = strcspn(req->uri, "?");
    if (uri_length == 0) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty path");
    }
    bool directory = req->uri[uri_length - 1] == '/';
    int path_length = snprintf(filepath, sizeof(filepath) - 3, "%s%.*s",
                               rest_context->base_path,
                               directory ? (int)strlen("/index.html") : (int)uri_length,
                               directory ? "/index.html" : req->uri);
    if (path_length < 0 || (size_t)path_length >= sizeof(filepath) - 3) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Path too long");
    }
    bool is_html = has_file_extension(filepath, ".html");
    set_content_type_from_file(req, filepath);
    memcpy(filepath + path_length, ".gz", sizeof(".gz"));
    int fd = open(filepath, O_RDONLY, 0);
    if (fd == -1) {
        if (is_html) {
            return rest_recovery_handler(req);
        }
        // Set status
        httpd_resp_set_status(req, "302 Temporary Redirect");
        // Redirect to the "/" root directory
        httpd_resp_set_hdr(req, "Location", "/");
        // iOS requires content in the response to detect a captive portal, simply redirecting is not sufficient.
        httpd_resp_send(req, "Redirect to the captive portal", HTTPD_RESP_USE_STRLEN);

        ESP_LOGI(TAG, "Redirecting to root");
        return ESP_OK;
    }

    if (is_html) {
        httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    } else {
        httpd_resp_set_hdr(req, "Cache-Control", "max-age=2592000");
    }

    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");

    char * chunk = rest_context->scratch;
    ssize_t read_bytes;
    while ((read_bytes = read(fd, chunk, SCRATCH_BUFSIZE)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
            close(fd);
            return ESP_FAIL;
        }
    }
    close(fd);
    if (read_bytes < 0) {
        ESP_LOGW(TAG, "Failed to read file: %s", filepath);
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t handle_options_request(httpd_req_t * req)
{
    if (http_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    // Set CORS headers for OPTIONS request
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    // Send a blank response for OPTIONS request
    httpd_resp_send(req, NULL, 0);

    return ESP_OK;
}

static esp_err_t PATCH_update_settings(httpd_req_t * req)
{

    if (http_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    char *buffer = ((rest_server_context_t *)req->user_ctx)->scratch;
    cJSON *root = http_read_json_object(req, buffer, SCRATCH_BUFSIZE);
    if (root == NULL) return ESP_FAIL;

    static const struct { const char *json; const char *key; size_t max_length; } strings[] = {
        {"stratumURL", NVS_CONFIG_STRATUM_URL, 253},
        {"fallbackStratumURL", NVS_CONFIG_FALLBACK_STRATUM_URL, 253},
        {"stratumUser", NVS_CONFIG_STRATUM_USER, 191},
        {"stratumPassword", NVS_CONFIG_STRATUM_PASS, 255},
        {"fallbackStratumUser", NVS_CONFIG_FALLBACK_STRATUM_USER, 191},
        {"fallbackStratumPassword", NVS_CONFIG_FALLBACK_STRATUM_PASS, 255},
        {"sv2Host", NVS_CONFIG_SV2_HOST, 253},
        {"sv2AuthorityPublicKey", NVS_CONFIG_SV2_AUTHORITY_PUBLIC_KEY, 96},
        {"fallbackSv2Host", NVS_CONFIG_FALLBACK_SV2_HOST, 253},
        {"fallbackSv2AuthorityPublicKey", NVS_CONFIG_FALLBACK_SV2_AUTHORITY_PUBLIC_KEY, 96},
        {"socks5ProxyHost", NVS_CONFIG_SOCKS5_PROXY_HOST, 253},
        {"socks5ProxyUser", NVS_CONFIG_SOCKS5_PROXY_USER, 191},
        {"socks5ProxyPass", NVS_CONFIG_SOCKS5_PROXY_PASS, 255},
        {"ssid", NVS_CONFIG_WIFI_SSID, 32},
        {"wifiPass", NVS_CONFIG_WIFI_PASS, 64},
        {"hostname", NVS_CONFIG_HOSTNAME, 253},
    };
    static const struct { const char *json; const char *key; uint16_t min; uint16_t max; } numbers[] = {
        {"stratumProtocol", NVS_CONFIG_STRATUM_PROTOCOL, STRATUM_PROTOCOL_V1, STRATUM_PROTOCOL_V2},
        {"fallbackStratumProtocol", NVS_CONFIG_FALLBACK_STRATUM_PROTOCOL, STRATUM_PROTOCOL_V1, STRATUM_PROTOCOL_V2},
        {"stratumPort", NVS_CONFIG_STRATUM_PORT, 1, UINT16_MAX},
        {"fallbackStratumPort", NVS_CONFIG_FALLBACK_STRATUM_PORT, 1, UINT16_MAX},
        {"sv2Port", NVS_CONFIG_SV2_PORT, 1, UINT16_MAX},
        {"fallbackSv2Port", NVS_CONFIG_FALLBACK_SV2_PORT, 1, UINT16_MAX},
        {"socks5ProxyEnabled", NVS_CONFIG_SOCKS5_PROXY_ENABLED, 0, 1},
        {"socks5ProxyPort", NVS_CONFIG_SOCKS5_PROXY_PORT, 1, UINT16_MAX},
        {"coreVoltage", NVS_CONFIG_ASIC_VOLTAGE, NVS_CONFIG_ASIC_VOLTAGE_MIN_MV, NVS_CONFIG_ASIC_VOLTAGE_MAX_MV},
        {"frequency", NVS_CONFIG_ASIC_FREQ, NVS_CONFIG_ASIC_FREQUENCY_MIN_MHZ, NVS_CONFIG_ASIC_FREQUENCY_MAX_MHZ},
        {"flipscreen", NVS_CONFIG_FLIP_SCREEN, 0, 1},
        {"invertscreen", NVS_CONFIG_INVERT_SCREEN, 0, 1},
        {"invertfanpolarity", NVS_CONFIG_INVERT_FAN_POLARITY, 0, 1},
        {"autofanspeed", NVS_CONFIG_AUTO_FAN_SPEED, 0, 1},
        {"fanspeed", NVS_CONFIG_FAN_SPEED, NVS_CONFIG_FAN_SPEED_MIN_PERCENT, NVS_CONFIG_FAN_SPEED_MAX_PERCENT},
        {"overclockEnabled", NVS_CONFIG_OVERCLOCK_ENABLED, 0, 1},
    };
    nvs_config_value_t values[sizeof(strings) / sizeof(strings[0]) + sizeof(numbers) / sizeof(numbers[0])];
    size_t count = 0;
    for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++) {
        cJSON *item = cJSON_GetObjectItem(root, strings[i].json);
        if (item == NULL) continue;
        if (!cJSON_IsString(item) || strlen(item->valuestring) > strings[i].max_length) goto invalid;
        values[count++] = (nvs_config_value_t){.key = strings[i].key, .type = NVS_VALUE_STRING,
                                             .value.string = item->valuestring};
    }
    for (size_t i = 0; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
        cJSON *item = cJSON_GetObjectItem(root, numbers[i].json);
        if (item == NULL) continue;
        uint16_t value;
        if ((cJSON_IsNumber(item) && !isfinite(item->valuedouble)) ||
                !json_u16_in_range(item, numbers[i].json, numbers[i].min, numbers[i].max, &value)) goto invalid;
        values[count++] = (nvs_config_value_t){.key = numbers[i].key, .type = NVS_VALUE_U16, .value.u16 = value};
    }
    esp_err_t err = nvs_config_set_values(values, count);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save settings");
    }
    return httpd_resp_send_chunk(req, NULL, 0);

invalid:
    cJSON_Delete(root);
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid settings field");
}

static esp_err_t POST_restart(httpd_req_t * req)
{
    if (http_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Restarting System because of API Request");
    return http_reboot(req);
}

/* Simple handler for getting system handler */
static esp_err_t GET_system_info(httpd_req_t * req)
{
    if (http_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    httpd_resp_set_type(req, "application/json");

    // Set CORS headers
    if (set_cors_headers(req) != ESP_OK) {
        httpd_resp_send_500(req);
        return ESP_OK;
    }

    SYSTEM_update_hashrate_estimate(GLOBAL_STATE);
    SystemRuntimeSnapshot snapshot;
    SYSTEM_get_runtime_snapshot(GLOBAL_STATE, &snapshot);

    ASICDispatchConfig dispatch_config;
    ASIC_get_dispatch_config(GLOBAL_STATE, &dispatch_config);
    uint32_t stratum_difficulty;
    SYSTEM_get_stratum_snapshot(GLOBAL_STATE, &stratum_difficulty);

    char * ssid = nvs_config_get_string(NVS_CONFIG_WIFI_SSID, CONFIG_ESP_WIFI_SSID);
    char * hostname = nvs_config_get_string(NVS_CONFIG_HOSTNAME, CONFIG_LWIP_LOCAL_HOSTNAME);
    uint8_t mac[6];
    char formattedMac[18];
    char * stratumURL = nvs_config_get_string(NVS_CONFIG_STRATUM_URL, CONFIG_STRATUM_URL);
    char * fallbackStratumURL = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_URL, CONFIG_FALLBACK_STRATUM_URL);
    char * stratumUser = nvs_config_get_string(NVS_CONFIG_STRATUM_USER, CONFIG_STRATUM_USER);
    char * fallbackStratumUser = nvs_config_get_string(NVS_CONFIG_FALLBACK_STRATUM_USER, CONFIG_FALLBACK_STRATUM_USER);
    char * sv2Host = nvs_config_get_string(NVS_CONFIG_SV2_HOST, CONFIG_SV2_HOST);
    char * sv2AuthorityPublicKey =
        nvs_config_get_string(NVS_CONFIG_SV2_AUTHORITY_PUBLIC_KEY, CONFIG_SV2_AUTHORITY_PUBLIC_KEY);
    char * fallbackSv2Host = nvs_config_get_string(NVS_CONFIG_FALLBACK_SV2_HOST, CONFIG_FALLBACK_SV2_HOST);
    char * fallbackSv2AuthorityPublicKey =
        nvs_config_get_string(NVS_CONFIG_FALLBACK_SV2_AUTHORITY_PUBLIC_KEY,
                              CONFIG_FALLBACK_SV2_AUTHORITY_PUBLIC_KEY);
    char * socks5ProxyHost = nvs_config_get_string(NVS_CONFIG_SOCKS5_PROXY_HOST, CONFIG_SOCKS5_PROXY_HOST);
    char * socks5ProxyUser = nvs_config_get_string(NVS_CONFIG_SOCKS5_PROXY_USER, CONFIG_SOCKS5_PROXY_USER);
    char * board_version = nvs_config_get_string(NVS_CONFIG_BOARD_VERSION, "unknown");

    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(formattedMac, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    int8_t wifi_rssi = -90;
    bool wifi_connected = get_wifi_current_rssi(&wifi_rssi) == ESP_OK;
    bool provisioning_required = snapshot.ap_enabled && !wifi_connected;

    cJSON * root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "power", snapshot.power.power);
    cJSON_AddNumberToObject(root, "voltage", snapshot.power.voltage);
    cJSON_AddNumberToObject(root, "current", snapshot.power.current);
    cJSON_AddNumberToObject(root, "temp", snapshot.power.chip_temp_avg);
    cJSON_AddNumberToObject(root, "vrTemp", snapshot.power.vr_temp);
    cJSON_AddNumberToObject(root, "maxPower", Power_get_max_settings(GLOBAL_STATE));
    cJSON_AddNumberToObject(root, "nominalVoltage", Power_get_nominal_voltage(GLOBAL_STATE));
    cJSON_AddNumberToObject(root, "hashRate", SYSTEM_get_current_hashrate(GLOBAL_STATE));
    cJSON_AddStringToObject(root, "bestDiff", snapshot.best_diff_string);
    cJSON_AddStringToObject(root, "bestSessionDiff", snapshot.best_session_diff_string);
    cJSON_AddNumberToObject(root, "stratumDiff", stratum_difficulty);

    cJSON_AddNumberToObject(root, "isUsingFallbackStratum", snapshot.is_using_fallback);

    cJSON_AddNumberToObject(root, "isPSRAMAvailable", GLOBAL_STATE->psram_is_available);

    cJSON_AddNumberToObject(root, "freeHeap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "coreVoltage",
                            nvs_config_get_u16_clamped(NVS_CONFIG_ASIC_VOLTAGE,
                                                       CONFIG_ASIC_VOLTAGE,
                                                       NVS_CONFIG_ASIC_VOLTAGE_MIN_MV,
                                                       NVS_CONFIG_ASIC_VOLTAGE_MAX_MV));
    cJSON_AddNumberToObject(root, "coreVoltageActual", snapshot.power.core_voltage);
    cJSON_AddNumberToObject(root, "frequency",
                            nvs_config_get_u16_clamped(NVS_CONFIG_ASIC_FREQ,
                                                       CONFIG_ASIC_FREQUENCY,
                                                       NVS_CONFIG_ASIC_FREQUENCY_MIN_MHZ,
                                                       NVS_CONFIG_ASIC_FREQUENCY_MAX_MHZ));
    cJSON_AddStringToObject(root, "ssid", ssid);
    cJSON_AddStringToObject(root, "macAddr", formattedMac);
    cJSON_AddStringToObject(root, "hostname", hostname);
    cJSON_AddStringToObject(root, "wifiStatus", snapshot.wifi_status);
    cJSON_AddNumberToObject(root, "wifiRSSI", wifi_rssi);
    cJSON_AddNumberToObject(root, "apEnabled", snapshot.ap_enabled);
    cJSON_AddBoolToObject(root, "provisioningRequired", provisioning_required);
    cJSON_AddNumberToObject(root, "sharesAccepted", snapshot.shares_accepted);
    cJSON_AddNumberToObject(root, "sharesRejected", snapshot.shares_rejected);
    cJSON_AddNumberToObject(root, "shareSubmitQueueDepth",
                            GLOBAL_STATE->stratum_submit_queue != NULL
                                ? uxQueueMessagesWaiting(GLOBAL_STATE->stratum_submit_queue)
                                : 0);
    cJSON_AddNumberToObject(root, "shareSubmitQueueHighWater",
                            snapshot.share_submit_queue_high_water);
    cJSON_AddNumberToObject(root, "shareSubmitQueueDrops",
                            snapshot.share_submit_queue_drops);
    cJSON_AddNumberToObject(root, "blockSubmitQueueDrops",
                            snapshot.block_submit_queue_drops);
    cJSON_AddNumberToObject(root, "shareSubmitLastQueueWaitMs",
                            snapshot.share_submit_last_queue_wait_ms);
    cJSON_AddNumberToObject(root, "shareSubmitMaxQueueWaitMs",
                            snapshot.share_submit_max_queue_wait_ms);
    cJSON_AddNumberToObject(root, "shareSubmitLastSendMs",
                            snapshot.share_submit_last_send_ms);
    cJSON_AddNumberToObject(root, "shareSubmitMaxSendMs",
                            snapshot.share_submit_max_send_ms);

    cJSON *error_array = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "sharesRejectedReasons", error_array);
    
    for (int i = 0; i < snapshot.rejected_reason_stats_count; i++) {
        cJSON *error_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(error_obj, "message", snapshot.rejected_reason_stats[i].message);
        cJSON_AddNumberToObject(error_obj, "count", snapshot.rejected_reason_stats[i].count);
        cJSON_AddItemToArray(error_array, error_obj);
    }

    cJSON_AddNumberToObject(root, "uptimeSeconds", (esp_timer_get_time() - GLOBAL_STATE->SYSTEM_MODULE.start_time) / 1000000);
    cJSON_AddNumberToObject(root, "asicCount", ASIC_get_asic_count(GLOBAL_STATE));
    cJSON_AddNumberToObject(root, "expectedAsicCount", ASIC_get_expected_asic_count(GLOBAL_STATE));
    cJSON_AddNumberToObject(root, "smallCoreCount", ASIC_get_small_core_count(GLOBAL_STATE));
    cJSON_AddNumberToObject(root, "actualFrequency",
                            snapshot.power.actual_frequency);
    cJSON_AddNumberToObject(root, "asicJobIntervalMs",
                            (double)dispatch_config.current_interval_us / 1000.0);
    cJSON_AddNumberToObject(root, "asicRawResults",
                            (double)snapshot.raw_result_count);
    cJSON_AddNumberToObject(root, "asicDuplicateResults",
                            (double)snapshot.duplicate_result_count);
    cJSON_AddNumberToObject(root, "asicJobLookupMisses",
                            (double)snapshot.job_lookup_miss_count);
    cJSON_AddNumberToObject(root, "asicValidResults",
                            (double)snapshot.valid_result_count);
    cJSON_AddNumberToObject(root, "asicInvalidResults",
                            (double)snapshot.invalid_result_count);
    cJSON_AddNumberToObject(root, "asicRetiredJobMatches",
                            (double)snapshot.retired_job_match_count);
    cJSON_AddStringToObject(root, "ASICModel", GLOBAL_STATE->asic_model_str);
    cJSON_AddStringToObject(root, "stratumURL", stratumURL);
    cJSON_AddStringToObject(root, "fallbackStratumURL", fallbackStratumURL);
    cJSON_AddNumberToObject(root, "stratumProtocol",
                            nvs_config_get_u16_clamped(NVS_CONFIG_STRATUM_PROTOCOL,
                                                       CONFIG_STRATUM_PROTOCOL,
                                                       STRATUM_PROTOCOL_V1,
                                                       STRATUM_PROTOCOL_V2));
    cJSON_AddNumberToObject(root, "fallbackStratumProtocol",
                            nvs_config_get_u16_clamped(NVS_CONFIG_FALLBACK_STRATUM_PROTOCOL,
                                                       CONFIG_FALLBACK_STRATUM_PROTOCOL,
                                                       STRATUM_PROTOCOL_V1,
                                                       STRATUM_PROTOCOL_V2));
    cJSON_AddNumberToObject(root, "stratumPort", nvs_config_get_u16(NVS_CONFIG_STRATUM_PORT, CONFIG_STRATUM_PORT));
    cJSON_AddNumberToObject(root, "fallbackStratumPort", nvs_config_get_u16(NVS_CONFIG_FALLBACK_STRATUM_PORT, CONFIG_FALLBACK_STRATUM_PORT));
    cJSON_AddStringToObject(root, "stratumUser", stratumUser);
    cJSON_AddStringToObject(root, "fallbackStratumUser", fallbackStratumUser);
    cJSON_AddStringToObject(root, "sv2Host", sv2Host);
    cJSON_AddNumberToObject(root, "sv2Port", nvs_config_get_u16(NVS_CONFIG_SV2_PORT, CONFIG_SV2_PORT));
    cJSON_AddStringToObject(root, "sv2AuthorityPublicKey", sv2AuthorityPublicKey);
    cJSON_AddStringToObject(root, "fallbackSv2Host", fallbackSv2Host);
    cJSON_AddNumberToObject(root, "fallbackSv2Port",
                            nvs_config_get_u16(NVS_CONFIG_FALLBACK_SV2_PORT, CONFIG_FALLBACK_SV2_PORT));
    cJSON_AddStringToObject(root, "fallbackSv2AuthorityPublicKey", fallbackSv2AuthorityPublicKey);
    cJSON_AddNumberToObject(root, "socks5ProxyEnabled",
                            nvs_config_get_u16(NVS_CONFIG_SOCKS5_PROXY_ENABLED, DEFAULT_SOCKS5_PROXY_ENABLED));
    cJSON_AddStringToObject(root, "socks5ProxyHost", socks5ProxyHost);
    cJSON_AddNumberToObject(root, "socks5ProxyPort",
                            nvs_config_get_u16(NVS_CONFIG_SOCKS5_PROXY_PORT, CONFIG_SOCKS5_PROXY_PORT));
    cJSON_AddStringToObject(root, "socks5ProxyUser", socks5ProxyUser);

    cJSON_AddStringToObject(root, "version", esp_app_get_description()->version);
    cJSON_AddStringToObject(root, "idfVersion", esp_get_idf_version());
    cJSON_AddStringToObject(root, "boardVersion", board_version);
    cJSON_AddStringToObject(root, "runningPartition", esp_ota_get_running_partition()->label);

    cJSON_AddNumberToObject(root, "flipscreen", nvs_config_get_u16(NVS_CONFIG_FLIP_SCREEN, 1));
    cJSON_AddNumberToObject(root, "overheat_mode", snapshot.overheat_mode);
    cJSON_AddNumberToObject(root, "overclockEnabled", nvs_config_get_u16(NVS_CONFIG_OVERCLOCK_ENABLED, 0));
    cJSON_AddNumberToObject(root, "invertscreen", nvs_config_get_u16(NVS_CONFIG_INVERT_SCREEN, 0));

    cJSON_AddNumberToObject(root, "invertfanpolarity", nvs_config_get_u16(NVS_CONFIG_INVERT_FAN_POLARITY, 1));
    cJSON_AddNumberToObject(root, "autofanspeed", nvs_config_get_u16(NVS_CONFIG_AUTO_FAN_SPEED, 1));

    cJSON_AddNumberToObject(root, "fanspeed", snapshot.power.fan_perc);
    cJSON_AddNumberToObject(root, "fanrpm", snapshot.power.fan_rpm);
    
    if (snapshot.power_fault > 0 && snapshot.power_fault_message[0] != '\0') {
        cJSON_AddStringToObject(root, "power_fault", snapshot.power_fault_message);
    }
    cJSON_AddStringToObject(root,"chipSubmitStr",snapshot.chip_submit_srt);

    free(ssid);
    free(hostname);
    free(stratumURL);
    free(fallbackStratumURL);
    free(stratumUser);
    free(fallbackStratumUser);
    free(sv2Host);
    free(sv2AuthorityPublicKey);
    free(fallbackSv2Host);
    free(fallbackSv2AuthorityPublicKey);
    free(socks5ProxyHost);
    free(socks5ProxyUser);
    free(board_version);

    char * sys_info = cJSON_PrintUnformatted(root);
    if (sys_info == NULL) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Failed to serialize system info");
    }
    httpd_resp_sendstr(req, sys_info);
    free(sys_info);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t POST_WWW_update(httpd_req_t * req)
{
    if (http_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Not allowed in AP mode");
        return ESP_OK;
    }

    return http_update_www(req, &GLOBAL_STATE->SYSTEM_MODULE);
}

/*
 * Handle OTA file upload
 */
static esp_err_t POST_OTA_update(httpd_req_t * req)
{
    if (http_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    wifi_mode_t mode;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA)
    {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Not allowed in AP mode");
        return ESP_OK;
    }
    
    return http_update_firmware(req, &GLOBAL_STATE->SYSTEM_MODULE);
}

static int log_to_queue(const char * format, va_list args)
{
    va_list args_copy;
    va_copy(args_copy, args);

    // Calculate the required buffer size
    int needed_size = vsnprintf(NULL, 0, format, args_copy);
    va_end(args_copy);
    if (needed_size < 0) {
        return needed_size;
    }

    // Allocate the buffer dynamically
    char * log_buffer = (char *) calloc((size_t)needed_size + 2U, sizeof(char));
    if (log_buffer == NULL) {
        return vprintf(format, args);
    }

    // Format the string into the allocated buffer
    va_copy(args_copy, args);
    vsnprintf(log_buffer, (size_t)needed_size + 1U, format, args_copy);
    va_end(args_copy);

    // Ensure the log message ends with a newline
    size_t len = strlen(log_buffer);
    if (len > 0 && log_buffer[len - 1] != '\n') {
        log_buffer[len] = '\n';
        log_buffer[len + 1] = '\0';
        len++;
    }

    // Print to standard output
    printf("%s", log_buffer);

    if (log_queue == NULL ||
            xQueueSendToBack(log_queue, &log_buffer, (TickType_t)0) != pdPASS) {
        free(log_buffer);
    }

    return needed_size;
}

/* Invoked by httpd_queue_work in the HTTP task, which owns client sessions. */
static void send_log_to_websocket(void *argument)
{
    char *message = argument;
    // Prepare the WebSocket frame
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t *)message;
    ws_pkt.len = strlen(message);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;

    // Ensure server and fd are valid
    if (server != NULL && fd >= 0) {
        if (httpd_ws_get_fd_info(server, fd) != HTTPD_WS_CLIENT_WEBSOCKET ||
                httpd_ws_send_frame_async(server, fd, &ws_pkt) != ESP_OK) {
            fd = -1;
            esp_log_set_vprintf(vprintf);
        }
    }

    // Free the allocated buffer
    free((void*)message);
}

/* The log stream is output-only, but incoming frames must still be consumed. */
static esp_err_t echo_handler(httpd_req_t * req)
{
    if (http_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }

    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "Handshake done, the new connection was opened");
        fd = httpd_req_to_sockfd(req);
        esp_log_set_vprintf(log_to_queue);
        return ESP_OK;
    }
    httpd_ws_frame_t packet = {0};
    esp_err_t result = httpd_ws_recv_frame(req, &packet, 0);
    if (result != ESP_OK) {
        return result;
    }
    uint8_t payload[128];
    if (packet.len > sizeof(payload)) {
        return ESP_FAIL;
    }
    if (packet.len > 0) {
        packet.payload = payload;
        return httpd_ws_recv_frame(req, &packet, sizeof(payload));
    }
    return ESP_OK;
}

// HTTP Error (404) Handler - Redirects all requests to the root page
static esp_err_t http_404_error_handler(httpd_req_t * req, httpd_err_code_t err)
{
    // Set status
    httpd_resp_set_status(req, "302 Temporary Redirect");
    // Redirect to the "/" root directory
    httpd_resp_set_hdr(req, "Location", "/");
    // iOS requires content in the response to detect a captive portal, simply redirecting is not sufficient.
    httpd_resp_send(req, "Redirect to the captive portal", HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "Redirecting to root");
    return ESP_OK;
}

static void websocket_log_handler(void *argument)
{
    (void)argument;
    while (true)
    {
        char *message = NULL;
        if (xQueueReceive(log_queue, &message, (TickType_t) portMAX_DELAY) != pdPASS) {
            if (message != NULL) {
                free((void*)message);
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
            continue;
        }

        if (httpd_queue_work(server, send_log_to_websocket, message) != ESP_OK) {
            esp_log_set_vprintf(vprintf);
            free(message);
        }
    }
}

esp_err_t start_rest_server(void * pvParameters)
{
    GLOBAL_STATE = (GlobalState *) pvParameters;
    const char * base_path = "";

    if (init_fs() != ESP_OK) {
        ESP_LOGW(TAG, "Serving the embedded recovery page until WWW is mounted");
    }

    REST_CHECK(base_path, "wrong base path", err);
    rest_server_context_t * rest_context = calloc(1, sizeof(rest_server_context_t));
    REST_CHECK(rest_context, "No memory for rest context", err);
    strlcpy(rest_context->base_path, base_path, sizeof(rest_context->base_path));

    log_queue = xQueueCreate(MESSAGE_QUEUE_SIZE, sizeof(char*));
    REST_CHECK(log_queue != NULL, "No memory for log queue", err_start);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;
    config.core_id = 1;
    config.lru_purge_enable = true;
    config.max_open_sockets = 10;
    config.max_uri_handlers = 20;

    ESP_LOGI(TAG, "Starting HTTP Server");
    REST_CHECK(httpd_start(&server, &config) == ESP_OK, "Start server failed", err_queue);
    if (xTaskCreate(websocket_log_handler, "websocket_log_handler", 4096,
                    NULL, 2, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create websocket log task");
        httpd_stop(server);
        server = NULL;
        goto err_queue;
    }

    httpd_uri_t recovery_explicit_get_uri = {
        .uri = "/recovery", 
        .method = HTTP_GET, 
        .handler = rest_recovery_handler, 
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &recovery_explicit_get_uri);
    
    // Register theme API endpoints
    ESP_ERROR_CHECK(register_theme_api_endpoints(server, rest_context));

    /* URI handler for fetching system info */
    httpd_uri_t system_info_get_uri = {
        .uri = "/api/system/info", 
        .method = HTTP_GET, 
        .handler = GET_system_info, 
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &system_info_get_uri);

    /* URI handler for WiFi scan */
    httpd_uri_t wifi_scan_get_uri = {
        .uri = "/api/system/wifi/scan",
        .method = HTTP_GET,
        .handler = GET_wifi_scan,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &wifi_scan_get_uri);

    httpd_uri_t swarm_options_uri = {
        .uri = "/api/swarm",
        .method = HTTP_OPTIONS,
        .handler = handle_options_request,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &swarm_options_uri);

    httpd_uri_t system_restart_uri = {
        .uri = "/api/system/restart", .method = HTTP_POST, 
        .handler = POST_restart, 
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &system_restart_uri);

    httpd_uri_t system_restart_options_uri = {
        .uri = "/api/system/restart", 
        .method = HTTP_OPTIONS, 
        .handler = handle_options_request, 
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &system_restart_options_uri);

    httpd_uri_t update_system_settings_uri = {
        .uri = "/api/system", 
        .method = HTTP_PATCH, 
        .handler = PATCH_update_settings, 
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &update_system_settings_uri);

    httpd_uri_t system_options_uri = {
        .uri = "/api/system",
        .method = HTTP_OPTIONS,
        .handler = handle_options_request,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(server, &system_options_uri);

    httpd_uri_t update_post_ota_firmware = {
        .uri = "/api/system/OTA", 
        .method = HTTP_POST, 
        .handler = POST_OTA_update, 
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &update_post_ota_firmware);

    httpd_uri_t update_post_ota_www = {
        .uri = "/api/system/OTAWWW", 
        .method = HTTP_POST, 
        .handler = POST_WWW_update, 
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server, &update_post_ota_www);

    httpd_uri_t ws = {
        .uri = "/api/ws", 
        .method = HTTP_GET, 
        .handler = echo_handler, 
        .user_ctx = NULL, 
        .is_websocket = true
    };
    httpd_register_uri_handler(server, &ws);

    httpd_uri_t common_get_uri = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = rest_common_get_handler,
        .user_ctx = rest_context
    };
    httpd_register_uri_handler(server, &common_get_uri);

    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, http_404_error_handler);

    // Start the DNS server that will redirect all queries to the softAP IP
    dns_server_config_t dns_config = DNS_SERVER_CONFIG_SINGLE("*" /* all A queries */, "WIFI_AP_DEF" /* softAP netif ID */);
    start_dns_server(&dns_config);

    return ESP_OK;
err_queue:
    vQueueDelete(log_queue);
    log_queue = NULL;
err_start:
    free(rest_context);
err:
    return ESP_FAIL;
}
