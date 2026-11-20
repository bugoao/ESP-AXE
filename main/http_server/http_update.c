#include "http_update.h"

#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define UPLOAD_TIMEOUT_US (300LL * 1000000LL)

static const char *TAG = "http_update";

esp_err_t http_mount_www(void)
{
    if (!esp_spiffs_mounted("www")) {
        esp_vfs_spiffs_conf_t config = {
            .base_path = "", .partition_label = "www", .max_files = 5,
            .format_if_mount_failed = false,
        };
        esp_err_t err = esp_vfs_spiffs_register(&config);
        if (err != ESP_OK) {
            return err;
        }
    }
    int index = open("/index.html.gz", O_RDONLY);
    if (index < 0) {
        (void)esp_vfs_spiffs_unregister("www");
        return ESP_FAIL;
    }
    close(index);
    return ESP_OK;
}

static esp_err_t upload_error(httpd_req_t *req, SystemModule *module,
                               httpd_err_code_t status, const char *message)
{
    snprintf(module->firmware_update_status, sizeof(module->firmware_update_status), "%s", message);
    module->is_firmware_update = false;
    httpd_resp_send_err(req, status, message);
    // Close the session if there are still unread upload bytes.
    return ESP_FAIL;
}

static esp_err_t receive_image(httpd_req_t *req, SystemModule *module,
                                const esp_partition_t *partition,
                                esp_ota_handle_t ota, bool firmware,
                                const char **failure)
{
    char buffer[1024];
    size_t offset = 0;
    int64_t deadline = esp_timer_get_time() + UPLOAD_TIMEOUT_US;

    while (offset < req->content_len) {
        if (esp_timer_get_time() >= deadline) {
            *failure = "Upload timed out";
            return ESP_ERR_TIMEOUT;
        }
        size_t remaining = req->content_len - offset;
        size_t chunk = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        int count = httpd_req_recv(req, buffer, chunk);
        if (count == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (count <= 0 || (size_t)count > chunk) {
            *failure = "Protocol Error";
            return ESP_FAIL;
        }
        esp_err_t err = firmware ? esp_ota_write(ota, buffer, (size_t)count) :
                                  esp_partition_write(partition, offset, buffer, (size_t)count);
        if (err != ESP_OK) {
            *failure = "Write Error";
            return err;
        }
        offset += (size_t)count;
        unsigned progress = (unsigned)((uint64_t)offset * 100U / req->content_len);
        snprintf(module->firmware_update_status, sizeof(module->firmware_update_status),
                 "Working (%u%%)", progress);
    }
    return ESP_OK;
}

esp_err_t http_update_www(httpd_req_t *req, SystemModule *module)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "www");
    if (partition == NULL) {
        return upload_error(req, module, HTTPD_500_INTERNAL_SERVER_ERROR, "WWW not found");
    }
    // The build produces a SPIFFS image for the entire partition. Validate
    // its length before unmounting or erasing the live filesystem.
    if (req->content_len != partition->size) {
        return upload_error(req, module, HTTPD_400_BAD_REQUEST, "Invalid image size");
    }

    module->is_firmware_update = true;
    snprintf(module->firmware_update_filename, sizeof(module->firmware_update_filename), "www.bin");
    snprintf(module->firmware_update_status, sizeof(module->firmware_update_status), "Starting...");

    // SPIFFS must not keep cached metadata for a partition being erased.
    if (esp_spiffs_mounted("www") && esp_vfs_spiffs_unregister("www") != ESP_OK) {
        return upload_error(req, module, HTTPD_500_INTERNAL_SERVER_ERROR, "Unmount failed");
    }
    if (esp_partition_erase_range(partition, 0, partition->size) != ESP_OK) {
        (void)http_mount_www();
        return upload_error(req, module, HTTPD_500_INTERNAL_SERVER_ERROR, "Erase failed");
    }

    const char *failure = NULL;
    esp_err_t err = receive_image(req, module, partition, 0, false, &failure);
    if (err != ESP_OK) {
        // Leave an incomplete filesystem unmounted. The embedded recovery page
        // remains available and the next upload can replace the broken image.
        return upload_error(req, module, err == ESP_ERR_TIMEOUT ? HTTPD_408_REQ_TIMEOUT :
                            HTTPD_500_INTERNAL_SERVER_ERROR, failure);
    }
    if (http_mount_www() != ESP_OK) {
        return upload_error(req, module, HTTPD_500_INTERNAL_SERVER_ERROR, "WWW mount failed");
    }

    snprintf(module->firmware_update_status, sizeof(module->firmware_update_status), "Finished...");
    module->is_firmware_update = false;
    return httpd_resp_sendstr(req, "WWW update complete\n");
}

esp_err_t http_update_firmware(httpd_req_t *req, SystemModule *module)
{
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == NULL) {
        return upload_error(req, module, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA not found");
    }
    if (req->content_len == 0 || req->content_len > partition->size) {
        return upload_error(req, module, HTTPD_400_BAD_REQUEST, "Invalid image size");
    }

    module->is_firmware_update = true;
    snprintf(module->firmware_update_filename, sizeof(module->firmware_update_filename), "esp-miner.bin");
    snprintf(module->firmware_update_status, sizeof(module->firmware_update_status), "Starting...");
    esp_ota_handle_t ota = 0;
    if (esp_ota_begin(partition, req->content_len, &ota) != ESP_OK) {
        return upload_error(req, module, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
    }

    const char *failure = NULL;
    esp_err_t err = receive_image(req, module, partition, ota, true, &failure);
    if (err != ESP_OK) {
        (void)esp_ota_abort(ota);
        return upload_error(req, module, err == ESP_ERR_TIMEOUT ? HTTPD_408_REQ_TIMEOUT :
                            HTTPD_500_INTERNAL_SERVER_ERROR, failure);
    }
    // esp_ota_end consumes the handle on success and failure.
    if (esp_ota_end(ota) != ESP_OK) {
        return upload_error(req, module, HTTPD_400_BAD_REQUEST, "Validation Error");
    }
    if (esp_ota_set_boot_partition(partition) != ESP_OK) {
        (void)esp_ota_set_boot_partition(esp_ota_get_running_partition());
        return upload_error(req, module, HTTPD_500_INTERNAL_SERVER_ERROR, "Activation Error");
    }

    snprintf(module->firmware_update_status, sizeof(module->firmware_update_status), "Rebooting...");
    module->is_firmware_update = false;
    esp_err_t response = httpd_resp_sendstr(req, "Firmware update complete, rebooting now!\n");
    ESP_LOGI(TAG, "Firmware update validated, restarting");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return response;
}

esp_err_t http_reboot(httpd_req_t *req)
{
    esp_err_t result = httpd_resp_sendstr(req, "System will restart shortly.");
    if (result != ESP_OK) {
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}
