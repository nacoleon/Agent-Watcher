#pragma once

#include <stdbool.h>
#include <time.h>

// Initialize SNTP (after WiFi connects) and start the scheduled reboot task.
// Reboots the device daily at 03:00 local time (Pacific) once time is synced.
// Fallback: reboots after 25h of uptime if SNTP never syncs.
void pw_time_sync_init(void);

// True once SNTP has synchronized at least once.
bool pw_time_is_synced(void);

// Returns Unix epoch seconds for the next scheduled reboot (today's or
// tomorrow's 3am local, whichever is in the future). Returns 0 if not synced.
time_t pw_time_next_reboot_epoch(void);
