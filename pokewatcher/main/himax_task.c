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
static sscma_client_handle_t s_early_client = NULL;

// Call early in app_main BEFORE SD card init — registers Himax on SPI2 bus first
void pw_himax_early_init(void)
{
    s_early_client = bsp_sscma_client_init();
    if (s_early_client) {
        ESP_LOGI(TAG, "SSCMA client created (early init, before SD card)");
    } else {
        ESP_LOGE(TAG, "Failed to create SSCMA client");
    }
}

static void on_log(sscma_client_handle_t client, const sscma_client_reply_t *reply, void *user_ctx)
{
    ESP_LOGI(TAG, "on_log: %.*s", (int)reply->len, reply->data);
}

static void on_connect(sscma_client_handle_t client, const sscma_client_reply_t *reply, void *user_ctx)
{
    ESP_LOGI(TAG, "on_connect callback fired");
}

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
    ESP_LOGI(TAG, "Himax task started");

    // Use client from early init (created before SD card to avoid SPI2 bus contention)
    sscma_client_handle_t client = s_early_client;
    if (client == NULL) {
        ESP_LOGE(TAG, "No SSCMA client — pw_himax_early_init() not called or failed");
        vTaskDelete(NULL);
        return;
    }

    // Register all callbacks for diagnostics
    sscma_client_callback_t callback = {
        .on_event = on_event,
        .on_log = on_log,
        .on_connect = on_connect,
    };
    sscma_client_register_callback(client, &callback, NULL);

    // Wait for Himax to boot
    ESP_LOGI(TAG, "Waiting 3s for Himax boot...");
    vTaskDelay(pdMS_TO_TICKS(3000));

    s_client = client;

    // Check sync pin state before and after reset
    // Sync pin = IO expander pin 6 (BSP_SSCMA_CLIENT_SPI_SYNC)
    uint8_t sync = bsp_exp_io_get_level(IO_EXPANDER_PIN_NUM_6);
    ESP_LOGI(TAG, "Sync pin BEFORE reset: %d", sync);

    // Hardware reset + handshake (matches stock firmware sequence)
    esp_err_t init_err = sscma_client_init(client);
    ESP_LOGI(TAG, "sscma_client_init: 0x%x (%s)", init_err, esp_err_to_name(init_err));

    // Check sync pin after reset — should go HIGH if chip has firmware and is ready
    for (int i = 0; i < 10; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        sync = bsp_exp_io_get_level(IO_EXPANDER_PIN_NUM_6);
        ESP_LOGI(TAG, "Sync pin %.1fs after reset: %d", (i + 1) * 0.5f, sync);
        if (sync) break;
    }

    // Send raw AT command and poll for response
    const char *at_cmd = "\r\nAT+ID?\r\n";
    ESP_LOGI(TAG, "Sending raw AT+ID? command...");
    esp_err_t write_err = sscma_client_write(client, at_cmd, strlen(at_cmd));
    ESP_LOGI(TAG, "write() returned: 0x%x (%s)", write_err, esp_err_to_name(write_err));

    // Poll for response over 5 seconds
    for (int i = 0; i < 25; i++) {
        vTaskDelay(pdMS_TO_TICKS(200));
        size_t avail = 0;
        sscma_client_available(client, &avail);
        if (avail > 0) {
            char buf[256] = {0};
            size_t to_read = avail > sizeof(buf) - 1 ? sizeof(buf) - 1 : avail;
            sscma_client_read(client, buf, to_read);
            buf[to_read] = '\0';
            ESP_LOGI(TAG, "Response (%zu bytes): %s", to_read, buf);
            break;
        }
        if (i % 5 == 4) {
            ESP_LOGI(TAG, "  still waiting... (%ds)", (i + 1) / 5);
        }
    }

    // Select person detection model (model ID 1 at 0x400000)
    sscma_client_set_model(client, 1);

    // Query device info to verify chip is responding
    sscma_client_info_t *info = NULL;
    esp_err_t info_err = sscma_client_get_info(client, &info, true);
    if (info_err == ESP_OK && info) {
        ESP_LOGI(TAG, "Himax: id=%s name=%s fw=%s",
                 info->id ? info->id : "?",
                 info->name ? info->name : "?",
                 info->fw_ver ? info->fw_ver : "?");
    } else {
        ESP_LOGE(TAG, "Himax not responding: 0x%x (%s) — chip may need firmware flash",
                 info_err, esp_err_to_name(info_err));
        vTaskDelete(NULL);
        return;
    }

    // Configure sensor (416x416 resolution)
    sscma_client_set_sensor(client, 1, 1, true);
    vTaskDelay(pdMS_TO_TICKS(50));

    // Start continuous inference
    ESP_LOGI(TAG, "Starting inference...");
    esp_err_t invoke_err = sscma_client_invoke(client, -1, false, false);
    if (invoke_err != ESP_OK) {
        ESP_LOGE(TAG, "invoke failed: 0x%x (%s)", invoke_err, esp_err_to_name(invoke_err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Person detection running");

    // Keep task alive — handle pause/resume for SPI arbitration
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
    xTaskCreate(himax_task, "himax", 4096, NULL, 6, NULL);
}
