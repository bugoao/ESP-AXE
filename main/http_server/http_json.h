#pragma once

#include "cJSON.h"
#include "esp_http_server.h"

cJSON *http_read_json_object(httpd_req_t *req, char *buffer, size_t capacity);
