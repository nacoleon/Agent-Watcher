#include "himax_task.h"
#include "event_queue.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sensecap-watcher.h"
#include "sscma_client_io.h"
#include "sscma_client_ops.h"
#include "sscma_client_flasher.h"
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

static void himax_task(void *arg)
{
    ESP_LOGI(TAG, "Himax task started — using UART transport");

    // Explicit power cycle of Himax AI chip before communication
    ESP_LOGI(TAG, "Power-cycling Himax AI chip...");
    bsp_exp_io_set_level(BSP_PWR_AI_CHIP, 0);  // Power OFF
    vTaskDelay(pdMS_TO_TICKS(500));
    bsp_exp_io_set_level(BSP_PWR_AI_CHIP, 1);  // Power ON
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "Himax power cycle complete");

    // Init UART1 bus for Himax communication
    bsp_uart_bus_init();
    ESP_LOGI(TAG, "UART1 initialized (921600 baud, TX=GPIO17, RX=GPIO18)");

    // Create UART IO handle
    sscma_client_io_handle_t io_handle = NULL;
    sscma_client_io_uart_config_t uart_io_config = {
        .user_ctx = NULL,
    };
    esp_err_t io_err = sscma_client_new_io_uart_bus(
        (sscma_client_uart_bus_handle_t)BSP_SSCMA_FLASHER_UART_NUM,
        &uart_io_config, &io_handle);
    if (io_err != ESP_OK || !io_handle) {
        ESP_LOGE(TAG, "Failed to create UART IO: 0x%x", io_err);
        vTaskDelete(NULL);
        return;
    }

    // Create SSCMA client with UART IO (instead of SPI)
    sscma_client_config_t client_config = SSCMA_CLIENT_CONFIG_DEFAULT();
    // Reset pin still via IO expander (same as SPI approach)
    client_config.reset_gpio_num = BSP_SSCMA_CLIENT_RST;
    client_config.io_expander = bsp_io_expander_init();
    client_config.flags.reset_use_expander = true;

    sscma_client_handle_t client = NULL;
    esp_err_t client_err = sscma_client_new(io_handle, &client_config, &client);
    if (client_err != ESP_OK || !client) {
        ESP_LOGE(TAG, "Failed to create SSCMA client: 0x%x", client_err);
        vTaskDelete(NULL);
        return;
    }

    // Register callbacks
    sscma_client_callback_t callback = {
        .on_event = on_event,
        .on_log = on_log,
    };
    sscma_client_register_callback(client, &callback, NULL);

    // Wait for Himax to boot
    ESP_LOGI(TAG, "Waiting 3s for Himax boot...");
    vTaskDelay(pdMS_TO_TICKS(3000));

    s_client = client;

    // Hardware reset + init
    esp_err_t init_err = sscma_client_init(client);
    ESP_LOGI(TAG, "sscma_client_init: 0x%x (%s)", init_err, esp_err_to_name(init_err));

    // Wait for chip to be ready after reset
    vTaskDelay(pdMS_TO_TICKS(1000));

    // Query device info
    ESP_LOGI(TAG, "Querying device info via UART...");
    sscma_client_info_t *info = NULL;
    esp_err_t info_err = sscma_client_get_info(client, &info, true);
    if (info_err == ESP_OK && info) {
        ESP_LOGI(TAG, "Himax: id=%s name=%s fw=%s",
                 info->id ? info->id : "?",
                 info->name ? info->name : "?",
                 info->fw_ver ? info->fw_ver : "?");
    } else {
        ESP_LOGW(TAG, "Himax not responding via UART (0x%x) — attempting firmware flash", info_err);

        // Init UART-based flasher using proper API from sscma_client_flasher.h
        sscma_client_flasher_we2_config_t flasher_config = {
            .reset_gpio_num = BSP_SSCMA_CLIENT_RST,
            .io_expander = bsp_io_expander_init(),
            .flags.reset_use_expander = true,
        };

        sscma_client_flasher_handle_t flasher = NULL;
        esp_err_t flash_err = sscma_client_new_flasher_we2_uart(io_handle, &flasher_config, &flasher);
        if (flash_err != ESP_OK || !flasher) {
            ESP_LOGE(TAG, "UART flasher init failed: 0x%x", flash_err);
            vTaskDelete(NULL);
            return;
        }

        ESP_LOGI(TAG, "Flashing firmware.img via UART...");
        if (sscma_client_ota_start(client, flasher, 0x000000) != ESP_OK) {
            ESP_LOGE(TAG, "ota_start failed");
            vTaskDelete(NULL);
            return;
        }

        FILE *f = fopen("/sdcard/himax/firmware.img", "rb");
        if (!f) {
            ESP_LOGE(TAG, "Cannot open firmware.img");
            sscma_client_ota_finish(client);
            vTaskDelete(NULL);
            return;
        }

        fseek(f, 0, SEEK_END);
        long file_size = ftell(f);
        fseek(f, 0, SEEK_SET);
        ESP_LOGI(TAG, "firmware.img: %ld bytes", file_size);

        char buf[256];
        size_t total = 0;
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
                ESP_LOGI(TAG, "  %zu / %ld bytes (%d%%)", total, file_size, (int)(total * 100 / file_size));
            }
        }
        fclose(f);
        sscma_client_ota_finish(client);

        if (!ok) {
            ESP_LOGE(TAG, "Firmware flash failed");
            vTaskDelete(NULL);
            return;
        }

        ESP_LOGI(TAG, "Firmware flashed (%zu bytes). Waiting 5s for reboot...", total);
        vTaskDelay(pdMS_TO_TICKS(5000));

        // Re-init and try again
        sscma_client_init(client);
        vTaskDelay(pdMS_TO_TICKS(1000));

        info_err = sscma_client_get_info(client, &info, false);
        if (info_err != ESP_OK) {
            ESP_LOGE(TAG, "Still not responding after flash — giving up");
            vTaskDelete(NULL);
            return;
        }
        ESP_LOGI(TAG, "Himax responding after flash! id=%s fw=%s",
                 info->id ? info->id : "?", info->fw_ver ? info->fw_ver : "?");
    }

    // Select person detection model
    sscma_client_set_model(client, 1);
    vTaskDelay(pdMS_TO_TICKS(100));

    // Configure sensor (416x416)
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

    ESP_LOGI(TAG, "Person detection running (UART transport)");

    // Keep task alive
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
