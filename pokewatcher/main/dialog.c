#include "dialog.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "pw_dialog";

static lv_obj_t *s_dialog_container = NULL;
static lv_obj_t *s_dialog_label = NULL;
static lv_obj_t *s_name_label = NULL;
static bool s_visible = false;
static int64_t s_show_time_ms = 0;
static char s_last_text[PW_DIALOG_MAX_TEXT] = "";

// Typewriter effect state
static char s_full_text[PW_DIALOG_MAX_TEXT] = "";
static int s_full_text_len = 0;
static int s_revealed_chars = 0;
static bool s_typing = false;
#define TYPEWRITER_CHARS_PER_TICK  3  // chars per frame at 10 FPS = 30 chars/sec

// Staged height reveal — grow container gradually to avoid large SPI dirty regions
// SPI stalls on dirty regions > ~288x70, so we grow in small increments
#define DIALOG_MIN_HEIGHT    30   // just "Zidane:" label
#define DIALOG_MAX_HEIGHT    120  // max for ~5 lines
#define DIALOG_HEIGHT_STEP   10   // pixels per frame growth
static int s_current_height = DIALOG_MIN_HEIGHT;
static int s_target_height = DIALOG_MIN_HEIGHT;
static bool s_growing = false;

// Border colors per message level
static const uint32_t LEVEL_COLOR_HEX[] = {
    [PW_MSG_LEVEL_INFO]    = 0x5566AA,
    [PW_MSG_LEVEL_SUCCESS] = 0x55AA66,
    [PW_MSG_LEVEL_WARNING] = 0xAAAA55,
    [PW_MSG_LEVEL_ERROR]   = 0xAA5555,
};

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

// Estimate target height from text length (Montserrat 14 at 260px width ≈ 20 chars/line, 18px/line)
static int estimate_height(int text_len)
{
    int lines = (text_len + 19) / 20;  // ~20 chars per line at Montserrat 14
    if (lines < 1) lines = 1;
    int h = 28 + lines * 18;  // 28px for "Zidane:" header + padding, 18px per text line
    if (h < DIALOG_MIN_HEIGHT) h = DIALOG_MIN_HEIGHT;
    if (h > DIALOG_MAX_HEIGHT) h = DIALOG_MAX_HEIGHT;
    return h;
}

void pw_dialog_init(lv_obj_t *parent)
{
    s_dialog_container = lv_obj_create(parent);
    // Start at min height — will grow when message arrives
    lv_obj_set_size(s_dialog_container, 288, DIALOG_MIN_HEIGHT);
    lv_obj_align(s_dialog_container, LV_ALIGN_TOP_MID, 0, 74);
    lv_obj_set_style_pad_all(s_dialog_container, 8, 0);
    lv_obj_set_style_radius(s_dialog_container, 8, 0);

    lv_obj_set_style_bg_color(s_dialog_container, lv_color_hex(0x0A0A2E), 0);
    lv_obj_set_style_bg_opa(s_dialog_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_dialog_container, lv_color_hex(0x5566AA), 0);
    lv_obj_set_style_border_width(s_dialog_container, 2, 0);
    lv_obj_clear_flag(s_dialog_container, LV_OBJ_FLAG_SCROLLABLE);

    // Clip children to container bounds — text beyond current height is hidden
    lv_obj_set_style_clip_corner(s_dialog_container, true, 0);

    // Name label: "Zidane:" in green
    s_name_label = lv_label_create(s_dialog_container);
    lv_label_set_text(s_name_label, "Zidane:");
    lv_obj_set_style_text_color(s_name_label, lv_color_hex(0x7BE87B), 0);
    lv_obj_set_style_text_font(s_name_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_name_label, LV_ALIGN_TOP_LEFT, 4, 0);

    // Message text label
    s_dialog_label = lv_label_create(s_dialog_container);
    lv_label_set_text(s_dialog_label, "");
    lv_obj_set_style_text_color(s_dialog_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_dialog_label, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_dialog_label, 260);
    lv_label_set_long_mode(s_dialog_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_dialog_label, LV_ALIGN_TOP_LEFT, 4, 20);

    lv_obj_add_flag(s_dialog_container, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
    s_current_height = DIALOG_MIN_HEIGHT;

    ESP_LOGI(TAG, "Dialog renderer initialized");
}

void pw_dialog_show(const char *text, pw_msg_level_t level)
{
    if (!s_dialog_container) return;

    strncpy(s_full_text, text, PW_DIALOG_MAX_TEXT - 1);
    s_full_text[PW_DIALOG_MAX_TEXT - 1] = '\0';
    s_full_text_len = strlen(s_full_text);

    strncpy(s_last_text, text, PW_DIALOG_MAX_TEXT - 1);
    s_last_text[PW_DIALOG_MAX_TEXT - 1] = '\0';

    // Start with empty text and small container
    lv_label_set_text(s_dialog_label, " ");
    s_revealed_chars = 0;
    s_typing = true;

    // Set container to min height before unhiding — small dirty region
    s_current_height = DIALOG_MIN_HEIGHT;
    lv_obj_set_height(s_dialog_container, s_current_height);
    s_target_height = estimate_height(s_full_text_len);
    s_growing = (s_target_height > s_current_height);

    if (level < 4) {
        lv_obj_set_style_border_color(s_dialog_container, lv_color_hex(LEVEL_COLOR_HEX[level]), 0);
    }

    // Unhide at min height — dirty region is only 288x30 (safe for SPI)
    lv_obj_clear_flag(s_dialog_container, LV_OBJ_FLAG_HIDDEN);
    s_visible = true;
    s_show_time_ms = now_ms();

    ESP_LOGI(TAG, "Dialog: [%s] %s (%d chars, target_h=%d)",
        level == PW_MSG_LEVEL_SUCCESS ? "success" :
        level == PW_MSG_LEVEL_WARNING ? "warning" :
        level == PW_MSG_LEVEL_ERROR   ? "error"   : "info",
        text, s_full_text_len, s_target_height);
}

void pw_dialog_hide(void)
{
    if (!s_dialog_container || !s_visible) return;

    // Shrink to min before hiding — reduces the dirty region from hide
    lv_obj_set_height(s_dialog_container, DIALOG_MIN_HEIGHT);
    lv_obj_add_flag(s_dialog_container, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
    s_typing = false;
    s_growing = false;
    s_current_height = DIALOG_MIN_HEIGHT;
}

bool pw_dialog_is_visible(void)
{
    return s_visible;
}

void pw_dialog_tick(void)
{
    if (!s_visible) return;

    // Stage 1: Grow container height gradually (10px per frame)
    if (s_growing) {
        s_current_height += DIALOG_HEIGHT_STEP;
        if (s_current_height >= s_target_height) {
            s_current_height = s_target_height;
            s_growing = false;
        }
        lv_obj_set_height(s_dialog_container, s_current_height);
    }

    // Stage 2: Typewriter — reveal text as box grows
    if (s_typing) {
        s_revealed_chars += TYPEWRITER_CHARS_PER_TICK;
        if (s_revealed_chars >= s_full_text_len) {
            s_revealed_chars = s_full_text_len;
            s_typing = false;
            s_show_time_ms = now_ms();
            ESP_LOGI(TAG, "Typewriter done: %d chars, height=%d", s_full_text_len, s_current_height);
        }

        char saved = s_full_text[s_revealed_chars];
        s_full_text[s_revealed_chars] = '\0';
        lv_label_set_text(s_dialog_label, s_full_text);
        s_full_text[s_revealed_chars] = saved;
        return;
    }

    // Stage 3: Dismiss after timeout
    int64_t elapsed = now_ms() - s_show_time_ms;
    if (elapsed > PW_DIALOG_DISPLAY_MS) {
        pw_dialog_hide();
    }
}

const char *pw_dialog_get_last_text(void)
{
    return s_last_text;
}
