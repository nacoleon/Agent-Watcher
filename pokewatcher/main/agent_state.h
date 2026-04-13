#ifndef POKEWATCHER_AGENT_STATE_H
#define POKEWATCHER_AGENT_STATE_H

#include "event_queue.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    pw_agent_state_t current_state;
    pw_agent_state_t previous_state;
    int64_t state_entered_at_ms;
    bool person_present;
} pw_agent_state_data_t;

typedef void (*pw_state_change_cb_t)(pw_agent_state_t old_state, pw_agent_state_t new_state);

void pw_agent_state_init(void);
void pw_agent_state_set(pw_agent_state_t state);
pw_agent_state_data_t pw_agent_state_get(void);
const char *pw_agent_state_to_string(pw_agent_state_t state);
pw_agent_state_t pw_agent_state_from_string(const char *str);
void pw_agent_state_set_person_present(bool present);
void pw_agent_state_tick(void);
void pw_agent_state_set_change_cb(pw_state_change_cb_t cb);
void pw_agent_state_task_start(void);

#endif
