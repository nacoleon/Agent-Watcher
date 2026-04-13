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

// Border colors per message level (must use function, not static init)
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

void pw_dialog_init(lv_obj_t *parent)
{
    // Container: positioned within circular safe zone
    // 18% from bottom = 74px, 15% from sides = 62px on 412px display
    s_dialog_container = lv_obj_create(parent);
    lv_obj_set_size(s_dialog_container, 288, 70);
    lv_obj_align(s_dialog_container, LV_ALIGN_BOTTOM_MID, 0, -74);
    lv_obj_set_style_pad_all(s_dialog_container, 8, 0);
    lv_obj_set_style_radius(s_dialog_container, 8, 0);

    // Background: dark blue gradient
    lv_obj_set_style_bg_color(s_dialog_container, lv_color_hex(0x0A0A2E), 0);
    lv_obj_set_style_bg_opa(s_dialog_container, LV_OPA_COVER, 0);

    // Border: default blue, 2px
    lv_obj_set_style_border_color(s_dialog_container, lv_color_hex(0x5566AA), 0);
    lv_obj_set_style_border_width(s_dialog_container, 2, 0);

    // No scrollbar
    lv_obj_clear_flag(s_dialog_container, LV_OBJ_FLAG_SCROLLABLE);

    // Name label: "Zidane:" in green
    s_name_label = lv_label_create(s_dialog_container);
    lv_label_set_text(s_name_label, "Zidane:");
    lv_obj_set_style_text_color(s_name_label, lv_color_hex(0x7BE87B), 0);
    lv_obj_set_style_text_font(s_name_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_name_label, LV_ALIGN_TOP_LEFT, 4, 0);

    // Message text label: white
    s_dialog_label = lv_label_create(s_dialog_container);
    lv_label_set_text(s_dialog_label, "");
    lv_obj_set_style_text_color(s_dialog_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_dialog_label, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_dialog_label, 260);
    lv_label_set_long_mode(s_dialog_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_dialog_label, LV_ALIGN_TOP_LEFT, 4, 20);

    // Start hidden
    lv_obj_add_flag(s_dialog_container, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;

    ESP_LOGI(TAG, "Dialog renderer initialized");
}

void pw_dialog_show(const char *text, pw_msg_level_t level)
{
    if (!s_dialog_container) return;

    strncpy(s_last_text, text, PW_DIALOG_MAX_TEXT - 1);
    s_last_text[PW_DIALOG_MAX_TEXT - 1] = '\0';

    lv_label_set_text(s_dialog_label, text);

    // Set border color per level
    if (level < 4) {
        lv_obj_set_style_border_color(s_dialog_container, lv_color_hex(LEVEL_COLOR_HEX[level]), 0);
    }

    lv_obj_set_style_opa(s_dialog_container, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_dialog_container, LV_OBJ_FLAG_HIDDEN);
    s_visible = true;
    s_show_time_ms = now_ms();

    ESP_LOGI(TAG, "Dialog: [%s] %s",
        level == PW_MSG_LEVEL_SUCCESS ? "success" :
        level == PW_MSG_LEVEL_WARNING ? "warning" :
        level == PW_MSG_LEVEL_ERROR   ? "error"   : "info",
        text);
}

void pw_dialog_hide(void)
{
    if (!s_dialog_container || !s_visible) return;
    lv_obj_add_flag(s_dialog_container, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
}

bool pw_dialog_is_visible(void)
{
    return s_visible;
}

void pw_dialog_tick(void)
{
    if (!s_visible) return;

    int64_t elapsed = now_ms() - s_show_time_ms;

    if (elapsed > PW_DIALOG_DISPLAY_MS + PW_DIALOG_FADE_MS) {
        pw_dialog_hide();
    } else if (elapsed > PW_DIALOG_DISPLAY_MS) {
        // Fade out over PW_DIALOG_FADE_MS
        int64_t fade_elapsed = elapsed - PW_DIALOG_DISPLAY_MS;
        uint8_t opa = (uint8_t)(255 - (fade_elapsed * 255 / PW_DIALOG_FADE_MS));
        lv_obj_set_style_opa(s_dialog_container, opa, 0);
    }
}

const char *pw_dialog_get_last_text(void)
{
    return s_last_text;
}
