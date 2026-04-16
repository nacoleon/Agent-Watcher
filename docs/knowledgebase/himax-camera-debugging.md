# Himax Camera Debugging Log

Documents all attempts to get the Himax WE2 AI camera working with our custom pokewatcher firmware. The camera worked with the stock SenseCap firmware but does not respond to AT commands with our firmware.

## Symptom

- `sscma_client_init()` returns ESP_OK (hardware reset via IO expander pin 7 works)
- Sync pin (IO expander pin 6) goes HIGH after reset (chip boots)
- `sscma_client_available()` returns ESP_OK with 0 bytes (SPI transport works)
- `sscma_client_write()` returns ESP_OK (SPI write succeeds)
- `sscma_client_get_info()` times out after 20s (no AT command response)
- `sscma_client_set_model()` times out after 20s
- No `on_log`, `on_connect`, or `on_event` callbacks fire
- Raw AT+ID? command sent manually — no response after 5s of polling

## What This Tells Us

- SPI2 bus is functional (reads and writes complete without error)
- Himax chip powers on and boots (sync pin goes HIGH)
- The chip's bootloader/ROM is alive but the application firmware either isn't loaded or isn't processing AT commands
- The SSCMA process task runs and polls `available()` but never gets data back

## Attempts (in chronological order)

### 1. Simple re-enable (2026-04-15)
**What:** Uncommented `pw_himax_task_start()` in app_main.c
**Result:** `get_info` timeout. No detection events.
**Conclusion:** Not just a matter of enabling it.

### 2. DMA fix context (2026-04-15)
**What:** Previously fixed SPI DMA OOM (`CONFIG_BSP_LCD_SPI_DMA_SIZE_DIV=12`). Both SPI2 (Himax) and SPI3 (LCD) use `SPI_DMA_CH_AUTO` and compete for DMA memory. The old 32KB LCD chunks exhausted internal SRAM.
**Result:** SPI transfers no longer fail with 0x101, but Himax still doesn't respond to AT commands.
**Conclusion:** DMA fix helped SPI reliability but wasn't the Himax root cause.

### 3. Added diagnostic logging (2026-04-15)
**What:** Added `sscma_client_init` return code, `get_info`/`get_model` queries, timing logs.
**Result:** `init` = ESP_OK, `get_info` = ESP_ERR_TIMEOUT (0x107), `get_model` = ESP_ERR_TIMEOUT.
**Conclusion:** Confirmed the chip doesn't respond to any AT commands.

### 4. Sync pin monitoring (2026-04-15)
**What:** Read IO expander pin 6 (sync) before and after reset, polling every 500ms.
**Result:** Sync = 0 before reset, goes to 1 within 500ms after reset.
**Conclusion:** Chip boots (bootloader runs, asserts sync), but application layer doesn't process commands.

### 5. Raw SPI available() polling (2026-04-15)
**What:** Called `sscma_client_available()` 5 times after reset, 200ms apart.
**Result:** All return ESP_OK with avail=0. No data from chip.
**Conclusion:** SPI transport layer works. Chip just has nothing to say.

### 6. Raw AT command write + poll (2026-04-15)
**What:** Manually wrote `\r\nAT+ID?\r\n` via `sscma_client_write()`, then polled `available()` for 5 seconds.
**Result:** Write = ESP_OK, 5 seconds of polling = 0 bytes available.
**Conclusion:** Chip receives SPI data (no error) but never responds. Strongly suggests no application firmware running.

### 7. Init order — Himax before renderer (2026-04-15)
**What:** Moved `pw_himax_early_init()` (calls `bsp_sscma_client_init()`) to before `pw_renderer_init()`, so Himax registers on SPI2 before LCD starts SPI3 activity.
**Result:** No change. Same timeout.
**Conclusion:** Init order between renderer and Himax doesn't matter.

### 8. Init order — Himax before SD card (2026-04-15)
**What:** Split Himax init into early (`bsp_sscma_client_init`) and late (task start). Early init runs before SD card mount since both share SPI2.
**Result:** No change. Same timeout.
**Conclusion:** SD card mount on SPI2 doesn't interfere with Himax.

### 9. SSCMA sdkconfig — match stock firmware (2026-04-15)
**What:** Found major differences in SSCMA task configuration vs stock firmware:
- Process task stack: 4096 → 10240 (stock uses 10KB)
- Monitor task stack: 4096 → 10240
- Task affinity: no affinity → CPU1 (stock pins to CPU1)
- Stack allocation: internal RAM → SPIRAM (stock uses external)
- Small mem: internal → external

Applied all stock settings. Full rebuild with clean sdkconfig.
**Result:** No change. Same timeout.
**Conclusion:** Task stack/priority/affinity configuration is not the issue.

### 10. All callbacks registered (2026-04-15)
**What:** Added `on_log` and `on_connect` callbacks in addition to `on_event`.
**Result:** None of the callbacks ever fire.
**Conclusion:** The SSCMA client never receives any data from the chip to trigger callbacks.

## Stock Firmware Differences Investigated

| Aspect | Stock | Ours | Tested? |
|--------|-------|------|---------|
| IO expander power sequence | BSP_PWR_AI_CHIP via BSP_PWR_START_UP | Same (bsp_io_expander_init) | Same code path |
| SPI2 bus init | Via bsp_spi_bus_init() | Same | Same code path |
| SD card CS deassert | In bsp_sscma_client_init() | Same | Same code path |
| Init order | sscma_client_init after lvgl_init | Tried both before and after | No difference |
| SSCMA task config | 10KB stack, CPU1, SPIRAM | Tried matching exactly | No difference |
| DMA chunk size | Unknown (stock default) | Fixed to ~28KB | Already applied |

## Key Technical Details

- **Sync pin**: IO expander pin 6 (input). Himax asserts HIGH when ready. The `sscma_client_available()` function checks this pin — if LOW, returns immediately without querying SPI.
- **Reset pin**: IO expander pin 7. `sscma_client_init()` → `sscma_client_reset()` toggles LOW 100ms, HIGH 200ms.
- **SPI protocol**: 256-byte fixed packets, header (4 bytes) + payload + trailer (2 bytes). Uses FEATURE_TRANSPORT_CMD_AVAILABLE to query pending data.
- **AT protocol**: Commands like `\r\nAT+ID?\r\n`, responses wrapped in `\r{...}\n` JSON.
- **Process task**: Internal SSCMA task polls `available()` every 10ms, reads data, parses JSON, matches to pending requests.

## Next Step

Try firmware flash from SD card using `sscma_client_ota_start/write/finish` API. Files needed:
- `/sdcard/himax/firmware.img` (548KB) — Himax application firmware
- `/sdcard/himax/person.tflite` (1.6MB) — Person detection model at offset 0x400000

The chip's ROM bootloader is alive (sync pin works, SPI responds), so OTA flash should be possible even if application firmware is missing/corrupted.
