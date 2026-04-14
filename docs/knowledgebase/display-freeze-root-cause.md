# Display Freeze Root Cause — SOLVED

## The Bug

The Watcher display would permanently freeze when switching states or sending messages. The sprite animation stopped, the display showed the last rendered frame, but the firmware kept running (API still responded).

## What We Thought

We assumed it was an SPI bus collision between the LCD (SPI3) and the Himax AI camera (SPI2). We tried:
- Pausing Himax before LCD flushes — didn't help
- Disabling Himax entirely — **still froze**
- Disabling the dialog — **still froze**

This ruled out both Himax SPI and the dialog as causes.

## The Actual Cause

**Per-frame opacity animation on the dialog container.**

`pw_dialog_tick()` was calling `lv_obj_set_style_opa()` on the 288x70 dialog container **every single frame** during the fade-out period. Each call:
1. Marked the container's area as a dirty region in LVGL
2. Triggered an SPI flush of that region to the LCD
3. The SPI transfer queue couldn't keep up with per-frame style changes on a large area
4. Eventually the flush callback never completed, blocking LVGL's timer task
5. The renderer's `lvgl_port_lock(0)` (wait forever) then blocked permanently

## The Fix

Removed the per-frame opacity fade. Dialog now simply shows and hides with a timeout — no gradual animation.

```c
// BEFORE (broken): opacity fade every frame
void pw_dialog_tick(void) {
    if (elapsed > PW_DIALOG_DISPLAY_MS) {
        int64_t fade_elapsed = elapsed - PW_DIALOG_DISPLAY_MS;
        uint8_t opa = (uint8_t)(255 - (fade_elapsed * 255 / PW_DIALOG_FADE_MS));
        lv_obj_set_style_opa(s_dialog_container, opa, 0);  // SPI flush EVERY FRAME
    }
}

// AFTER (fixed): simple timeout, no per-frame style changes
void pw_dialog_tick(void) {
    if (elapsed > PW_DIALOG_DISPLAY_MS) {
        pw_dialog_hide();  // single hide, one flush
    }
}
```

## The Rule

**Never call `lv_obj_set_style_*()` every frame on large LVGL objects.** Each style change marks the object's area as dirty and triggers an SPI flush. On the SPD2010 LCD with its SPI bus, this overwhelms the transfer queue.

Safe per-frame operations:
- `lv_img_set_src()` — updates image data pointer, small dirty region (sprite size)
- `lv_obj_set_pos()` — moves an object, dirty region is old + new position

Unsafe per-frame operations:
- `lv_obj_set_style_bg_color()` on screen — full 412x412 dirty region
- `lv_obj_set_style_opa()` on large containers — large dirty region every frame
- `lv_obj_set_style_bg_opa()` — same issue
- Any style change on objects larger than ~100x100 pixels
