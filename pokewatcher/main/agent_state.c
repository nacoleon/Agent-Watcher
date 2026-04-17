#include "agent_state.h"
#include "renderer.h"
#include "dialog.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "pw_agent";

static pw_agent_state_data_t s_state;
static pw_state_change_cb_t s_change_cb = NULL;

// Presence event log (last 5 events)
#define PRESENCE_LOG_SIZE 20
typedef struct {
    int64_t timestamp_ms;
    bool arrived;  // true = arrived, false = left
} presence_log_entry_t;
static presence_log_entry_t s_presence_log[PRESENCE_LOG_SIZE] = {0};
static int s_presence_log_count = 0;

// Gesture event log (last 5 events)
#define GESTURE_LOG_SIZE 20
typedef struct {
    int64_t timestamp_ms;
    char gesture[16];
    uint8_t score;
} gesture_log_entry_t;
static gesture_log_entry_t s_gesture_log[GESTURE_LOG_SIZE] = {0};
static int s_gesture_log_count = 0;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static const char *STATE_STRINGS[] = {
    [PW_STATE_IDLE]      = "idle",
    [PW_STATE_WORKING]   = "working",
    [PW_STATE_WAITING]   = "waiting",
    [PW_STATE_ALERT]     = "alert",
    [PW_STATE_GREETING]  = "greeting",
    [PW_STATE_SLEEPING]  = "sleeping",
    [PW_STATE_REPORTING] = "reporting",
    [PW_STATE_DOWN]      = "down",
    [PW_STATE_WAKEUP]    = "wakeup",
};

void pw_agent_state_init(void)
{
    s_state.current_state = PW_STATE_IDLE;
    s_state.previous_state = PW_STATE_IDLE;
    s_state.state_entered_at_ms = now_ms();
    s_state.person_present = false;
    ESP_LOGI(TAG, "Agent state engine initialized (starting IDLE)");
}

void pw_agent_state_set(pw_agent_state_t state)
{
    if (state >= PW_STATE_COUNT) return;
    if (state == s_state.current_state) return;

    pw_agent_state_t old = s_state.current_state;
    s_state.previous_state = old;
    s_state.current_state = state;
    s_state.state_entered_at_ms = now_ms();

    ESP_LOGI(TAG, "State: %s -> %s", STATE_STRINGS[old], STATE_STRINGS[state]);

    if (s_change_cb) {
        s_change_cb(old, state);
    }
}

pw_agent_state_data_t pw_agent_state_get(void)
{
    return s_state;
}

const char *pw_agent_state_to_string(pw_agent_state_t state)
{
    if (state < PW_STATE_COUNT) return STATE_STRINGS[state];
    return "unknown";
}

pw_agent_state_t pw_agent_state_from_string(const char *str)
{
    for (int i = 0; i < PW_STATE_COUNT; i++) {
        if (strcmp(STATE_STRINGS[i], str) == 0) return (pw_agent_state_t)i;
    }
    return PW_STATE_IDLE;
}

static void log_presence_event(bool arrived)
{
    for (int i = PRESENCE_LOG_SIZE - 1; i > 0; i--) {
        s_presence_log[i] = s_presence_log[i - 1];
    }
    s_presence_log[0].timestamp_ms = now_ms();
    s_presence_log[0].arrived = arrived;
    if (s_presence_log_count < PRESENCE_LOG_SIZE) s_presence_log_count++;
}

int pw_agent_state_get_presence_log(int64_t *timestamps, bool *arrived, int max_entries)
{
    int count = s_presence_log_count < max_entries ? s_presence_log_count : max_entries;
    for (int i = 0; i < count; i++) {
        timestamps[i] = s_presence_log[i].timestamp_ms;
        arrived[i] = s_presence_log[i].arrived;
    }
    return count;
}

static void log_gesture_event(const char *gesture, uint8_t score)
{
    for (int i = GESTURE_LOG_SIZE - 1; i > 0; i--) {
        s_gesture_log[i] = s_gesture_log[i - 1];
    }
    s_gesture_log[0].timestamp_ms = now_ms();
    strncpy(s_gesture_log[0].gesture, gesture, sizeof(s_gesture_log[0].gesture) - 1);
    s_gesture_log[0].gesture[sizeof(s_gesture_log[0].gesture) - 1] = '\0';
    s_gesture_log[0].score = score;
    if (s_gesture_log_count < GESTURE_LOG_SIZE) s_gesture_log_count++;
}

int pw_agent_state_get_gesture_log(int64_t *timestamps, char gestures[][16], uint8_t *scores, int max_entries)
{
    int count = s_gesture_log_count < max_entries ? s_gesture_log_count : max_entries;
    for (int i = 0; i < count; i++) {
        timestamps[i] = s_gesture_log[i].timestamp_ms;
        strncpy(gestures[i], s_gesture_log[i].gesture, 16);
        scores[i] = s_gesture_log[i].score;
    }
    return count;
}

void pw_agent_state_set_person_present(bool present)
{
    s_state.person_present = present;
}

void pw_agent_state_tick(void)
{
}

void pw_agent_state_set_change_cb(pw_state_change_cb_t cb)
{
    s_change_cb = cb;
}

static void agent_state_task(void *arg)
{
    ESP_LOGI(TAG, "Agent state task started");
    while (1) {
        pw_event_t event;
        if (pw_event_receive(&event, 1000)) {
            switch (event.type) {
                case PW_EVENT_PERSON_DETECTED:
                    pw_agent_state_set_person_present(true);
                    log_presence_event(true);
                    pw_renderer_wake_display();
                    if (s_state.current_state == PW_STATE_SLEEPING ||
                        s_state.current_state == PW_STATE_DOWN) {
                        pw_agent_state_set(PW_STATE_IDLE);
                    }
                    break;
                case PW_EVENT_PERSON_LEFT:
                    pw_agent_state_set_person_present(false);
                    log_presence_event(false);
                    if (s_state.current_state == PW_STATE_IDLE &&
                        !pw_dialog_is_visible()) {
                        pw_agent_state_set(PW_STATE_SLEEPING);
                    }
                    break;
                case PW_EVENT_GESTURE_DETECTED:
                    log_gesture_event(event.data.gesture.gesture, event.data.gesture.score);
                    ESP_LOGI(TAG, "Gesture: %s (score=%d)", event.data.gesture.gesture, event.data.gesture.score);
                    break;
                default:
                    break;
            }
        }
        pw_agent_state_tick();
    }
}

void pw_agent_state_task_start(void)
{
    xTaskCreate(agent_state_task, "agent_state", 4096, NULL, 5, NULL);
}
