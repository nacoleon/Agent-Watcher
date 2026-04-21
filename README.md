# Agent Watcher

Turn a [SenseCap Watcher](https://www.seeedstudio.com/SenseCAP-Watcher-W1-p-5979.html) into a physical AI desk companion powered by [OpenClaw](https://openclaw.ai). An animated character lives on the 412x412 round LCD, reacts to your presence, listens to your voice, speaks back, and stays connected to your AI agent 24/7.

## What It Does

- **Animated character** — Zidane (Final Fantasy IX) with 16 sprite animations, smooth walk transitions, and 9 context-aware states (idle, working, alert, greeting, sleeping, etc.)
- **AI vision** — Himax WE2 camera with person detection, pet detection, and Rock/Paper/Scissors gesture recognition
- **Voice input** — Double-click the knob to record up to 90 seconds, press again to stop early. Transcribed locally via whisper.cpp and sent to your AI agent.
- **Voice output** — Your agent speaks back through the Watcher's speaker using Piper TTS. Configurable reply mode: both (voice + text), voice only, or text only.
- **Presence awareness** — Detects when you arrive or leave, notifies your agent so it can greet you or go to sleep
- **FF9 dialog system** — Messages display in a paginated Final Fantasy IX-style dialog box
- **Background rotation** — 34 curated FFRK backgrounds rotate every 5 minutes with strip-wipe transitions
- **Always connected** — Heartbeat system with auto-recovery if the agent goes down
- **Web UI** — Built-in dashboard at the Watcher's IP with live status, state controls, message input, background cycling, AI model switching, heartbeat monitor, gesture/presence event logs, and voice configuration (model + volume + reply mode)

## Architecture

```
┌─────────────────────────────────────────────────┐
│                 OpenClaw Agent                    │
│                                                   │
│  Receives: voice transcriptions, presence events  │
│  Sends: messages, state changes, speech, heartbeat│
└──────────┬────────────────────┬──────────────────┘
           │ MCP (stdio)        │ WebSocket
           ▼                    ▼
┌──────────────────┐  ┌──────────────────────────┐
│   MCP Server     │  │    Watcher Daemon         │
│  (stateless)     │  │  (LaunchAgent, 24/7)      │
│                  │  │                            │
│  7 tools         │  │  Voice → transcribe        │
│  1 resource      │  │  Presence → notify         │
│                  │  │  Dismiss → next message    │
│  localhost:8378  │  │  Reboot → recovery         │
└────────┬─────────┘  └───────────┬────────────────┘
         │     HTTP (port 80)     │
         ▼                        ▼
┌─────────────────────────────────────────────────┐
│           SenseCap Watcher (ESP32-S3)            │
│                                                   │
│  Renderer  │  Web Server  │  Camera  │  Voice    │
│  412x412 LCD  │  Speaker  │  Mic  │  Knob  │ LED│
└─────────────────────────────────────────────────┘
```

## Repository Structure

```
pokewatcher/              ESP32-S3 firmware (C, ESP-IDF 5.2.1)
├── main/                   Source files, web UI, config
├── partitions.csv          Flash partition layout
└── sdkconfig.defaults      Build configuration

watcher-mcp/              MCP server + daemon (TypeScript)
├── src/index.ts            MCP server entry point (stdio, stateless)
├── src/daemon.ts           24/7 background poller + message queue
├── src/tools.ts            7 MCP tools with auto-pairing logic
├── src/openclaw-client.ts  Persistent WebSocket to OpenClaw gateway
└── src/tts.ts              Piper TTS integration

zidane-dashboard/         Sprite & background planning tool
├── server.py               Python dev server (localhost:8091)
├── index.html              Live animation preview
├── sprites.html            Frame-by-frame sprite catalog
└── backgrounds.html        Background tile picker

sdcard_prep/              SD card assets
├── characters/zidane/      Sprite sheet + frame definitions
└── himax/                  Camera firmware (one-time recovery)

docs/
├── QUICK-START.md          Zero-to-running setup guide
├── DETAILED-GUIDE.md       Complete feature reference
└── knowledgebase/          Internal dev notes (SPI debugging, pin maps, etc.)
```

## Quick Start

See the full [Quick Start Guide](docs/QUICK-START.md) for the complete 12-step setup. The short version:

```bash
# 1. Install ESP-IDF 5.2.1, Node.js 20+, Piper TTS, Python 3
# 2. Clone this repo + Seeed Studio SDK
git clone https://github.com/nacoleon/Agent-Watcher.git
git clone https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware.git /tmp/SenseCAP-Watcher-Firmware

# 3. Prep SD card (FAT32) with sprites from sdcard_prep/characters/zidane/
# 4. Set WiFi credentials
cp pokewatcher/main/config.local.h.example pokewatcher/main/config.local.h
# Edit config.local.h with your SSID and password

# 5. Back up factory partition (first-time only!)
# 6. Build & flash firmware
source ~/esp/esp-idf/export.sh
cp -r pokewatcher /tmp/pokewatcher-build && cd /tmp/pokewatcher-build
idf.py set-target esp32s3 && idf.py build
idf.py -p /dev/cu.usbmodemXXXXX flash    # first time: 'flash'
                                           # future:     'app-flash'

# 7. Build MCP server + download Whisper model
cd watcher-mcp && npm install && npm run build

# 8. Start daemon, configure MCP in OpenClaw
```

## MCP Tools

| Tool | Description |
|---|---|
| `display_message` | Show dialog + set state. Auto-speaks via TTS on voice replies (if reply mode = both). |
| `set_state` | Change visual state (idle, working, alert, greeting, sleeping, reporting, down) |
| `get_status` | Read device state, presence, uptime, queue depth |
| `get_queue` | Check pending messages |
| `speak` | Text-to-speech via Piper TTS. Auto-displays text on voice replies (if reply mode = both). |
| `heartbeat` | Keep-alive signal (1.5hr timeout → "down" state) |
| `reboot` | Hardware reboot |

## Documentation

- **[Quick Start Guide](docs/QUICK-START.md)** — 12-step zero-to-running setup (brand-new device + fresh Mac)
- **[Detailed Guide](docs/DETAILED-GUIDE.md)** — Complete reference: all features, REST API, MCP tools, daemon, troubleshooting

## Requirements

- SenseCap Watcher device + USB-C cable + microSD card (FAT32)
- Mac with Homebrew
- ESP-IDF 5.2.1 (with ESP32-S3 target)
- Node.js 20+
- Python 3
- Piper TTS (`pip3 install piper-tts`)
- OpenClaw gateway running on `ws://127.0.0.1:18789`

## License

Private project.
