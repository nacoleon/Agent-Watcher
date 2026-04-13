#ifndef POKEWATCHER_RENDERER_H
#define POKEWATCHER_RENDERER_H

#include "event_queue.h"
#include "sprite_loader.h"
#include <stdbool.h>

void pw_renderer_init(void);
bool pw_renderer_load_pokemon(const char *pokemon_id);
void pw_renderer_set_mood(pw_mood_t mood);
void pw_renderer_play_evolution(const char *new_pokemon_id);
void pw_renderer_wake_display(void);
void pw_renderer_task_start(void);

#endif
