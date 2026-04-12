#ifndef POKEWATCHER_EVENT_QUEUE_H
#define POKEWATCHER_EVENT_QUEUE_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "config.h"

typedef enum {
    PW_EVENT_PERSON_DETECTED,
    PW_EVENT_PERSON_LEFT,
    PW_EVENT_LLM_COMMENTARY,
    PW_EVENT_ROSTER_CHANGE,
    PW_EVENT_MOOD_CHANGED,
    PW_EVENT_EVOLUTION_TRIGGERED,
} pw_event_type_t;

typedef enum {
    PW_MOOD_EXCITED,
    PW_MOOD_HAPPY,
    PW_MOOD_CURIOUS,
    PW_MOOD_LONELY,
    PW_MOOD_SLEEPY,
    PW_MOOD_OVERJOYED,
} pw_mood_t;

typedef struct {
    pw_event_type_t type;
    union {
        struct {
            pw_mood_t new_mood;
            pw_mood_t old_mood;
        } mood;
        struct {
            char commentary[PW_LLM_MAX_RESPONSE_LEN];
        } llm;
        struct {
            char pokemon_id[32];
        } roster;
    } data;
} pw_event_t;

void pw_event_queue_init(void);
bool pw_event_send(const pw_event_t *event);
bool pw_event_receive(pw_event_t *event, uint32_t timeout_ms);
QueueHandle_t pw_event_queue_handle(void);

#endif
