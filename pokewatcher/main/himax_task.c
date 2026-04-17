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

static bool s_person_present = false;
static int64_t s_last_person_seen_ms = 0;
static volatile bool s_himax_paused = false;
static volatile bool s_himax_ready = false;
static sscma_client_handle_t s_client = NULL;
static SemaphoreHandle_t s_connect_sem = NULL;

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
    if (!s_himax_ready) return;  // Ignore stale data before init complete
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
    ESP_LOGI(TAG, "Camera ready, sending break to stop auto-inference...");

    esp_err_t brk = sscma_client_break(client);
    ESP_LOGI(TAG, "Break result: 0x%x", brk);
    // Camera's internal buffer has ~2s of binary inference data that drains after break
    vTaskDelay(pdMS_TO_TICKS(3000));

    // Start inference with result_only=true (boxes only, no base64 images)
    // This prevents the massive binary data flood that overwhelms the SPI bus
    s_himax_ready = true;
    ESP_LOGI(TAG, "Starting inference (result_only mode)...");
    esp_err_t inv = sscma_client_invoke(client, -1, false, true);
    if (inv != ESP_OK) {
        ESP_LOGW(TAG, "invoke failed (0x%x), listening for auto-inference events", inv);
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
