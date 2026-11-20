#ifndef HTTP_SERVER_H_
#define HTTP_SERVER_H_
#include <esp_http_server.h>

esp_err_t start_rest_server(void *pvParameters);
esp_err_t http_network_allowed(httpd_req_t *req);

#endif
