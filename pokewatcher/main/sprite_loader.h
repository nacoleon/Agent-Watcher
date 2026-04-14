#ifndef POKEWATCHER_SPRITE_LOADER_H
#define POKEWATCHER_SPRITE_LOADER_H

#include "event_queue.h"
#include <stdint.h>
#include <stdbool.h>

#define PW_MAX_FRAMES_PER_ANIM  8
#define PW_MAX_ANIMATIONS       16
#define PW_ANIM_NAME_LEN        32

typedef struct {
    uint16_t x;
    uint16_t y;
    uint16_t w;  // 0 = use default frame_width
    uint16_t h;  // 0 = use default frame_height
} pw_frame_coord_t;

typedef struct {
    char name[PW_ANIM_NAME_LEN];
    pw_frame_coord_t frames[PW_MAX_FRAMES_PER_ANIM];
    int frame_count;
    bool loop;
    bool mirror;
} pw_animation_t;

typedef struct {
    uint16_t frame_width;
    uint16_t frame_height;
    pw_animation_t animations[PW_MAX_ANIMATIONS];
    int animation_count;
    char state_anim_names[PW_STATE_COUNT][PW_ANIM_NAME_LEN];
    uint8_t *sprite_sheet_data;
    uint32_t sheet_width;
    uint32_t sheet_height;
} pw_sprite_data_t;

bool pw_sprite_load(const char *pokemon_id, pw_sprite_data_t *sprite);
void pw_sprite_free(pw_sprite_data_t *sprite);
const pw_animation_t *pw_sprite_get_state_anim(const pw_sprite_data_t *sprite, pw_agent_state_t state);
const pw_animation_t *pw_sprite_get_anim_by_name(const pw_sprite_data_t *sprite, const char *name);
uint8_t *pw_sprite_extract_frame_scaled(const pw_sprite_data_t *sprite, const pw_frame_coord_t *coord, uint16_t scale);
uint8_t *pw_sprite_extract_frame_scaled_ex(const pw_sprite_data_t *sprite, const pw_frame_coord_t *coord, uint16_t scale, bool mirror);

#endif
