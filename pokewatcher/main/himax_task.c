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

#define PERSON_LEFT_TIMEOUT_MS  60000  // 60 seconds before "person left"

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

// Callback fired when the SSCMA client receives detection events from Himax
static void on_event(sscma_client_handle_t client, const sscma_client_reply_t *reply, void *user_ctx)
{
    sscma_client_box_t *boxes = NULL;
    int num_boxes = 0;

    esp_err_t err = sscma_utils_fetch_boxes_from_reply(reply, &boxes, &num_boxes);
    if (err != ESP_OK || boxes == NULL || num_boxes == 0) {
        process_detection(false);
        return;
    }

    // Check if any detected box is a person (target 0 = person in COCO)
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

static void himax_task(void *arg)
{
    ESP_LOGI(TAG, "Himax task started, initializing SSCMA client...");

    // Use BSP function — handles SPI bus, IO expander, GPIO, and reset
    sscma_client_handle_t client = bsp_sscma_client_init();
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize SSCMA client");
        vTaskDelete(NULL);
        return;
    }

    // Register callback for detection events
    sscma_client_callback_t callback = {
        .on_event = on_event,
    };
    sscma_client_register_callback(client, &callback, NULL);
    ESP_LOGI(TAG, "Callback registered, waiting 3s for Himax boot...");

    // Wait for Himax to boot (it outputs noise initially)
    vTaskDelay(pdMS_TO_TICKS(3000));

    s_client = client;
    ESP_LOGI(TAG, "Calling sscma_client_init...");

    // Initialize the SSCMA client (handshake with Himax chip)
    esp_err_t init_err = sscma_client_init(client);
    ESP_LOGI(TAG, "sscma_client_init returned: 0x%x (%s)", init_err, esp_err_to_name(init_err));

    // Query device info
    ESP_LOGI(TAG, "Querying device info...");
    sscma_client_info_t *info = NULL;
    esp_err_t info_err = sscma_client_get_info(client, &info, true);
    ESP_LOGI(TAG, "get_info returned: 0x%x (%s)", info_err, esp_err_to_name(info_err));
    if (info_err == ESP_OK && info) {
        ESP_LOGI(TAG, "Himax ID: %s, name: %s, hw_ver: %s, fw_ver: %s",
                 info->id ? info->id : "null",
                 info->name ? info->name : "null",
                 info->hw_ver ? info->hw_ver : "null",
                 info->fw_ver ? info->fw_ver : "null");
    }

    // Query model info
    ESP_LOGI(TAG, "Querying model info...");
    sscma_client_model_t *model = NULL;
    esp_err_t model_err = sscma_client_get_model(client, &model, true);
    ESP_LOGI(TAG, "get_model returned: 0x%x (%s)", model_err, esp_err_to_name(model_err));
    if (model_err == ESP_OK && model) {
        ESP_LOGI(TAG, "Model ID: %d, uuid: %s, name: %s",
                 model->id,
                 model->uuid ? model->uuid : "null",
                 model->name ? model->name : "null");
    }

    // Start continuous inference on the Himax chip
    ESP_LOGI(TAG, "Starting continuous inference...");
    esp_err_t invoke_err = sscma_client_invoke(client, -1, false, false);
    ESP_LOGI(TAG, "sscma_client_invoke returned: 0x%x (%s)", invoke_err, esp_err_to_name(invoke_err));

    ESP_LOGI(TAG, "Himax detection running");

    // Keep task alive — handle pause/resume for SPI arbitration
    while (1) {
        if (s_himax_paused) {
            // Stop inference while LCD needs SPI bus
            sscma_client_break(client);
            while (s_himax_paused) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            // Restart inference
            sscma_client_invoke(client, -1, false, false);
            ESP_LOGI(TAG, "Himax resumed");
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void pw_himax_pause(void)
{
    s_himax_paused = true;
    // Give Himax task time to stop inference
    vTaskDelay(pdMS_TO_TICKS(150));
}

void pw_himax_resume(void)
{
    s_himax_paused = false;
}

void pw_himax_task_start(void)
{
    xTaskCreate(himax_task, "himax", 4096, NULL, 6, NULL);
}
