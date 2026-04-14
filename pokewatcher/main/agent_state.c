#include "agent_state.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "pw_agent";

static pw_agent_state_data_t s_state;
static pw_state_change_cb_t s_change_cb = NULL;

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

void pw_agent_state_set_person_present(bool present)
{
    s_state.person_present = present;
}

void pw_agent_state_tick(void)
{
    int64_t elapsed = now_ms() - s_state.state_entered_at_ms;

    if (s_state.current_state == PW_STATE_GREETING && elapsed > PW_GREETING_TIMEOUT_MS) {
        pw_agent_state_set(PW_STATE_IDLE);
    }
    if (s_state.current_state == PW_STATE_ALERT && elapsed > PW_ALERT_TIMEOUT_MS) {
        pw_agent_state_set(PW_STATE_IDLE);
    }
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
                    break;
                case PW_EVENT_PERSON_LEFT:
                    pw_agent_state_set_person_present(false);
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
