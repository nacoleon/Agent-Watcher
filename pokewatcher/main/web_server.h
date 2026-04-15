#ifndef POKEWATCHER_WEB_SERVER_H
#define POKEWATCHER_WEB_SERVER_H

#include <stdint.h>

void pw_web_server_start(void);
void pw_web_server_stop(void);
int64_t pw_web_get_last_heartbeat_ms(void);

#endif
