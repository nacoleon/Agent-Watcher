# Watcher Documentation — Quick Guide & Detailed Guide

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Create two documentation files — a Quick Start guide (get running in 15 minutes) and a Detailed Guide (complete feature reference, architecture, troubleshooting) — covering firmware installation, MCP setup, daemon configuration, and OpenClaw integration.

**Architecture:** Two standalone markdown files in `docs/`. The Quick Guide is a linear checklist — do steps 1-N, you're done. The Detailed Guide is a reference document organized by subsystem (firmware, MCP, daemon, OpenClaw, API, hardware). Both target a developer with a brand-new Watcher device and a Mac with Homebrew.

**Tech Stack:** Markdown documentation, ESP-IDF 5.2.1, Node.js/TypeScript, Piper TTS, whisper.cpp, OpenClaw CLI, macOS LaunchAgent

---

## File Structure

| File | Purpose |
|------|---------|
| `docs/QUICK-START.md` | Linear setup checklist: hardware → firmware → SD card → WiFi → MCP → daemon → OpenClaw → verify |
| `docs/DETAILED-GUIDE.md` | Complete reference: all features, architecture, API endpoints, MCP tools, troubleshooting, customization |

---

### Task 1: Create Quick Start Guide

**Files:**
- Create: `docs/QUICK-START.md`

- [ ] **Step 1: Write the Quick Start Guide**

Create `docs/QUICK-START.md` with the following sections and content. This is the complete file:

```markdown
# Zidane Watcher — Quick Start Guide

Get your SenseCap Watcher running as an OpenClaw AI desk companion in ~15 minutes.

## Prerequisites

| Requirement | How to get it |
|---|---|
| SenseCap Watcher device | [Seeed Studio](https://www.seeedstudio.com/SenseCAP-Watcher-W1-p-5979.html) |
| USB-C cable | Included with Watcher |
| microSD card (FAT32, ≤32GB) | Any brand works |
| Mac with Homebrew | `xcode-select --install` if needed |
| Node.js 20+ | `brew install node` |
| ESP-IDF 5.2.1 | [ESP-IDF install guide](https://docs.espressif.com/projects/esp-idf/en/v5.2.1/esp32s3/get-started/) |
| Piper TTS | `pip install piper-tts` |
| OpenClaw CLI | Must be installed and configured with an agent named `main` |

## Step 1: Clone the Repository

```bash
git clone <repo-url> ~/HQ/02-Projects/SenseCap\ Watcher
cd ~/HQ/02-Projects/SenseCap\ Watcher
```

## Step 2: Prepare the SD Card

The Watcher loads sprites and Himax camera firmware from the SD card at boot.

```bash
# Format SD card as FAT32, then copy these files to its root:
# From sdcard_prep/characters/zidane/:
#   - overworld.raw     (RGB565 sprite sheet)
#   - frames.json       (animation frame definitions)
# From sdcard_prep/himax/:
#   - All .img firmware files
```

Insert the SD card into the Watcher's slot and power it on. The firmware loads sprites once at boot, then powers off the SD card to free the SPI bus for the camera.

## Step 3: Configure WiFi

Edit `pokewatcher/main/config.h` and set your WiFi credentials:

```c
#define WIFI_SSID "YourNetworkName"
#define WIFI_PASS "YourPassword"
```

The Watcher gets a static IP or DHCP address. Note the IP from the serial console after boot — you'll need it for the MCP config. The default in this project is `10.0.0.40`.

## Step 4: Build & Flash Firmware

**Critical:** The project path contains a space (`SenseCap Watcher`), which breaks the ESP-IDF linker. You MUST build from `/tmp`.

```bash
# 1. Copy project to /tmp (no spaces in path)
rm -rf /tmp/pokewatcher-build
cp -r pokewatcher /tmp/pokewatcher-build

# 2. Source ESP-IDF environment
source ~/esp/esp-idf/export.sh

# 3. Build
cd /tmp/pokewatcher-build
idf.py build

# 4. Flash (app partition only — preserves NVS settings)
idf.py -p /dev/cu.usbmodem5A8A0533623 app-flash

# 5. Monitor serial output to confirm boot
idf.py -p /dev/cu.usbmodem5A8A0533623 monitor
```

You should see Zidane appear on the 412×412 round LCD, walking to the idle position with a background image.

## Step 5: Verify Watcher is Online

Open a browser to `http://<WATCHER_IP>` (e.g., `http://10.0.0.40`). You should see the web UI with state buttons, message input, and logs.

Or from terminal:
```bash
curl http://10.0.0.40/api/status
```

Expected: JSON with `agent_state`, `person_present`, `uptime_seconds`, etc.

## Step 6: Build the MCP Server

```bash
cd watcher-mcp
npm install
npm run build
```

This compiles TypeScript to `dist/`. The MCP server runs via stdio (spawned by OpenClaw on demand).

**Also builds whisper.cpp** — the `whisper-node` dependency compiles the native binary. Verify it exists:
```bash
ls node_modules/whisper-node/lib/whisper.cpp/main
```

Download the Whisper model:
```bash
cd node_modules/whisper-node/lib/whisper.cpp
bash models/download-ggml-model.sh base.en
```

## Step 7: Configure MCP in OpenClaw

Add the Watcher MCP server to your OpenClaw configuration. The exact config location depends on your OpenClaw setup, but the MCP server entry looks like:

```json
{
  "mcpServers": {
    "watcher": {
      "command": "node",
      "args": ["/absolute/path/to/watcher-mcp/dist/index.js"],
      "transportType": "stdio"
    }
  }
}
```

**Important:** Update the `WATCHER_URL` in `watcher-mcp/src/config.ts` if your Watcher isn't at `10.0.0.40`:
```typescript
export const WATCHER_URL = "http://YOUR_WATCHER_IP";
```
Then rebuild: `npm run build`.

## Step 8: Start the Daemon

The daemon runs 24/7 in the background, polling the Watcher for voice audio, presence changes, and message dismissals.

```bash
# Start it manually first to verify it works:
node watcher-mcp/dist/daemon.js

# You should see:
# [system] Watcher daemon started
# [system] Polling http://10.0.0.40 every 5000ms
# [poll] alive — uptime=...
```

To run it permanently as a macOS LaunchAgent:

```bash
# Create the plist
cat > ~/Library/LaunchAgents/ai.openclaw.watcher-daemon.plist << 'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>ai.openclaw.watcher-daemon</string>
    <key>ProgramArguments</key>
    <array>
        <string>/opt/homebrew/bin/node</string>
        <string>/absolute/path/to/watcher-mcp/dist/daemon.js</string>
    </array>
    <key>WorkingDirectory</key>
    <string>/absolute/path/to/watcher-mcp</string>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>/Users/YOU/.openclaw/logs/watcher-daemon.log</string>
    <key>StandardErrorPath</key>
    <string>/Users/YOU/.openclaw/logs/watcher-daemon.log</string>
</dict>
</plist>
EOF

# Update paths in the plist, then load it:
mkdir -p ~/.openclaw/logs
launchctl load ~/Library/LaunchAgents/ai.openclaw.watcher-daemon.plist
```

Verify it's running:
```bash
launchctl list | grep watcher
# Should show the daemon with PID
```

## Step 9: Install Piper TTS

```bash
pip install piper-tts
# Verify:
piper --help
```

Voice models are auto-downloaded on first use to `/tmp/piper-voices/`. The default voice is `en_US-bryce-medium`.

## Step 10: Test Everything

```bash
# 1. Check Watcher status via MCP (if OpenClaw is running):
#    Use the get_status tool — should return device state

# 2. Send a test message:
curl -X POST http://10.0.0.40/api/message \
  -H "Content-Type: application/json" \
  -d '{"text": "Hello from Quick Start!", "level": "info"}'

# 3. Test voice: double-click the knob on the Watcher
#    LED turns blue (recording), then green (ready)
#    Daemon picks up audio, transcribes, sends to OpenClaw

# 4. Test TTS (via MCP speak tool or direct):
curl -X PUT http://10.0.0.40/api/agent-state \
  -H "Content-Type: application/json" \
  -d '{"state": "greeting"}'
```

## Optional: Sprite & Background Planning Dashboard

Plan custom sprites and curate backgrounds before burning to SD card — no hardware needed:

```bash
python3 zidane-dashboard/server.py
# Open http://localhost:8091
```

- **Main preview** (`/`) — See how your character looks on the 412×412 circular display with live animations
- **Sprite catalog** (`/sprites.html`) — Frame-by-frame reference with coordinates, usable/unusable markers, and animation previews. Essential when creating custom `frames.json` files.
- **Background catalog** (`/backgrounds.html`) — Toggle which background tiles to include in the firmware's rotation pool

See the [Detailed Guide](DETAILED-GUIDE.md#sprite--background-planning-dashboard) for full documentation.

## You're Done!

Zidane is now running on your Watcher, connected to OpenClaw via MCP. The daemon handles voice transcription, presence detection, and message queuing automatically.

**Next:** Read the [Detailed Guide](DETAILED-GUIDE.md) for the full feature reference, API documentation, and troubleshooting.
```

- [ ] **Step 2: Commit**

```bash
git add docs/QUICK-START.md
git commit -m "docs: add Quick Start guide for new Watcher setup"
```

---

### Task 2: Create Detailed Guide — Header & Table of Contents

**Files:**
- Create: `docs/DETAILED-GUIDE.md`

- [ ] **Step 1: Write the document header and table of contents**

Create `docs/DETAILED-GUIDE.md` starting with:

```markdown
# Zidane Watcher — Detailed Guide

Complete reference for the SenseCap Watcher firmware, MCP integration, and OpenClaw setup.

## Table of Contents

1. [Project Overview](#project-overview)
2. [Hardware](#hardware)
3. [Firmware Features](#firmware-features)
   - [Agent States & Animations](#agent-states--animations)
   - [Sprite System](#sprite-system)
   - [Background Images](#background-images)
   - [Dialog System](#dialog-system)
   - [Display Sleep](#display-sleep)
   - [Knob Controls](#knob-controls)
   - [RGB LED](#rgb-led)
   - [Voice Input (Push-to-Talk)](#voice-input-push-to-talk)
   - [Speaker Output (TTS)](#speaker-output-tts)
   - [Himax AI Camera](#himax-ai-camera)
   - [Person Detection & Presence](#person-detection--presence)
   - [Gesture Recognition](#gesture-recognition)
   - [OpenClaw Heartbeat](#openclaw-heartbeat)
   - [Web UI](#web-ui)
4. [Sprite & Background Planning Dashboard](#sprite--background-planning-dashboard)
   - [Running the Dashboard](#running-the-dashboard)
   - [Main Preview](#main-preview)
   - [Sprite Catalog](#sprite-catalog)
   - [Background Catalog](#background-catalog)
5. [REST API Reference](#rest-api-reference)
6. [MCP Server](#mcp-server)
   - [Tools](#tools)
   - [Resources](#resources)
7. [Watcher Daemon](#watcher-daemon)
   - [What It Does](#what-it-does)
   - [Message Queue](#message-queue)
   - [Voice Pipeline](#voice-pipeline)
   - [Presence Notifications](#presence-notifications)
8. [OpenClaw Integration](#openclaw-integration)
   - [MCP Configuration](#mcp-configuration)
   - [Agent Events](#agent-events)
9. [Building & Flashing](#building--flashing)
   - [Build from /tmp](#build-from-tmp)
   - [sdkconfig Rules](#sdkconfig-rules)
   - [Flash Commands](#flash-commands)
   - [Serial Monitor](#serial-monitor)
10. [SD Card Setup](#sd-card-setup)
    - [Sprite Assets](#sprite-assets)
    - [Himax Firmware](#himax-firmware)
11. [Customization](#customization)
    - [Adding Sprites](#adding-sprites)
    - [Changing Backgrounds](#changing-backgrounds)
    - [Voice Configuration](#voice-configuration)
12. [Troubleshooting](#troubleshooting)
13. [Architecture Diagram](#architecture-diagram)

---
```

- [ ] **Step 2: Commit the skeleton**

```bash
git add docs/DETAILED-GUIDE.md
git commit -m "docs: add Detailed Guide skeleton with TOC"
```

---

### Task 3: Detailed Guide — Project Overview & Hardware

**Files:**
- Modify: `docs/DETAILED-GUIDE.md`

- [ ] **Step 1: Write Project Overview section**

Append to `docs/DETAILED-GUIDE.md`:

```markdown
## Project Overview

The Zidane Watcher turns a SenseCap Watcher device into a physical AI desk companion for OpenClaw. It displays an animated Zidane (Final Fantasy IX) character on a 412×412 round LCD screen, detects people and gestures via a Himax AI camera, accepts voice input through a built-in microphone, and speaks responses through its speaker using Piper TTS.

The system has three components:

| Component | Runs on | Role |
|---|---|---|
| **Firmware** (`pokewatcher/`) | ESP32-S3 (Watcher device) | Display rendering, camera AI, web server, voice recording, hardware control |
| **MCP Server** (`watcher-mcp/src/index.ts`) | Mac (spawned by OpenClaw) | Stateless stdio bridge — 7 tools for OpenClaw to control the Watcher |
| **Daemon** (`watcher-mcp/src/daemon.ts`) | Mac (LaunchAgent, 24/7) | Polls Watcher every 5s for voice audio, presence changes, message dismissals. Owns the message queue. Sends events to OpenClaw. |

Communication: Mac ↔ Watcher over HTTP (port 80). MCP Server ↔ Daemon over localhost HTTP (port 8378). Daemon → OpenClaw via `openclaw agent` CLI.

## Hardware

The SenseCap Watcher is an ESP32-S3 device with these peripherals:

| Peripheral | Interface | Details |
|---|---|---|
| LCD Display | SPI (HSPI) | 412×412 round, ST7701 controller |
| Himax WE2 Camera | SPI2 (VSPI) | AI chip with onboard models, 12 MHz SPI clock |
| Microphone | I2S | 16kHz/16-bit mono, PSRAM buffer |
| Speaker | I2S + codec | 16kHz/16-bit mono PCM playback |
| Rotary Knob | GPIO (encoder + button) | Press, double-press, long-press, rotate |
| RGB LED | GPIO | Programmable color, used for state indication |
| SD Card | SPI2 (shared with Himax) | FAT32, powered off after boot to free SPI bus |
| WiFi | Built-in ESP32-S3 | 2.4GHz, used for REST API and web UI |
| IO Expander | I2C | Controls power to LCD, camera, SD card |
| Battery | ADC | Optional LiPo, not used in desk mode |
```

- [ ] **Step 2: Commit**

```bash
git add docs/DETAILED-GUIDE.md
git commit -m "docs: add Project Overview and Hardware sections"
```

---

### Task 4: Detailed Guide — Firmware Features (Part 1: Display & Animation)

**Files:**
- Modify: `docs/DETAILED-GUIDE.md`

- [ ] **Step 1: Write Agent States, Sprite System, Backgrounds, Dialog, Display Sleep sections**

Append to `docs/DETAILED-GUIDE.md`:

```markdown
## Firmware Features

### Agent States & Animations

Zidane has 9 visual states, each with a unique sprite animation and screen position:

| State | Animation | Position | When |
|---|---|---|---|
| `idle` | Walk down, stand | Bottom center | Default state |
| `working` | Typing/working loop | Center-left | Processing a task |
| `waiting` | Idle fidget | Center-right | Waiting for input |
| `alert` | Combat stance | Center | Urgent notification |
| `greeting` | Wave animation | Center-right | Person arrives |
| `sleeping` | Eyes closed + ZZZ overlay | Bottom center | Night/idle timeout |
| `reporting` | Presenting pose | Center | Showing a message |
| `down` | Fallen/KO | Bottom center | OpenClaw heartbeat timeout |
| `wakeup` | Standing up | Bottom center | Transitional after sleep |

State transitions trigger smooth walk animations — Zidane walks in a straight line from current position to the new state's position before switching to the destination animation.

### Sprite System

Sprites are loaded from the SD card at boot:

- **Sprite sheet:** `overworld.raw` — RGB565 format, contains all animation frames in a grid
- **Frame definitions:** `frames.json` — JSON array defining each frame's x, y, width, height, animation name, frame index, speed, and mirror flag
- **16 animations** with per-frame size overrides and mirror support
- **Location:** SD card root → loaded into PSRAM → SD card powered off

To create new sprites, see [Customization > Adding Sprites](#adding-sprites).

### Background Images

- **34 pre-loaded background tiles** stored in PSRAM at boot
- Auto-rotates every 5 minutes with a strip-wipe transition (20 rows/frame)
- Can be set manually via `PUT /api/background` or the web UI
- Backgrounds are 412×412 RGB565 `.raw` files

### Dialog System

FF9-style dialog box with grainy gray background and speech tail:

- **Max 1000 characters**, paginated at ~95 chars/page
- **Knob wheel** scrolls between pages, page indicator shows current/total
- **Short knob press** dismisses the dialog
- **UTF-8 sanitization:** em-dashes, smart quotes, and ellipsis are converted to ASCII
- Messages sent via `POST /api/message` or the `display_message` MCP tool

### Display Sleep

- **3 minutes idle** (no knob interaction, no dialog) → display turns off
- **Wakes on:** knob press, new message displayed, state change
- Saves power and prevents burn-in

### Knob Controls

| Action | Effect |
|---|---|
| Short press | Dismiss current dialog / wake display |
| Double press | Start 10-second voice recording |
| Long press (6s) | Hardware reboot |
| Rotate | Scroll dialog pages |
```

- [ ] **Step 2: Commit**

```bash
git add docs/DETAILED-GUIDE.md
git commit -m "docs: add display and animation firmware features"
```

---

### Task 5: Detailed Guide — Firmware Features (Part 2: LED, Voice, Camera)

**Files:**
- Modify: `docs/DETAILED-GUIDE.md`

- [ ] **Step 1: Write RGB LED, Voice Input, Speaker Output, Camera, Presence, Gesture sections**

Append to `docs/DETAILED-GUIDE.md`:

```markdown
### RGB LED

State-specific LED behavior:

| State/Event | LED Color | Pattern |
|---|---|---|
| `alert` | Red | Blinking |
| `waiting` | Orange | Blinking |
| `greeting` | Pink | Blinking |
| `reporting` | Green | Blinking |
| Voice recording | Blue | Solid |
| Voice ready | Green | Solid |
| Gesture confirmed | Purple | Double flash |
| All others | Off | — |

### Voice Input (Push-to-Talk)

1. **Double-click the knob** → LED turns blue, 10-second recording starts
2. Audio captured at 16kHz/16-bit mono into PSRAM
3. LED turns green when recording complete
4. **Daemon polls** `GET /api/status` every 5s, sees `audio_ready: true`
5. Daemon fetches audio via `GET /api/audio`, then `DELETE /api/audio` to clear
6. **whisper.cpp** transcribes the WAV (base.en model, ~2-5 seconds)
7. Transcribed text sent to OpenClaw: `openclaw agent --agent main -m "[Voice from Watcher] <text>"`

### Speaker Output (TTS)

The `speak` MCP tool converts text to speech and plays it through the Watcher's speaker:

1. OpenClaw calls the `speak` tool with text
2. **Piper TTS** runs locally on Mac, generates 22050Hz PCM audio
3. Audio resampled to 16kHz (linear interpolation) for the Watcher's codec
4. PCM buffer sent to Watcher via `POST /api/audio/play`
5. Speaker unmutes, plays audio, re-mutes

**Default voice:** `en_US-bryce-medium` at length-scale 0.7, volume 90.
Voice models auto-download from Hugging Face to `/tmp/piper-voices/` on first use.

### Himax AI Camera

The Himax WE2 is a separate AI chip connected via SPI2 with onboard ML models:

- **SPI clock:** 12 MHz
- **3 models available:**
  - Model 1: Person Detection
  - Model 2: Pet Detection
  - Model 3: Gesture Detection (Rock/Paper/Scissors)
- **Auto model switching:** Person detected → switch to Gesture mode. 20 min idle → switch back to Person mode.
- **OTA firmware:** Himax firmware files on SD card are flashed to the AI chip at boot if newer

**Critical:** `CONFIG_FREERTOS_HZ` must be 1000 (not 100). The Himax SPI protocol requires 2ms delays — at 100Hz tick rate, `vTaskDelay(pdMS_TO_TICKS(2))` rounds to 0, breaking the camera.

### Person Detection & Presence

- Camera runs Person Detection model by default
- **Presence tracking:** 3-minute timeout — if no person detected for 3 min, `person_present` flips to `false`
- **Daemon detects changes** with 2-poll debounce (10 seconds) to filter noise
- Events sent to OpenClaw: `person_arrived` / `person_left`
- Presence log available in web UI and via `GET /api/status`

### Gesture Recognition

- Activated when a person is detected (auto model switch)
- **Rock/Paper/Scissors** with 85% confidence threshold
- **4-frame confirmation** required (must see same gesture 4 consecutive frames)
- **Rock false-positive filter:** bounding box width must be ≥130px (real fist is 144+, false positives from face/body are smaller)
- **6-second re-detection cooldown** between confirmed gestures
- **Purple double LED flash** on confirmed gesture
- Gesture log: 20-entry circular buffer, visible in web UI

### OpenClaw Heartbeat

- OpenClaw sends heartbeat via `heartbeat` MCP tool (should be called every hour)
- **1.5-hour timeout:** if no heartbeat received, Watcher enters `down` state (Zidane KO animation)
- **Auto-recovery:** sending a heartbeat while in `down` state returns to `idle`
- Heartbeat log visible in web UI

### Web UI

Built-in web interface at `http://<WATCHER_IP>` (default: `http://10.0.0.40`):

- **State buttons** — click to change Zidane's state
- **Message input** — send text to the dialog system
- **Background controls** — select background tile
- **AI model toggle** — switch between Person/Pet/Gesture detection
- **Logs panel** — heartbeat log, presence log, gesture log
- **Voice config** — current voice and volume settings
```

- [ ] **Step 2: Commit**

```bash
git add docs/DETAILED-GUIDE.md
git commit -m "docs: add LED, voice, camera, and web UI features"
```

---

### Task 6: Detailed Guide — Sprite & Background Planning Dashboard

**Files:**
- Modify: `docs/DETAILED-GUIDE.md`

- [ ] **Step 1: Write Dashboard section**

Append to `docs/DETAILED-GUIDE.md`:

```markdown
## Sprite & Background Planning Dashboard

The **Zidane Dashboard** (`zidane-dashboard/`) is a local browser tool for planning custom sprites and backgrounds before burning them to the SD card. Use it to preview how animations will look on the 412×412 circular display, audit which sprite frames are usable, and curate which background tiles to include.

### Running the Dashboard

```bash
python3 zidane-dashboard/server.py
# Opens on http://localhost:8091
# Optional custom port:
python3 zidane-dashboard/server.py 9000
```

No dependencies beyond Python 3. The server serves static HTML/CSS/JS and provides mock API endpoints matching the firmware's REST API — no hardware needed.

### Main Preview (`/`)

Interactive 412×412 circular display preview — see exactly how your character will look on the Watcher:

- **Live sprite animation** — Character rendered at 4× scale (16×24 → 64×96) with walking, facing directions, and mirror support
- **State control buttons** — Click any of the 8 states to preview their animation and position. Auto-cycles by default; clicking locks to a specific state.
- **FF9 dialog box** — Preview how messages look in the dialog system (word-wrapped, paginated)
- **ZZZ overlay** — See the sleeping state's floating text effect
- **State behavior tuning** — Walk chance, turn chance, speed, and pause duration are visible per-state, matching firmware behavior

Use this to validate that your custom sprite sheet and `frames.json` produce the animations you expect before copying to the SD card.

### Sprite Catalog (`/sprites.html`)

**Essential for planning custom sprites.** Frame-by-frame reference for the sprite sheet:

- **Full sprite sheet** at 4× scale with hover coordinates — identify exactly which pixel regions to use
- **Frame grid** — Every 16×24 frame labeled by group (front-facing, back-facing, side-facing, combat, attack), showing coordinates, sizes, and which animations reference each frame
- **"DO NOT USE" markers** — White/transparent ghost frames are flagged so you don't accidentally reference them in your `frames.json`
- **Animation previews** — Each of the 16 animations with live looping preview, frame count, loop/mirror flags, and per-frame speed settings

When creating a new character, use this page to map out your sprite sheet layout and verify all frame coordinates in `frames.json` are correct.

### Background Catalog (`/backgrounds.html`)

**Curate your background pool** before writing to firmware:

- **6×14 grid** of all 84 background tiles from the source sheet
- **Click to toggle** enabled/disabled — green border = included, red/faded = excluded
- **Live counters** showing enabled vs disabled count
- **Filter buttons** — All / Enabled Only / Disabled Only
- **Background IDs** listed — use these IDs when updating the firmware's background tile array

The firmware loads a subset of these tiles into PSRAM at boot and rotates through them every 5 minutes. Use this catalog to pick which tiles make the cut.

### Mock API Endpoints

The dashboard serves mock endpoints matching the firmware's REST API, so you can test integrations locally:

| Endpoint | Method | Behavior |
|---|---|---|
| `/api/status` | GET | Returns mock state, presence, uptime, RSSI, messages |
| `/api/timeline` | GET | Returns array of recent mock messages |
| `/api/agent-state` | PUT | Sets state, locks auto-cycle |
| `/api/message` | POST | Accepts message text |
| `/sprites/zidane` | GET | Returns sprite sheet PNG |
| `/frames/zidane` | GET | Returns `frames.json` animation definitions |
| `/backgrounds` | GET | Returns background composite PNG |
```

- [ ] **Step 2: Commit**

```bash
git add docs/DETAILED-GUIDE.md
git commit -m "docs: add Sprite & Background Planning Dashboard section"
```

---

### Task 7: Detailed Guide — REST API Reference

**Files:**
- Modify: `docs/DETAILED-GUIDE.md`

- [ ] **Step 1: Write REST API Reference section**

Append to `docs/DETAILED-GUIDE.md`:

```markdown
## REST API Reference

All endpoints are served by the Watcher's built-in web server on port 80.

### `GET /api/status`
Returns full device state.

**Response:**
```json
{
  "agent_state": "idle",
  "person_present": true,
  "uptime_seconds": 3600,
  "wifi_rssi": -45,
  "dialog_visible": false,
  "dismiss_count": 12,
  "audio_ready": false,
  "heartbeat_log": [...],
  "presence_log": [...],
  "gesture_log": [...]
}
```

### `PUT /api/agent-state`
Change Zidane's visual state.

**Body:** `{"state": "greeting"}`
**Valid states:** `idle`, `working`, `waiting`, `alert`, `greeting`, `sleeping`, `reporting`, `down`

### `POST /api/message`
Display a dialog message.

**Body:** `{"text": "Hello!", "level": "info"}`
**Levels:** `info`, `warning`, `alert`
**Max:** 1000 characters, paginated at ~95 chars/page

### `POST /api/heartbeat`
OpenClaw heartbeat signal. Resets the 1.5-hour timeout.

### `POST /api/reboot`
Hardware reboot — power cycles LCD and AI chip before ESP32 restart.

### `PUT /api/background`
Set the background tile index.

**Body:** `{"index": 5}`

### `POST /api/audio/play`
Play PCM audio through the speaker.

**Content-Type:** `application/octet-stream`
**Body:** Raw 16kHz 16-bit mono PCM data

### `GET /api/audio`
Fetch recorded voice audio (WAV format). Only available when `audio_ready` is true.

### `DELETE /api/audio`
Clear the recorded audio buffer.

### `GET /api/voice`
Get current voice configuration.

**Response:** `{"voice": "en_US-bryce-medium", "volume": 90}`

### `PUT /api/voice`
Set voice model and volume (persisted to NVS flash).

**Body:** `{"voice": "en_US-bryce-medium", "volume": 90}`
```

- [ ] **Step 2: Commit**

```bash
git add docs/DETAILED-GUIDE.md
git commit -m "docs: add REST API reference"
```

---

### Task 8: Detailed Guide — MCP Server & Daemon

**Files:**
- Modify: `docs/DETAILED-GUIDE.md`

- [ ] **Step 1: Write MCP Server and Daemon sections**

Append to `docs/DETAILED-GUIDE.md`:

```markdown
## MCP Server

The MCP server (`watcher-mcp/src/index.ts`) is a stateless stdio server spawned on-demand by OpenClaw. Version 1.4.0.

### Tools

| Tool | Parameters | Description |
|---|---|---|
| `display_message` | `text` (string, max 1000), `state` (enum, default "reporting"), `level` (enum, default "info") | Show dialog + set state. Messages queue if one is already showing. |
| `set_state` | `state` (enum) | Change visual state without showing a message. |
| `get_status` | — | Read live device state (agent_state, person_present, uptime, etc.) |
| `get_queue` | — | Check message queue: currently showing, pending messages, count. |
| `reboot` | — | Hardware reboot (power cycle LCD and AI chip). |
| `heartbeat` | — | Send alive signal. Call every hour. 1.5hr timeout → "down" state. |
| `speak` | `text` (string), `voice` (string, optional) | Text-to-speech via Piper. Streams PCM to Watcher speaker. |

**Valid states for `display_message` and `set_state`:**
`idle`, `working`, `waiting`, `alert`, `greeting`, `sleeping`, `reporting`, `down`

### Resources

| Resource URI | Description |
|---|---|
| `watcher://status` | JSON snapshot of live device state |

## Watcher Daemon

The daemon (`watcher-mcp/src/daemon.ts`) runs 24/7 as a macOS LaunchAgent. It's the stateful counterpart to the stateless MCP servers.

### What It Does

Polls the Watcher every 5 seconds and handles four concerns:

1. **Voice audio pickup** — detects `audio_ready`, fetches WAV, transcribes with whisper.cpp, sends to OpenClaw
2. **Presence change detection** — tracks `person_present` with 2-poll debounce, sends `person_arrived`/`person_left`
3. **Message dismiss detection** — compares `dismiss_count` to trigger next queued message
4. **Reboot recovery** — detects uptime drop, re-sends the message that was showing

### Message Queue

- Owned by the daemon (not MCP server) so messages survive MCP process restarts
- **Max 10 messages**, FIFO order
- Each message pairs text + level + optional state
- State is applied when the message reaches the screen (not when queued)
- MCP servers enqueue via daemon HTTP API on `localhost:8378`

**Daemon API:**
| Method | Path | Body | Response |
|---|---|---|---|
| `POST` | `/queue` | `{"text": "...", "level": "info", "state": "reporting"}` | `{"sent": true, "queued": false, "pending": 0}` |
| `GET` | `/queue` | — | `{"currently_showing": true, "pending": [...], "count": 2}` |

### Voice Pipeline

```
Knob double-click → 10s recording (16kHz mono) → PSRAM
    ↓
Daemon polls /api/status → audio_ready: true
    ↓
GET /api/audio → WAV file → DELETE /api/audio
    ↓
whisper.cpp transcribe (base.en, ~3s)
    ↓
openclaw agent --agent main -m "[Voice from Watcher] hello"
```

### Presence Notifications

```
Himax Person Detection → person_present: true/false
    ↓
Daemon polls /api/status every 5s
    ↓
2-poll debounce (10s) to filter noise
    ↓
openclaw agent --agent main -m "[Watcher event] person_arrived"
```

**Logs:** `~/.openclaw/watcher-daemon-logs/daemon-YYYY-MM-DD.log`
```

- [ ] **Step 2: Commit**

```bash
git add docs/DETAILED-GUIDE.md
git commit -m "docs: add MCP server tools and daemon reference"
```

---

### Task 9: Detailed Guide — OpenClaw Integration

**Files:**
- Modify: `docs/DETAILED-GUIDE.md`

- [ ] **Step 1: Write OpenClaw Integration section**

Append to `docs/DETAILED-GUIDE.md`:

```markdown
## OpenClaw Integration

### MCP Configuration

Add the Watcher MCP server to your OpenClaw MCP configuration:

```json
{
  "mcpServers": {
    "watcher": {
      "command": "node",
      "args": ["/absolute/path/to/watcher-mcp/dist/index.js"],
      "transportType": "stdio"
    }
  }
}
```

The MCP server is spawned on-demand by the OpenClaw gateway. It's fully stateless — no timers, no pollers. Multiple instances are safe (they all talk to the same daemon via localhost:8378).

### Agent Events

The daemon sends these events to OpenClaw via `openclaw agent --agent main -m "<message>"`:

| Event | Message Format | Trigger |
|---|---|---|
| Voice input | `[Voice from Watcher] <transcribed text>` | User double-clicks knob and speaks |
| Person arrived | `[Watcher event] person_arrived` | Person detected for 10s+ (2-poll debounce) |
| Person left | `[Watcher event] person_left` | No person for 3 min + 10s debounce |

### Recommended Agent Setup

Your OpenClaw agent (`main`) should:

1. **Call `heartbeat` every hour** to prevent the Watcher entering `down` state
2. **Use `display_message` with appropriate states** — `greeting` for hellos, `alert` for urgent items, `reporting` for general info
3. **Use `speak` for voice responses** — keep them short (1-2 sentences), conversational
4. **Check `get_status` before decisions** — presence, current state, queue depth
5. **React to `person_arrived`/`person_left`** — greet on arrival, sleep on departure
```

- [ ] **Step 2: Commit**

```bash
git add docs/DETAILED-GUIDE.md
git commit -m "docs: add OpenClaw integration guide"
```

---

### Task 10: Detailed Guide — Build, SD Card, Customization, Troubleshooting

**Files:**
- Modify: `docs/DETAILED-GUIDE.md`

- [ ] **Step 1: Write Build, SD Card, Customization, Troubleshooting, and Architecture sections**

Append to `docs/DETAILED-GUIDE.md`:

```markdown
## Building & Flashing

### Build from /tmp

The project path contains a space (`SenseCap Watcher`) which breaks the ESP-IDF linker. Always build from `/tmp`:

```bash
rm -rf /tmp/pokewatcher-build
cp -r pokewatcher /tmp/pokewatcher-build
source ~/esp/esp-idf/export.sh
cd /tmp/pokewatcher-build
idf.py build
```

The firmware references SDK components via an absolute path in `CMakeLists.txt`:
```
/tmp/SenseCAP-Watcher-Firmware/components
```
Make sure this SDK exists. During full rebuilds, move it to `_SDK_BAK` to avoid conflicts, then move it back.

### sdkconfig Rules

- `sdkconfig` is auto-generated from `sdkconfig.defaults`
- **Delete `sdkconfig` before changing `sdkconfig.defaults`** — the old config silently takes precedence
- Critical settings that MUST NOT change:
  - `CONFIG_FREERTOS_HZ=1000` — camera breaks at 100
  - `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=262144` — DMA bounce buffer needs internal RAM
  - `CONFIG_LVGL_PORT_LOCK_MUTEX_TIMEOUT=500` — prevents SPI flush deadlock

### Flash Commands

```bash
# App-only flash (preserves NVS/WiFi settings):
idf.py -p /dev/cu.usbmodem5A8A0533623 app-flash

# Full flash (erases NVS — you'll need to re-enter WiFi):
idf.py -p /dev/cu.usbmodem5A8A0533623 flash

# NEVER use full flash unless you need to reset NVS
```

### Serial Monitor

```bash
idf.py -p /dev/cu.usbmodem5A8A0533623 monitor
# Ctrl+] to exit
```

Look for `[BOOT]` messages confirming each init step (1/7 through 7/7).

## SD Card Setup

### Sprite Assets

Place these files at the SD card root:

| File | Source | Description |
|---|---|---|
| `overworld.raw` | `sdcard_prep/characters/zidane/` | RGB565 sprite sheet |
| `frames.json` | `sdcard_prep/characters/zidane/` | Animation frame definitions |

### Himax Firmware

Place all `.img` files from `sdcard_prep/himax/` at the SD card root. The firmware OTA-flashes the Himax chip if the SD version is newer than what's on-chip.

**Note:** After loading sprites and checking Himax firmware, the firmware powers off the SD card to free the SPI2 MISO line for the camera. The SD card is not accessible after boot.

## Customization

### Adding Sprites

To create new character sprites:

1. Get a sprite sheet PNG with transparent background
2. Convert to RGB565 `.raw` format:
   ```bash
   python3 sdcard_prep/convert_sprites.py input.png output.raw
   ```
3. Create a `frames.json` defining each animation frame with `x`, `y`, `width`, `height`, `animation`, `frame_index`, `speed`, `mirror`
4. Place both files on the SD card root
5. Update animation mappings in `pokewatcher/main/sprite_loader.c` if animation names differ

### Changing Backgrounds

Background tiles are 412×412 RGB565 `.raw` files loaded at boot. To add new backgrounds:

1. Create a 412×412 PNG
2. Convert to RGB565: `python3 sdcard_prep/convert_bg.py input.png output.raw`
3. Add to the background tile array in the firmware
4. Rebuild and flash

### Voice Configuration

Set voice and volume via the API (persisted to NVS):

```bash
curl -X PUT http://10.0.0.40/api/voice \
  -H "Content-Type: application/json" \
  -d '{"voice": "en_US-amy-medium", "volume": 80}'
```

Available Piper voices: any voice from [rhasspy/piper-voices](https://huggingface.co/rhasspy/piper-voices) in the format `{lang}-{name}-{quality}` (e.g., `en_US-bryce-medium`, `en_US-amy-low`).

## Troubleshooting

### Watcher not reachable on network
- Check WiFi credentials in `config.h`
- Verify the IP in serial monitor output
- Ensure your Mac is on the same network/subnet

### Camera not detecting anything
- Verify `CONFIG_FREERTOS_HZ=1000` in sdkconfig
- Check serial logs for Himax SPI errors
- Confirm SD card had `.img` firmware files (check boot logs for OTA flash)
- Camera won't work if SD card power-off failed (SPI2 MISO contention)

### Display frozen / DMA errors
- Ensure `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=262144` in sdkconfig
- Never call `lv_obj_set_style_*()` every frame on objects >100×100 pixels
- All LVGL calls must be in the renderer task only — other threads set volatile flags

### Build fails with "cannot find -l..." or linker errors
- You're building from a path with spaces. Use `/tmp/pokewatcher-build/`
- Check that `/tmp/SenseCAP-Watcher-Firmware/components` exists

### Voice not transcribing
- Verify whisper.cpp binary: `ls watcher-mcp/node_modules/whisper-node/lib/whisper.cpp/main`
- Verify model: `ls watcher-mcp/node_modules/whisper-node/lib/whisper.cpp/models/ggml-base.en.bin`
- Check daemon logs: `tail -f ~/.openclaw/watcher-daemon-logs/daemon-$(date +%Y-%m-%d).log`

### TTS not working
- Verify Piper: `piper --help`
- Check voice model downloaded: `ls /tmp/piper-voices/en_US-bryce-medium.onnx`
- Check daemon logs for `tts` category errors

### Daemon not running
- Check LaunchAgent: `launchctl list | grep watcher`
- Manual start: `node watcher-mcp/dist/daemon.js`
- Port conflict: `lsof -i :8378` — only one daemon instance allowed

## Architecture Diagram

```
┌──────────────────────────────────────────────────────┐
│                    OpenClaw Agent                      │
│                   ("main" agent)                       │
│                                                        │
│  Receives: voice transcriptions, presence events       │
│  Sends: display_message, speak, set_state, heartbeat   │
└───────────────┬───────────────────┬───────────────────┘
                │ MCP (stdio)       │ CLI
                ▼                   ▼
┌───────────────────┐  ┌─────────────────────────────┐
│   MCP Server      │  │     Watcher Daemon           │
│  (stateless)      │  │  (LaunchAgent, 24/7)         │
│                   │  │                               │
│  7 tools          │  │  Polls every 5s:              │
│  1 resource       │  │  - Voice audio → transcribe   │
│                   │  │  - Presence → notify           │
│  Enqueues msgs    │  │  - Dismiss → next message     │
│  via daemon API   │  │  - Reboot → recovery          │
│  localhost:8378   │  │                               │
└───────┬───────────┘  │  Owns message queue (max 10)  │
        │              │  API: localhost:8378            │
        │              └──────────┬────────────────────┘
        │                         │
        │    HTTP (port 80)       │
        ▼                         ▼
┌──────────────────────────────────────────────────────┐
│              SenseCap Watcher (ESP32-S3)               │
│                                                        │
│  ┌─────────┐ ┌──────────┐ ┌────────────┐ ┌────────┐  │
│  │Renderer │ │Web Server│ │ Himax Task │ │ Voice  │  │
│  │ (LVGL)  │ │ (REST)   │ │ (Camera AI)│ │ Input  │  │
│  └─────────┘ └──────────┘ └────────────┘ └────────┘  │
│                                                        │
│  412×412 LCD  │  Speaker  │  Mic  │  Knob  │  LED     │
└──────────────────────────────────────────────────────┘
```
```

- [ ] **Step 2: Commit**

```bash
git add docs/DETAILED-GUIDE.md
git commit -m "docs: add build, customization, troubleshooting, and architecture"
```

---

### Task 11: Final Review & Cross-Link

**Files:**
- Modify: `docs/QUICK-START.md` (if any fixes needed)
- Modify: `docs/DETAILED-GUIDE.md` (if any fixes needed)

- [ ] **Step 1: Read both files end-to-end and verify**

Checklist:
- All MCP tools documented (7 tools: display_message, set_state, get_status, get_queue, reboot, heartbeat, speak) ✓
- All API endpoints documented (10 endpoints) ✓
- All firmware features covered (states, sprites, backgrounds, dialog, sleep, knob, LED, voice, TTS, camera, presence, gesture, heartbeat, web UI) ✓
- Build steps include /tmp workaround ✓
- SD card setup complete ✓
- Daemon setup with LaunchAgent ✓
- OpenClaw MCP config shown ✓
- Troubleshooting covers common issues ✓

- [ ] **Step 2: Commit any fixes**

```bash
git add docs/QUICK-START.md docs/DETAILED-GUIDE.md
git commit -m "docs: final review pass on Quick Start and Detailed Guide"
```
