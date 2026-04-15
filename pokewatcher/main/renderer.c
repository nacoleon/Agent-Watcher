#include "renderer.h"
#include "config.h"
#include "sprite_loader.h"
#include "agent_state.h"
#include "dialog.h"
#include "sensecap-watcher.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "pw_renderer";

static SemaphoreHandle_t s_render_mutex = NULL;

static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_bg_img = NULL;
static lv_obj_t *s_sprite_img = NULL;
static lv_obj_t *s_zzz_label = NULL;

// Background state
static uint8_t *s_bg_buf = NULL;
static lv_img_dsc_t s_bg_dsc = {};
static bool s_bg_available = false;
static char s_bg_path[300] = "";
static bool s_bg_needs_load = false;
static int s_bg_pending_idx = -1;
static int s_current_bg_idx = -1;

static pw_sprite_data_t s_sprite = {};
static pw_agent_state_t s_current_state = PW_STATE_IDLE;
static const pw_animation_t *s_current_anim = NULL;
static int s_current_frame = 0;
static uint8_t *s_frame_buf = NULL;
static lv_img_dsc_t s_frame_dsc = {};

// --- Background colors per agent state ---
static const uint32_t STATE_BG_COLORS[] = {
    [PW_STATE_IDLE]      = 0xF5B882,  // Sandy peach
    [PW_STATE_WORKING]   = 0xE8F0E8,
    [PW_STATE_WAITING]   = 0xE8E0F0,
    [PW_STATE_ALERT]     = 0x402020,
    [PW_STATE_GREETING]  = 0xFFF0C0,
    [PW_STATE_SLEEPING]  = 0x404060,
    [PW_STATE_REPORTING] = 0xFDE8C8,
    [PW_STATE_DOWN]      = 0x2A2A2A,  // Dark gray
    [PW_STATE_WAKEUP]    = 0x404060,  // Same as sleeping (waking from sleep)
};

// --- Background indices per state (matching dashboard) ---
// Disabled: SD card too slow for runtime loading. Re-enable when cached in PSRAM.
#if 0
#define BG_PER_STATE 12
static const uint8_t STATE_BGS[][BG_PER_STATE] = {
    [PW_STATE_IDLE]      = {2, 5, 10, 11, 15, 27, 42, 50, 70, 71, 75, 25},
    [PW_STATE_WORKING]   = {26, 30, 31, 37, 44, 46, 33, 81, 20, 41, 12, 59},
    [PW_STATE_WAITING]   = {28, 32, 36, 45, 54, 55, 66, 68, 79, 83, 6, 75},
    [PW_STATE_ALERT]     = {8, 13, 38, 39, 48, 53, 56, 58, 60, 61, 76, 29},
    [PW_STATE_GREETING]  = {1, 6, 17, 29, 40, 49, 51, 52, 64, 72, 74, 50},
    [PW_STATE_SLEEPING]  = {3, 4, 24, 33, 34, 35, 43, 47, 62, 63, 69, 77},
    [PW_STATE_REPORTING] = {7, 12, 16, 20, 41, 57, 59, 78, 80, 10, 46, 83},
};
#endif

static bool load_background_tile(int bg_idx)
{
    if (!s_bg_available) return false;

    // Load individual tile file: /sdcard/characters/zidane/bg/XX.raw
    char path[300];
    snprintf(path, sizeof(path), "%s/zidane/bg/%02d.raw", PW_SD_CHARACTER_DIR, bg_idx);

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open bg tile: %s", path);
        return false;
    }

    // Read header (4 bytes: width, height)
    uint16_t dims[2];
    fread(dims, 2, 2, f);
    uint16_t tw = dims[0], th = dims[1];

    // Read entire tile in one sequential read (~80KB)
    size_t tile_size = tw * th * 2;
    uint16_t *tile_buf = heap_caps_malloc(tile_size, MALLOC_CAP_SPIRAM);
    if (!tile_buf) { fclose(f); ESP_LOGE(TAG, "tile alloc failed"); return false; }
    fread(tile_buf, 2, tw * th, f);
    fclose(f);

    // Allocate display buffer (412*412*2 = 339488 bytes)
    size_t dst_size = PW_DISPLAY_WIDTH * PW_DISPLAY_HEIGHT * 2;
    if (!s_bg_buf) {
        s_bg_buf = heap_caps_malloc(dst_size, MALLOC_CAP_SPIRAM);
        if (!s_bg_buf) {
            heap_caps_free(tile_buf);
            ESP_LOGE(TAG, "bg display alloc failed");
            return false;
        }
    }

    // Scale tile to display via nearest-neighbor
    uint16_t *dst = (uint16_t *)s_bg_buf;
    for (int dy = 0; dy < PW_DISPLAY_HEIGHT; dy++) {
        int sy = dy * th / PW_DISPLAY_HEIGHT;
        for (int dx = 0; dx < PW_DISPLAY_WIDTH; dx++) {
            int sx = dx * tw / PW_DISPLAY_WIDTH;
            uint16_t px = tile_buf[sy * tw + sx];
            dst[dy * PW_DISPLAY_WIDTH + dx] = (px >> 8) | (px << 8);  // byte-swap for LV_COLOR_16_SWAP
        }
    }
    heap_caps_free(tile_buf);

    // Set up LVGL image descriptor
    s_bg_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_bg_dsc.header.always_zero = 0;
    s_bg_dsc.header.reserved = 0;
    s_bg_dsc.header.w = PW_DISPLAY_WIDTH;
    s_bg_dsc.header.h = PW_DISPLAY_HEIGHT;
    s_bg_dsc.data_size = dst_size;
    s_bg_dsc.data = s_bg_buf;

    return true;
}

// --- Behavior state machine (matches web UI) ---

typedef enum {
    BEHAV_IDLE,
    BEHAV_WALKING,
    BEHAV_PAUSING,
    BEHAV_TRANSITIONING,  // walking toward target position for state change
} behav_state_t;

typedef enum {
    DIR_DOWN = 0,
    DIR_UP,
    DIR_LEFT,
    DIR_RIGHT,
} facing_dir_t;

static const char *DIR_NAMES[] = { "down", "up", "left", "right" };
static const int DIR_VEC_X[] = { 0, 0, -1, 1 };
static const int DIR_VEC_Y[] = { 1, -1, 0, 0 };

typedef struct {
    uint16_t walk_chance;   // out of 1000
    uint16_t turn_chance;   // out of 1000
    uint8_t  walk_steps_min;
    uint8_t  walk_steps_max;
    uint8_t  speed_x10;    // speed * 10 (fixed point, e.g. 15 = 1.5)
    uint8_t  bounce_amp;
    uint8_t  pause_min;
    uint8_t  pause_max;
} mood_behavior_t;

// Agent state behavior parameters
static const mood_behavior_t STATE_BEHAVIORS[] = {
    [PW_STATE_IDLE]      = { .walk_chance = 60, .turn_chance = 20, .walk_steps_min = 6,  .walk_steps_max = 14, .speed_x10 = 15, .bounce_amp = 0, .pause_min = 20,  .pause_max = 40  },
    [PW_STATE_WORKING]   = { .walk_chance = 50, .turn_chance = 30, .walk_steps_min = 8,  .walk_steps_max = 16, .speed_x10 = 15, .bounce_amp = 0, .pause_min = 20,  .pause_max = 50  },
    [PW_STATE_WAITING]   = { .walk_chance = 0,  .turn_chance = 0,  .walk_steps_min = 0,  .walk_steps_max = 0,  .speed_x10 = 0,  .bounce_amp = 0, .pause_min = 100, .pause_max = 200 },
    [PW_STATE_ALERT]     = { .walk_chance = 0,  .turn_chance = 0,  .walk_steps_min = 0,  .walk_steps_max = 0,  .speed_x10 = 0,  .bounce_amp = 0, .pause_min = 100, .pause_max = 200 },
    [PW_STATE_GREETING]  = { .walk_chance = 0,  .turn_chance = 0,  .walk_steps_min = 0,  .walk_steps_max = 0,  .speed_x10 = 0,  .bounce_amp = 0, .pause_min = 100, .pause_max = 200 },
    [PW_STATE_SLEEPING]  = { .walk_chance = 0,  .turn_chance = 0,  .walk_steps_min = 0,  .walk_steps_max = 0,  .speed_x10 = 0,  .bounce_amp = 0, .pause_min = 100, .pause_max = 200 },
    [PW_STATE_REPORTING] = { .walk_chance = 0,  .turn_chance = 0,  .walk_steps_min = 0,  .walk_steps_max = 0,  .speed_x10 = 0,  .bounce_amp = 0, .pause_min = 100, .pause_max = 200 },
    [PW_STATE_DOWN]      = { .walk_chance = 0,  .turn_chance = 0,  .walk_steps_min = 0,  .walk_steps_max = 0,  .speed_x10 = 0,  .bounce_amp = 0, .pause_min = 100, .pause_max = 200 },
    [PW_STATE_WAKEUP]    = { .walk_chance = 0,  .turn_chance = 0,  .walk_steps_min = 0,  .walk_steps_max = 0,  .speed_x10 = 0,  .bounce_amp = 0, .pause_min = 100, .pause_max = 200 },
};

// Position in display pixels (fixed-point x10 for sub-pixel movement)
static int s_pos_x10 = 0;
static int s_pos_y10 = 0;
static facing_dir_t s_facing = DIR_DOWN;
static behav_state_t s_behav_state = BEHAV_IDLE;
static int s_state_timer = 0;
static int s_walk_steps_left = 0;
static int s_pause_target = 0;
static int s_frame_tick = 0;
static facing_dir_t s_last_walk_dir = DIR_DOWN;  // anti-reversal tracking

// Transition state — walk to target position before applying new state
static int s_target_x10 = 0;
static int s_target_y10 = 0;
static pw_agent_state_t s_transition_target_state = PW_STATE_IDLE;
static bool s_state_visuals_dirty = false;  // triggers ZZZ show/hide etc.

// Display sleep
static bool s_display_sleeping = false;
static int64_t s_last_activity_ms = 0;

// Wakeup animation queue — when display wakes from off, play wakeup first
static bool s_wakeup_playing = false;
static pw_agent_state_t s_wakeup_queued_state = PW_STATE_IDLE;
static bool s_wakeup_has_queued_state = false;

// Lock-free state change — just set flags, render loop picks them up
static volatile pw_agent_state_t s_pending_state = PW_STATE_IDLE;
static volatile bool s_state_changed = false;

// Display geometry
#define CENTER_X   (PW_DISPLAY_WIDTH / 2)
#define CENTER_Y   (PW_DISPLAY_HEIGHT / 2)
#define SPRITE_HALF (PW_SPRITE_DST_SIZE / 2)
// Circular boundary: keep sprite center within this radius of display center
#define MOVE_RADIUS (CENTER_X - SPRITE_HALF - 10)

static uint32_t s_rand_state = 0;

static uint32_t simple_rand(void)
{
    if (s_rand_state == 0) {
        s_rand_state = (uint32_t)esp_timer_get_time();
        if (s_rand_state == 0) s_rand_state = 12345;
    }
    s_rand_state ^= s_rand_state << 13;
    s_rand_state ^= s_rand_state >> 17;
    s_rand_state ^= s_rand_state << 5;
    return s_rand_state;
}

static int rand_range(int min, int max)
{
    if (min >= max) return min;
    return min + (int)(simple_rand() % (uint32_t)(max - min));
}

static bool rand_chance(uint16_t per_thousand)
{
    return (simple_rand() % 1000) < per_thousand;
}

static void clamp_pos(void)
{
    // Circular boundary: distance from center must be <= MOVE_RADIUS
    int dx = s_pos_x10 / 10 - CENTER_X;
    int dy = s_pos_y10 / 10 - CENTER_Y;
    int dist_sq = dx * dx + dy * dy;
    if (dist_sq > MOVE_RADIUS * MOVE_RADIUS && dist_sq > 0) {
        float dist = sqrtf((float)dist_sq);
        float scale = (float)MOVE_RADIUS / dist;
        s_pos_x10 = (int)((CENTER_X + dx * scale) * 10);
        s_pos_y10 = (int)((CENTER_Y + dy * scale) * 10);
    }
}

static bool at_boundary(void)
{
    int dx = s_pos_x10 / 10 - CENTER_X;
    int dy = s_pos_y10 / 10 - CENTER_Y;
    return (dx * dx + dy * dy) >= (MOVE_RADIUS - 5) * (MOVE_RADIUS - 5);
}

static void set_anim_by_name(const char *prefix, facing_dir_t dir)
{
    char name[PW_ANIM_NAME_LEN];
    snprintf(name, sizeof(name), "%s_%s", prefix, DIR_NAMES[dir]);
    const pw_animation_t *anim = pw_sprite_get_anim_by_name(&s_sprite, name);
    if (anim && anim != s_current_anim) {
        s_current_anim = anim;
        s_current_frame = 0;
    }
}

static const facing_dir_t OPPOSITE_DIR[] = {
    [DIR_DOWN] = DIR_UP, [DIR_UP] = DIR_DOWN,
    [DIR_LEFT] = DIR_RIGHT, [DIR_RIGHT] = DIR_LEFT,
};

static void pick_walk_dir(void)
{
    // Prefer walking toward center if far away
    int dx = CENTER_X - s_pos_x10 / 10;
    int dy = CENTER_Y - s_pos_y10 / 10;
    int dist_sq = dx * dx + dy * dy;

    if (dist_sq > 40 * 40 && rand_chance(600)) {
        // Walk toward center
        if (abs(dx) > abs(dy)) {
            s_facing = dx > 0 ? DIR_RIGHT : DIR_LEFT;
        } else {
            s_facing = dy > 0 ? DIR_DOWN : DIR_UP;
        }
    } else {
        // Pick random direction but never the opposite of last walk
        facing_dir_t opposite = OPPOSITE_DIR[s_last_walk_dir];
        facing_dir_t choice;
        do {
            choice = (facing_dir_t)(simple_rand() % 4);
        } while (choice == opposite);
        s_facing = choice;
    }
    s_last_walk_dir = s_facing;
}

static void behavior_tick(void)
{
    const mood_behavior_t *b = &STATE_BEHAVIORS[s_current_state];
    s_state_timer++;

    switch (s_behav_state) {
    case BEHAV_IDLE:
        set_anim_by_name("idle", s_facing);
        if (rand_chance(b->walk_chance)) {
            pick_walk_dir();
            s_walk_steps_left = rand_range(b->walk_steps_min, b->walk_steps_max);
            s_behav_state = BEHAV_WALKING;
            s_state_timer = 0;
        }
        break;

    case BEHAV_WALKING:
        set_anim_by_name("walk", s_facing);
        s_pos_x10 += DIR_VEC_X[s_facing] * b->speed_x10;
        s_pos_y10 += DIR_VEC_Y[s_facing] * b->speed_x10;
        clamp_pos();
        s_walk_steps_left--;

        if (s_walk_steps_left <= 0 || at_boundary()) {
            s_behav_state = BEHAV_PAUSING;
            s_state_timer = 0;
            s_pause_target = rand_range(b->pause_min, b->pause_max);
            set_anim_by_name("idle", s_facing);
        }
        break;

    case BEHAV_PAUSING:
        set_anim_by_name("idle", s_facing);
        if (s_state_timer >= s_pause_target) {
            s_behav_state = BEHAV_IDLE;
            s_state_timer = 0;
        }
        break;

    case BEHAV_TRANSITIONING: {
        // Walk toward target position, then apply state change
        int dx = s_target_x10 - s_pos_x10;
        int dy = s_target_y10 - s_pos_y10;
        int dist_sq = (dx / 10) * (dx / 10) + (dy / 10) * (dy / 10);

        if (dist_sq < 10 * 10) {
            // Close enough — snap to target and apply state
            s_pos_x10 = s_target_x10;
            s_pos_y10 = s_target_y10;
            s_current_state = s_transition_target_state;
            const pw_animation_t *state_anim = pw_sprite_get_state_anim(&s_sprite, s_current_state);
            if (state_anim) {
                s_current_anim = state_anim;
                s_current_frame = 0;
            }
            s_behav_state = BEHAV_IDLE;
            s_state_visuals_dirty = true;  // re-trigger ZZZ show/hide after transition
            ESP_LOGI(TAG, "Transition complete: %s", pw_agent_state_to_string(s_current_state));
        } else {
            // Pick facing direction toward target
            if (abs(dx) > abs(dy)) {
                s_facing = dx > 0 ? DIR_RIGHT : DIR_LEFT;
            } else {
                s_facing = dy > 0 ? DIR_DOWN : DIR_UP;
            }
            set_anim_by_name("walk", s_facing);

            // Move toward target at transition speed
            int speed = 20;  // slightly faster than normal walk
            if (abs(dx) > abs(dy)) {
                s_pos_x10 += (dx > 0 ? speed : -speed);
                // small y correction
                if (abs(dy) > 50) s_pos_y10 += (dy > 0 ? speed / 2 : -speed / 2);
            } else {
                s_pos_y10 += (dy > 0 ? speed : -speed);
                if (abs(dx) > 50) s_pos_x10 += (dx > 0 ? speed / 2 : -speed / 2);
            }
        }
        break;
    }
    }
}

static void display_sleep(void)
{
    if (!s_display_sleeping) {
        s_display_sleeping = true;
        bsp_lcd_brightness_set(0);
        ESP_LOGI(TAG, "Display sleeping (idle timeout)");
    }
}

static void display_wake(void)
{
    if (s_display_sleeping) {
        s_display_sleeping = false;
        bsp_lcd_brightness_set(80);
        ESP_LOGI(TAG, "Display waking up");
    }
    s_last_activity_ms = esp_timer_get_time() / 1000;
}

// Cached values from prepare_frame for use in commit_frame
static int s_next_screen_x = 0;
static int s_next_screen_y = 0;
static bool s_frame_ready = false;

// Phase 1: CPU work — behavior, sprite extraction, position calc. NO LVGL calls.
static int s_prepare_call_count = 0;
static void prepare_frame(void)
{
    s_prepare_call_count++;
    if (s_prepare_call_count <= 3) {
        ESP_LOGI(TAG, "prepare_frame #%d: anim=%p frames=%d sprite_data=%p",
            s_prepare_call_count, s_current_anim,
            s_current_anim ? s_current_anim->frame_count : -1,
            s_sprite.sprite_sheet_data);
    }
    if (!s_current_anim || s_current_anim->frame_count == 0) { s_frame_ready = false; return; }

    // States that stay centered — no wandering
    static const bool FIXED_STATES[] = {
        [PW_STATE_IDLE]      = false,
        [PW_STATE_WORKING]   = true,
        [PW_STATE_WAITING]   = true,
        [PW_STATE_ALERT]     = true,
        [PW_STATE_GREETING]  = true,
        [PW_STATE_SLEEPING]  = true,
        [PW_STATE_REPORTING] = true,
        [PW_STATE_DOWN]      = true,
        [PW_STATE_WAKEUP]    = true,
    };

    if (s_behav_state == BEHAV_TRANSITIONING) {
        // During transition, behavior_tick handles movement toward target
        behavior_tick();
    } else if (FIXED_STATES[s_current_state]) {
        s_pos_x10 = CENTER_X * 10;
        if (s_current_state == PW_STATE_SLEEPING || s_current_state == PW_STATE_DOWN || s_current_state == PW_STATE_WAKEUP) {
            s_pos_y10 = (CENTER_Y + 40) * 10;
        } else {
            s_pos_y10 = (PW_DISPLAY_HEIGHT - SPRITE_HALF - 20) * 10;
        }
    } else {
        behavior_tick();
    }

    const pw_frame_coord_t *coord = &s_current_anim->frames[s_current_frame];

    if (s_frame_buf) {
        heap_caps_free(s_frame_buf);
        s_frame_buf = NULL;
    }

    s_frame_buf = pw_sprite_extract_frame_scaled_ex(&s_sprite, coord, PW_SPRITE_SCALE, s_current_anim->mirror);
    if (!s_frame_buf) { s_frame_ready = false; return; }

    uint16_t src_w = coord->w > 0 ? coord->w : s_sprite.frame_width;
    uint16_t src_h = coord->h > 0 ? coord->h : s_sprite.frame_height;
    uint16_t scaled_w = src_w * PW_SPRITE_SCALE;
    uint16_t scaled_h = src_h * PW_SPRITE_SCALE;

    s_frame_dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    s_frame_dsc.header.always_zero = 0;
    s_frame_dsc.header.reserved = 0;
    s_frame_dsc.header.w = scaled_w;
    s_frame_dsc.header.h = scaled_h;
    s_frame_dsc.data_size = scaled_w * scaled_h * 3;
    s_frame_dsc.data = (const uint8_t *)s_frame_buf;

    const mood_behavior_t *b = &STATE_BEHAVIORS[s_current_state];
    int bounce_y = (b->bounce_amp > 0) ? (int)(sinf(s_frame_tick * 0.3f) * b->bounce_amp) : 0;

    s_next_screen_x = s_pos_x10 / 10 - scaled_w / 2;
    s_next_screen_y = s_pos_y10 / 10 - scaled_h / 2 + bounce_y;
    s_frame_ready = true;

    // Advance sprite frame
    s_frame_tick++;
    // Wakeup uses slower frame rate (every 6 ticks instead of 3, matching 400ms speed)
    int frame_divisor = (s_current_state == PW_STATE_WAKEUP) ? 6 : 3;
    if (s_frame_tick % frame_divisor == 0) {
        s_current_frame++;
        if (s_current_frame >= s_current_anim->frame_count) {
            s_current_frame = s_current_anim->loop ? 0 : s_current_anim->frame_count - 1;

            // Non-looping animation finished — check if wakeup completed
            if (!s_current_anim->loop && s_wakeup_playing) {
                s_wakeup_playing = false;
                ESP_LOGI(TAG, "Wakeup animation finished");

                if (s_wakeup_has_queued_state) {
                    // Re-queue the original state change
                    s_pending_state = s_wakeup_queued_state;
                    s_state_changed = true;
                    s_wakeup_has_queued_state = false;
                    ESP_LOGI(TAG, "Applying queued state: %s", pw_agent_state_to_string(s_wakeup_queued_state));
                } else {
                    // No queued state — go to idle
                    s_pending_state = PW_STATE_IDLE;
                    s_state_changed = true;
                }
            }
        }
    }
}

// Phase 2: LVGL widget updates only. Caller MUST hold lvgl_port_lock.
static int s_commit_call_count = 0;
static void commit_frame(void)
{
    s_commit_call_count++;
    if (s_commit_call_count <= 3) {
        ESP_LOGI(TAG, "commit_frame #%d: ready=%d sprite_img=%p frame_buf=%p pos=(%d,%d) w=%d h=%d",
            s_commit_call_count, s_frame_ready, s_sprite_img, s_frame_buf,
            s_next_screen_x, s_next_screen_y,
            s_frame_dsc.header.w, s_frame_dsc.header.h);
    }
    if (!s_frame_ready || !s_sprite_img) return;
    lv_img_cache_invalidate_src(&s_frame_dsc);
    lv_img_set_src(s_sprite_img, &s_frame_dsc);
    lv_obj_set_pos(s_sprite_img, s_next_screen_x, s_next_screen_y);
}

void pw_renderer_init(void)
{
    s_render_mutex = xSemaphoreCreateMutex();

    // IO expander must be initialized first (powers LCD via BSP_PWR_LCD)
    // This is called from app_main before pw_renderer_init

    // Use BSP to initialize LCD hardware + LVGL display driver
    lv_disp_t *disp = bsp_lvgl_init();
    if (!disp) {
        ESP_LOGE(TAG, "BSP LVGL init failed");
        return;
    }
    bsp_lcd_brightness_set(80);

    lvgl_port_lock(0);

    // Log what LVGL's default screen looks like before we create ours
    lv_obj_t *def_scr = lv_scr_act();
    lv_color_t def_bg = lv_obj_get_style_bg_color(def_scr, 0);
    lv_opa_t def_opa = lv_obj_get_style_bg_opa(def_scr, 0);
    ESP_LOGI(TAG, "Default screen bg: color=0x%04X opa=%d", lv_color_to16(def_bg), def_opa);

    // Instead of creating a new screen, use the active one directly
    // This avoids potential theme/default screen conflicts
    s_screen = lv_scr_act();

    uint32_t bg_hex = STATE_BG_COLORS[s_current_state];
    ESP_LOGI(TAG, "Setting bg color: 0x%06X for state %d", (unsigned int)bg_hex, s_current_state);

    lv_obj_set_style_bg_color(s_screen, lv_color_hex(bg_hex), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    // Verify it was set
    lv_color_t set_bg = lv_obj_get_style_bg_color(s_screen, 0);
    lv_opa_t set_opa = lv_obj_get_style_bg_opa(s_screen, 0);
    ESP_LOGI(TAG, "After set: bg color=0x%04X opa=%d", lv_color_to16(set_bg), set_opa);

    // Remove all children from default screen (theme may have added some)
    lv_obj_clean(s_screen);

    // Background image (behind sprite, hidden until loaded)
    s_bg_img = lv_img_create(s_screen);
    lv_obj_remove_style_all(s_bg_img);
    lv_obj_set_pos(s_bg_img, 0, 0);
    lv_obj_add_flag(s_bg_img, LV_OBJ_FLAG_HIDDEN);

    s_sprite_img = lv_img_create(s_screen);
    lv_obj_remove_style_all(s_sprite_img);
    lv_obj_set_pos(s_sprite_img, CENTER_X - SPRITE_HALF, CENTER_Y - SPRITE_HALF);

    // ZZZ overlay for sleeping state (hidden by default)
    s_zzz_label = lv_label_create(s_screen);
    lv_label_set_text(s_zzz_label, "z");
    lv_obj_set_style_text_color(s_zzz_label, lv_color_black(), 0);
    lv_obj_set_style_text_font(s_zzz_label, &lv_font_montserrat_28, 0);
    lv_obj_add_flag(s_zzz_label, LV_OBJ_FLAG_HIDDEN);

    ESP_LOGI(TAG, "Screen children: %u", (unsigned)lv_obj_get_child_cnt(s_screen));

    lvgl_port_unlock();

    // Initialize position to center
    s_pos_x10 = CENTER_X * 10;
    s_pos_y10 = CENTER_Y * 10;

    // Seed PRNG with tick count for variety
    simple_rand();

    // Start activity timer
    s_last_activity_ms = esp_timer_get_time() / 1000;

    // Initialize FF9 dialog box overlay
    lvgl_port_lock(0);
    pw_dialog_init(s_screen);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "Renderer initialized (%dx%d display)", PW_DISPLAY_WIDTH, PW_DISPLAY_HEIGHT);
}

bool pw_renderer_load_character(const char *character_id)
{
    xSemaphoreTake(s_render_mutex, portMAX_DELAY);

    display_wake();
    pw_sprite_free(&s_sprite);

    if (!pw_sprite_load(character_id, &s_sprite)) {
        ESP_LOGE(TAG, "Failed to load sprites for %s", character_id);
        xSemaphoreGive(s_render_mutex);
        return false;
    }

    // Start with idle_down, falling back to state anim
    s_facing = DIR_DOWN;
    s_current_anim = pw_sprite_get_anim_by_name(&s_sprite, "idle_down");
    if (!s_current_anim) {
        s_current_anim = pw_sprite_get_state_anim(&s_sprite, s_current_state);
    }
    s_current_frame = 0;
    s_behav_state = BEHAV_IDLE;
    s_state_timer = 0;
    // First frame will be rendered by the render loop

    // Check for background tiles directory (try loading tile 01 as a test)
    char bg_test[300];
    snprintf(bg_test, sizeof(bg_test), "%s/%s/bg/01.raw", PW_SD_CHARACTER_DIR, character_id);
    FILE *bf = fopen(bg_test, "rb");
    if (bf) {
        fclose(bf);
        s_bg_available = true;
        ESP_LOGI(TAG, "Background tiles found at %s/%s/bg/", PW_SD_CHARACTER_DIR, character_id);

        // Pre-load initial background before Himax starts
        if (load_background_tile(16)) {
            s_current_bg_idx = 16;
            lvgl_port_lock(0);
            if (s_bg_img) {
                lv_img_set_src(s_bg_img, &s_bg_dsc);
                lv_obj_clear_flag(s_bg_img, LV_OBJ_FLAG_HIDDEN);
            }
            lvgl_port_unlock();
            ESP_LOGI(TAG, "Initial background loaded: tile 01");
        }
    } else {
        s_bg_available = false;
        ESP_LOGW(TAG, "No background tiles found, using solid colors");
    }

    xSemaphoreGive(s_render_mutex);
    ESP_LOGI(TAG, "Loaded character: %s", character_id);
    return true;
}

// Lock-free background change
static volatile int s_pending_bg_idx = -1;
static volatile bool s_bg_change_requested = false;
static volatile bool s_wake_requested = false;

// Pending message
static char s_pending_msg[PW_DIALOG_MAX_TEXT] = "";
static volatile pw_msg_level_t s_pending_msg_level = PW_MSG_LEVEL_INFO;
static volatile bool s_msg_pending = false;

void pw_renderer_set_state(pw_agent_state_t state)
{
    s_pending_state = state;
    s_state_changed = true;
    s_wake_requested = true;
    ESP_LOGI(TAG, "State change queued: %s", pw_agent_state_to_string(state));
}

void pw_renderer_show_message(const char *text, pw_msg_level_t level)
{
    strncpy(s_pending_msg, text, PW_DIALOG_MAX_TEXT - 1);
    s_pending_msg[PW_DIALOG_MAX_TEXT - 1] = '\0';
    s_pending_msg_level = level;
    s_msg_pending = true;
    s_wake_requested = true;
}

void pw_renderer_wake_display(void)
{
    s_wake_requested = true;
}

void pw_renderer_set_background(int bg_idx)
{
    s_pending_bg_idx = bg_idx;
    s_bg_change_requested = true;
    s_wake_requested = true;
    ESP_LOGI(TAG, "Background change queued: %d", bg_idx);
}

int pw_renderer_get_background(void)
{
    return s_current_bg_idx;
}

static void renderer_task(void *arg)
{
    ESP_LOGI(TAG, "Renderer task started");

    TickType_t frame_delay = pdMS_TO_TICKS(1000 / PW_ANIM_FPS);

    while (1) {
        // --- Check knob button wake (display off + button press = wake) ---
        if (s_display_sleeping && pw_dialog_consume_btn_wake()) {
            s_wake_requested = true;  // feed into the standard wake flow below
            ESP_LOGI(TAG, "Knob button pressed while display off — waking");
        }

        // --- Process pending wake (no LVGL lock needed) ---
        // NOTE: s_wake_requested is consumed here — do NOT check it later in the loop
        bool woke_this_frame = false;
        bool woke_from_display_off = false;
        if (s_wake_requested) {
            s_wake_requested = false;
            woke_from_display_off = s_display_sleeping;  // capture before display_wake clears it
            woke_this_frame = true;
            display_wake();

            // If display was actually off, play wakeup animation before anything else
            if (woke_from_display_off) {
                // Queue whatever state change is pending — we'll apply it after wakeup
                if (s_state_changed) {
                    s_wakeup_queued_state = s_pending_state;
                    s_wakeup_has_queued_state = true;
                    s_state_changed = false;  // consume — will re-apply after wakeup
                }

                // Start wakeup animation at current position (sleeping/down pos)
                s_wakeup_playing = true;
                s_current_state = PW_STATE_WAKEUP;
                const pw_animation_t *wake_anim = pw_sprite_get_state_anim(&s_sprite, PW_STATE_WAKEUP);
                if (wake_anim) {
                    s_current_anim = wake_anim;
                    s_current_frame = 0;
                    s_frame_tick = 0;
                }
                s_behav_state = BEHAV_IDLE;
                s_state_visuals_dirty = true;
                ESP_LOGI(TAG, "Display woke from off — playing wakeup animation");
            }
        }

        // --- Process pending background change (file I/O outside LVGL lock) ---
        if (s_bg_change_requested) {
            s_bg_change_requested = false;
            int new_idx = s_pending_bg_idx;
            if (new_idx != s_current_bg_idx && load_background_tile(new_idx)) {
                s_current_bg_idx = new_idx;
                // lv_img_set_src is a small update — safe inside LVGL lock
                if (lvgl_port_lock(500)) {
                    if (s_bg_img) {
                        lv_img_set_src(s_bg_img, &s_bg_dsc);
                        lv_obj_clear_flag(s_bg_img, LV_OBJ_FLAG_HIDDEN);
                    }
                    lvgl_port_unlock();
                    ESP_LOGI(TAG, "Background changed to tile %d", new_idx);
                } else {
                    ESP_LOGW(TAG, "LVGL lock timeout during bg change — will retry");
                    s_bg_change_requested = true;  // retry next frame
                }
            }
        }

        // --- Process pending state change (no LVGL lock needed for behavior) ---
        if (s_state_changed) {
            s_state_changed = false;
            pw_agent_state_t new_state = s_pending_state;

            // Check if new state is fixed-position (needs transition walk)
            static const bool FIXED_POS_STATES[] = {
                [PW_STATE_IDLE] = false, [PW_STATE_WORKING] = true,
                [PW_STATE_WAITING] = true, [PW_STATE_ALERT] = true,
                [PW_STATE_GREETING] = true, [PW_STATE_SLEEPING] = true,
                [PW_STATE_REPORTING] = true, [PW_STATE_DOWN] = true,
                [PW_STATE_WAKEUP] = true,
            };

            // Calculate target position for new state
            int new_target_x10 = CENTER_X * 10;
            int new_target_y10;
            if (!FIXED_POS_STATES[new_state]) {
                // Idle: target is current position (no walk needed for idle)
                new_target_x10 = s_pos_x10;
                new_target_y10 = s_pos_y10;
            } else if (new_state == PW_STATE_SLEEPING || new_state == PW_STATE_DOWN || new_state == PW_STATE_WAKEUP) {
                new_target_y10 = (CENTER_Y + 40) * 10;
            } else {
                new_target_y10 = (PW_DISPLAY_HEIGHT - SPRITE_HALF - 20) * 10;
            }

            // Check if sprite needs to walk to the new position
            int dx = new_target_x10 - s_pos_x10;
            int dy = new_target_y10 - s_pos_y10;
            int dist_sq = (dx / 10) * (dx / 10) + (dy / 10) * (dy / 10);

            if (dist_sq > 15 * 15 && FIXED_POS_STATES[new_state]) {
                // Walk to target position before applying new state
                s_target_x10 = new_target_x10;
                s_target_y10 = new_target_y10;
                s_transition_target_state = new_state;
                s_behav_state = BEHAV_TRANSITIONING;
                s_state_timer = 0;
                ESP_LOGI(TAG, "Transitioning to %s (dist=%d)", pw_agent_state_to_string(new_state), (int)sqrtf((float)dist_sq));
            } else {
                // Close enough or going to idle — apply immediately
                s_current_state = new_state;
                s_behav_state = BEHAV_IDLE;
                s_state_timer = 0;

                if (FIXED_POS_STATES[new_state]) {
                    s_pos_x10 = new_target_x10;
                    s_pos_y10 = new_target_y10;
                }

                const pw_animation_t *state_anim = pw_sprite_get_state_anim(&s_sprite, s_current_state);
                if (state_anim) {
                    s_current_anim = state_anim;
                    s_current_frame = 0;
                } else {
                    set_anim_by_name("idle", s_facing);
                }
                ESP_LOGI(TAG, "State applied: %s", pw_agent_state_to_string(s_current_state));
            }

            s_state_visuals_dirty = true;
        }

        // Check pre-sleep and display sleep timeouts
        int64_t now_ms = esp_timer_get_time() / 1000;
        int64_t idle_ms = now_ms - s_last_activity_ms;

        // Trigger sleeping state before auto display off
        static bool s_pre_sleep_triggered = false;
        if (!s_display_sleeping && !s_pre_sleep_triggered &&
            idle_ms >= (PW_DISPLAY_SLEEP_TIMEOUT_MS - PW_SLEEP_STATE_BEFORE_MS) &&
            s_current_state != PW_STATE_SLEEPING) {
            s_pre_sleep_triggered = true;
            s_pending_state = PW_STATE_SLEEPING;
            s_state_changed = true;
            ESP_LOGI(TAG, "Pre-sleep: switching to sleeping state");
        }

        // Display off: either auto-timeout OR 15s after sleeping state was set (by API or auto)
        static int64_t s_sleeping_state_started_ms = 0;
        if (s_current_state == PW_STATE_SLEEPING && s_sleeping_state_started_ms == 0) {
            s_sleeping_state_started_ms = now_ms;
        }
        if (s_current_state != PW_STATE_SLEEPING) {
            s_sleeping_state_started_ms = 0;
        }

        bool auto_sleep = idle_ms >= PW_DISPLAY_SLEEP_TIMEOUT_MS;
        bool state_sleep = s_sleeping_state_started_ms > 0 &&
                           (now_ms - s_sleeping_state_started_ms) >= PW_SLEEP_AFTER_STATE_MS;

        if (!s_display_sleeping && (auto_sleep || state_sleep)) {
            display_sleep();
        }

        // Reset pre-sleep flag when activity resumes (use woke_this_frame since
        // s_wake_requested was consumed at top of loop)
        if (woke_this_frame) {
            s_pre_sleep_triggered = false;
            s_sleeping_state_started_ms = 0;
        }

        // --- Single LVGL lock for ALL display operations this frame ---
        if (!s_display_sleeping) {
            // Phase 1: CPU work outside LVGL lock (behavior, sprite extraction)
            prepare_frame();

            if (!s_frame_ready) {
                ESP_LOGW(TAG, "Frame not ready, skipping commit");
                vTaskDelay(frame_delay);
                continue;
            }

            // Log heap every 100 frames to detect leaks
            static int s_heap_log_counter = 0;
            if (++s_heap_log_counter >= 100) {
                s_heap_log_counter = 0;
                ESP_LOGI(TAG, "HEAP: free=%u largest=%u PSRAM_free=%u",
                    (unsigned)esp_get_free_heap_size(),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            }

            // Phase 2: single LVGL lock for ALL widget updates
            // Use 500ms timeout instead of wait-forever to recover from SPI flush stalls
            static int s_frame_counter = 0;
            s_frame_counter++;

            int64_t lock_start = esp_timer_get_time() / 1000;
            if (!lvgl_port_lock(500)) {
                ESP_LOGW(TAG, "LVGL lock TIMEOUT frame=%d (SPI flush stalled?)", s_frame_counter);
                vTaskDelay(frame_delay);
                continue;
            }
            int64_t lock_acquired = esp_timer_get_time() / 1000;
            if (lock_acquired - lock_start > 50) {
                ESP_LOGW(TAG, "LVGL lock SLOW: %dms frame=%d", (int)(lock_acquired - lock_start), s_frame_counter);
            }

            commit_frame();

            // State change: only update ZZZ overlay (no bg color change — backgrounds handled separately)
            if (s_state_visuals_dirty) {
                s_state_visuals_dirty = false;
                if (s_zzz_label) {
                    if (s_current_state == PW_STATE_SLEEPING) {
                        lv_obj_clear_flag(s_zzz_label, LV_OBJ_FLAG_HIDDEN);
                    } else {
                        lv_obj_add_flag(s_zzz_label, LV_OBJ_FLAG_HIDDEN);
                    }
                }
            }

            // Animate ZZZ text cycle and position (follows sprite)
            if (s_zzz_label && s_current_state == PW_STATE_SLEEPING) {
                static const char *ZZZ_FRAMES[] = { "z", "z Z", "z Z z", "z Z", "z" };
                static int s_zzz_tick = 0;
                int phase = (s_zzz_tick / 15) % 5;
                lv_label_set_text_static(s_zzz_label, ZZZ_FRAMES[phase]);
                // Position above and right of sprite's current position
                int sprite_y = s_pos_y10 / 10;
                lv_obj_set_pos(s_zzz_label, CENTER_X + 10, sprite_y - SPRITE_HALF - 30);
                s_zzz_tick++;
            }

            // Apply pending message
            if (s_msg_pending) {
                s_msg_pending = false;
                ESP_LOGI(TAG, ">>> DIALOG SHOW start frame=%d", s_frame_counter);
                pw_dialog_show(s_pending_msg, s_pending_msg_level);
                ESP_LOGI(TAG, ">>> DIALOG SHOW done frame=%d", s_frame_counter);
            }

            // Tick dialog auto-dismiss/typewriter
            pw_dialog_tick();

            int64_t before_unlock = esp_timer_get_time() / 1000;
            lvgl_port_unlock();
            int64_t after_unlock = esp_timer_get_time() / 1000;

            // Log if unlock (which triggers SPI flush) takes too long
            int64_t lock_held_ms = before_unlock - lock_acquired;
            int64_t unlock_ms = after_unlock - before_unlock;
            if (lock_held_ms > 20 || unlock_ms > 50) {
                ESP_LOGW(TAG, "FRAME SLOW: frame=%d lock_held=%dms unlock=%dms dialog=%d typing=%d",
                    s_frame_counter, (int)lock_held_ms, (int)unlock_ms,
                    pw_dialog_is_visible(), s_msg_pending);
            }

            // Periodic alive log
            if (s_frame_counter % 100 == 0) {
                ESP_LOGI(TAG, "ALIVE frame=%d state=%s dialog=%d",
                    s_frame_counter, pw_agent_state_to_string(s_current_state),
                    pw_dialog_is_visible());
            }
        }

        vTaskDelay(s_display_sleeping ? pdMS_TO_TICKS(1000) : frame_delay);
    }
}

void pw_renderer_task_start(void)
{
    xTaskCreate(renderer_task, "renderer", 12288, NULL, 4, NULL);
}
