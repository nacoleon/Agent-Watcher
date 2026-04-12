# PokéWatcher v1 — AI Pokemon Companion for SenseCAP Watcher

## Overview

An AI-driven Pokemon companion that lives on the SenseCAP Watcher's round display. The Pokemon reacts autonomously to its environment using the Himax AI chip for person detection, cycles through mood states, and has an LLM-powered personality engine. Users manage a roster of Pokemon they own via a local web dashboard, and Pokemon evolve over time.

This is not a game — it's an autonomous AI pet with Pokemon aesthetics.

## Hardware Target

- **SoC:** ESP32-S3 @ 240MHz, 8MB PSRAM, 32MB flash
- **Display:** 1.45" circular touchscreen, 412×412 resolution, LVGL-driven
- **AI Chip:** Himax WiseEye2 HX6538 (person/face detection, communicates via UART)
- **Peripherals:** Camera, microphone, speaker, microSD slot
- **Connectivity:** WiFi (for LLM API calls and web dashboard)
- **Firmware base:** Fork of [SenseCAP-Watcher-Firmware](https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware) SDK
- **Build system:** ESP-IDF v5.2.1

## Architecture

Multi-task event-driven architecture using FreeRTOS. Five tasks communicate through a central mood event queue.

```
┌──────────────────────────────────────────────────┐
│         HIMAX AI CHIP (always-on)                │
│  Camera → Person Detection → Face Detection      │
│  Sends structured events over UART to ESP32      │
└──────────────────┬───────────────────────────────┘
                   │ UART Events
┌──────────────────▼───────────────────────────────┐
│            ESP32-S3 MAIN APPLICATION             │
│                                                  │
│  ┌────────────┐ ┌────────────┐ ┌──────────────┐ │
│  │ HIMAX TASK │ │  LLM TASK  │ │ WEB SERVER   │ │
│  │ UART read  │ │ Text event │ │ TASK         │ │
│  │ → mood     │ │ summary →  │ │ Dashboard +  │ │
│  │   queue    │ │ LLM API →  │ │ REST API     │ │
│  │            │ │ mood queue │ │              │ │
│  └─────┬──────┘ └─────┬──────┘ └──────────────┘ │
│        │              │                          │
│  ┌─────▼──────────────▼──────┐                   │
│  │    MOOD EVENT QUEUE       │                   │
│  │  person_detected          │                   │
│  │  person_left              │                   │
│  │  llm_commentary           │                   │
│  │  roster_change            │                   │
│  └───────────┬───────────────┘                   │
│              │                                   │
│  ┌───────────▼───────────────┐                   │
│  │    MOOD ENGINE TASK       │                   │
│  │  State machine            │                   │
│  │  Evolution timer          │                   │
│  │  → display commands       │                   │
│  └───────────┬───────────────┘                   │
│              │                                   │
│  ┌───────────▼───────────────┐                   │
│  │    RENDERER TASK (LVGL)   │                   │
│  │  412×412 round display    │                   │
│  │  Sprite animation loop    │                   │
│  │  Background scene         │                   │
│  │  Mood transitions         │                   │
│  └───────────────────────────┘                   │
│                                                  │
│  ┌───────────────────────────────────────────┐   │
│  │  PERSISTENT STORAGE (NVS + microSD)       │   │
│  │  • Roster config    • Evolution progress  │   │
│  │  • Active Pokemon   • Sprite assets (SD)  │   │
│  │  • WiFi + LLM creds • Pokemon JSON defs   │   │
│  └───────────────────────────────────────────┘   │
└──────────────────────────────────────────────────┘
```

### Task Responsibilities

**Himax Task**
- Reads UART detection events from the Himax WiseEye2 chip
- Parses person detection packets (person detected, person left)
- Pushes `person_detected` and `person_left` events to the mood queue

**LLM Task**
- Triggered on mood state transitions (not on a timer)
- Composes a text summary of recent mood history and current state
- Sends HTTP POST to a configurable LLM API endpoint (any OpenAI-compatible API)
- Parses response into personality-flavored commentary
- Pushes `llm_commentary` event to mood queue (for dashboard display)
- Graceful fallback: if WiFi is down or API fails, mood system runs purely on Himax detection

**Mood Engine Task**
- Consumes all events from the mood queue
- Runs the mood state machine (see Mood System below)
- Tracks evolution timer (cumulative hours as active companion)
- Pushes display commands to the renderer (mood change, evolution trigger, sprite swap)

**Renderer Task**
- Owns the LVGL display loop
- Renders overworld sprites with mood-driven animations
- Renders background scene with mood-tinted lighting
- Handles sprite transitions on mood changes and evolution events
- Manages the circular display mask

**Web Server Task**
- HTTP server on the ESP32's WiFi interface
- Serves static HTML/CSS/JS dashboard
- Exposes REST API for roster management, settings, and status
- Accessible via mDNS at `pokewatcher.local`

## Mood System

### States

| State | Trigger | Duration | Animation |
|-------|---------|----------|-----------|
| **Excited** | Person arrives | 10 seconds | Rapid walk cycle, bouncing up/down |
| **Happy** | Person present (sustained) | While person present | Slow idle sway, occasional turn |
| **Curious** | Person just left | Up to 5 minutes | Head turns left/right, walks a few steps |
| **Lonely** | No person for 5+ min | Up to 15 minutes | Slow idle, faces away occasionally |
| **Sleepy** | No person for 20+ min | Until person returns | Settles down, subtle breathing motion |
| **Overjoyed** | Person returns after long absence (lonely/sleepy) | 15 seconds | Fast bounce + walk cycle, spins around |

### State Transitions

```
                    ┌──────────────┐
   Person arrives → │   EXCITED    │ (10s auto-transition)
                    └──────┬───────┘
                           │
                    ┌──────▼───────┐
   Person present → │    HAPPY     │ (stays while present)
                    └──────┬───────┘
                           │ person leaves
                    ┌──────▼───────┐
                    │   CURIOUS    │ (5 min timeout)
                    └──────┬───────┘
                           │ still no person
                    ┌──────▼───────┐
                    │    LONELY    │ (15 min timeout)
                    └──────┬───────┘
                           │ still no person
                    ┌──────▼───────┐
                    │    SLEEPY    │ (indefinite)
                    └──────┬───────┘
                           │ person returns
                    ┌──────▼───────┐
                    │  OVERJOYED   │ (15s → HAPPY)
                    └──────────────┘
```

### Transition Rules

- Person detected while in SLEEPY or LONELY → OVERJOYED
- Person detected while in CURIOUS → HAPPY (skip overjoyed, absence was short)
- Person present continuously → stay in HAPPY
- EXCITED auto-transitions to HAPPY after 10 seconds
- OVERJOYED auto-transitions to HAPPY after 15 seconds
- All timer thresholds are configurable via the web dashboard

## Sprite System

### Source

- Pokemon HeartGold/SoulSilver overworld sprites
- Source: [The Spriters Resource — HGSS](https://www.spriters-resource.com/ds_dsi/pokemonheartgoldsoulsilver/)
- Format: PNG sprite sheets, ~32×32 pixels per frame
- Each Pokemon has walk cycle frames (4 directions × 3-4 frames) plus idle frames

### Storage

- Sprite sheet PNGs stored on microSD card
- JSON manifest per Pokemon defines frame coordinates, animation sequences, and metadata
- On boot: active Pokemon's sprite sheet loaded from SD into PSRAM
- On roster change: new sprite sheet loaded, old one freed

### Rendering

- Sprites scaled 4× to ~128×128 pixels using nearest-neighbor interpolation (preserves pixel art crispness)
- Pokemon positioned in lower-center of the 412×412 display
- LVGL circular mask clips all content to the round display boundary
- LVGL image objects swap frames on a timer for animation
- Frame rate: ~10 FPS for sprite animation (appropriate for pixel art)

### Background Scene (v1)

- Simple pixel art environment: grass patch, flowers, small rock or tree
- Static background tiles, pre-rendered as a single image
- Background tint shifts subtly with mood:
  - Happy/Excited/Overjoyed: warm tones (golden sunlight)
  - Curious: neutral
  - Lonely: cool tones (bluish)
  - Sleepy: dark, night-like

## LLM Integration

### Role in v1: Personality Engine

The LLM does not process images in v1. It receives text-based event summaries and generates personality-flavored commentary from the Pokemon's perspective.

### Flow

1. Mood state transition occurs
2. LLM Task composes a text summary:
   ```
   Pokemon: Pikachu. Current mood: lonely (since 14:32).
   History: person was present for 2 hours (happy), left 35 minutes ago.
   Transition: happy → curious → lonely.
   ```
3. System prompt instructs the LLM to respond as the Pokemon character in 1-2 sentences
4. Response displayed on the web dashboard timeline

### Configuration

- API endpoint URL (any OpenAI-compatible API)
- API key
- Model name
- All configured via web dashboard Settings page

### Failure Handling

- If WiFi is disconnected: mood system continues on Himax detection alone, no commentary generated
- If API returns an error: log it, skip commentary, retry on next transition
- If API is slow: LLM Task runs independently, never blocks mood engine or renderer

## Evolution System

### Mechanic

- Time-based: each Pokemon tracks cumulative hours as the active companion
- Evolution threshold is configurable per Pokemon (default: 24 hours)
- Clock ticks in any mood state — just needs to be the selected active Pokemon
- Progress persists across reboots (stored in NVS)

### Pokemon Definition (JSON on microSD)

```json
{
  "id": "charmander",
  "name": "Charmander",
  "sprite_sheet": "charmander.png",
  "frame_manifest": "charmander_frames.json",
  "evolves_to": "charmeleon",
  "evolution_hours": 24
}
```

- Pokemon with no `evolves_to` field are final forms
- Multi-stage chains link naturally: Charmander → Charmeleon → Charizard

### Evolution Trigger

1. Mood engine detects evolution threshold crossed
2. Renderer plays evolution animation (sprite flashes, screen whites out briefly)
3. Sprite swaps to evolved form's overworld sprite sheet
4. Roster updates automatically (pre-evolution form replaced)
5. Evolution timer resets for next stage (if applicable)
6. Evolution event logged on web dashboard

## Web Dashboard

### Access

- Served locally by ESP32 HTTP server over WiFi
- Accessible at `pokewatcher.local` via mDNS (or direct IP)
- Vanilla HTML/CSS/JS — no frameworks, works on any browser
- Static assets embedded in firmware flash or served from SD card

### Pages

**Home / Status**
- Active Pokemon with current mood state indicator
- Time in current state
- Evolution progress bar (hours accumulated / hours needed)
- LLM commentary timeline (last 10 entries)

**Roster**
- Grid of all owned Pokemon showing overworld sprite preview
- Tap/click to set as active companion
- Add new Pokemon (select from available sprite sheets on SD card)
- Remove Pokemon from roster
- Evolution history per Pokemon

**Settings**
- WiFi configuration (SSID, password)
- LLM API configuration (endpoint, API key, model)
- Mood timer thresholds (curious, lonely, sleepy timeouts)
- Evolution hours override per Pokemon

### REST API

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET` | `/api/status` | Current mood, active Pokemon, evolution progress |
| `GET` | `/api/roster` | List all owned Pokemon |
| `POST` | `/api/roster` | Add Pokemon to roster |
| `DELETE` | `/api/roster/:id` | Remove Pokemon from roster |
| `PUT` | `/api/roster/active` | Set active Pokemon |
| `GET` | `/api/settings` | Get all settings |
| `PUT` | `/api/settings` | Update settings |
| `GET` | `/api/timeline` | LLM commentary history |

## Flashing & Setup

### Prerequisites

- ESP-IDF v5.2.1 installed
- USB-C cable connected to bottom port (data port, not back power port)

### First-Time Setup

1. **Back up nvsfactory partition** — contains device EUI and SenseCraft credentials. Check the partition table (`idf.py partition-table`) for the exact offset and size of the `nvsfactory` partition, then:
   ```bash
   esptool.py read_flash <offset> <size> nvsfactory_backup.bin
   ```
2. Clone and build:
   ```bash
   git clone https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware
   cd SenseCAP-Watcher
   git submodule update --init
   idf.py set-target esp32s3
   idf.py build
   idf.py --port /dev/ttyACM0 app-flash  # app-flash to protect nvsfactory
   ```
3. Copy sprite assets and Pokemon JSON definitions to microSD card
4. Insert microSD, power on Watcher
5. Connect to Watcher's WiFi AP for initial configuration, or connect it to your WiFi via serial console
6. Open `pokewatcher.local` in browser to configure LLM API and set up roster

## What We Keep From the Watcher SDK

- Display driver stack (SPI + LVGL integration)
- LVGL framework and image decoding
- Himax UART communication layer
- WiFi stack (ESP-IDF standard)
- Camera driver (ESP32-S3 camera interface)
- Audio I2S output driver (for future Pokemon cries)
- microSD SPI/SDMMC driver
- NVS (non-volatile storage) for persistent config

## What We Strip

- SenseCraft cloud binding and task system
- Default C-3PO / XiaoZhi UI
- Notification framework
- AI assistant / agent functionality
- Monitoring and telemetry systems

## Future Milestones (Not in v1)

- **Tile-based micro-world:** Full navigable pixel art world where the Pokemon moves autonomously
- **Camera → LLM vision:** Send camera frames to LLM for scene understanding ("there's a cat" → curious)
- **Additional mood inputs:** Microphone (loud sounds → startled, music → playful), touchscreen (pet the Pokemon), time of day (morning → energetic)
- **Pokemon cries:** Audio output through the speaker matching mood states
- **Multiple active Pokemon:** More than one companion on screen at a time
- **Trading:** Transfer Pokemon between Watcher devices
