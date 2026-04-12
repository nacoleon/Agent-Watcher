#include "event_queue.h"
#include "esp_log.h"

static const char *TAG = "pw_event_queue";
static QueueHandle_t s_event_queue = NULL;

void pw_event_queue_init(void)
{
    s_event_queue = xQueueCreate(PW_EVENT_QUEUE_SIZE, sizeof(pw_event_t));
    if (s_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create event queue");
    } else {
        ESP_LOGI(TAG, "Event queue created (depth=%d)", PW_EVENT_QUEUE_SIZE);
    }
}

bool pw_event_send(const pw_event_t *event)
{
    if (s_event_queue == NULL) {
        ESP_LOGE(TAG, "Queue not initialized");
        return false;
    }
    BaseType_t ret = xQueueSend(s_event_queue, event, 0);
    if (ret != pdTRUE) {
        ESP_LOGW(TAG, "Event queue full, dropping event type=%d", event->type);
        return false;
    }
    return true;
}

bool pw_event_receive(pw_event_t *event, uint32_t timeout_ms)
{
    if (s_event_queue == NULL) {
        return false;
    }
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xQueueReceive(s_event_queue, event, ticks) == pdTRUE;
}

QueueHandle_t pw_event_queue_handle(void)
{
    return s_event_queue;
}
