# Fix Sprite Transparency on LVGL v8 + ESP32-S3

## The Problem

Sprites rendered on the SenseCAP Watcher's round display show a solid colored rectangle behind the Pokemon instead of a transparent background. The sprite itself renders correctly (Pikachu is visible, correct colors, animating), but the area around the Pokemon that should be see-through shows as a filled rectangle on top of the mood-colored background.

## Hardware

- SenseCAP Watcher (Seeed Studio)
- ESP32-S3, 8MB PSRAM, 32MB flash
- 1.45" round display, 412x412 pixels, SPD2010 LCD controller via QSPI (SPI3)
- Display driver from SenseCAP Watcher BSP (`bsp_lvgl_init()`)

## Software Stack

- ESP-IDF v5.2.1
- LVGL v8.x (bundled with the SenseCAP Watcher SDK at `/tmp/SenseCAP-Watcher-Firmware/components/lvgl/`)
- 16-bit color depth (RGB565)
- `CONFIG_LV_COLOR_16_SWAP=y` (required by this display)
- `CONFIG_LV_COLOR_CHROMA_KEY_HEX=0xFF00FF`

## Sprite Data Format

- Source: Pokemon HeartGold/SoulSilver overworld sprites, 32x32 pixels per frame
- Stored on SD card as `.raw` RGB565 binary files (converted from PNG via Python script)
- Transparent pixels stored as magenta `0xF81F` in RGB565
- Sprite sheets are 96x128 (3 columns x 4 rows of 32x32 frames)
- At runtime, individual frames are extracted and scaled 4x to 128x128 pixels

## What We've Tried (All Failed)

### Attempt 1: LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED
Set the image format to chroma-keyed and configured the chroma key to magenta. The transparent pixels were byte-swapped along with all other pixels for LV_COLOR_16_SWAP. Result: square still visible.

### Attempt 2: LV_IMG_CF_TRUE_COLOR_ALPHA
Changed the pixel buffer to 3 bytes per pixel (2 bytes RGB565 color + 1 byte alpha). Transparent pixels get alpha=0x00, opaque pixels get alpha=0xFF. The color bytes are byte-swapped for LV_COLOR_16_SWAP. Result: square still visible.

### Attempt 3: Fixed sdkconfig regeneration
Discovered that `sdkconfig` wasn't regenerating from `sdkconfig.defaults` — the actual build had `LV_COLOR_16_SWAP=off` and chroma key=green instead of magenta. Fixed by deleting `sdkconfig` and rebuilding. Confirmed correct values in built sdkconfig. Result: square still visible.

## Current Code

### sprite_loader.c — frame extraction (3 bytes per pixel, alpha channel)
```c
uint8_t *pw_sprite_extract_frame_scaled(const pw_sprite_data_t *sprite,
                                         const pw_frame_coord_t *coord,
                                         uint16_t scale)
{
    uint16_t dst_w = sprite->frame_width * scale;   // 32 * 4 = 128
    uint16_t dst_h = sprite->frame_height * scale;  // 32 * 4 = 128
    size_t buf_size = dst_w * dst_h * 3;

    uint8_t *buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!buf) return NULL;

    const uint16_t *sheet = (const uint16_t *)sprite->sprite_sheet_data;

    for (uint16_t dy = 0; dy < dst_h; dy++) {
        uint16_t sy = coord->y + (dy / scale);
        if (sy >= sprite->sheet_height) sy = sprite->sheet_height - 1;
        for (uint16_t dx = 0; dx < dst_w; dx++) {
            uint16_t sx = coord->x + (dx / scale);
            if (sx >= sprite->sheet_width) sx = sprite->sheet_width - 1;
            uint16_t px = sheet[sy * sprite->sheet_width + sx];
            size_t idx = (dy * dst_w + dx) * 3;

            if (px == 0xF81F) {
                buf[idx]     = 0;
                buf[idx + 1] = 0;
                buf[idx + 2] = 0x00;  // alpha = fully transparent
            } else {
                uint16_t swapped = (px >> 8) | (px << 8);
                buf[idx]     = swapped & 0xFF;
                buf[idx + 1] = swapped >> 8;
                buf[idx + 2] = 0xFF;  // alpha = fully opaque
            }
        }
    }
    return buf;
}
```

### renderer.c — LVGL image descriptor setup
```c
s_frame_dsc.header.w = PW_SPRITE_DST_SIZE;   // 128
s_frame_dsc.header.h = PW_SPRITE_DST_SIZE;   // 128
s_frame_dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
s_frame_dsc.data_size = PW_SPRITE_DST_SIZE * PW_SPRITE_DST_SIZE * 3;
s_frame_dsc.data = (const uint8_t *)s_frame_buf;

if (s_sprite_img) {
    lvgl_port_lock(0);
    lv_img_set_src(s_sprite_img, &s_frame_dsc);
    lv_obj_set_pos(s_sprite_img, screen_x, screen_y);
    lvgl_port_unlock();
}
```

### renderer.c — screen and sprite object creation
```c
s_screen = lv_obj_create(NULL);
lv_obj_set_style_bg_color(s_screen, lv_color_hex(MOOD_BG_COLORS[PW_MOOD_SLEEPY]), 0);
lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
lv_obj_set_style_radius(s_screen, PW_DISPLAY_WIDTH / 2, 0);
lv_obj_set_style_clip_corner(s_screen, true, 0);

s_sprite_img = lv_img_create(s_screen);
lv_obj_set_pos(s_sprite_img, CENTER_X - SPRITE_HALF, CENTER_Y - SPRITE_HALF);

lv_scr_load(s_screen);
```

## Key Files

- `pokewatcher/main/sprite_loader.c` — loads .raw RGB565 from SD, extracts and scales frames
- `pokewatcher/main/sprite_loader.h` — function declarations and sprite data structs
- `pokewatcher/main/renderer.c` — LVGL display, sprite rendering, behavior state machine
- `pokewatcher/main/config.h` — display size (412x412), sprite scale (4x), frame size (32x32)
- `pokewatcher/sdkconfig.defaults` — build config including LV_COLOR_16_SWAP and chroma key

## What You Need to Do

Search the web (LVGL forums, GitHub issues, ESP32 forums, Stack Overflow, LVGL docs) and find the correct way to render sprites with transparent backgrounds using LVGL v8 on ESP32. The solution must work with dynamically created pixel buffers in PSRAM (not pre-converted images from the LVGL image converter tool).

Come back with working code — not theory.
