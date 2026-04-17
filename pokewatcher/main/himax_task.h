#ifndef POKEWATCHER_HIMAX_TASK_H
#define POKEWATCHER_HIMAX_TASK_H

#include <stdbool.h>
#include <stddef.h>

void pw_himax_task_start(void);
void pw_himax_pause(void);
void pw_himax_resume(void);

// Set firmware data to flash before starting camera (call before pw_himax_task_start)
void pw_himax_set_firmware(void *data, size_t size);

#endif
