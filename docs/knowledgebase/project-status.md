# Project Status — Zidane Watcher

Last updated: 2026-04-16

## What's Done

### Firmware (pokewatcher/)
- Agent state engine: 9 states (idle, working, waiting, alert, greeting, sleeping, reporting, down, wakeup)
- "Down" state for when OpenClaw is offline (laying-down sprite pose, dark bg)
- Per-state sprite animations: each state uses dedicated animation from frames.json
- Mirror support: right-facing sprites flip left-facing frames horizontally
- Per-frame custom sizes: frames.json supports w/h overrides (e.g., 25x13 "down" pose)
- Fixed-position states: alert/reporting/greeting/working/waiting at bottom-center, sleeping/down slightly below center
- Smooth walk-to-position transitions: sprite walks between ANY two states (not just from idle)
- Wakeup animation: 7-frame sequence plays automatically when display wakes from off, queues pending state for after
- Anti-reversal walking: idle walk never picks opposite direction (no up→down→up jitter)
- ZZZ overlay for sleeping: animated "z Z z" text cycling, Montserrat 28 font, follows sprite position
- FF9 dialog box at top of screen (stays until knob press dismiss, no auto-timeout)
- Background image loading: pre-loads tile from SD card at boot, scaled to 412x412 in PSRAM
- Background switching via PUT /api/background with web UI controls (prev/next/set)
- Background auto-rotation every 5 min with strip wipe transition (20 rows/frame, ~2s wipe)
- Dialog pagination: 80 chars/page, knob wheel scrolls pages, page indicator (1/3), 1000 char max
- Knob button: short press dismisses dialog + resets to idle, long press (6s) reboots, press while display off wakes
- Display sleep: 3min idle → sleeping state → 15s later display off. Also sleeps 15s after sleeping state set via API
- Speaker muted at boot (bsp_codec_mute_set) to prevent idle amp pops
- Hardware-like reboot from web UI (power cycles LCD/AI chip before restart)
- RGB LED (WS2812): blinks state-specific color every 5s — red (alert/down), orange (waiting), pink (greeting), green (reporting). Off on knob dismiss, display sleep, and non-blinking states.
- OpenClaw heartbeat: POST /api/heartbeat endpoint, 1.5hr timeout triggers "down" state, auto-recovers to idle on next beat. Web UI shows heartbeat status and last 5 timestamps.
- API endpoints: GET /api/status, PUT /api/agent-state, POST /api/message, POST /api/reboot, PUT /api/background, POST /api/heartbeat
- Web UI at http://10.0.0.40 with state buttons (including "down", "wakeup"), message input, background controls, heartbeat log, reboot
- WiFi auto-connect (YOUR_WIFI_SSID, IP: 10.0.0.40, mDNS: zidane.local)
- Himax camera disabled (SPI collision with LCD confirmed via diagnostic logs)
- SPI-safe renderer: prepare/commit split, single LVGL lock per frame (500ms timeout), dirty flags

### Dashboard Preview (zidane-dashboard/)
- Runs on localhost:8091 (python3 zidane-dashboard/server.py)
- FFRK Zidane sprite with corrected animations per state
- Mirror support for right-facing sprites
- 72 curated backgrounds rotating every 5 minutes (single pool, not per-state)
- States only control sprite animation, backgrounds are independent
- Sprite catalog page at /sprites.html with hover coordinates, animation previews
- FF9 dialog box with grainy gray, speech tail, dynamic height
- Matches Watcher display at 412x412

### SD Card (sdcard_prep/characters/zidane/)
- overworld.raw — sprite sheet (RGB565)
- frames.json — 16 animations (idle_down/up/left/right, walk_down/up/left/right, greeting, sleeping, working, alert, reporting, waiting, down, wakeup) with mirror flag and per-animation speed support
- bg/ — 72 background tiles (.raw, 240x170 RGB565)

### MCP Server (watcher-mcp/)
- Stdio MCP server, spawned by OpenClaw gateway on demand
- Tools: display_message, set_state, get_status, notify, reboot, get_queue, heartbeat
- Message queue: FIFO (max 10), states paired with messages, sends next on dismiss
- Dismiss detection via dismiss_count counter (no polling gap bugs)
- Presence poller (5s, 2-poll debounce) sends MCP notifications: person_arrived/person_left
- Read confirmations: message_read/queue_empty notifications to Zidane
- Status resource: watcher://status with live device state

### OpenClaw Integration
- MCP server registered: `openclaw mcp set watcher ...`
- Zidane auto-discovers watcher__* tools via MCP protocol
- HEARTBEAT.md updated for MCP presence notifications
- Old bridge (Express on port 3847) and LaunchAgent retired

## What Needs To Be Done

### SPI Flush Issue (RESOLVED)
- [x] **Root cause found** — Error 0x101 was `ESP_ERR_NO_MEM` (DMA bounce buffer allocation failure), NOT `ESP_ERR_INVALID_STATE`. LVGL draw buffers in PSRAM require DMA bounce buffers in internal SRAM; SPI chunks exceeded available contiguous DMA memory.
- [x] **Fix applied** — `CONFIG_BSP_LCD_SPI_DMA_SIZE_DIV=12` (chunks ~28KB, fits in available ~31KB DMA blocks) + `CONFIG_LVGL_DRAW_BUFF_HEIGHT=40` (render in 40-row strips). See `docs/knowledgebase/spi-flush-stall-bug.md`
- [x] **lvgl_port_lock timeout** — 500ms timeout (safety net, rarely triggered now)

### Background Images
- [x] Pre-load single background at boot — DONE
- [x] Runtime background switching via web UI with strip wipe — DONE
- [x] Auto-rotate backgrounds every 5 minutes — DONE (72 tiles, random selection)
- [x] Double-buffered staging for SPI-safe transitions — DONE

### End-to-End Integration
- [x] **End-to-end MCP test** — Zidane calls watcher__* tools via MCP, Watcher responds (verified 2026-04-15)
- [ ] Dashboard (8091) should proxy to real Watcher (10.0.0.40) instead of mock data

### Sprite Improvements
- [ ] Custom Zidane pixel art (currently using FFRK rips from Spriters Resource)
- [ ] Portrait sprite for dialog box
- [ ] Fix sleeping animation frames (currently uses front-facing, should use spc1/spc2 at y=200)

### Firmware Polish
- [ ] Remove debug logging from prepare_frame/commit_frame (s_prepare_call_count, s_commit_call_count)
- [ ] Remove heap monitoring logs (s_heap_log_counter)
- [ ] Auto-show dialog for alert/greeting/reporting states with default messages
- [ ] Re-enable Himax when SPI issue is resolved

### MCP Server Improvements
- [ ] Adaptive polling — faster when messages are queued, slower when idle
- [x] Health monitoring — heartbeat tool + 1.5hr timeout auto-down + auto-recover

### Future Features
- [ ] Audio/speaker output (codec initialized and muted, ready for use — unmute with `bsp_codec_mute_set(false)`)
- [x] RGB LED integration — state blinks (alert/down=red, waiting=orange, greeting=pink, reporting=green)
- [ ] BLE phone connectivity
- [ ] Touch screen interaction

## Critical Knowledge for Future Agents

1. **READ `docs/knowledgebase/spi-bus-conflict.md` AND `docs/knowledgebase/spi-flush-stall-bug.md`** — Display issues had TWO causes: (a) per-frame LVGL style changes on large objects (fixed), (b) DMA bounce buffer OOM from oversized SPI chunks (fixed with `CONFIG_BSP_LCD_SPI_DMA_SIZE_DIV=12`)
2. **NEVER call `lv_obj_set_style_*()` every frame on objects > 100x100 pixels** — This overwhelms the SPI flush queue and freezes permanently. Use `lv_img_set_src` and `lv_obj_set_pos` for per-frame updates.
3. **Build from /tmp** — Space in project path breaks linker. See `docs/knowledgebase/building-and-flashing-firmware.md`
4. **Move SDK aside during full rebuilds** — `SenseCAP-Watcher-Firmware/` in project root causes linker path issues. Move to `_SDK_BAK` before build, restore after.
5. **Delete sdkconfig when changing defaults** — Old sdkconfig overrides sdkconfig.defaults silently
6. **Serial port is /dev/cu.usbmodem5A8A0533623** — The other port (621) doesn't work for flashing
7. **Flash with app-flash only** — Preserves NVS and nvsfactory partitions
8. **Renderer is single-threaded** — All LVGL calls happen in renderer_task. Other threads set volatile flags only.
9. **Speaker is muted** — Codec initialized at boot, muted with `bsp_codec_mute_set(true)`. Unmute with `bsp_codec_mute_set(false)` when audio needed.
10. **Himax is disabled** — Commented out in app_main.c. SPI collision was a red herring; the real issue was DMA memory exhaustion (now fixed).
