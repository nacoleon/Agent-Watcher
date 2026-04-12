#ifndef POKEWATCHER_MOOD_ENGINE_H
#define POKEWATCHER_MOOD_ENGINE_H

#include "event_queue.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    pw_mood_t current_mood;
    pw_mood_t previous_mood;
    int64_t mood_entered_at_ms;
    int64_t last_person_seen_ms;
    bool person_present;
    uint32_t evolution_seconds;
    int64_t evolution_last_tick_ms;
} pw_mood_state_t;

typedef struct {
    uint32_t excited_duration_ms;
    uint32_t overjoyed_duration_ms;
    uint32_t curious_timeout_ms;
    uint32_t lonely_timeout_ms;
    uint32_t evolution_threshold_hours;
} pw_mood_config_t;

void pw_mood_engine_init(void);
bool pw_mood_engine_process_event(const pw_event_t *event);
bool pw_mood_engine_tick(void);
pw_mood_state_t pw_mood_engine_get_state(void);
pw_mood_config_t pw_mood_engine_get_config(void);
void pw_mood_engine_set_config(const pw_mood_config_t *config);
const char *pw_mood_to_string(pw_mood_t mood);

typedef void (*pw_mood_change_cb_t)(pw_mood_t old_mood, pw_mood_t new_mood);
void pw_mood_engine_set_change_cb(pw_mood_change_cb_t cb);

typedef void (*pw_roster_change_cb_t)(const char *pokemon_id);
typedef void (*pw_evolution_cb_t)(void);
void pw_mood_engine_set_roster_cb(pw_roster_change_cb_t cb);
void pw_mood_engine_set_evolution_cb(pw_evolution_cb_t cb);

void pw_mood_engine_task_start(void);

#endif
