#ifndef POKEWATCHER_RENDERER_H
#define POKEWATCHER_RENDERER_H

#include "event_queue.h"
#include "sprite_loader.h"
#include <stdbool.h>

void pw_renderer_init(void);
bool pw_renderer_load_character(const char *character_id);
void pw_renderer_set_state(pw_agent_state_t state);
void pw_renderer_show_message(const char *text, pw_msg_level_t level);
void pw_renderer_wake_display(void);
void pw_renderer_task_start(void);

#endif
