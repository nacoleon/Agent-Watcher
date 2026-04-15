# Project Status — Zidane Watcher

Last updated: 2026-04-14

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
- FF9 dialog box at top of screen (shows on message, auto-hides after timeout, no opacity fade)
- Background image loading: pre-loads tile from SD card at boot, scaled to 412x412 in PSRAM
- Background switching via PUT /api/background with web UI controls (prev/next/set)
- Background auto-rotation every 5 min with strip wipe transition (20 rows/frame, ~2s wipe)
- Dialog pagination: 80 chars/page, knob wheel scrolls pages, page indicator (1/3), 1000 char max
- Knob button: short press dismisses dialog, long press (6s) reboots, press while display off wakes
- Display sleep: 3min idle → sleeping state → 15s later display off. Also sleeps 15s after sleeping state set via API
- Speaker muted at boot (bsp_codec_mute_set) to prevent idle amp pops
- Hardware-like reboot from web UI (power cycles LCD/AI chip before restart)
- API endpoints: GET /api/status, PUT /api/agent-state, POST /api/message, POST /api/reboot, PUT /api/background
- Web UI at http://10.0.0.40 with state buttons (including "down", "wakeup"), message input, background controls, reboot
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

### Bridge (bridge/)
- Node.js Express on port 3847, runs as LaunchAgent
- Tool endpoints: /tools/display_message, /tools/set_state, /tools/get_status, /tools/notify
- Presence poller (5s, debounced) writes to ~/.openclaw/watcher-events.json
- Context file at ~/.openclaw/watcher-context.json

### OpenClaw Integration
- watcher.* tools registered in ~/clawd/TOOLS.md
- Desk context in ~/clawd/HEARTBEAT.md
- LaunchAgent: ai.openclaw.watcher-bridge

## What Needs To Be Done

### SPI Flush Issue (Mitigated)
- [x] **SPD2010 driver retry** — Retries full CASET+RASET+RAMWR on failure with 10ms delay
- [x] **LVGL flush deadlock fix** — `lv_disp_flush_ready()` called on error to prevent permanent mutex lock
- [x] **lvgl_port_lock timeout** — 500ms timeout instead of wait-forever
- [ ] **Root cause still open** — ESP-IDF SPI driver race condition between polling and queued transactions. See `docs/knowledgebase/spi-flush-stall-bug.md`

### Background Images
- [x] Pre-load single background at boot — DONE
- [x] Runtime background switching via web UI with strip wipe — DONE
- [x] Auto-rotate backgrounds every 5 minutes — DONE (72 tiles, random selection)
- [x] Double-buffered staging for SPI-safe transitions — DONE

### End-to-End Integration
- [ ] **End-to-end bridge test** — Verify Zidane agent can call bridge tools and Watcher responds
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

### Bridge Improvements
- [ ] Add /tools/reboot endpoint to bridge
- [ ] Health monitoring — restart bridge if Watcher goes offline
- [ ] mDNS resolution too slow for Node.js — using direct IP (10.0.0.40)

### Future Features
- [ ] Audio/speaker output (codec initialized and muted, ready for use — unmute with `bsp_codec_mute_set(false)`)
- [ ] RGB LED integration (hardware ready)
- [ ] BLE phone connectivity
- [ ] Touch screen interaction

## Critical Knowledge for Future Agents

1. **READ `docs/knowledgebase/spi-bus-conflict.md` AND `docs/knowledgebase/display-freeze-root-cause.md`** — Display freeze has TWO causes: (a) per-frame LVGL style changes on large objects, (b) intermittent SPI driver race condition
2. **NEVER call `lv_obj_set_style_*()` every frame on objects > 100x100 pixels** — This overwhelms the SPI flush queue and freezes permanently. Use `lv_img_set_src` and `lv_obj_set_pos` for per-frame updates.
3. **Build from /tmp** — Space in project path breaks linker. See `docs/knowledgebase/building-and-flashing-firmware.md`
4. **Move SDK aside during full rebuilds** — `SenseCAP-Watcher-Firmware/` in project root causes linker path issues. Move to `_SDK_BAK` before build, restore after.
5. **Delete sdkconfig when changing defaults** — Old sdkconfig overrides sdkconfig.defaults silently
6. **Serial port is /dev/cu.usbmodem5A8A0533623** — The other port (621) doesn't work for flashing
7. **Flash with app-flash only** — Preserves NVS and nvsfactory partitions
8. **Renderer is single-threaded** — All LVGL calls happen in renderer_task. Other threads set volatile flags only.
9. **Speaker is muted** — Codec initialized at boot, muted with `bsp_codec_mute_set(true)`. Unmute with `bsp_codec_mute_set(false)` when audio needed.
10. **Himax is disabled** — Commented out in app_main.c. Was causing SPI collision but intermittent SPI issue exists even without it.
