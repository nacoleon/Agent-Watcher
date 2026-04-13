# SPI Bus Conflict — CRITICAL for All Firmware Work

The single most important hardware constraint on the SenseCap Watcher. Every firmware developer must understand this before touching display or rendering code.

## The Problem

The LCD display (SPI3/QSPI) and the Himax AI camera chip (SPI2) share the SPI peripheral infrastructure. When LVGL tries to flush pixels to the LCD while the SSCMA client is running continuous inference on the Himax chip, the SPI transfers collide:

```
E (xxxx) lcd_panel.io.spi: panel_io_spi_tx_color(390): spi transmit (queue) color failed
```

This causes:
1. The LVGL task hangs waiting for SPI
2. Any thread holding `lvgl_port_lock` blocks forever
3. The renderer task blocks waiting for the LVGL lock
4. Display freezes permanently — requires reboot

## When It Happens

The collision occurs when **any LVGL style/property change triggers a display flush** while the Himax task is running. This includes:
- `lv_obj_set_style_bg_color()` — triggers dirty region + flush
- `lv_obj_set_style_bg_opa()` — same
- Any LVGL call that marks a region as "dirty" and needs LCD SPI transfer

It does NOT happen during init because the Himax task hasn't started yet.

## Safe Patterns

### DO: Minimize LVGL lock time
```c
// Prepare data OUTSIDE the lock (CPU only, no SPI)
prepare_frame();  // behavior tick, sprite extraction, position calc

// Take lock ONCE, do minimal LVGL calls, release immediately
lvgl_port_lock(0);
commit_frame();       // lv_img_set_src + lv_obj_set_pos (fast)
pw_dialog_tick();     // opacity change (fast)
lvgl_port_unlock();
```

### DO: Only update styles when something changed
```c
// BAD — triggers SPI flush every frame:
lv_obj_set_style_bg_color(s_screen, color, 0);  // EVERY FRAME

// GOOD — only when state changes:
if (s_bg_color_dirty) {
    s_bg_color_dirty = false;
    lv_obj_set_style_bg_color(s_screen, color, 0);  // ONCE
}
```

### DO: Use volatile flags for cross-thread communication
```c
// Other threads set flags (lock-free):
void pw_renderer_set_state(pw_agent_state_t state) {
    s_pending_state = state;
    s_state_changed = true;  // volatile flag
}

// Render loop picks up flags (single-threaded):
if (s_state_changed) {
    s_state_changed = false;
    s_current_state = s_pending_state;
    s_bg_color_dirty = true;
}
```

### DON'T: Call LVGL from non-renderer threads
```c
// BAD — web server thread calling LVGL directly:
void handle_api_state(httpd_req_t *req) {
    lvgl_port_lock(0);  // DEADLOCK with renderer
    lv_obj_set_style_bg_color(...);
    lvgl_port_unlock();
}

// BAD — agent state callback calling renderer with locks:
void on_state_changed(...) {
    pw_renderer_set_state(state);  // if this takes lvgl_port_lock → DEADLOCK
}
```

### DON'T: Create empty LVGL image objects
```c
// BAD — empty lv_img renders as white rectangle covering bg:
s_bg_img = lv_img_create(s_screen);
lv_obj_remove_style_all(s_bg_img);
lv_obj_add_flag(s_bg_img, LV_OBJ_FLAG_HIDDEN);  // HIDDEN flag may not work

// GOOD — only create when you have actual image data to show
```

### DON'T: Do file I/O while holding LVGL lock
```c
// BAD — 400kHz SD card read blocks for seconds:
lvgl_port_lock(0);
fread(buffer, size, 1, file);  // BLOCKS → SPI collision → watchdog
lvgl_port_unlock();
```

## Current Renderer Architecture (after fixes)

```
renderer_task loop:
  1. Process flags (no lock): wake, state change, behavior reset
  2. prepare_frame() — CPU work, NO LVGL, NO SPI
     - behavior_tick() — state machine, position calc
     - sprite_extract_frame_scaled() — pixel scaling in PSRAM
     - bounce/position calc
  3. lvgl_port_lock(0) — SINGLE lock per frame
     - commit_frame() — lv_img_set_src + lv_obj_set_pos
     - bg color update (only if dirty)
     - dialog show (only if pending)
     - dialog_tick() — opacity fade
  4. lvgl_port_unlock()
  5. vTaskDelay(frame_delay)
```

## The Background Color Problem

The screen bg color IS set correctly (confirmed via logging: `0x59FF` = warm peach in RGB565 with byte swap). But subsequent `lv_obj_set_style_bg_color` calls trigger SPI flushes that collide with Himax. The init color works because Himax hasn't started yet. State change colors only work with the dirty-flag pattern.

The "white background" users reported was actually the near-white LVGL default theme color (`0xF7BE` ≈ `#F0F4F0`) that showed before our color was applied, or an empty `lv_img` object covering the bg.

## Background Images (Disabled)

Loading 240x170 background tiles from SD card was attempted but is too slow on the 400kHz SPI bus — even individual 80KB files take long enough to trigger the watchdog. The tile files exist on the SD card at `/sdcard/characters/zidane/bg/*.raw` but the loading code is disabled with `#if 0`. Re-enable when:
- SD card speed can be increased (needs faster card or higher SPI clock)
- OR backgrounds are pre-cached in PSRAM at boot (before Himax starts)
- OR a dedicated background loading task with proper SPI arbitration is implemented
