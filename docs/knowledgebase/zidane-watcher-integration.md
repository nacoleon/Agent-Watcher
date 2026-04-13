# Zidane Watcher Integration — Lessons Learned

Everything discovered during the Zidane × OpenClaw Watcher integration build.

## Architecture

- **Three components**: Watcher firmware (ESP32-S3) ← HTTP → Bridge (Node.js on Mac) ← tool calls → Zidane (OpenClaw agent)
- Bridge runs on port 3847, Watcher on port 80 (IP: 10.0.0.40, mDNS: zidane.local)
- Bridge writes presence events to `~/.openclaw/watcher-events.json` and context to `~/.openclaw/watcher-context.json`
- Bridge runs as LaunchAgent `ai.openclaw.watcher-bridge`

## Firmware API

| Endpoint | Method | Body | Purpose |
|----------|--------|------|---------|
| `/api/status` | GET | — | Returns agent_state, person_present, last_message, uptime, wifi_rssi |
| `/api/agent-state` | PUT | `{"state":"working"}` | Set agent state |
| `/api/message` | POST | `{"text":"...","level":"info"}` | Show FF9 dialog box |

Valid states: idle, working, waiting, alert, greeting, sleeping, reporting
Valid levels: info, success, warning, error

## mDNS Resolution Issue

Node.js `http.request` with `.local` mDNS hostnames times out. The bridge config uses the direct IP (`http://10.0.0.40`) instead of `http://zidane.local`. If DHCP assigns a different IP, update `bridge/config.json`.

## LVGL 8.x Color Gotchas

- `LV_COLOR_MAKE(r, g, b)` macro expands to a compound literal `{{...}}` which **cannot** be used in:
  - Static array initializers (`static const lv_color_t arr[] = {...}`)
  - Function arguments (the commas inside the struct confuse the preprocessor)
- **Fix**: Use `lv_color_hex(0xRRGGBB)` for function calls, and `uint32_t` hex values for static arrays with runtime `lv_color_hex()` conversion
- `lv_font_montserrat_12` is not enabled by default — use `lv_font_montserrat_14` unless you add `CONFIG_LV_FONT_MONTSERRAT_12=y` to sdkconfig.defaults

## FFRK Sprite Sheet Layout

Source: `zidane-dashboard/zidane_spritesheet.png` (384x256 RGBA)
- Left half = normal Zidane, right half = Trance (pink) — never use Trance frames
- Main character sprites are **16x24 pixels** (some special poses are 15x24)
- No strict grid — sprites are irregularly packed, use flood-fill to find bounding boxes
- See `docs/knowledgebase/zidane-sprite-catalog.md` for complete frame reference

### White/Ghost Frames to Avoid

These are KO/petrify/ghost versions with pure white pixels (brightness >200, saturation <0.1):
- `(1,64)`, `(18,64)` — ghost side-facing
- `(52,89)` — ghost action
- `(69,114)` — ghost slash
- `(101,51)`, `(129,51)`, `(129,2)` — white standing
- `(129,126)` — gray petrify

### Sleeping Frames

The "eyes closed" sprites at `(1,200)` and `(17,200)` are actually **side-facing hurt/KO poses**, not front-facing sleeping. For sleeping, use front-facing idle `(1,14)` as a still frame with a code-drawn ZZZ overlay.

## Pictlogica FF Backgrounds

Source: `zidane-dashboard/backgrounds.png` (1447x2453 RGBA)
- Grid: 6 columns × 14 rows = 84 backgrounds
- Cell size: 240×170 pixels
- 1px white separators between cells
- Column starts: [1, 242, 483, 724, 965, 1206]
- Row starts: [1, 172, 343, 514, 685, 856, 1027, 1198, 1369, 1540, 1711, 1882, 2053, 2224]
- Index formula: `idx = row * 6 + col`

### Excluded Backgrounds

| # | Description | Reason |
|---|-------------|--------|
| 9 | Snowy/icy field | Doesn't fit any state |
| 14 | Blue vortex + fire/lava | Too chaotic |
| 18 | Christmas scene | Seasonal, out of place |
| 19 | Clouds with chain archway | Doesn't fit working |
| 21 | Golden sun rays | Too abstract |
| 22 | Town with carousel | Doesn't fit working |
| 23 | Blue crystal interior | Too cool for reporting |
| 65 | Ornate brick/carpet | Too busy |
| 67 | Rocky cliff with water | Doesn't fit working |
| 73 | Ship deck with clouds | Doesn't fit working |
| 82 | Stairs with door | Too specific |

## Dashboard Preview Server

- `zidane-dashboard/server.py` — runs on port 8091
- Canvas is 412×412 to match Watcher hardware exactly
- Sprite scale: 4x (16×24 → 64×96 pixels on canvas)
- Backgrounds stretch to fill 412×412 from 240×170 source
- State auto-cycling stops when user manually clicks a state button
- FF9 dialog box: grainy gray background (deterministic dot pattern, not random per frame), rendered on canvas above Zidane with speech tail triangle
- Dialog box grows dynamically (max 8 lines, word-wrap at 36 chars/line)
- Sending a message switches to reporting state

## SD Card File Structure

```
/sdcard/characters/zidane/
  sprite_sheet.raw    # RGB565+Alpha (3 bytes/pixel), converted from PNG
  frames.json         # Animation frame coordinates
  portrait.raw        # Dialog box face (future)
```

## ESP32 Watcher Crash

The Watcher crashed (WiFi connection reset) after receiving multiple rapid API calls while no sprites were loaded (NULL sprite data). Always ensure sprites are on the SD card before testing API endpoints. The renderer's `update_frame()` checks for null animation but the behavior tick may still run.

## Build Reminders

- Must build from `/tmp/pokewatcher-build/` (space in project path breaks linker)
- Must move `SenseCAP-Watcher-Firmware/` to `_SDK_BAK` during build (linker resolves through spaced path)
- Delete `sdkconfig` + `build/` when changing `sdkconfig.defaults` or `config.h`
- Flash with `app-flash` only (preserves NVS + nvsfactory)
- Serial port: `/dev/cu.usbmodem5A8A0533623` (the 623 one, not 621)
