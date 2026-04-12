#include "himax_task.h"
#include "event_queue.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sscma_client_io.h"
#include "sscma_client_ops.h"

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

static void himax_task(void *arg)
{
    ESP_LOGI(TAG, "Himax task started, initializing SSCMA client...");

    sscma_client_handle_t client = NULL;
    sscma_client_io_handle_t io = NULL;

    sscma_client_io_uart_config_t uart_config = {
        .port = UART_NUM_1,
        .baud_rate = 921600,
        .tx_pin = 21,
        .rx_pin = 20,
        .rx_buffer_size = 4096,
        .tx_buffer_size = 1024,
    };

    esp_err_t err = sscma_client_new_io_uart_bus(&uart_config, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create SSCMA UART IO: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    sscma_client_config_t client_config = SSCMA_CLIENT_CONFIG_DEFAULT();
    err = sscma_client_new(io, &client_config, &client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create SSCMA client: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(3000));

    sscma_client_invoke(client, -1, false, false);

    ESP_LOGI(TAG, "Himax detection running");

    while (1) {
        sscma_client_reply_t reply = {};
        err = sscma_client_get_invoke(client, &reply, pdMS_TO_TICKS(500));

        if (err == ESP_OK && reply.boxes != NULL) {
            bool found_person = false;
            for (int i = 0; i < reply.box_count; i++) {
                if (reply.boxes[i].target == 0 && reply.boxes[i].score > 60) {
                    found_person = true;
                    break;
                }
            }
            process_detection(found_person);
            sscma_client_reply_clear(&reply);
        } else {
            process_detection(false);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void pw_himax_task_start(void)
{
    xTaskCreate(himax_task, "himax", 4096, NULL, 6, NULL);
}
