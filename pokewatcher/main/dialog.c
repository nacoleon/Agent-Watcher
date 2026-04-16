#include "dialog.h"
#include "renderer.h"
#include "config.h"
#include "sensecap-watcher.h"
#include "iot_knob.h"
#include "iot_button.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "pw_dialog";

// Replace common Unicode characters with ASCII equivalents for font compatibility
static void sanitize_text(char *dst, const char *src, size_t max_len)
{
    size_t d = 0;
    for (size_t i = 0; src[i] && d < max_len - 1; ) {
        uint8_t c = (uint8_t)src[i];
        if (c < 0x80) {
            // Plain ASCII — copy as-is
            dst[d++] = src[i++];
        } else if (c == 0xE2 && (uint8_t)src[i+1] == 0x80) {
            // UTF-8 sequence starting with E2 80 xx
            uint8_t c3 = (uint8_t)src[i+2];
            if (c3 == 0x94) {           // U+2014 em dash —
                if (d + 1 < max_len) dst[d++] = '-';
                i += 3;
            } else if (c3 == 0x93) {    // U+2013 en dash –
                if (d + 1 < max_len) dst[d++] = '-';
                i += 3;
            } else if (c3 == 0x98) {    // U+2018 left single quote '
                if (d + 1 < max_len) dst[d++] = '\'';
                i += 3;
            } else if (c3 == 0x99) {    // U+2019 right single quote '
                if (d + 1 < max_len) dst[d++] = '\'';
                i += 3;
            } else if (c3 == 0x9C) {    // U+201C left double quote "
                if (d + 1 < max_len) dst[d++] = '"';
                i += 3;
            } else if (c3 == 0x9D) {    // U+201D right double quote "
                if (d + 1 < max_len) dst[d++] = '"';
                i += 3;
            } else if (c3 == 0xA6) {    // U+2026 horizontal ellipsis …
                if (d + 2 < max_len) { dst[d++] = '.'; dst[d++] = '.'; }
                i += 3;
            } else {
                i += 3;  // skip unknown E2 80 xx
            }
        } else if (c >= 0xF0) {
            i += 4;  // skip 4-byte UTF-8 (emoji, etc.)
        } else if (c >= 0xE0) {
            i += 3;  // skip other 3-byte UTF-8
        } else if (c >= 0xC0) {
            i += 2;  // skip 2-byte UTF-8
        } else {
            i++;      // skip invalid continuation byte
        }
    }
    dst[d] = '\0';
}

static lv_obj_t *s_dialog_container = NULL;
static lv_obj_t *s_dialog_label = NULL;
static lv_obj_t *s_name_label = NULL;
static lv_obj_t *s_page_label = NULL;
static bool s_visible = false;
static int64_t s_show_time_ms = 0;
static int s_dismiss_count = 0;
static char s_last_text[PW_DIALOG_MAX_TEXT] = "";

// Pagination state
static char s_full_text[PW_DIALOG_MAX_TEXT] = "";
static int s_full_text_len = 0;
static int s_current_page = 0;
static int s_total_pages = 0;
#define CHARS_PER_PAGE  95

// Knob scroll flags (set from ISR callbacks, consumed in tick)
static volatile bool s_knob_next = false;
static volatile bool s_knob_prev = false;
static volatile bool s_knob_pressed = false;
static knob_handle_t s_knob_handle = NULL;
static button_handle_t s_btn_handle = NULL;

static void knob_left_cb(void *arg, void *data)
{
    s_knob_prev = true;
}

static void knob_right_cb(void *arg, void *data)
{
    s_knob_next = true;
}

static volatile bool s_knob_long_pressed = false;

// Exposed for renderer to detect button press while display is off
static volatile bool s_btn_wake_requested = false;

static void knob_btn_cb(void *arg, void *data)
{
    s_knob_pressed = true;
    s_btn_wake_requested = true;
}

static void knob_btn_long_cb(void *arg, void *data)
{
    s_knob_long_pressed = true;
}

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

static void update_page_indicator(void)
{
    if (!s_page_label) return;
    if (s_total_pages <= 1) {
        lv_obj_add_flag(s_page_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d/%d", s_current_page + 1, s_total_pages);
        lv_label_set_text(s_page_label, buf);
        lv_obj_clear_flag(s_page_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_current_page(void)
{
    int start = s_current_page * CHARS_PER_PAGE;
    int remaining = s_full_text_len - start;
    if (remaining <= 0) return;
    int len = remaining > CHARS_PER_PAGE ? CHARS_PER_PAGE : remaining;

    // Temporarily null-terminate at page boundary
    char saved = s_full_text[start + len];
    s_full_text[start + len] = '\0';
    lv_label_set_text(s_dialog_label, &s_full_text[start]);
    s_full_text[start + len] = saved;

    // Add bottom padding when page indicator is visible to prevent text overlap
    if (s_total_pages > 1) {
        lv_obj_set_style_pad_bottom(s_dialog_container, 28, 0);
    } else {
        lv_obj_set_style_pad_bottom(s_dialog_container, 8, 0);
    }

    update_page_indicator();
}

void pw_dialog_init(lv_obj_t *parent)
{
    s_dialog_container = lv_obj_create(parent);
    // Width fixed at 340, height auto-sizes to content (min ~68px for 1 line, max 170px)
    lv_obj_set_width(s_dialog_container, 340);
    lv_obj_set_height(s_dialog_container, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(s_dialog_container, 68, 0);
    lv_obj_set_style_max_height(s_dialog_container, 170, 0);
    lv_obj_align(s_dialog_container, LV_ALIGN_TOP_MID, 0, 100);
    lv_obj_set_style_pad_all(s_dialog_container, 8, 0);
    lv_obj_set_style_radius(s_dialog_container, 8, 0);

    lv_obj_set_style_bg_color(s_dialog_container, lv_color_hex(0x0A0A2E), 0);
    lv_obj_set_style_bg_opa(s_dialog_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_dialog_container, lv_color_hex(0x5566AA), 0);
    lv_obj_set_style_border_width(s_dialog_container, 2, 0);
    lv_obj_clear_flag(s_dialog_container, LV_OBJ_FLAG_SCROLLABLE);

    // Name label: "Zidane:" in green
    s_name_label = lv_label_create(s_dialog_container);
    lv_label_set_text(s_name_label, "Zidane:");
    lv_obj_set_style_text_color(s_name_label, lv_color_hex(0x7BE87B), 0);
    lv_obj_set_style_text_font(s_name_label, &lv_font_montserrat_22, 0);
    lv_obj_align(s_name_label, LV_ALIGN_TOP_LEFT, 4, 0);

    // Message text label
    s_dialog_label = lv_label_create(s_dialog_container);
    lv_label_set_text(s_dialog_label, "");
    lv_obj_set_style_text_color(s_dialog_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(s_dialog_label, &lv_font_montserrat_22, 0);
    lv_obj_set_width(s_dialog_label, 312);
    lv_label_set_long_mode(s_dialog_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_dialog_label, LV_ALIGN_TOP_LEFT, 4, 26);

    // Page indicator: "1/3" bottom-right, small dim text, hidden when single page
    s_page_label = lv_label_create(s_dialog_container);
    lv_label_set_text(s_page_label, "");
    lv_obj_set_style_text_color(s_page_label, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(s_page_label, &lv_font_montserrat_22, 0);
    lv_obj_align(s_page_label, LV_ALIGN_BOTTOM_RIGHT, -2, -2);
    lv_obj_add_flag(s_page_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(s_dialog_container, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;

    // Initialize knob for page scrolling
    const knob_config_t knob_cfg = {
        .default_direction = 0,
        .gpio_encoder_a = BSP_KNOB_A,
        .gpio_encoder_b = BSP_KNOB_B,
    };
    s_knob_handle = iot_knob_create(&knob_cfg);
    if (s_knob_handle) {
        iot_knob_register_cb(s_knob_handle, KNOB_LEFT, knob_left_cb, NULL);
        iot_knob_register_cb(s_knob_handle, KNOB_RIGHT, knob_right_cb, NULL);
        ESP_LOGI(TAG, "Knob registered for dialog page scrolling");
    }

    // Register knob button press to dismiss dialog
    const button_config_t btn_cfg = {
        .type = BUTTON_TYPE_CUSTOM,
        .long_press_time = 6000,
        .short_press_time = 200,
        .custom_button_config = {
            .active_level = 0,
            .button_custom_init = bsp_knob_btn_init,
            .button_custom_deinit = bsp_knob_btn_deinit,
            .button_custom_get_key_value = bsp_knob_btn_get_key_value,
        },
    };
    s_btn_handle = iot_button_create(&btn_cfg);
    if (s_btn_handle) {
        iot_button_register_cb(s_btn_handle, BUTTON_PRESS_UP, knob_btn_cb, NULL);
        iot_button_register_cb(s_btn_handle, BUTTON_LONG_PRESS_START, knob_btn_long_cb, NULL);
        ESP_LOGI(TAG, "Knob button registered (press=dismiss, long=reboot)");
    }

    ESP_LOGI(TAG, "Dialog renderer initialized");
}

void pw_dialog_show(const char *text, pw_msg_level_t level)
{
    if (!s_dialog_container) return;

    sanitize_text(s_full_text, text, PW_DIALOG_MAX_TEXT);
    s_full_text_len = strlen(s_full_text);

    strncpy(s_last_text, s_full_text, PW_DIALOG_MAX_TEXT - 1);
    s_last_text[PW_DIALOG_MAX_TEXT - 1] = '\0';

    // Calculate pages
    s_total_pages = (s_full_text_len + CHARS_PER_PAGE - 1) / CHARS_PER_PAGE;
    if (s_total_pages < 1) s_total_pages = 1;
    s_current_page = 0;

    // Show first page
    show_current_page();

    if (level < 4) {
        lv_obj_set_style_border_color(s_dialog_container, lv_color_hex(LEVEL_COLOR_HEX[level]), 0);
    }

    lv_obj_clear_flag(s_dialog_container, LV_OBJ_FLAG_HIDDEN);
    s_visible = true;
    s_show_time_ms = now_ms();

    ESP_LOGI(TAG, "Dialog: [%s] (%d chars, %d pages) %s",
        level == PW_MSG_LEVEL_SUCCESS ? "success" :
        level == PW_MSG_LEVEL_WARNING ? "warning" :
        level == PW_MSG_LEVEL_ERROR   ? "error"   : "info",
        s_full_text_len, s_total_pages, text);
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

int pw_dialog_get_dismiss_count(void)
{
    return s_dismiss_count;
}

void pw_dialog_next_page(void)
{
    if (!s_visible || s_total_pages <= 1) return;
    s_current_page = (s_current_page + 1) % s_total_pages;
    show_current_page();
    // Reset dismiss timer on page change
    s_show_time_ms = now_ms();
    ESP_LOGI(TAG, "Dialog page %d/%d", s_current_page + 1, s_total_pages);
}

void pw_dialog_prev_page(void)
{
    if (!s_visible || s_total_pages <= 1) return;
    s_current_page = (s_current_page - 1 + s_total_pages) % s_total_pages;
    show_current_page();
    s_show_time_ms = now_ms();
    ESP_LOGI(TAG, "Dialog page %d/%d", s_current_page + 1, s_total_pages);
}

void pw_dialog_tick(void)
{
    // Long press reboot — works whether dialog is visible or not
    if (s_knob_long_pressed) {
        s_knob_long_pressed = false;
        ESP_LOGW(TAG, "Long press detected — rebooting");
        esp_restart();
    }

    if (!s_visible) {
        // Drain stale button presses so they don't dismiss the next dialog
        s_knob_pressed = false;
        s_knob_next = false;
        s_knob_prev = false;
        return;
    }

    // Check knob input flags (set from ISR, consumed here inside LVGL lock)
    if (s_knob_pressed) {
        s_knob_pressed = false;
        s_dismiss_count++;
        ESP_LOGI(TAG, "Dialog dismissed by knob press (dismiss_count=%d)", s_dismiss_count);
        pw_dialog_hide();
        pw_renderer_set_state(PW_STATE_IDLE);
        bsp_rgb_set(0, 0, 0);
        return;
    }
    if (s_knob_next) {
        s_knob_next = false;
        pw_dialog_next_page();
    }
    if (s_knob_prev) {
        s_knob_prev = false;
        pw_dialog_prev_page();
    }
}

bool pw_dialog_consume_btn_wake(void)
{
    if (s_btn_wake_requested) {
        s_btn_wake_requested = false;
        return true;
    }
    return false;
}

void pw_dialog_consume_knob_press(void)
{
    s_knob_pressed = false;
}

const char *pw_dialog_get_last_text(void)
{
    return s_last_text;
}
