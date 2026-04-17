#include <string.h>
#include "himax_task.h"
#include "event_queue.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "sensecap-watcher.h"
#include "sscma_client_ops.h"
#include "sscma_client_types.h"

static const char *TAG = "pw_himax";

#define PERSON_LEFT_TIMEOUT_MS  60000
#define OTA_CHUNK_SIZE 256

static void *s_fw_data = NULL;
static size_t s_fw_size = 0;

static bool s_person_present = false;
static int64_t s_last_person_seen_ms = 0;
static volatile bool s_himax_paused = false;
static volatile bool s_himax_ready = false;
static volatile int s_pending_model = 0;  // 0 = no switch pending
static int s_active_model = 1;
static sscma_client_handle_t s_client = NULL;
static SemaphoreHandle_t s_connect_sem = NULL;

static const char *GESTURE_NAMES[] = { "Paper", "Rock", "Scissors" };

static void process_detection(bool person_detected)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (person_detected) {
        s_last_person_seen_ms = now_ms;
        if (!s_person_present) {
            s_person_present = true;
            pw_event_t evt = { .type = PW_EVENT_PERSON_DETECTED };
            pw_event_send(&evt);
            ESP_LOGI(TAG, "Person detected");
        }
    } else {
        if (s_person_present && s_last_person_seen_ms > 0 &&
            now_ms - s_last_person_seen_ms >= PERSON_LEFT_TIMEOUT_MS) {
            s_person_present = false;
            pw_event_t evt = { .type = PW_EVENT_PERSON_LEFT };
            pw_event_send(&evt);
            ESP_LOGI(TAG, "Person left (60s timeout)");
        }
    }
}

static void on_event(sscma_client_handle_t client, const sscma_client_reply_t *reply, void *user_ctx)
{
    if (!s_himax_ready) return;
    sscma_client_box_t *boxes = NULL;
    int num_boxes = 0;
    esp_err_t err = sscma_utils_fetch_boxes_from_reply(reply, &boxes, &num_boxes);
    if (err != ESP_OK || boxes == NULL || num_boxes == 0) {
        if (s_active_model != 3) process_detection(false);
        return;
    }

    if (s_active_model == 3) {
        // Gesture model: target 0=paper, 1=rock, 2=scissors
        for (int i = 0; i < num_boxes; i++) {
            if (boxes[i].score > 60 && boxes[i].target < 3) {
                pw_event_t evt = { .type = PW_EVENT_GESTURE_DETECTED };
                strncpy(evt.data.gesture.gesture, GESTURE_NAMES[boxes[i].target], 15);
                evt.data.gesture.score = boxes[i].score;
                pw_event_send(&evt);
            }
        }
    } else {
        // Person/Pet model: target 0=person (model 1), target 0=cat,1=dog,2=person (model 2)
        bool found_person = false;
        for (int i = 0; i < num_boxes; i++) {
            int t = boxes[i].target;
            bool is_person = (s_active_model == 1 && t == 0) ||
                             (s_active_model == 2 && t == 2);
            if (is_person && boxes[i].score > 60) {
                found_person = true;
                break;
            }
        }
        process_detection(found_person);
    }
    free(boxes);
}

static void on_log(sscma_client_handle_t client, const sscma_client_reply_t *reply, void *user_ctx)
{
    ESP_LOGI(TAG, "on_log: %.*s", (int)reply->len, reply->data);
}

static void on_connect(sscma_client_handle_t client, const sscma_client_reply_t *reply, void *user_ctx)
{
    ESP_LOGI(TAG, "on_connect: Himax connected!");
    if (s_connect_sem) xSemaphoreGive(s_connect_sem);
}

static void himax_task(void *arg)
{
    ESP_LOGI(TAG, "Himax task started");

    // Init SSCMA client — same as monitor example
    sscma_client_handle_t client = bsp_sscma_client_init();
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to create SSCMA client");
        vTaskDelete(NULL);
        return;
    }

    sscma_client_callback_t callback = {
        .on_event = on_event,
        .on_log = on_log,
        .on_connect = on_connect,
    };
    sscma_client_register_callback(client, &callback, NULL);
    s_client = client;

    s_connect_sem = xSemaphoreCreateBinary();

    // Flash firmware if loaded from SD card
    if (s_fw_data && s_fw_size > 0) {
        ESP_LOGI(TAG, "=== Flashing Himax firmware (%zu bytes) ===", s_fw_size);
        sscma_client_flasher_handle_t flasher = bsp_sscma_flasher_init();
        if (flasher == NULL) {
            ESP_LOGE(TAG, "Failed to init flasher");
        } else {
            sscma_client_init(client);
            esp_err_t ota_err = sscma_client_ota_start(client, flasher, 0x000000);
            if (ota_err != ESP_OK) {
                ESP_LOGE(TAG, "OTA start failed: 0x%x", ota_err);
            } else {
                size_t written = 0;
                bool failed = false;
                while (written < s_fw_size) {
                    size_t chunk = s_fw_size - written;
                    if (chunk > OTA_CHUNK_SIZE) chunk = OTA_CHUNK_SIZE;
                    // Pad last chunk to 256 bytes
                    char buf[OTA_CHUNK_SIZE];
                    memset(buf, 0, OTA_CHUNK_SIZE);
                    memcpy(buf, (char *)s_fw_data + written, chunk);
                    if (sscma_client_ota_write(client, buf, OTA_CHUNK_SIZE) != ESP_OK) {
                        ESP_LOGE(TAG, "OTA write failed at offset %zu", written);
                        sscma_client_ota_abort(client);
                        failed = true;
                        break;
                    }
                    written += chunk;
                    if ((written % (64 * 1024)) == 0 || written >= s_fw_size) {
                        ESP_LOGI(TAG, "Flash progress: %zu / %zu bytes", written, s_fw_size);
                    }
                }
                if (!failed) {
                    sscma_client_ota_finish(client);
                    ESP_LOGI(TAG, "=== Himax firmware flash complete! ===");
                    vTaskDelay(pdMS_TO_TICKS(1000));
                }
            }
        }
        free(s_fw_data);
        s_fw_data = NULL;
        s_fw_size = 0;
    }

    sscma_client_init(client);

    // Wait for camera to boot and send connect event
    if (xSemaphoreTake(s_connect_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGE(TAG, "Himax did not connect within 5s — camera disabled");
        vSemaphoreDelete(s_connect_sem);
        s_connect_sem = NULL;
        vTaskDelete(NULL);
        return;
    }
    vSemaphoreDelete(s_connect_sem);
    s_connect_sem = NULL;
    ESP_LOGI(TAG, "Camera ready, configuring person detection...");

    esp_err_t err;
    sscma_client_info_t *info = NULL;
    err = sscma_client_get_info(client, &info, true);
    if (err == ESP_OK && info) {
        ESP_LOGI(TAG, "Himax: id=%s name=%s fw=%s",
                 info->id ? info->id : "?",
                 info->name ? info->name : "?",
                 info->fw_ver ? info->fw_ver : "?");
    } else {
        ESP_LOGW(TAG, "get_info: 0x%x", err);
    }

    err = sscma_client_set_model(client, 1);
    ESP_LOGI(TAG, "set_model: 0x%x", err);

    err = sscma_client_set_sensor(client, 1, 1, true);
    ESP_LOGI(TAG, "set_sensor: 0x%x", err);
    vTaskDelay(pdMS_TO_TICKS(50));

    s_himax_ready = true;
    err = sscma_client_invoke(client, -1, false, true);
    ESP_LOGI(TAG, "invoke: 0x%x", err);
    ESP_LOGI(TAG, "Person detection running");

    while (1) {
        if (s_himax_paused) {
            sscma_client_break(client);
            while (s_himax_paused) vTaskDelay(pdMS_TO_TICKS(100));
            sscma_client_invoke(client, -1, false, true);
            ESP_LOGI(TAG, "Himax resumed");
        }
        // Check for pending model switch
        int new_model = s_pending_model;
        if (new_model > 0 && new_model != s_active_model) {
            ESP_LOGI(TAG, "Switching model %d -> %d", s_active_model, new_model);
            sscma_client_break(client);
            err = sscma_client_set_model(client, new_model);
            ESP_LOGI(TAG, "set_model(%d): 0x%x", new_model, err);
            if (err == ESP_OK) {
                s_active_model = new_model;
                sscma_client_set_sensor(client, 1, 1, true);
                vTaskDelay(pdMS_TO_TICKS(50));
                sscma_client_invoke(client, -1, false, true);
                ESP_LOGI(TAG, "Model %d running", new_model);
            }
            s_pending_model = 0;
        } else if (new_model > 0) {
            s_pending_model = 0;  // Same model, clear flag
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void pw_himax_pause(void)
{
    s_himax_paused = true;
    vTaskDelay(pdMS_TO_TICKS(150));
}

void pw_himax_resume(void)
{
    s_himax_paused = false;
}

void pw_himax_switch_model(int model_id)
{
    if (model_id >= 1 && model_id <= 3) {
        s_pending_model = model_id;
    }
}

int pw_himax_get_model(void)
{
    return s_active_model;
}

void pw_himax_set_firmware(void *data, size_t size)
{
    s_fw_data = data;
    s_fw_size = size;
}

void pw_himax_task_start(void)
{
    xTaskCreate(himax_task, "himax", 8192, NULL, 6, NULL);
}
