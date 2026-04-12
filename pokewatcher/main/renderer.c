#include "renderer.h"
#include "config.h"
#include "sprite_loader.h"
#include "mood_engine.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "pw_renderer";

static SemaphoreHandle_t s_render_mutex = NULL;

static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_sprite_img = NULL;

static pw_sprite_data_t s_sprite = {};
static pw_mood_t s_current_mood = PW_MOOD_SLEEPY;
static const pw_animation_t *s_current_anim = NULL;
static int s_current_frame = 0;
static uint16_t *s_frame_buf = NULL;
static lv_img_dsc_t s_frame_dsc = {};

static const uint32_t MOOD_BG_COLORS[] = {
    [PW_MOOD_EXCITED]   = 0xFDE8C8,
    [PW_MOOD_HAPPY]     = 0xFDE8C8,
    [PW_MOOD_CURIOUS]   = 0xE8F0E8,
    [PW_MOOD_LONELY]    = 0xC8D8F0,
    [PW_MOOD_SLEEPY]    = 0x404060,
    [PW_MOOD_OVERJOYED] = 0xFFF0C0,
};

static void update_frame(void)
{
    if (!s_current_anim || s_current_anim->frame_count == 0) return;

    const pw_frame_coord_t *coord = &s_current_anim->frames[s_current_frame];

    if (s_frame_buf) {
        heap_caps_free(s_frame_buf);
        s_frame_buf = NULL;
    }

    s_frame_buf = pw_sprite_extract_frame_scaled(&s_sprite, coord, PW_SPRITE_SCALE);
    if (!s_frame_buf) return;

    s_frame_dsc.header.w = PW_SPRITE_DST_SIZE;
    s_frame_dsc.header.h = PW_SPRITE_DST_SIZE;
    s_frame_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_frame_dsc.data_size = PW_SPRITE_DST_SIZE * PW_SPRITE_DST_SIZE * sizeof(uint16_t);
    s_frame_dsc.data = (const uint8_t *)s_frame_buf;

    if (s_sprite_img) {
        lvgl_port_lock(0);
        lv_img_set_src(s_sprite_img, &s_frame_dsc);
        lvgl_port_unlock();
    }

    s_current_frame++;
    if (s_current_frame >= s_current_anim->frame_count) {
        if (s_current_anim->loop) {
            s_current_frame = 0;
        } else {
            s_current_frame = s_current_anim->frame_count - 1;
        }
    }
}

void pw_renderer_init(void)
{
    s_render_mutex = xSemaphoreCreateMutex();

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&lvgl_cfg);

    lvgl_port_lock(0);

    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(MOOD_BG_COLORS[PW_MOOD_SLEEPY]), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_screen, PW_DISPLAY_WIDTH / 2, 0);
    lv_obj_set_style_clip_corner(s_screen, true, 0);

    s_sprite_img = lv_img_create(s_screen);
    lv_obj_align(s_sprite_img, LV_ALIGN_CENTER, 0, 40);

    lv_scr_load(s_screen);

    lvgl_port_unlock();

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

    s_current_anim = pw_sprite_get_mood_anim(&s_sprite, s_current_mood);
    s_current_frame = 0;
    update_frame();

    xSemaphoreGive(s_render_mutex);
    ESP_LOGI(TAG, "Loaded Pokemon: %s", pokemon_id);
    return true;
}

void pw_renderer_set_mood(pw_mood_t mood)
{
    xSemaphoreTake(s_render_mutex, portMAX_DELAY);

    s_current_mood = mood;

    const pw_animation_t *new_anim = pw_sprite_get_mood_anim(&s_sprite, mood);
    if (new_anim && new_anim != s_current_anim) {
        s_current_anim = new_anim;
        s_current_frame = 0;
    }

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
