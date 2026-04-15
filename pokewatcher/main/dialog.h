#ifndef POKEWATCHER_DIALOG_H
#define POKEWATCHER_DIALOG_H

#include "event_queue.h"
#include "lvgl.h"
#include <stdbool.h>

void pw_dialog_init(lv_obj_t *parent);
void pw_dialog_show(const char *text, pw_msg_level_t level);
void pw_dialog_hide(void);
bool pw_dialog_is_visible(void);
int pw_dialog_get_dismiss_count(void);
void pw_dialog_tick(void);
void pw_dialog_next_page(void);
void pw_dialog_prev_page(void);
bool pw_dialog_consume_btn_wake(void);
const char *pw_dialog_get_last_text(void);

#endif
