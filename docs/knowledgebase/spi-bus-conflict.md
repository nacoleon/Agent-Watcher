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

## SPI2 Bus: SD Card vs Himax Camera — SOLVED

**Root cause:** The physical SD card, once initialized into SPI mode by `esp_vfs_fat_sdspi_mount()`, does NOT tri-state its MISO output when CS (GPIO46) is deasserted. It continues driving MISO high (0xFF), preventing the Himax camera from communicating on the shared SPI2 bus.

**Fix:** Power off the SD card via IO expander after loading sprites:
```c
bsp_sdcard_deinit_default();  // Unmount filesystem + remove SPI device
esp_io_expander_set_level(io_exp, BSP_PWR_SDCARD, 0);  // Kill power
gpio_set_level(BSP_SD_SPI_CS, 1);  // Hold CS high
```

This is implemented in `app_main.c` after sprite loading (step 5b). The SD card is only needed at boot for loading sprites to SPIRAM. See `himax-camera-debugging.md` Phase 8 for the full 10-experiment investigation.

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

## The Background Color Problem (UPDATED 2026-04-13)

**Previous theory was wrong.** We thought `lv_obj_set_style_bg_color` freezes were caused by SPI collision with Himax. Testing proved Himax was NOT the cause — the freeze happened even with Himax completely disabled.

**Actual cause:** Per-frame `lv_obj_set_style_opa()` calls on a 288x70 dialog container overwhelmed the LCD SPI transfer queue. Each style change marks the area as dirty and triggers a flush. At ~10 FPS, the SPI queue backed up and the flush callback never completed, blocking LVGL forever.

**See:** `docs/knowledgebase/display-freeze-root-cause.md` for the full investigation and fix.

**The rule:** Never call `lv_obj_set_style_*()` every frame on objects larger than ~100x100 pixels. Use `lv_img_set_src()` and `lv_obj_set_pos()` for per-frame updates (these have small dirty regions).

The "white background" users reported was actually the near-white LVGL default theme color (`0xF7BE` ≈ `#F0F4F0`) that showed before our color was applied, or an empty `lv_img` object covering the bg.

## DMA Bounce Buffer Issue (RESOLVED 2026-04-15)

The "SPI flush stall" (error 0x101) was NOT caused by SPI bus conflicts between LCD and Himax. It was `ESP_ERR_NO_MEM` — the SPI driver failed to allocate a DMA bounce buffer in internal SRAM when flushing PSRAM-backed LVGL draw buffers. See `docs/knowledgebase/spi-flush-stall-bug.md` for the full investigation and fix (`CONFIG_BSP_LCD_SPI_DMA_SIZE_DIV=12`).

## Background Images

Background tiles (72 files, 240x170 RGB565) are loaded from SD card, scaled to 412x412 in PSRAM, and displayed with strip wipe transitions. Auto-rotation every 5 minutes. Toggle via web UI or `PUT /api/background {"auto_rotate": false}`.
