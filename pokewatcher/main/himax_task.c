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
#include <string.h>

static const char *TAG = "pw_himax";

#define PERSON_LEFT_TIMEOUT_MS  60000  // 60 seconds before "person left"

static bool s_person_present = false;
static int64_t s_last_person_seen_ms = 0;
static volatile bool s_himax_paused = false;
static sscma_client_handle_t s_client = NULL;
static sscma_client_handle_t s_early_client = NULL;

// Call early in app_main AFTER renderer init (DMA bounce buffer must be allocated first)
void pw_himax_early_init(void)
{
    s_early_client = bsp_sscma_client_init();
    if (s_early_client) {
        ESP_LOGI(TAG, "SSCMA client created (early init)");
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

// Flash a file from SD card to Himax at the given address
static bool flash_file(sscma_client_handle_t client,
                       sscma_client_flasher_handle_t flasher,
                       const char *path, uint32_t address)
{
    ESP_LOGI(TAG, "Flashing %s → 0x%06lx", path, (unsigned long)address);

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    ESP_LOGI(TAG, "File size: %ld bytes", file_size);

    if (sscma_client_ota_start(client, flasher, address) != ESP_OK) {
        ESP_LOGE(TAG, "ota_start failed");
        fclose(f);
        return false;
    }

    char buf[256];
    size_t total = 0;
    int64_t start_ms = esp_timer_get_time() / 1000;
    bool ok = true;

    while (1) {
        memset(buf, 0, sizeof(buf));
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n == 0) break;

        if (sscma_client_ota_write(client, buf, sizeof(buf)) != ESP_OK) {
            ESP_LOGE(TAG, "ota_write failed at %zu", total);
            ok = false;
            break;
        }
        total += n;

        if ((total % (64 * 1024)) == 0) {
            ESP_LOGI(TAG, "  %zu / %ld bytes (%d%%)",
                     total, file_size, (int)(total * 100 / file_size));
        }
    }
    fclose(f);

    sscma_client_ota_finish(client);
    vTaskDelay(pdMS_TO_TICKS(50));

    int64_t elapsed = (esp_timer_get_time() / 1000) - start_ms;
    if (ok) {
        ESP_LOGI(TAG, "Flashed %zu bytes in %lld ms", total, elapsed);
    }
    return ok;
}

static void himax_task(void *arg)
{
    ESP_LOGI(TAG, "Himax task started");

    sscma_client_handle_t client = s_early_client;
    if (client == NULL) {
        ESP_LOGE(TAG, "No SSCMA client — pw_himax_early_init() not called or failed");
        vTaskDelete(NULL);
        return;
    }

    sscma_client_callback_t callback = {
        .on_event = on_event,
        .on_log = on_log,
        .on_connect = on_connect,
    };
    sscma_client_register_callback(client, &callback, NULL);

    ESP_LOGI(TAG, "Waiting 3s for Himax boot...");
    vTaskDelay(pdMS_TO_TICKS(3000));

    s_client = client;

    // Hardware reset
    esp_err_t init_err = sscma_client_init(client);
    ESP_LOGI(TAG, "sscma_client_init: 0x%x (%s)", init_err, esp_err_to_name(init_err));

    // Wait for sync pin
    for (int i = 0; i < 10; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        uint8_t sync = bsp_exp_io_get_level(IO_EXPANDER_PIN_NUM_6);
        if (sync) {
            ESP_LOGI(TAG, "Sync pin HIGH after %.1fs", (i + 1) * 0.5f);
            break;
        }
    }

    // Try to talk to chip — retry get_info 3 times
    sscma_client_info_t *info = NULL;
    esp_err_t info_err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        ESP_LOGI(TAG, "get_info attempt %d...", attempt + 1);
        info_err = sscma_client_get_info(client, &info, false);
        if (info_err == ESP_OK) break;
        ESP_LOGW(TAG, "get_info failed: 0x%x (%s)", info_err, esp_err_to_name(info_err));
        // Reset and try again
        sscma_client_init(client);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    if (info_err != ESP_OK) {
        ESP_LOGE(TAG, "Himax not responding after 3 attempts — giving up");
        vTaskDelete(NULL);
        return;
    }

    // Log device info
    if (info) {
        ESP_LOGI(TAG, "Himax: id=%s name=%s fw=%s",
                 info->id ? info->id : "?",
                 info->name ? info->name : "?",
                 info->fw_ver ? info->fw_ver : "?");
    }

    // Configure sensor and start detection
    sscma_client_set_sensor(client, 1, 1, true);
    vTaskDelay(pdMS_TO_TICKS(50));

    ESP_LOGI(TAG, "Starting inference...");
    esp_err_t invoke_err = sscma_client_invoke(client, -1, false, false);
    if (invoke_err != ESP_OK) {
        ESP_LOGE(TAG, "invoke failed: 0x%x (%s)", invoke_err, esp_err_to_name(invoke_err));
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
    // Larger stack for OTA flash (file I/O + SPI transfers)
    xTaskCreate(himax_task, "himax", 8192, NULL, 6, NULL);
}
