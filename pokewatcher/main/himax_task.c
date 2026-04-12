#include "himax_task.h"
#include "event_queue.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensecap-watcher.h"
#include "sscma_client_ops.h"
#include "sscma_client_types.h"

static const char *TAG = "pw_himax";

#define PERSON_ABSENT_THRESHOLD  5

static bool s_person_present = false;
static int s_absent_count = 0;

static void process_detection(bool person_detected)
{
    if (person_detected) {
        s_absent_count = 0;
        if (!s_person_present) {
            s_person_present = true;
            pw_event_t evt = { .type = PW_EVENT_PERSON_DETECTED };
            pw_event_send(&evt);
            ESP_LOGI(TAG, "Person detected");
        }
    } else {
        if (s_person_present) {
            s_absent_count++;
            if (s_absent_count >= PERSON_ABSENT_THRESHOLD) {
                s_person_present = false;
                s_absent_count = 0;
                pw_event_t evt = { .type = PW_EVENT_PERSON_LEFT };
                pw_event_send(&evt);
                ESP_LOGI(TAG, "Person left");
            }
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

    // Wait for Himax to boot (it outputs noise initially)
    vTaskDelay(pdMS_TO_TICKS(3000));

    // Start continuous inference on the Himax chip
    sscma_client_invoke(client, -1, false, false);

    ESP_LOGI(TAG, "Himax detection running");

    // Keep task alive — callbacks handle detection events
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void pw_himax_task_start(void)
{
    xTaskCreate(himax_task, "himax", 4096, NULL, 6, NULL);
}
