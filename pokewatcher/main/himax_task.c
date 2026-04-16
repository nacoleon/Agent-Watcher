#include "himax_task.h"
#include "event_queue.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensecap-watcher.h"
#include "sscma_client_ops.h"
#include "sscma_client_types.h"

static const char *TAG = "pw_himax";

#define PERSON_LEFT_TIMEOUT_MS  60000

static bool s_person_present = false;
static int64_t s_last_person_seen_ms = 0;
static volatile bool s_himax_paused = false;
static sscma_client_handle_t s_client = NULL;

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
    sscma_client_box_t *boxes = NULL;
    int num_boxes = 0;
    esp_err_t err = sscma_utils_fetch_boxes_from_reply(reply, &boxes, &num_boxes);
    if (err != ESP_OK || boxes == NULL || num_boxes == 0) {
        process_detection(false);
        return;
    }
    bool found_person = false;
    for (int i = 0; i < num_boxes; i++) {
        if (boxes[i].target == 0 && boxes[i].score > 60) {
            found_person = true;
            break;
        }
    }
    process_detection(found_person);
    free(boxes);
}

static void on_log(sscma_client_handle_t client, const sscma_client_reply_t *reply, void *user_ctx)
{
    ESP_LOGI(TAG, "on_log: %.*s", (int)reply->len, reply->data);
}

static void on_connect(sscma_client_handle_t client, const sscma_client_reply_t *reply, void *user_ctx)
{
    ESP_LOGI(TAG, "on_connect: Himax connected!");
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

    // Wait for Himax boot — monitor example uses 3s
    ESP_LOGI(TAG, "Waiting 3s for Himax boot...");
    vTaskDelay(pdMS_TO_TICKS(3000));

    s_client = client;

    // Init — hardware reset (stops any auto-running inference)
    esp_err_t init_err = sscma_client_init(client);
    ESP_LOGI(TAG, "sscma_client_init: 0x%x (%s)", init_err, esp_err_to_name(init_err));

    // Wait for chip to stabilize after reset, then stop any stale inference
    vTaskDelay(pdMS_TO_TICKS(1000));
    sscma_client_break(client);
    vTaskDelay(pdMS_TO_TICKS(500));

    // Flush stale data from RX buffer
    ESP_LOGI(TAG, "Flushing stale data...");
    for (int i = 0; i < 20; i++) {
        size_t avail = 0;
        sscma_client_available(client, &avail);
        if (avail == 0) break;
        char discard[256];
        size_t to_read = avail > sizeof(discard) ? sizeof(discard) : avail;
        sscma_client_read(client, discard, to_read);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGI(TAG, "RX buffer flushed");

    sscma_client_set_model(client, 1);

    sscma_client_info_t *info = NULL;
    esp_err_t info_err = sscma_client_get_info(client, &info, true);
    if (info_err == ESP_OK && info) {
        ESP_LOGI(TAG, "Himax: id=%s name=%s fw=%s",
                 info->id ? info->id : "?",
                 info->name ? info->name : "?",
                 info->fw_ver ? info->fw_ver : "?");
    } else {
        ESP_LOGE(TAG, "Himax not responding: 0x%x — camera disabled", info_err);
        vTaskDelete(NULL);
        return;
    }

    sscma_client_set_sensor(client, 1, 1, true);
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "Starting inference...");
    esp_err_t invoke_err = sscma_client_invoke(client, -1, false, false);
    if (invoke_err != ESP_OK) {
        ESP_LOGE(TAG, "invoke failed: 0x%x", invoke_err);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Person detection running");

    while (1) {
        if (s_himax_paused) {
            sscma_client_break(client);
            while (s_himax_paused) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            sscma_client_invoke(client, -1, false, false);
            ESP_LOGI(TAG, "Himax resumed");
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

void pw_himax_task_start(void)
{
    xTaskCreate(himax_task, "himax", 8192, NULL, 6, NULL);
}
