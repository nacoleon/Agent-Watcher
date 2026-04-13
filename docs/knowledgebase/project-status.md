# Project Status — Zidane Watcher

Last updated: 2026-04-13

## What's Done

### Firmware (pokewatcher/)
- Agent state engine: 7 states (idle, working, waiting, alert, greeting, sleeping, reporting)
- FF9 dialog box renderer (LVGL, grainy gray texture)
- API endpoints: GET /api/status, PUT /api/agent-state, POST /api/message, POST /api/reboot
- Sprite system using FFRK Zidane (16x24 at 4x scale)
- Web UI at http://10.0.0.40 with state controls, message input, reboot button
- WiFi auto-connect (YOUR_WIFI_SSID, IP: 10.0.0.40, mDNS: zidane.local)
- Person detection via Himax camera
- Display sleep after 5 min idle, wake on API calls
- SPI-safe renderer: prepare/commit split, single LVGL lock per frame, dirty flags

### Bridge (bridge/)
- Node.js Express on port 3847, runs as LaunchAgent
- Tool endpoints: /tools/display_message, /tools/set_state, /tools/get_status, /tools/notify
- Presence poller (5s, debounced) writes to ~/.openclaw/watcher-events.json
- Context file at ~/.openclaw/watcher-context.json

### OpenClaw Integration
- watcher.* tools registered in ~/clawd/TOOLS.md
- Desk context in ~/clawd/HEARTBEAT.md
- LaunchAgent: ai.openclaw.watcher-bridge

### Dashboard Preview (zidane-dashboard/)
- Runs on localhost:8091 (python3 zidane-dashboard/server.py)
- FFRK Zidane sprite with all animations
- 84 Pictlogica FF backgrounds (12 per state, curated)
- FF9 dialog box with grainy gray, speech tail, dynamic height
- Matches Watcher display at 412x412

### SD Card (sdcard_prep/characters/zidane/)
- overworld.raw — sprite sheet (RGB565)
- frames.json — 14 animations
- bg/ — 72 background tiles (.raw, unused by firmware currently)

## What Needs To Be Done

### Immediate
- [ ] **Revert IDLE bg color** — Change `0xFF0000` (test red) back to `0xFDE8C8` (warm peach) in renderer.c STATE_BG_COLORS
- [ ] **Test state changes from web UI** — Verify bg colors change for all 7 states without freeze
- [ ] **End-to-end bridge test** — Verify Zidane agent can call bridge tools and Watcher responds

### Background Images on Firmware
- [ ] Background tile loading is disabled (#if 0 in renderer.c) because SD card reads at 400kHz cause SPI collision with Himax
- [ ] **Option A**: Pre-cache backgrounds in PSRAM at boot (before Himax starts). ~340KB per scaled tile, could cache 1 per state = 2.4MB of 8MB PSRAM
- [ ] **Option B**: Increase SD card SPI clock (needs faster card, currently 400kHz for old 256MB card)
- [ ] **Option C**: Dedicated background task with SPI mutex arbitration

### Sprite Improvements
- [ ] Custom Zidane pixel art (currently using FFRK rips from Spriters Resource)
- [ ] Portrait sprite for dialog box (spec calls for face next to text)
- [ ] More animation variety (current side-facing reuses same 2 frames)

### Firmware Stability
- [ ] Remove debug logging from prepare_frame/commit_frame (s_prepare_call_count, s_commit_call_count)
- [ ] Investigate if display still freezes under sustained API load
- [ ] Add null checks in renderer for edge cases (no sprite loaded, no animation)

### Bridge Improvements
- [ ] Add /tools/reboot endpoint to bridge (currently only direct Watcher API)
- [ ] Health monitoring — restart bridge if Watcher goes offline
- [ ] mDNS resolution too slow for Node.js — using direct IP (10.0.0.40), fragile with DHCP

### Dashboard
- [ ] Dashboard (8091) should proxy to real Watcher (10.0.0.40) instead of mock data
- [ ] Sync dashboard state controls with actual Watcher state

### Future Features (from design spec)
- [ ] Audio/speaker output (hardware ready)
- [ ] RGB LED integration (hardware ready)
- [ ] BLE phone connectivity
- [ ] ZZZ overlay on Watcher display for sleeping state
- [ ] Touch screen interaction

## Critical Knowledge for Future Agents

1. **READ `docs/knowledgebase/spi-bus-conflict.md` BEFORE touching renderer.c** — SPI collision between LCD and Himax will cause permanent freeze if you call LVGL from wrong thread or too frequently
2. **Build from /tmp** — Space in project path breaks linker. See `docs/knowledgebase/building-and-flashing-firmware.md`
3. **Move SDK aside during build** — `SenseCAP-Watcher-Firmware/` in project root causes linker path issues
4. **Delete sdkconfig when changing defaults** — Old sdkconfig overrides sdkconfig.defaults silently
5. **Serial port is /dev/cu.usbmodem5A8A0533623** — The other port (621) doesn't work for flashing
6. **Flash with app-flash only** — Preserves NVS and nvsfactory partitions
7. **Renderer is single-threaded** — All LVGL calls happen in renderer_task. Other threads set volatile flags only.
