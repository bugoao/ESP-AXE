#include "theme_api.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "nvs_config.h"
#include "cJSON.h"
#include "http_json.h"
#include "http_server.h"

//static const char *TAG = "theme_api";

// Helper function to set CORS headers
static esp_err_t set_cors_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    return ESP_OK;
}

// Handle OPTIONS requests for CORS
static esp_err_t theme_options_handler(httpd_req_t *req)
{
    if (http_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }
    set_cors_headers(req);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// GET /api/theme handler
static esp_err_t theme_get_handler(httpd_req_t *req)
{
    if (http_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }
    httpd_resp_set_type(req, "application/json");
    set_cors_headers(req);

    char *scheme = nvs_config_get_string(NVS_CONFIG_THEME_SCHEME, "dark");
    char *name = nvs_config_get_string(NVS_CONFIG_THEME_NAME, "dark");
    char *colors = nvs_config_get_string(NVS_CONFIG_THEME_COLORS, 
        "{"
        "\"--primary-color\":\"#F80421\","
        "\"--primary-color-text\":\"#ffffff\","
        "\"--highlight-bg\":\"#F80421\","
        "\"--highlight-text-color\":\"#ffffff\","
        "\"--focus-ring\":\"0 0 0 0.2rem rgba(248,4,33,0.2)\","
        "\"--slider-bg\":\"#dee2e6\","
        "\"--slider-range-bg\":\"#F80421\","
        "\"--slider-handle-bg\":\"#F80421\","
        "\"--progressbar-bg\":\"#dee2e6\","
        "\"--progressbar-value-bg\":\"#F80421\","
        "\"--checkbox-border\":\"#F80421\","
        "\"--checkbox-bg\":\"#F80421\","
        "\"--checkbox-hover-bg\":\"#df031d\","
        "\"--button-bg\":\"#F80421\","
        "\"--button-hover-bg\":\"#df031d\","
        "\"--button-focus-shadow\":\"0 0 0 2px #ffffff, 0 0 0 4px #F80421\","
        "\"--togglebutton-bg\":\"#F80421\","
        "\"--togglebutton-border\":\"1px solid #F80421\","
        "\"--togglebutton-hover-bg\":\"#df031d\","
        "\"--togglebutton-hover-border\":\"1px solid #df031d\","
        "\"--togglebutton-text-color\":\"#ffffff\""
        "}"
    );

    if (scheme == NULL || name == NULL || colors == NULL) {
        free(scheme);
        free(name);
        free(colors);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Out of memory");
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL || cJSON_AddStringToObject(root, "colorScheme", scheme) == NULL ||
            cJSON_AddStringToObject(root, "theme", name) == NULL) {
        cJSON_Delete(root);
        free(scheme);
        free(name);
        free(colors);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Out of memory");
    }
    
    // Parse stored colors JSON string
    cJSON *colors_json = cJSON_Parse(colors);
    if (colors_json) {
        if (!cJSON_AddItemToObject(root, "accentColors", colors_json)) {
            cJSON_Delete(colors_json);
            cJSON_Delete(root);
            free(scheme);
            free(name);
            free(colors);
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "Out of memory");
        }
    }

    char *response = cJSON_PrintUnformatted(root);
    if (response == NULL) {
        cJSON_Delete(root);
        free(scheme);
        free(name);
        free(colors);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Failed to serialize theme");
    }
    esp_err_t send_result = httpd_resp_sendstr(req, response);

    free(scheme);
    free(name);
    free(colors);
    free(response);
    cJSON_Delete(root);

    return send_result;
}

// POST /api/theme handler
static esp_err_t theme_post_handler(httpd_req_t *req)
{
    if (http_network_allowed(req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
    }
    set_cors_headers(req);

    char content[2048];
    cJSON *root = http_read_json_object(req, content, sizeof(content));
    if (root == NULL) return ESP_FAIL;

    cJSON *scheme = cJSON_GetObjectItem(root, "colorScheme");
    cJSON *theme = cJSON_GetObjectItem(root, "theme");
    cJSON *colors = cJSON_GetObjectItem(root, "accentColors");
    if ((scheme != NULL && !cJSON_IsString(scheme)) ||
            (theme != NULL && !cJSON_IsString(theme)) ||
            (colors != NULL && !cJSON_IsObject(colors))) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid theme fields");
    }

    char *colors_str = colors != NULL ? cJSON_PrintUnformatted(colors) : NULL;
    if (colors != NULL && colors_str == NULL) {
        cJSON_Delete(root);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    }
    nvs_config_value_t values[3];
    size_t count = 0;
    if (scheme != NULL) values[count++] = (nvs_config_value_t){
        .key = NVS_CONFIG_THEME_SCHEME, .type = NVS_VALUE_STRING, .value.string = scheme->valuestring};
    if (theme != NULL) values[count++] = (nvs_config_value_t){
        .key = NVS_CONFIG_THEME_NAME, .type = NVS_VALUE_STRING, .value.string = theme->valuestring};
    if (colors_str != NULL) values[count++] = (nvs_config_value_t){
        .key = NVS_CONFIG_THEME_COLORS, .type = NVS_VALUE_STRING, .value.string = colors_str};
    esp_err_t err = nvs_config_set_values(values, count);
    free(colors_str);
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save theme");
    }
    return httpd_resp_sendstr(req, "{\"status\":\"ok\"}");
}

esp_err_t register_theme_api_endpoints(httpd_handle_t server, void* ctx)
{
    httpd_uri_t theme_get = {
        .uri = "/api/theme",
        .method = HTTP_GET,
        .handler = theme_get_handler,
        .user_ctx = ctx
    };

    httpd_uri_t theme_post = {
        .uri = "/api/theme",
        .method = HTTP_POST,
        .handler = theme_post_handler,
        .user_ctx = ctx
    };

    httpd_uri_t theme_options = {
        .uri = "/api/theme",
        .method = HTTP_OPTIONS,
        .handler = theme_options_handler,
        .user_ctx = ctx
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &theme_get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &theme_post));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &theme_options));

    return ESP_OK;
}
