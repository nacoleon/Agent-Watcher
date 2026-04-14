#ifndef POKEWATCHER_HIMAX_TASK_H
#define POKEWATCHER_HIMAX_TASK_H

#include <stdbool.h>

void pw_himax_task_start(void);
void pw_himax_pause(void);   // Pause SPI inference — call before LCD-heavy operations
void pw_himax_resume(void);  // Resume SPI inference

#endif
