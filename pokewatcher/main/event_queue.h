#ifndef POKEWATCHER_EVENT_QUEUE_H
#define POKEWATCHER_EVENT_QUEUE_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "config.h"

typedef enum {
    PW_EVENT_PERSON_DETECTED,
    PW_EVENT_PERSON_LEFT,
    PW_EVENT_AGENT_STATE_CHANGED,
    PW_EVENT_MESSAGE_RECEIVED,
} pw_event_type_t;

typedef enum {
    PW_STATE_IDLE,
    PW_STATE_WORKING,
    PW_STATE_WAITING,
    PW_STATE_ALERT,
    PW_STATE_GREETING,
    PW_STATE_SLEEPING,
    PW_STATE_REPORTING,
    PW_STATE_COUNT,
} pw_agent_state_t;

typedef enum {
    PW_MSG_LEVEL_INFO,
    PW_MSG_LEVEL_SUCCESS,
    PW_MSG_LEVEL_WARNING,
    PW_MSG_LEVEL_ERROR,
} pw_msg_level_t;

typedef struct {
    pw_event_type_t type;
    union {
        struct {
            pw_agent_state_t new_state;
            pw_agent_state_t old_state;
        } state;
        struct {
            char text[81];
            pw_msg_level_t level;
        } message;
    } data;
} pw_event_t;

void pw_event_queue_init(void);
bool pw_event_send(const pw_event_t *event);
bool pw_event_receive(pw_event_t *event, uint32_t timeout_ms);
QueueHandle_t pw_event_queue_handle(void);

#endif
