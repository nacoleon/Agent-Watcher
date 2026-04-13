# Zidane × Watcher Integration Design

**Date:** 2026-04-13
**Status:** Draft
**Goal:** Transform the SenseCap Watcher from a Pokémon desk pet into Zidane's physical body — a visual representation of the OpenClaw lead agent that moves, reacts to presence, and communicates via FF9-style dialogs.

---

## Architecture

Three components connected via HTTP:

```
┌─────────────────┐    tool calls     ┌──────────────────┐    HTTP API     ┌─────────────────┐
│  Zidane Agent    │ ───────────────→  │  Watcher Bridge   │ ────────────→  │  SenseCap Watcher│
│  (OpenClaw)      │                   │  (Node.js on Mac) │                │  (ESP32-S3)      │
│                  │ ←─────────────── │                   │ ←──────────── │                  │
│  ~/clawd/        │  events/context   │  bridge/ in repo  │  poll status   │  10.0.0.40:80    │
└─────────────────┘                   └──────────────────┘                └─────────────────┘
                                             port 3847
```

### Why This Shape

- **ESP32 is resource-constrained** — no WebSocket client, no Ed25519 auth, limited TLS. It stays as a simple HTTP server.
- **Zidane already knows how to use tools** — registering `watcher.*` in TOOLS.md means zero new agent protocol work.
- **Bridge is thin** — Express server + poller + file writer. ~200 lines of TypeScript.
- **Decoupled** — each component can be developed, tested, and restarted independently.

### Communication Direction

| Flow | Mechanism | Latency |
|------|-----------|---------|
| Zidane → Watcher | Tool call → Bridge HTTP → Watcher HTTP | ~100ms |
| Watcher → Zidane (event) | Bridge polls Watcher → writes event file → Zidane reads on heartbeat | 5-15s |
| Watcher → Zidane (context) | Bridge writes context file → Zidane reads in heartbeat | refreshed every 5s |

---

## Component 1: Watcher Firmware Changes

### Agent State Engine

Replaces the Pokémon mood engine. States are driven externally by Zidane via the bridge, not internally by timers.

| State | Background | Sprite Behavior | Dialog Border | Trigger |
|-------|-----------|-----------------|---------------|---------|
| IDLE | `#FDE8C8` warm peachy | Casual wander, 30% walk, 20% turn | Blue `#5566aa` | Default / Zidane sets after completing work |
| WORKING | `#E8F0E8` soft mint | Focused pacing, 50% walk, faster 1.5x | Blue `#5566aa` | Zidane sets when executing tasks |
| WAITING | `#E8E0F0` light lavender | Standing still, occasional turn | Yellow `#aaaa55` | Zidane sets when awaiting approval |
| ALERT | `#402020` dark crimson | Fast bounce, 80% walk, 3x speed | Red `#aa5555` | Zidane sets on failures/urgent |
| GREETING | `#FFF0C0` bright yellow | Bounce toward center, wave anim | Blue `#5566aa` | Zidane sets on person arrival |
| SLEEPING | `#404060` dark slate | Minimal move, ZZZ overlay, 0.5x | Dim `#334466` | Zidane sets when user leaves |
| REPORTING | `#FDE8C8` warm peachy | Stand center, face forward | Green `#55aa66` | Zidane sets when delivering updates |

**Timeout fallbacks** (if bridge goes offline):
- GREETING → IDLE after 10 seconds
- ALERT → IDLE after 60 seconds
- All other states persist until changed

**Person detection stays local** — the Himax camera still runs on-device with the same debounced detection. The bridge reads `person_present` from the status API.

### New API Endpoints

#### `PUT /api/agent-state`

```json
// Request
{"state": "working"}

// Response
{"ok": true, "state": "working"}
```

Valid states: `idle`, `working`, `waiting`, `alert`, `greeting`, `sleeping`, `reporting`.

#### `POST /api/message`

```json
// Request
{"text": "Deploy finished!", "level": "success"}

// Response
{"ok": true}
```

Levels and their dialog border colors:
- `info` — blue `#5566aa` (default)
- `success` — green `#55aa66`
- `warning` — yellow `#aaaa55`
- `error` — red `#aa5555`

Message constraints:
- Max 80 characters (2 lines on display)
- Auto-dismiss after 10 seconds with fade
- New message replaces current (no queue)

#### `GET /api/status` (extended)

```json
{
  "agent_state": "working",
  "person_present": true,
  "last_message": "Deploy finished!",
  "uptime_seconds": 3600,
  "wifi_rssi": -65
}
```

### FF9 Dialog Box Renderer

Rendered with LVGL, positioned within the circular display safe zone.

**Layout:**
- **Position:** 18% from bottom edge, 15% side margins (74px from bottom, 62px from sides on 412px display)
- **Background:** Linear gradient `#1a1a4e` → `#0a0a2e`
- **Border:** 2px solid, color per message level
- **Portrait:** Zidane's face (32x32 scaled to 48x48) on the left side of the dialog box
- **Name label:** "Zidane:" in green `#7BE87B`, monospace font
- **Text:** White `#FFFFFF`, monospace, max 2 lines
- **Dismiss:** Fade out over 500ms after 10 second display

**Display mode switch:**
- **Overworld mode** (default): Zidane chibi sprite walks around full screen
- **Dialog mode** (on message): Sprite moves to upper third, dialog box appears in lower area with portrait inside the box

### Sprite System Refactor

Replace Pokémon-specific code with a generic character system.

**Directory change:** `/sdcard/pokemon/{id}/` → `/sdcard/characters/{id}/`

**Per-character files:**
```
/sdcard/characters/zidane/
  sprite_sheet.raw      # Overworld walk/idle frames (32x32 RGB565+Alpha)
  frame_manifest.json   # Animation definitions
  portrait.raw          # Dialog box face (32x32 RGB565+Alpha)
```

**Frame manifest format** (unchanged from current):
```json
{
  "animations": {
    "idle_down": {"frames": [{"x": 0, "y": 0}, {"x": 32, "y": 0}], "loop": true},
    "walk_down": {"frames": [{"x": 0, "y": 32}, {"x": 32, "y": 32}, {"x": 64, "y": 32}, {"x": 96, "y": 32}], "loop": true},
    "greeting": {"frames": [{"x": 0, "y": 128}, {"x": 32, "y": 128}, {"x": 64, "y": 128}], "loop": false},
    "sleeping": {"frames": [{"x": 0, "y": 160}, {"x": 32, "y": 160}], "loop": true}
  }
}
```

**State → animation mapping:**
| State | Primary Animation | Notes |
|-------|------------------|-------|
| IDLE | `idle_{direction}` / `walk_{direction}` | Random wander |
| WORKING | `walk_{direction}` | Faster, more walking |
| WAITING | `idle_down` | Mostly still |
| ALERT | `walk_{direction}` | Fast, bouncing |
| GREETING | `greeting` | Special animation, then IDLE |
| SLEEPING | `sleeping` | Minimal, ZZZ overlay |
| REPORTING | `idle_down` | Center, facing viewer |

---

## Component 2: Watcher Bridge (Node.js)

### Project Location

`bridge/` directory in this repo (`/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/bridge/`).

### Structure

```
bridge/
  src/
    index.ts          # Entry point, Express server setup
    watcher-client.ts # HTTP client for Watcher API
    presence.ts       # Poller + change detection
    events.ts         # Event file writer
    config.ts         # Configuration loader
  config.json         # Runtime config
  package.json
  tsconfig.json
```

### Tool API Server (Express, port 3847)

| Endpoint | Method | Body | Action |
|----------|--------|------|--------|
| `/tools/display_message` | POST | `{text, level}` | → Watcher `POST /api/message` |
| `/tools/set_state` | POST | `{state}` | → Watcher `PUT /api/agent-state` |
| `/tools/get_status` | GET | — | → Watcher `GET /api/status` (proxy) |
| `/tools/notify` | POST | `{text, level, state}` | → Watcher `PUT /api/agent-state` + `POST /api/message` |

All endpoints return JSON. Errors forwarded with HTTP status codes.

### Presence Poller

- Polls `http://zidane.local/api/status` every 5 seconds
- Tracks `person_present` boolean
- Debounce: requires 2 consecutive changed readings (10s confirmation) before firing event
- On change: writes to event file + updates context file

### Event File

**Path:** `~/.openclaw/watcher-events.json`

Append-only JSON lines. Zidane reads and clears on each heartbeat.

```jsonl
{"type": "presence_changed", "present": true, "timestamp": "2026-04-13T10:30:00Z"}
{"type": "presence_changed", "present": false, "timestamp": "2026-04-13T11:45:00Z"}
```

### Context File

**Path:** `~/.openclaw/watcher-context.json`

Overwritten every poll cycle. Zidane reads in heartbeat for desk awareness.

```json
{
  "person_present": true,
  "agent_state": "working",
  "last_message": "Deploy finished!",
  "last_message_at": "2026-04-13T10:30:00Z",
  "uptime_seconds": 3600,
  "wifi_rssi": -65,
  "bridge_uptime_seconds": 7200,
  "updated_at": "2026-04-13T10:30:05Z"
}
```

### Configuration

**`bridge/config.json`:**

```json
{
  "watcher_url": "http://zidane.local",
  "bridge_port": 3847,
  "poll_interval_ms": 5000,
  "debounce_count": 2,
  "events_file": "~/.openclaw/watcher-events.json",
  "context_file": "~/.openclaw/watcher-context.json"
}
```

### LaunchAgent

Installed as `ai.openclaw.watcher-bridge` at `~/Library/LaunchAgents/ai.openclaw.watcher-bridge.plist`. Starts on login, restarts on crash, logs to `~/.openclaw/logs/watcher-bridge.log`.

---

## Component 3: Zidane Tool Registration

### TOOLS.md Addition

Added to `/Users/nacoleon/clawd/TOOLS.md`:

```markdown
## Watcher (Desk Companion)

Physical desk device running your avatar on a circular LCD. Controls via bridge at localhost:3847.

### watcher.display_message
Show a FF9-style dialog box on the Watcher screen. Auto-dismisses after 10s.
- POST http://localhost:3847/tools/display_message
- Body: {"text": "string (max 80 chars)", "level": "info|success|warning|error"}
- Use for: task completions, status updates, greetings

### watcher.set_state
Change your visual state on the Watcher.
- POST http://localhost:3847/tools/set_state
- Body: {"state": "idle|working|waiting|alert|greeting|sleeping|reporting"}
- Use for: reflecting what you're currently doing

### watcher.get_status
Read the Watcher's current state and sensor data.
- GET http://localhost:3847/tools/get_status
- Returns: {person_present, agent_state, last_message, uptime_seconds, wifi_rssi}

### watcher.notify
Convenience: set state + show message in one call.
- POST http://localhost:3847/tools/notify
- Body: {"text": "string", "level": "info|success|warning|error", "state": "reporting"}
```

### HEARTBEAT.md Addition

Added to `/Users/nacoleon/clawd/HEARTBEAT.md`:

```markdown
## Desk Context

Read ~/.openclaw/watcher-context.json for physical desk state.

- If person_present is true, Manny is at his desk — prefer watcher.notify for updates.
- If person_present is false, skip watcher messages (he won't see them).
- Check ~/.openclaw/watcher-events.json for presence changes since last heartbeat. Clear after reading.

On person arrival:
  1. watcher.set_state("greeting")
  2. watcher.display_message with a short greeting + summary of recent work
  3. After 10s, watcher.set_state("idle") or "reporting" if there's news

On person departure:
  1. watcher.set_state("sleeping")

During active work:
  - watcher.set_state("working") when executing tasks
  - watcher.set_state("idle") when between tasks
  - watcher.notify on task completion (level: "success")
  - watcher.notify on failures (level: "error", state: "alert")

Message discipline:
  - Max 1 message per 5 minutes unless ALERT
  - Keep messages to 1 line when possible
  - Only use ALERT for failures or things needing immediate attention
```

---

## Zidane Sprite Assets

### Overworld Sprite (32x32 frames)

**Primary source:** FFRK (Final Fantasy Record Keeper) Zidane sprite — official Square Enix pixel art in classic FF overworld proportions. Available on The Spriters Resource.

**Secondary source:** FFTA-style custom Zidane sprite sheet (fan-made, Spriters Resource) — purpose-built for top-down RPG use, ~24x24 frames scalable to 32x32.

**Tertiary source:** Pictlogica Final Fantasy Zidane — another official SE pixel sprite.

**Required animations:**
| Animation | Frames | Source |
|-----------|--------|--------|
| `idle_down/up/left/right` | 2-4 each | Adapt from FFRK standing poses |
| `walk_down/up/left/right` | 4 each | Adapt from FFRK walk cycle |
| `greeting` | 4-6 | Custom — waving/jumping |
| `sleeping` | 2-4 | Custom — eyes closed, ZZZ |
| `working` | 2-4 | Custom or reuse walk cycle |
| `alert` | 2-4 | Custom — exclamation pose |

**Pipeline:** PNG (transparent background) → Python converter → RGB565 + Alpha `.raw` file. Same pipeline as current Pokémon sprites.

### Portrait (Dialog Box Face)

**Source:** FF9 PS1 dialog portraits from The Spriters Resource. The original game's face sprites shown in text boxes.

**Spec:** 32x32 raw file, displayed at 48x48 inside the FF dialog box on the left side.

**Sprite resource URLs:**
- FFRK Zidane: `spriters-resource.com/mobile/finalfantasyrecordkeeper/asset/71235/`
- FFTA-style custom: `spriters-resource.com/custom_edited/finalfantasy9customs/sheet/156775/`
- Pictlogica Zidane: `spriters-resource.com/mobile/pictlogicafinalfantasy/asset/70915/`
- FF9 portraits: `spriters-resource.com/playstation/finalfantasy9/` (Portraits section)

---

## What Changes vs Current Firmware

| Area | Current (PokéWatcher) | New (Zidane Watcher) |
|------|----------------------|---------------------|
| Character system | Pokémon roster, evolution | Single character (Zidane), no evolution |
| Mood engine | Internal timer-driven, 6 moods | External API-driven, 7 agent states |
| Person detection | Triggers mood transitions locally | Reports to bridge, Zidane decides response |
| Display messages | LLM-generated commentary | Zidane's own messages via bridge |
| Dialog style | None (text on web dashboard only) | FF9-style dialog box with portrait |
| Sprite storage | `/sdcard/pokemon/{id}/` | `/sdcard/characters/{id}/` |
| Web dashboard | Roster mgmt, settings, timeline | Simplified: status view + message history (no roster, no LLM settings) |
| Network | HTTP server only | HTTP server + bridge polls it |

---

## What Stays the Same

- ESP32-S3 hardware, all peripherals
- LVGL rendering pipeline
- Sprite sheet format and converter
- SD card storage
- WiFi + HTTP server
- Himax person detection (on-device inference)
- NVS for persistent config
- Web server framework (esp_http_server)
- mDNS (renamed from `pokewatcher.local` to `zidane.local`)

---

## Out of Scope (Future)

- Audio/speaker output (hardware ready, not in this iteration)
- RGB LED integration (hardware ready, not in this iteration)
- BLE phone connectivity
- Multiple characters/roster (Zidane only for now)
- Watcher-initiated messages to Zidane (bridge handles all outbound)
- Touch screen interaction
