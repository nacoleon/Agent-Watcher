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

// Background state
static uint8_t *s_bg_buf = NULL;
static lv_img_dsc_t s_bg_dsc = {};
static bool s_bg_available = false;
static char s_bg_path[300] = "";
static bool s_bg_needs_load = false;
static int s_bg_pending_idx = -1;

static pw_sprite_data_t s_sprite = {};
static pw_agent_state_t s_current_state = PW_STATE_IDLE;
static const pw_animation_t *s_current_anim = NULL;
static int s_current_frame = 0;
static uint8_t *s_frame_buf = NULL;
static lv_img_dsc_t s_frame_dsc = {};

// --- Background colors per agent state ---
static const uint32_t STATE_BG_COLORS[] = {
    [PW_STATE_IDLE]      = 0xFDE8C8,
    [PW_STATE_WORKING]   = 0xE8F0E8,
    [PW_STATE_WAITING]   = 0xE8E0F0,
    [PW_STATE_ALERT]     = 0x402020,
    [PW_STATE_GREETING]  = 0xFFF0C0,
    [PW_STATE_SLEEPING]  = 0x404060,
    [PW_STATE_REPORTING] = 0xFDE8C8,
};

// --- Background indices per state (matching dashboard) ---
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
            dst[dy * PW_DISPLAY_WIDTH + dx] = tile_buf[sy * tw + sx];
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
    [PW_STATE_IDLE]      = { .walk_chance = 30, .turn_chance = 20, .walk_steps_min = 6,  .walk_steps_max = 14, .speed_x10 = 15, .bounce_amp = 0, .pause_min = 30,  .pause_max = 80  },
    [PW_STATE_WORKING]   = { .walk_chance = 50, .turn_chance = 30, .walk_steps_min = 8,  .walk_steps_max = 16, .speed_x10 = 15, .bounce_amp = 0, .pause_min = 20,  .pause_max = 50  },
    [PW_STATE_WAITING]   = { .walk_chance = 5,  .turn_chance = 15, .walk_steps_min = 2,  .walk_steps_max = 5,  .speed_x10 = 8,  .bounce_amp = 0, .pause_min = 80,  .pause_max = 200 },
    [PW_STATE_ALERT]     = { .walk_chance = 80, .turn_chance = 50, .walk_steps_min = 10, .walk_steps_max = 25, .speed_x10 = 30, .bounce_amp = 4, .pause_min = 5,   .pause_max = 15  },
    [PW_STATE_GREETING]  = { .walk_chance = 60, .turn_chance = 40, .walk_steps_min = 8,  .walk_steps_max = 20, .speed_x10 = 25, .bounce_amp = 3, .pause_min = 10,  .pause_max = 30  },
    [PW_STATE_SLEEPING]  = { .walk_chance = 3,  .turn_chance = 5,  .walk_steps_min = 2,  .walk_steps_max = 5,  .speed_x10 = 5,  .bounce_amp = 0, .pause_min = 100, .pause_max = 250 },
    [PW_STATE_REPORTING] = { .walk_chance = 10, .turn_chance = 10, .walk_steps_min = 3,  .walk_steps_max = 6,  .speed_x10 = 10, .bounce_amp = 0, .pause_min = 50,  .pause_max = 100 },
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

// Display sleep
static bool s_display_sleeping = false;
static int64_t s_last_activity_ms = 0;

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
    const mood_behavior_t *b = &STATE_BEHAVIORS[s_current_state];
    s_state_timer++;

    switch (s_behav_state) {
    case BEHAV_IDLE:
        set_anim_by_name("idle", s_facing);
        if (rand_chance(b->turn_chance)) {
            s_facing = (facing_dir_t)(simple_rand() % 4);
            set_anim_by_name("idle", s_facing);
            s_behav_state = BEHAV_PAUSING;
            s_state_timer = 0;
            s_pause_target = rand_range(b->pause_min, b->pause_max);
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

    // Use actual sprite dimensions, not hardcoded PW_SPRITE_DST_SIZE
    uint16_t scaled_w = s_sprite.frame_width * PW_SPRITE_SCALE;
    uint16_t scaled_h = s_sprite.frame_height * PW_SPRITE_SCALE;

    s_frame_dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
    s_frame_dsc.header.always_zero = 0;
    s_frame_dsc.header.reserved = 0;
    s_frame_dsc.header.w = scaled_w;
    s_frame_dsc.header.h = scaled_h;
    s_frame_dsc.data_size = scaled_w * scaled_h * 3;
    s_frame_dsc.data = (const uint8_t *)s_frame_buf;

    // Bounce effect
    const mood_behavior_t *b = &STATE_BEHAVIORS[s_current_state];
    int bounce_y = 0;
    if (b->bounce_amp > 0) {
        bounce_y = (int)(sinf(s_frame_tick * 0.3f) * b->bounce_amp);
    }

    // Position sprite on screen (use actual scaled dimensions)
    int screen_x = s_pos_x10 / 10 - scaled_w / 2;
    int screen_y = s_pos_y10 / 10 - scaled_h / 2 + bounce_y;

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
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(STATE_BG_COLORS[s_current_state]), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_screen, PW_DISPLAY_WIDTH / 2, 0);
    lv_obj_set_style_clip_corner(s_screen, true, 0);

    // Background image (behind sprite, full screen)
    s_bg_img = lv_img_create(s_screen);
    lv_obj_remove_style_all(s_bg_img);
    lv_obj_set_pos(s_bg_img, 0, 0);
    lv_obj_add_flag(s_bg_img, LV_OBJ_FLAG_HIDDEN);  // hidden until loaded

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
    update_frame();

    // Check for background tiles directory (try loading tile 01 as a test)
    char bg_test[300];
    snprintf(bg_test, sizeof(bg_test), "%s/%s/bg/01.raw", PW_SD_CHARACTER_DIR, character_id);
    FILE *bf = fopen(bg_test, "rb");
    if (bf) {
        fclose(bf);
        s_bg_available = true;
        ESP_LOGI(TAG, "Background tiles found at %s/%s/bg/", PW_SD_CHARACTER_DIR, character_id);
    } else {
        s_bg_available = false;
        ESP_LOGW(TAG, "No background tiles found, using solid colors");
    }

    xSemaphoreGive(s_render_mutex);
    ESP_LOGI(TAG, "Loaded character: %s", character_id);
    return true;
}

void pw_renderer_set_state(pw_agent_state_t state)
{
    xSemaphoreTake(s_render_mutex, portMAX_DELAY);

    display_wake();
    s_current_state = state;

    // Update background — schedule load for render loop (non-blocking)
    if (s_screen) {
        lvgl_port_lock(0);
        // Show solid color while background image loads
        lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s_screen, lv_color_hex(STATE_BG_COLORS[state]), 0);
        lv_obj_add_flag(s_bg_img, LV_OBJ_FLAG_HIDDEN);
        lvgl_port_unlock();

        if (s_bg_available && state < PW_STATE_COUNT) {
            s_bg_pending_idx = STATE_BGS[state][simple_rand() % BG_PER_STATE];
            s_bg_needs_load = true;
        }
    }

    // Reset to idle behavior
    s_behav_state = BEHAV_IDLE;
    s_state_timer = 0;
    set_anim_by_name("idle", s_facing);

    xSemaphoreGive(s_render_mutex);
    ESP_LOGI(TAG, "State visual set to: %s", pw_agent_state_to_string(state));
}

void pw_renderer_show_message(const char *text, pw_msg_level_t level)
{
    xSemaphoreTake(s_render_mutex, portMAX_DELAY);
    lvgl_port_lock(0);
    pw_dialog_show(text, level);
    lvgl_port_unlock();
    xSemaphoreGive(s_render_mutex);
}

void pw_renderer_wake_display(void)
{
    xSemaphoreTake(s_render_mutex, portMAX_DELAY);
    display_wake();
    xSemaphoreGive(s_render_mutex);
}

static void renderer_task(void *arg)
{
    ESP_LOGI(TAG, "Renderer task started");

    TickType_t frame_delay = pdMS_TO_TICKS(1000 / PW_ANIM_FPS);

    while (1) {
        // Load background outside mutex (slow SD read, don't block API/display)
        if (s_bg_needs_load && s_bg_pending_idx >= 0) {
            s_bg_needs_load = false;
            if (load_background_tile(s_bg_pending_idx)) {
                xSemaphoreTake(s_render_mutex, portMAX_DELAY);
                lvgl_port_lock(0);
                lv_img_cache_invalidate_src(&s_bg_dsc);
                lv_img_set_src(s_bg_img, &s_bg_dsc);
                lv_obj_clear_flag(s_bg_img, LV_OBJ_FLAG_HIDDEN);
                // Make screen bg transparent so the image shows
                lv_obj_set_style_bg_opa(s_screen, LV_OPA_TRANSP, 0);
                lvgl_port_unlock();
                xSemaphoreGive(s_render_mutex);
                ESP_LOGI(TAG, "Background loaded: tile %d", s_bg_pending_idx);
            }
            s_bg_pending_idx = -1;
        }

        xSemaphoreTake(s_render_mutex, portMAX_DELAY);

        // Check display sleep timeout
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (!s_display_sleeping && (now_ms - s_last_activity_ms) >= PW_DISPLAY_SLEEP_TIMEOUT_MS) {
            display_sleep();
        }

        if (!s_display_sleeping) {
            update_frame();
            // Tick dialog auto-dismiss
            lvgl_port_lock(0);
            pw_dialog_tick();
            lvgl_port_unlock();
        }

        xSemaphoreGive(s_render_mutex);
        vTaskDelay(s_display_sleeping ? pdMS_TO_TICKS(1000) : frame_delay);
    }
}

void pw_renderer_task_start(void)
{
    xTaskCreate(renderer_task, "renderer", 12288, NULL, 4, NULL);
}
