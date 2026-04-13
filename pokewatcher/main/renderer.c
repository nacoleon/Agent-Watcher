#include "renderer.h"
#include "config.h"
#include "sprite_loader.h"
#include "mood_engine.h"
#include "sensecap-watcher.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <math.h>
#include <stdlib.h>

static const char *TAG = "pw_renderer";

static SemaphoreHandle_t s_render_mutex = NULL;

static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_sprite_img = NULL;

static pw_sprite_data_t s_sprite = {};
static pw_mood_t s_current_mood = PW_MOOD_EXCITED;  // TEMP: match mood_engine for testing
static const pw_animation_t *s_current_anim = NULL;
static int s_current_frame = 0;
static uint8_t *s_frame_buf = NULL;
static lv_img_dsc_t s_frame_dsc = {};

// --- Background colors per mood ---
static const uint32_t MOOD_BG_COLORS[] = {
    [PW_MOOD_EXCITED]   = 0xFDE8C8,
    [PW_MOOD_HAPPY]     = 0xFDE8C8,
    [PW_MOOD_CURIOUS]   = 0xE8F0E8,
    [PW_MOOD_LONELY]    = 0xC8D8F0,
    [PW_MOOD_SLEEPY]    = 0x404060,
    [PW_MOOD_OVERJOYED] = 0xFFF0C0,
};

// --- Behavior state machine (matches web UI) ---

typedef enum {
    BEHAV_IDLE,
    BEHAV_WALKING,
    BEHAV_PAUSING,
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

// Matches the JS MOOD_BEHAVIOR table — chances scaled to per-1000
static const mood_behavior_t MOOD_BEHAVIORS[] = {
    [PW_MOOD_EXCITED]   = { .walk_chance = 60, .turn_chance = 40, .walk_steps_min = 8,  .walk_steps_max = 20, .speed_x10 = 25, .bounce_amp = 3, .pause_min = 10,  .pause_max = 30  },
    [PW_MOOD_HAPPY]     = { .walk_chance = 30, .turn_chance = 20, .walk_steps_min = 6,  .walk_steps_max = 14, .speed_x10 = 15, .bounce_amp = 0, .pause_min = 30,  .pause_max = 80  },
    [PW_MOOD_CURIOUS]   = { .walk_chance = 20, .turn_chance = 60, .walk_steps_min = 4,  .walk_steps_max = 10, .speed_x10 = 10, .bounce_amp = 0, .pause_min = 20,  .pause_max = 50  },
    [PW_MOOD_LONELY]    = { .walk_chance = 10, .turn_chance = 10, .walk_steps_min = 3,  .walk_steps_max = 8,  .speed_x10 = 8,  .bounce_amp = 0, .pause_min = 60,  .pause_max = 150 },
    [PW_MOOD_SLEEPY]    = { .walk_chance = 3,  .turn_chance = 5,  .walk_steps_min = 2,  .walk_steps_max = 5,  .speed_x10 = 5,  .bounce_amp = 0, .pause_min = 100, .pause_max = 250 },
    [PW_MOOD_OVERJOYED] = { .walk_chance = 80, .turn_chance = 50, .walk_steps_min = 10, .walk_steps_max = 25, .speed_x10 = 30, .bounce_amp = 4, .pause_min = 5,   .pause_max = 15  },
};

// Position in display pixels (fixed-point x10 for sub-pixel movement)
static int s_pos_x10 = 0;
static int s_pos_y10 = 0;
static facing_dir_t s_facing = DIR_DOWN;
static behav_state_t s_behav_state = BEHAV_IDLE;
static int s_state_timer = 0;
static int s_walk_steps_left = 0;
static int s_frame_tick = 0;

// Display geometry
#define CENTER_X   (PW_DISPLAY_WIDTH / 2)
#define CENTER_Y   (PW_DISPLAY_HEIGHT / 2)
#define SPRITE_HALF (PW_SPRITE_DST_SIZE / 2)
// Circular boundary: keep sprite center within this radius of display center
#define MOVE_RADIUS (CENTER_X - SPRITE_HALF - 10)

static uint32_t simple_rand(void)
{
    // xorshift32 — lightweight PRNG for behavior decisions
    static uint32_t state = 12345;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
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
        s_facing = (facing_dir_t)(simple_rand() % 4);
    }
}

static void behavior_tick(void)
{
    const mood_behavior_t *b = &MOOD_BEHAVIORS[s_current_mood];
    s_state_timer++;

    switch (s_behav_state) {
    case BEHAV_IDLE:
        set_anim_by_name("idle", s_facing);
        if (rand_chance(b->turn_chance)) {
            s_facing = (facing_dir_t)(simple_rand() % 4);
            set_anim_by_name("idle", s_facing);
            s_behav_state = BEHAV_PAUSING;
            s_state_timer = 0;
        } else if (rand_chance(b->walk_chance)) {
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
            set_anim_by_name("idle", s_facing);
        }
        break;

    case BEHAV_PAUSING:
        set_anim_by_name("idle", s_facing);
        if (s_state_timer > rand_range(b->pause_min, b->pause_max)) {
            s_behav_state = BEHAV_IDLE;
            s_state_timer = 0;
        }
        break;
    }
}

static void update_frame(void)
{
    if (!s_current_anim || s_current_anim->frame_count == 0) return;

    // Run behavior state machine
    behavior_tick();

    const pw_frame_coord_t *coord = &s_current_anim->frames[s_current_frame];

    if (s_frame_buf) {
        heap_caps_free(s_frame_buf);
        s_frame_buf = NULL;
    }

    s_frame_buf = pw_sprite_extract_frame_scaled(&s_sprite, coord, PW_SPRITE_SCALE);
    if (!s_frame_buf) return;

    s_frame_dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    s_frame_dsc.header.always_zero = 0;
    s_frame_dsc.header.reserved = 0;
    s_frame_dsc.header.w = PW_SPRITE_DST_SIZE;
    s_frame_dsc.header.h = PW_SPRITE_DST_SIZE;
    s_frame_dsc.data_size = PW_SPRITE_DST_SIZE * PW_SPRITE_DST_SIZE * 3;
    s_frame_dsc.data = (const uint8_t *)s_frame_buf;

    // Bounce effect for excited/overjoyed
    const mood_behavior_t *b = &MOOD_BEHAVIORS[s_current_mood];
    int bounce_y = 0;
    if (b->bounce_amp > 0) {
        bounce_y = (int)(sinf(s_frame_tick * 0.3f) * b->bounce_amp);
    }

    // Position sprite on screen
    int screen_x = s_pos_x10 / 10 - SPRITE_HALF;
    int screen_y = s_pos_y10 / 10 - SPRITE_HALF + bounce_y;

    if (s_sprite_img) {
        lvgl_port_lock(0);
        lv_img_cache_invalidate_src(&s_frame_dsc);
        lv_img_set_src(s_sprite_img, &s_frame_dsc);
        lv_obj_set_pos(s_sprite_img, screen_x, screen_y);
        lvgl_port_unlock();
    }

    // Advance sprite frame — slow cycle (every 3 ticks at 10 FPS = 300ms per frame)
    s_frame_tick++;
    if (s_frame_tick % 3 == 0) {
        s_current_frame++;
        if (s_current_frame >= s_current_anim->frame_count) {
            if (s_current_anim->loop) {
                s_current_frame = 0;
            } else {
                s_current_frame = s_current_anim->frame_count - 1;
            }
        }
    }
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

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(MOOD_BG_COLORS[s_current_mood]), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_screen, PW_DISPLAY_WIDTH / 2, 0);
    lv_obj_set_style_clip_corner(s_screen, true, 0);

    s_sprite_img = lv_img_create(s_screen);
    lv_obj_remove_style_all(s_sprite_img);  // Strip default theme bg/border
    // Start centered — position will be updated by behavior_tick
    lv_obj_set_pos(s_sprite_img, CENTER_X - SPRITE_HALF, CENTER_Y - SPRITE_HALF);

    lv_scr_load(s_screen);

    lvgl_port_unlock();

    // Initialize position to center
    s_pos_x10 = CENTER_X * 10;
    s_pos_y10 = CENTER_Y * 10;

    // Seed PRNG with tick count for variety
    simple_rand();

    ESP_LOGI(TAG, "Renderer initialized (%dx%d display)", PW_DISPLAY_WIDTH, PW_DISPLAY_HEIGHT);
}

bool pw_renderer_load_pokemon(const char *pokemon_id)
{
    xSemaphoreTake(s_render_mutex, portMAX_DELAY);

    pw_sprite_free(&s_sprite);

    if (!pw_sprite_load(pokemon_id, &s_sprite)) {
        ESP_LOGE(TAG, "Failed to load sprites for %s", pokemon_id);
        xSemaphoreGive(s_render_mutex);
        return false;
    }

    // Start with idle_down, falling back to mood anim
    s_facing = DIR_DOWN;
    s_current_anim = pw_sprite_get_anim_by_name(&s_sprite, "idle_down");
    if (!s_current_anim) {
        s_current_anim = pw_sprite_get_mood_anim(&s_sprite, s_current_mood);
    }
    s_current_frame = 0;
    s_behav_state = BEHAV_IDLE;
    s_state_timer = 0;
    update_frame();

    xSemaphoreGive(s_render_mutex);
    ESP_LOGI(TAG, "Loaded Pokemon: %s", pokemon_id);
    return true;
}

void pw_renderer_set_mood(pw_mood_t mood)
{
    xSemaphoreTake(s_render_mutex, portMAX_DELAY);

    s_current_mood = mood;

    // Reset to idle in current facing direction
    s_behav_state = BEHAV_IDLE;
    s_state_timer = 0;
    set_anim_by_name("idle", s_facing);

    lvgl_port_lock(0);
    if (s_screen) {
        lv_obj_set_style_bg_color(s_screen, lv_color_hex(MOOD_BG_COLORS[mood]), 0);
    }
    lvgl_port_unlock();

    xSemaphoreGive(s_render_mutex);
    ESP_LOGI(TAG, "Mood visual set to: %s", pw_mood_to_string(mood));
}

void pw_renderer_play_evolution(const char *new_pokemon_id)
{
    ESP_LOGI(TAG, "Playing evolution animation...");

    xSemaphoreTake(s_render_mutex, portMAX_DELAY);

    lvgl_port_lock(0);
    if (s_screen) {
        lv_obj_set_style_bg_color(s_screen, lv_color_white(), 0);
    }
    lvgl_port_unlock();

    xSemaphoreGive(s_render_mutex);

    vTaskDelay(pdMS_TO_TICKS(1500));

    pw_renderer_load_pokemon(new_pokemon_id);

    // Re-center after evolution
    s_pos_x10 = CENTER_X * 10;
    s_pos_y10 = CENTER_Y * 10;

    xSemaphoreTake(s_render_mutex, portMAX_DELAY);
    lvgl_port_lock(0);
    if (s_screen) {
        lv_obj_set_style_bg_color(s_screen, lv_color_hex(MOOD_BG_COLORS[s_current_mood]), 0);
    }
    lvgl_port_unlock();
    xSemaphoreGive(s_render_mutex);
}

static void renderer_task(void *arg)
{
    ESP_LOGI(TAG, "Renderer task started");

    TickType_t frame_delay = pdMS_TO_TICKS(1000 / PW_ANIM_FPS);

    while (1) {
        xSemaphoreTake(s_render_mutex, portMAX_DELAY);
        update_frame();
        xSemaphoreGive(s_render_mutex);
        vTaskDelay(frame_delay);
    }
}

void pw_renderer_task_start(void)
{
    xTaskCreate(renderer_task, "renderer", 8192, NULL, 4, NULL);
}
