#pragma once

#include "esp_http_server.h"
#include "global_state.h"

esp_err_t http_mount_www(void);
esp_err_t http_update_www(httpd_req_t *req, SystemModule *module);
esp_err_t http_update_firmware(httpd_req_t *req, SystemModule *module);
esp_err_t http_reboot(httpd_req_t *req);
