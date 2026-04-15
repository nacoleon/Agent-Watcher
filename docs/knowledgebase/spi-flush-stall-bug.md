# SPI Flush Stall Bug — Intermittent Display Freeze

## Status: Partially mitigated, not fully resolved

Last updated: 2026-04-14

## The Bug

The display freezes permanently. The sprite animation stops, ZZZ doesn't move, state changes via web UI have no effect. The web UI at 10.0.0.40 continues to respond (web server runs on a separate task). Requires reboot to recover.

## Root Cause

`spi_device_queue_trans` returns `ESP_ERR_INVALID_STATE` (0x101) meaning "polling transaction not terminated." This happens in the ESP-IDF SPI panel IO layer (`esp_lcd_panel_io_spi.c`) when queuing color data after a polling command transmit. The SPI driver's internal state isn't fully cleared between the polling transmit (RAMWR command) and the queued transmit (color data).

### The Deadlock Chain

1. `panel_spd2010_draw_bitmap` sends CASET (polling OK), RASET (polling OK), then RAMWR command (polling OK)
2. `spi_device_queue_trans` for color data fails with 0x101
3. `panel_io_spi_tx_color` returns error to `panel_spd2010_draw_bitmap`
4. `panel_spd2010_draw_bitmap` returns error to `lvgl_port_flush_callback`
5. **CRITICAL**: The flush callback did NOT call `lv_disp_flush_ready(drv)` on error
6. LVGL waits forever for flush completion → LVGL mutex held permanently
7. Renderer task blocks at `lvgl_port_lock(500)` → times out every 500ms indefinitely
8. Display frozen, renderer alive but unable to update anything

### Evidence from Serial Logs

```
I (22299) pw_renderer: >>> DIALOG SHOW start frame=132
I (22299) pw_renderer: >>> DIALOG SHOW done frame=132
E (22329) lcd_panel.io.spi: panel_io_spi_tx_color(390): spi transmit (queue) color failed
E (22329) LVGL: FLUSH ERR: ret=0x101 area=(56,69)-(355,118) task=LVGL task core=1
W (23009) pw_renderer: LVGL lock TIMEOUT frame=134 (SPI flush stalled?)
W (23609) pw_renderer: LVGL lock TIMEOUT frame=135 (SPI flush stalled?)
... (repeats forever)
```

## What We Tried

### Theory 1: Large dirty regions cause the stall (WRONG)
- Hypothesized that dirty regions > ~288x70 triggered the SPI failure
- Tested with staged dialog reveal (grow 10px/frame), typewriter text effect
- **Result**: Stall happened even with a 299x49 pixel dirty region (the smallest dialog size)
- **Conclusion**: Dirty region size is NOT the primary trigger. The SPI stall is truly intermittent and boot-dependent.

### Theory 2: LV_SIZE_CONTENT causes variable dirty regions (PARTIALLY RIGHT)
- Using `LV_SIZE_CONTENT` made the container resize dynamically, creating unpredictable dirty regions
- Reverted to fixed height container
- **Result**: Reduced frequency but didn't eliminate the stall
- **Conclusion**: Fixed sizes help but don't prevent the underlying SPI issue

### Theory 3: Web server body buffer overflow (FIXED — separate bug)
- `handle_api_message` used a 256-byte buffer for the HTTP body
- A 200-char message with JSON wrapper exceeds 256 bytes → memory corruption
- **Fixed**: Increased to 512 bytes
- **This was a real bug but separate from the SPI stall**

## Current Mitigations

### 1. SPD2010 driver retry (esp_lcd_spd2010.c)
When RAMWR fails, waits 10ms then retries the full CASET+RASET+RAMWR sequence:
```c
// In panel_spd2010_draw_bitmap, after tx_color fails:
vTaskDelay(pdMS_TO_TICKS(10));
// Re-send full CASET + RASET + RAMWR from scratch
```
This resets the display controller's state machine. The 10ms delay lets the SPI ISR finish cleaning up the stuck polling transaction.

### 2. LVGL flush callback safety net (esp_lvgl_port.c)
If draw_bitmap fails even after retry, calls `lv_disp_flush_ready(drv)` to prevent permanent LVGL mutex deadlock:
```c
if (flush_ret != ESP_OK) {
    lv_disp_flush_ready(drv);  // prevents permanent freeze
}
```
This may cause a one-frame artifact but prevents permanent freeze.

### 3. lvgl_port_lock timeout (renderer.c)
Changed from `lvgl_port_lock(0)` (wait forever) to `lvgl_port_lock(500)` (500ms timeout). Renderer logs timeout warnings instead of blocking permanently.

### 4. Staged dialog reveal (dialog.c)
Dialog container starts at 48px height, grows 10px/frame while typewriter types 3 chars/frame. Reduces per-frame dirty region size, which may reduce (but not eliminate) SPI stall probability.

### 5. Diagnostic logging (renderer.c)
- Frame counter with periodic ALIVE messages
- LVGL lock acquisition timing
- Lock held / unlock duration tracking
- Dialog show/done brackets
- Typewriter tick timing

## Files Modified

| File | Change |
|------|--------|
| `/tmp/SenseCAP-Watcher-Firmware/components/esp_lvgl_port/esp_lvgl_port.c` | flush_ready on error, FLUSH ERR logging |
| `/tmp/pokewatcher-build/managed_components/espressif__esp_lcd_spd2010/esp_lcd_spd2010.c` | Retry with 10ms delay on RAMWR failure |
| `pokewatcher/main/renderer.c` | lvgl_port_lock(500) timeout, diagnostic logging |
| `pokewatcher/main/dialog.c` | Staged height reveal, typewriter effect |
| `pokewatcher/main/config.h` | PW_DIALOG_MAX_TEXT=161 |
| `pokewatcher/main/web_server.c` | Body buffer 256→512 |
| `pokewatcher/main/web/index.html` | maxlength=160 |

**NOTE**: The SPD2010 and esp_lvgl_port changes are in managed_components / SDK, not in the pokewatcher source. They need to be re-applied after `idf.py build` regenerates managed_components. Consider patching these in a build script or moving them to a local component override.

## What Still Needs Investigation

1. **Why does the SPI polling state not clear?** The ESP-IDF `spi_device_polling_transmit` should be fully synchronous. On dual-core ESP32-S3, there may be a race between the polling completion on one core and the ISR on another.

2. **Is the Himax camera contributing?** It's disabled but the SSCMA client task might still be doing SPI bus probing. Check if disabling the SSCMA task entirely eliminates the stall.

3. **SPI bus frequency**: The LCD uses QSPI (SPI3). Check if reducing the SPI clock frequency reduces stall frequency.

4. **Transaction queue depth**: The panel IO `trans_queue_depth` setting controls how many SPI transactions can be in-flight. A smaller queue might prevent the race condition.

5. **ESP-IDF version**: We're on v5.2.1. Check if newer ESP-IDF versions have fixes for the SPI polling/queued transaction race condition.

## How to Reproduce

- Send messages via web UI repeatedly (short and long)
- Change states rapidly
- The stall is intermittent and boot-dependent — some boots never stall, others stall on the first message
- Serial monitor shows `FLUSH ERR: ret=0x101` when it happens

## Key Insight

The #1 bug was that `lvgl_port_flush_callback` never called `lv_disp_flush_ready()` on error. This turned an intermittent SPI hiccup into a permanent display freeze. With the retry + safety net, the SPI hiccup is survivable.
