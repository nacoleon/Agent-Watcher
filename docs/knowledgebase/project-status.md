# Project Status — Zidane Watcher

Last updated: 2026-04-17

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
- Himax camera SPI2 fix: SD card powered off after sprite loading frees MISO for Himax
- Himax camera FULLY WORKING: 4 SSCMA library bugs fixed, CONFIG_FREERTOS_HZ=1000 was root cause
- AI models: Person Detection (model 1), Pet Detection (model 2), Gesture Detection (model 3) — all verified
- Auto model switching: person detected → gesture mode, 20min idle → back to person mode
- Gesture detection: Rock/Paper/Scissors with confidence thresholds (85%), 4-frame confirmation, 6s re-detect
- Rock false-positive filter: requires box width >= 130px (body/face are 64-100px, real fist is 144+)
- Object presence: any camera detection counts as "arrived", 3min timeout for "left"
- Purple double LED flash on confirmed gesture, display wakes on gesture
- Gesture log with box sizes in web UI (20-entry circular buffer)
- OTA Himax firmware flash from SD card (factory firmware v2024.08.16 restored)
- 34 background tiles pre-loaded into PSRAM at boot (2.7MB), auto-rotate + manual switch work after SD power-off
- Web UI: AI Model section with 3-way toggle (Person/Pet/Gesture), expandable logs, background label + prev/next with real tile list
- API: PUT /api/model for model switching, gesture_log + active_model in GET /api/status
- SPI-safe renderer: prepare/commit split, single LVGL lock per frame (500ms timeout), dirty flags
- Push-to-talk voice input: double-click knob → 10s recording (16kHz/16-bit mono) → stored in PSRAM → daemon polls, fetches WAV, whisper.cpp transcribes → OpenClaw via `openclaw agent`. RGB LED: blue (recording), green (ready)
- TTS speaker output: `watcher__speak` MCP tool → Piper TTS on Mac → resample 22050→16000 Hz → stream PCM to `POST /api/audio/play` → speaker. Voice selection in web UI with NVS persistence
- Standalone watcher daemon: LaunchAgent (`ai.openclaw.watcher-daemon`) polls 24/7 for voice audio, presence events, independent of MCP sessions

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
- Audio receiver: Express HTTP server on :3848, whisper-node (whisper.cpp) transcription, voice_input logging messages to OpenClaw

### OpenClaw Integration
- MCP server registered: `openclaw mcp set watcher ...`
- Zidane auto-discovers watcher__* tools via MCP protocol
- HEARTBEAT.md updated for MCP presence notifications
- Old bridge (Express on port 3847) and LaunchAgent retired

## What Needs To Be Done

### SPI Flush Issue (RESOLVED)
- [x] **Root cause found** — Error 0x101 was `ESP_ERR_NO_MEM` (DMA bounce buffer allocation failure). LVGL draw buffers in PSRAM need DMA bounce buffers in internal SRAM; Himax SSCMA client fragments the DMA heap at runtime.
- [x] **Fix applied** — Pre-allocated 33KB DMA bounce buffer at boot (before SSCMA init). Custom LVGL flush callback copies PSRAM→DMA buffer→SPI. No runtime DMA allocation needed. Works with Himax camera running.
- [x] **lvgl_port_lock timeout** — 500ms timeout (safety net, rarely triggered now)

### Background Images
- [x] Pre-load single background at boot — DONE
- [x] Runtime background switching via web UI with strip wipe — DONE
- [x] Auto-rotate backgrounds every 5 minutes — DONE (72 tiles, random selection)
- [x] Double-buffered staging for SPI-safe transitions — DONE

### Pending

- [ ] **Gesture actions** — Zidane should react when gestures are detected (animation, dialog, state change). The gesture events (`PW_EVENT_GESTURE_DETECTED`) fire in `agent_state.c` but currently only log — no visual reaction on the sprite yet.
- [ ] **MCP person/gesture notifications** — Wire person arrived/left and gesture events to OpenClaw via the MCP server so Zidane can react conversationally. The MCP server (`watcher-mcp/`) has a presence poller but it's not connected to the firmware's real detection events.

### Completed (no longer needed)
- [x] Dashboard proxy (8091 → 10.0.0.40) — skipped, not needed
- [x] Custom pixel art — skipped, FFRK sprites are sufficient
- [x] Portrait sprite for dialog box — skipped, single character doesn't need it
- [x] Sleeping animation fix — done
- [x] Himax camera — FULLY WORKING (root cause: CONFIG_FREERTOS_HZ=100)
- [x] RGB LED — state blinks + purple double flash on gesture
- [x] Health monitoring — heartbeat + auto-down/recover

### Firmware Polish
- [ ] Remove debug logging from prepare_frame/commit_frame
- [ ] Auto-show dialog for alert/greeting/reporting states with default messages

### Future Features
- [x] Audio/speaker — mic input (push-to-talk) and speaker output (TTS via Piper) both working
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
