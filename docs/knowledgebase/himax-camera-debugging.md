# Himax Camera Debugging Log

Documents all attempts to get the Himax WE2 AI camera working with our custom pokewatcher firmware.

## Final Root Cause

**SD card on SPI2 breaks Himax communication — but only in our firmware.**

- Camera works: stock firmware, sscma_client_monitor example, our firmware WITHOUT SD card
- Camera fails: our firmware WITH SD card (any init order, any clock speed, any sdkconfig)
- The monitor example runs SD card + Himax on SPI2 simultaneously with no issues
- Something unique to our application code's runtime behavior prevents SPI2 bus sharing

## Himax Hardware Details

- **Chip:** Himax WE2 (HX6538) with SSCMA firmware v2024.08.16
- **SPI2 bus:** MOSI=GPIO5, MISO=GPIO6, SCLK=GPIO4, CS=GPIO21 (Himax), CS=GPIO46 (SD card)
- **Sync pin:** IO expander pin 6 (input, Himax asserts HIGH when ready)
- **Reset pin:** IO expander pin 7 (active LOW, 100ms LOW + 200ms HIGH)
- **Power:** IO expander pin 11 (BSP_PWR_AI_CHIP)
- **UART:** GPIO17 TX, GPIO18 RX, 921600 baud (for AT commands and OTA flash)
- **Model:** Person Detection (swift_yolo_nano_person_192_int8_vela.tflite) at flash address 0x400000

## Key Technical Findings

### SPI MISO returns 0xFF when SD card is present
When SD card has been mounted on SPI2, the Himax returns `0xFF 0xFF` on every AVAILABLE query. The SSCMA code treats `0xFFFF` as `len=0` (no data). The chip's SPI slave hardware clocks out all-ones because no software is driving MISO with real data — even though the Himax firmware IS running (verified by stock firmware test).

### Camera works perfectly without SD card
Without SD card init, `on_connect` fires, RX buffer floods with inference data from previous stock firmware session, person detection model responds. Full SPI communication works.

### UART transport also fails (with SD card)
Switched from SPI to UART transport (GPIO 17/18, 921600 baud). Same timeout — chip doesn't respond to AT commands or OTA flash commands over UART either when SD card is present on SPI2. This was surprising and suggests the SD card's SPI device registration affects the entire ESP32 SPI/peripheral subsystem, not just SPI2 data lines.

### Stock firmware init sequence
```
1. bsp_io_expander_init()     — powers Himax via BSP_PWR_AI_CHIP
2. bsp_sdcard_init_default()  — mounts SD card on SPI2 (conditional on detect pin)
3. bsp_lvgl_init()            — starts LCD on SPI3, calls bsp_spi_bus_init()
4. bsp_sscma_client_init()    — registers Himax device on SPI2
5. sscma_client_init()        — hardware reset, starts process/monitor tasks
6. sscma_client_set_model(1)  — select person detection
7. sscma_client_invoke(-1)    — start continuous inference
```

### Monitor example (works with SD + Himax)
Same BSP code, same SPI2 bus, same sdkconfig structure. SD card mounted via `bsp_sdcard_init_default()`. Himax initialized after. Both coexist on SPI2 simultaneously. Person detection at ~10 FPS with ~8KB JPEG per frame.

## Attempts (29 total)

### Phase 1: Basic enable and diagnostics (attempts 1-6)
| # | What | Result |
|---|------|--------|
| 1 | Uncommented `pw_himax_task_start()` | `get_info` timeout |
| 2 | DMA fix context (CONFIG_BSP_LCD_SPI_DMA_SIZE_DIV=12) | SPI errors fixed but Himax still no response |
| 3 | Diagnostic logging (init return codes, timing) | Confirmed ESP_ERR_TIMEOUT on all AT commands |
| 4 | Sync pin monitoring via IO expander | Sync goes 0→1 within 500ms after reset (chip boots) |
| 5 | Raw `sscma_client_available()` polling | All return ESP_OK with avail=0 (no data from chip) |
| 6 | Raw AT+ID? write + available poll | Write OK, 5s polling = 0 bytes. Chip ignores commands |

### Phase 2: Init order and configuration (attempts 7-10)
| # | What | Result |
|---|------|--------|
| 7 | Himax init before renderer | Same timeout |
| 8 | Himax init before SD card (split into early/late) | Same timeout |
| 9 | Matched stock SSCMA sdkconfig (10KB stack, CPU1, SPIRAM) | Same timeout |
| 10 | Registered all callbacks (on_log, on_connect, on_event) | None fire |

### Phase 3: Flash and transport experiments (attempts 11-13)
| # | What | Result |
|---|------|--------|
| 11 | OTA firmware flash via SPI from SD card | `sscma_client_ota_start` failed — SPI flasher timeout |
| 12 | Python sscma.cli tool from Mac via USB | Can't reach Himax — USB connects to ESP32 not Himax |
| 13 | **Flashed stock firmware V1.1.7** | **CAMERA WORKS** — hardware is fine |

### Phase 4: Configuration matching (attempts 14-18)
| # | What | Result |
|---|------|--------|
| 14 | Removed 33KB DMA bounce buffer | Same timeout (bounce buffer wasn't the issue) |
| 15 | Himax init before renderer (again, with new config) | Same timeout |
| 16 | Retry get_info 3× with reset between each | All 3 timeout |
| 17 | Skip renderer entirely (no LCD) | Boot loop — other code depends on renderer |
| 18 | Early explicit `bsp_spi_bus_init()` call | Same timeout |

### Phase 5: Deep SPI analysis (attempts 19-22)
| # | What | Result |
|---|------|--------|
| 19 | **SPI IO debug logging** — patched `sscma_client_io_spi.c` | Sync HIGH, AVAILABLE sent, chip returns **0xFF 0xFF** (all ones). MISO not driven. |
| 20 | **Switched to UART transport** (GPIO 17/18, 921600 baud) | `get_info` timeout on UART too |
| 21 | **UART OTA flash** (XMODEM protocol) | "enter ota mode failed" — chip unresponsive on UART |
| 22 | Explicit power cycle (BSP_PWR_AI_CHIP LOW 500ms, HIGH 1s) | Same timeout after power cycle |

### Phase 6: SD card isolation — BREAKTHROUGH (attempts 23-29)
| # | What | Result |
|---|------|--------|
| 23 | **No SD card + WiFi + renderer** | **CAMERA WORKS!** `on_connect` fires, RX buffer floods with data |
| 24 | SD card mount → load sprites → unmount → start Himax | Still fails — unmount doesn't clean SPI2 state |
| 25 | BSP SD init (20MHz) instead of custom (400kHz) | Still fails regardless of clock speed |
| 26 | SSCMA client created before SD card mount | Still fails |
| 27 | Keep SD card mounted (don't unmount) — like monitor example | Still fails |
| 28 | Monitor example's exact sdkconfig.defaults | Still fails |
| 29 | **Built sscma_client_monitor example** | **WORKS with SD + Himax** — 10 FPS person detection |

### Phase 7: Additional configuration tests
| # | What | Result |
|---|------|--------|
| — | CPU frequency fixed to 240MHz (was silently 160MHz due to wrong config key) | No effect on Himax |
| — | Stripped all custom sdkconfig overrides | No effect |
| — | SPI2 bus free + reinitialize after SD unmount | No effect |

## sdkconfig Settings Tested

| Setting | Monitor (works) | Our firmware (fails) | Tested matching? |
|---------|----------------|---------------------|-----------------|
| CPU freq | 240 MHz | 240 MHz (fixed) | Yes |
| SPIRAM reserve | 262144 | 262144 | Yes |
| SPIRAM always internal | 1024 | 1024 | Yes |
| SPI_MASTER_IN_IRAM | not set | tried both | No effect |
| BSP_LCD_SPI_DMA_SIZE_DIV | 1 (default) | tried 1, 12, 16, 24 | No effect |
| LVGL_DRAW_BUFF_HEIGHT | 412 (full screen) | tried 40 and default | No effect |
| LVGL_PORT_TASK_AFFINITY | no affinity | tried both | No effect |
| SSCMA task stack | 4096 (default) | tried 4096 and 10240 | No effect |
| SSCMA task affinity | no affinity | tried both | No effect |
| FREERTOS_HZ | 1000 | default (100) | Not tested yet |

## What's Different Between Monitor Example and Our Firmware

Same BSP, same hardware, same SD card, same SPI2 bus. Differences:
1. **Our app has more tasks running** — renderer (10 FPS LVGL), web server (HTTP), agent state, event queue
2. **Our LVGL rendering pattern differs** — continuous sprite animation vs static image display
3. **WiFi is active** — HTTP server handles requests concurrently with SPI operations
4. **Our firmware binary is larger** — more code, more memory usage
5. **FREERTOS_HZ** — monitor uses 1000, we use default (100). Not yet tested.

## Phase 8: Root Cause Found — SD Card MISO Contention (Attempts 30-40)

### Root Cause

**The physical SD card, once initialized into SPI mode, does NOT tri-state its MISO output when CS is deasserted.** It continues driving MISO high (0xFF), causing bus contention with the Himax camera on the same SPI2 bus. This is a known hardware behavior of some SD cards in SPI mode.

### Fix: Power Off SD Card After Loading Sprites

The working sequence:
1. Mount SD card on SPI2 (`bsp_sdcard_init_default()`)
2. Load sprites to SPIRAM
3. Unmount SD card (`bsp_sdcard_deinit_default()`)
4. **Power OFF SD card** via IO expander (`BSP_PWR_SDCARD = 0`)
5. Hold GPIO46 (SD CS) high
6. Power cycle Himax to clear stale state
7. Init SSCMA client → **camera works!**

### Experiment Results

| # | Experiment | Camera | Conclusion |
|---|-----------|--------|------------|
| 30 | Monitor + SD card init (`bsp_sdcard_init_default()`) | **FAIL** | SD card breaks Himax even in monitor example |
| 31 | Monitor WITHOUT SD card init | **PASS** | Confirms SD card is the issue |
| 32 | SD card in slot, CS held high, not initialized | **PASS** | Physical presence doesn't matter |
| 33 | Himax CS held high during SD init | **FAIL** | Not a floating CS issue |
| 34 | Dummy SPI device on SPI2 (no SD card traffic) | **PASS** | SPI device registration alone is fine |
| 35 | CMD0 sent to SD card + device removed | **PASS** | Simple SPI traffic is fine |
| 36 | Full SD mount + deinit + MISO clock flush | **FAIL** | Clock pulses don't release MISO |
| 37 | Full SD mount + deinit + Himax power cycle | **FAIL** | Not ESP32 or Himax state issue |
| 38 | Full SD mount + deinit + SPI2 bus free/reinit | **FAIL** | Not SPI bus state issue |
| 39 | Himax working, then SD card mounted live | **FAIL** | Images stop immediately on mount |
| 40 | **Full SD mount + deinit + SD POWER OFF** | **PASS** | Power off releases MISO! |

### Key Insight from Experiment 39

Camera was running at 10 FPS. SD card was mounted at runtime. **All image flow stopped immediately.** The SD card mount disrupted SPI2 bus mid-stream. After SD deinit, images did NOT resume — proving the SD card physically holds MISO even after ESP32 releases it.

### Remaining Issue: SSCMA Library Heap Crash

The SPI2 fix works — Himax connects (`on_connect` fires), but the SSCMA client library crashes in `mapping_insert` (`tlsf.c:266`) — a heap corruption in the TLSF allocator. This happens because:

1. Himax has a person detection model in its flash that auto-runs on power-up
2. The SSCMA monitor/process tasks start receiving unsolicited inference events
3. The library's reply parser can't match these to any pending request
4. Sustained flood of "request not found: MODEL" warnings
5. Heap corruption → Guru Meditation Error (LoadProhibited)

This is a bug in the SSCMA library's handling of unsolicited events, NOT an SPI2 issue.

### Potential Solutions for the Heap Crash

1. **Patch SSCMA library** to safely discard unsolicited events without heap corruption
2. **Flash Himax with no-autorun firmware** so it boots clean without inference
3. **Drain the SPI RX buffer** before SSCMA internal tasks start (requires library modification)
4. **Use SPIFFS instead of SD card** — eliminates the need for SPI2 bus sharing entirely

## Updated Status

- **SPI2 bus conflict: SOLVED** — Power off SD card after loading sprites
- **SSCMA heap crash: OPEN** — Library bug when handling stale autorun data
- **Himax communication: WORKING** — `on_connect` fires, model info retrieved successfully
