#include "mood_engine.h"
#include "roster.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "pw_mood";

static pw_mood_state_t s_state;
static pw_mood_config_t s_config;
static uint32_t s_last_persisted_evo_secs = 0;
#define PW_EVO_PERSIST_INTERVAL_SECS 60
static pw_mood_change_cb_t s_mood_change_cb = NULL;
static pw_roster_change_cb_t s_roster_change_cb = NULL;
static pw_evolution_cb_t s_evolution_cb = NULL;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void save_mood_config(void)
{
    nvs_handle_t handle;
    if (nvs_open(PW_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_blob(handle, "mood_cfg", &s_config, sizeof(pw_mood_config_t));
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void load_mood_config(void)
{
    nvs_handle_t handle;
    if (nvs_open(PW_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(pw_mood_config_t);
        esp_err_t err = nvs_get_blob(handle, "mood_cfg", &s_config, &len);
        nvs_close(handle);
        if (err == ESP_OK && len == sizeof(pw_mood_config_t)) {
            ESP_LOGI(TAG, "Mood config loaded from NVS");
            return;
        }
    }
    // Defaults
    s_config = (pw_mood_config_t){
        .excited_duration_ms = PW_EXCITED_DURATION_MS,
        .overjoyed_duration_ms = PW_OVERJOYED_DURATION_MS,
        .curious_timeout_ms = PW_CURIOUS_TIMEOUT_MS,
        .lonely_timeout_ms = PW_LONELY_TIMEOUT_MS,
        .evolution_threshold_hours = PW_DEFAULT_EVOLUTION_HOURS,
    };
}

static void set_mood(pw_mood_t new_mood)
{
    if (s_state.current_mood == new_mood) {
        return;
    }
    ESP_LOGI(TAG, "Mood: %s → %s", pw_mood_to_string(s_state.current_mood), pw_mood_to_string(new_mood));
    s_state.previous_mood = s_state.current_mood;
    s_state.current_mood = new_mood;
    s_state.mood_entered_at_ms = now_ms();
}

void pw_mood_engine_init(void)
{
    int64_t now = now_ms();

    s_state = (pw_mood_state_t){
        .current_mood = PW_MOOD_SLEEPY,
        .previous_mood = PW_MOOD_SLEEPY,
        .mood_entered_at_ms = now,
        .last_person_seen_ms = 0,
        .person_present = false,
        .evolution_seconds = 0,
        .evolution_last_tick_ms = now,
    };

    load_mood_config();

    // Restore evolution progress from roster
    pw_roster_t roster = pw_roster_get();
    const char *active = pw_roster_get_active_id();
    if (active) {
        for (int i = 0; i < roster.count; i++) {
            if (strcmp(roster.entries[i].id, active) == 0) {
                s_state.evolution_seconds = roster.entries[i].evolution_seconds;
                s_last_persisted_evo_secs = s_state.evolution_seconds;
                ESP_LOGI(TAG, "Restored evolution progress: %lu seconds", (unsigned long)s_state.evolution_seconds);
                break;
            }
        }
    }

    ESP_LOGI(TAG, "Mood engine initialized (starting in SLEEPY)");
}

bool pw_mood_engine_process_event(const pw_event_t *event)
{
    pw_mood_t old_mood = s_state.current_mood;

    switch (event->type) {
    case PW_EVENT_PERSON_DETECTED:
        s_state.person_present = true;
        s_state.last_person_seen_ms = now_ms();

        switch (s_state.current_mood) {
        case PW_MOOD_SLEEPY:
        case PW_MOOD_LONELY:
            set_mood(PW_MOOD_OVERJOYED);
            break;
        case PW_MOOD_CURIOUS:
            set_mood(PW_MOOD_HAPPY);
            break;
        case PW_MOOD_HAPPY:
        case PW_MOOD_EXCITED:
        case PW_MOOD_OVERJOYED:
            break;
        }
        break;

    case PW_EVENT_PERSON_LEFT:
        s_state.person_present = false;
        if (s_state.current_mood == PW_MOOD_HAPPY ||
            s_state.current_mood == PW_MOOD_EXCITED) {
            set_mood(PW_MOOD_CURIOUS);
        }
        break;

    default:
        break;
    }

    return s_state.current_mood != old_mood;
}

bool pw_mood_engine_tick(void)
{
    int64_t now = now_ms();
    int64_t in_mood_ms = now - s_state.mood_entered_at_ms;
    pw_mood_t old_mood = s_state.current_mood;

    switch (s_state.current_mood) {
    case PW_MOOD_EXCITED:
        if (in_mood_ms >= s_config.excited_duration_ms) {
            set_mood(PW_MOOD_HAPPY);
        }
        break;

    case PW_MOOD_OVERJOYED:
        if (in_mood_ms >= s_config.overjoyed_duration_ms) {
            set_mood(PW_MOOD_HAPPY);
        }
        break;

    case PW_MOOD_CURIOUS:
        if (!s_state.person_present && in_mood_ms >= s_config.curious_timeout_ms) {
            set_mood(PW_MOOD_LONELY);
        }
        break;

    case PW_MOOD_LONELY:
        if (!s_state.person_present && in_mood_ms >= s_config.lonely_timeout_ms) {
            set_mood(PW_MOOD_SLEEPY);
        }
        break;

    case PW_MOOD_HAPPY:
    case PW_MOOD_SLEEPY:
        break;
    }

    // Tick evolution timer
    if (now - s_state.evolution_last_tick_ms >= 1000) {
        uint32_t elapsed = (uint32_t)((now - s_state.evolution_last_tick_ms) / 1000);
        s_state.evolution_seconds += elapsed;
        s_state.evolution_last_tick_ms = now;

        // Persist evolution progress every 60 seconds
        if (s_state.evolution_seconds - s_last_persisted_evo_secs >= PW_EVO_PERSIST_INTERVAL_SECS) {
            pw_roster_update_evolution(s_state.evolution_seconds);
            pw_roster_save();
            s_last_persisted_evo_secs = s_state.evolution_seconds;
        }

        uint32_t threshold_seconds = s_config.evolution_threshold_hours * 3600;
        if (threshold_seconds > 0 && s_state.evolution_seconds >= threshold_seconds) {
            pw_event_t evt = { .type = PW_EVENT_EVOLUTION_TRIGGERED };
            pw_event_send(&evt);
            s_state.evolution_seconds = 0;
            s_last_persisted_evo_secs = 0;
        }
    }

    return s_state.current_mood != old_mood;
}

pw_mood_state_t pw_mood_engine_get_state(void)
{
    return s_state;
}

pw_mood_config_t pw_mood_engine_get_config(void)
{
    return s_config;
}

void pw_mood_engine_set_config(const pw_mood_config_t *config)
{
    s_config = *config;
    save_mood_config();
    ESP_LOGI(TAG, "Config updated and saved: excited=%lums, curious=%lums, lonely=%lums",
             (unsigned long)s_config.excited_duration_ms,
             (unsigned long)s_config.curious_timeout_ms,
             (unsigned long)s_config.lonely_timeout_ms);
}

const char *pw_mood_to_string(pw_mood_t mood)
{
    switch (mood) {
    case PW_MOOD_EXCITED:   return "excited";
    case PW_MOOD_HAPPY:     return "happy";
    case PW_MOOD_CURIOUS:   return "curious";
    case PW_MOOD_LONELY:    return "lonely";
    case PW_MOOD_SLEEPY:    return "sleepy";
    case PW_MOOD_OVERJOYED: return "overjoyed";
    default:                return "unknown";
    }
}

void pw_mood_engine_set_change_cb(pw_mood_change_cb_t cb)
{
    s_mood_change_cb = cb;
}

void pw_mood_engine_set_roster_cb(pw_roster_change_cb_t cb)
{
    s_roster_change_cb = cb;
}

void pw_mood_engine_set_evolution_cb(pw_evolution_cb_t cb)
{
    s_evolution_cb = cb;
}

static void notify_mood_change(pw_mood_t old_mood, pw_mood_t new_mood)
{
    if (s_mood_change_cb) {
        s_mood_change_cb(old_mood, new_mood);
    }
}

static void mood_engine_task(void *arg)
{
    ESP_LOGI(TAG, "Mood engine task started");
    pw_event_t event;

    while (1) {
        if (pw_event_receive(&event, 1000)) {
            switch (event.type) {
            case PW_EVENT_PERSON_DETECTED:
            case PW_EVENT_PERSON_LEFT: {
                pw_mood_t old = s_state.current_mood;
                pw_mood_engine_process_event(&event);
                if (s_state.current_mood != old) {
                    notify_mood_change(old, s_state.current_mood);
                }
                break;
            }
            case PW_EVENT_ROSTER_CHANGE:
                if (s_roster_change_cb) {
                    s_roster_change_cb(event.data.roster.pokemon_id);
                }
                break;
            case PW_EVENT_EVOLUTION_TRIGGERED:
                if (s_evolution_cb) {
                    s_evolution_cb();
                }
                break;
            default:
                break;
            }
        }
        pw_mood_t old = s_state.current_mood;
        pw_mood_engine_tick();
        if (s_state.current_mood != old) {
            notify_mood_change(old, s_state.current_mood);
        }
    }
}

void pw_mood_engine_task_start(void)
{
    xTaskCreate(mood_engine_task, "mood_engine", 4096, NULL, 5, NULL);
}
