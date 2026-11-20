#include "http_json.h"

#include <string.h>
#include "esp_timer.h"

cJSON *http_read_json_object(httpd_req_t *req, char *buffer, size_t capacity)
{
    if (req->content_len == 0 || req->content_len >= capacity) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON length");
        return NULL;
    }
    int64_t deadline = esp_timer_get_time() + 30000000LL;
    size_t received = 0;
    while (received < req->content_len) {
        if (esp_timer_get_time() >= deadline) {
            httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "JSON upload timed out");
            return NULL;
        }
        int count = httpd_req_recv(req, buffer + received, req->content_len - received);
        if (count == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (count <= 0 || (size_t)count > req->content_len - received) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Incomplete JSON body");
            return NULL;
        }
        received += (size_t)count;
    }
    buffer[received] = '\0';
    cJSON *root = memchr(buffer, '\0', received) == NULL ?
        cJSON_ParseWithLengthOpts(buffer, received + 1, NULL, true) : NULL;
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected a JSON object");
        return NULL;
    }
    return root;
}
