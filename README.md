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
- **Web UI** — Built-in dashboard at the Watcher's IP with live status, state controls, message input, background cycling, AI model switching, heartbeat monitor, gesture/presence event logs, and voice configuration (model + volume)

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

| Component | Location | Role |
|---|---|---|
| **Firmware** | `pokewatcher/` | ESP32-S3 firmware — display, camera AI, web server, voice recording |
| **MCP Server** | `watcher-mcp/` | Stateless stdio bridge — 7 tools for OpenClaw to control the Watcher |
| **Daemon** | `watcher-mcp/src/daemon.ts` | 24/7 background poller — voice transcription, presence detection, message queue |
| **Dashboard** | `zidane-dashboard/` | Browser-based sprite & background planning tool (localhost:8091) |

## Quick Start

See the full [Quick Start Guide](docs/QUICK-START.md) for step-by-step setup. The short version:

```bash
# 1. Prep SD card with sprites + Himax firmware from sdcard_prep/
# 2. Set WiFi credentials
cp pokewatcher/main/config.local.h.example pokewatcher/main/config.local.h
# Edit config.local.h with your SSID and password
# 3. Clone the SDK
git clone https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware.git /tmp/SenseCAP-Watcher-Firmware
# 4. Build & flash firmware (must build from /tmp — space in path breaks linker)
cp -r pokewatcher /tmp/pokewatcher-build
cd /tmp/pokewatcher-build
idf.py build
idf.py -p /dev/cu.usbmodemXXXXX app-flash  # find your port: ls /dev/cu.usb*

# 5. Build MCP server + download Whisper model
cd watcher-mcp && npm install && npm run build

# 6. Start daemon
node watcher-mcp/dist/daemon.js

# 6. Add MCP server to OpenClaw config
```

## MCP Tools

| Tool | Description |
|---|---|
| `display_message` | Show dialog + set state. Messages queue automatically. |
| `set_state` | Change visual state (idle, working, alert, greeting, sleeping, reporting, down) |
| `get_status` | Read device state, presence, uptime, queue depth |
| `get_queue` | Check pending messages |
| `speak` | Text-to-speech via Piper TTS through the Watcher's speaker |
| `heartbeat` | Keep-alive signal (1.5hr timeout → "down" state) |
| `reboot` | Hardware reboot |

## Documentation

- **[Quick Start Guide](docs/QUICK-START.md)** — Get running in ~15 minutes
- **[Detailed Guide](docs/DETAILED-GUIDE.md)** — Complete reference: all features, REST API, MCP tools, daemon, troubleshooting

## Requirements

- SenseCap Watcher device
- Mac with Homebrew
- ESP-IDF 5.2.1
- Node.js 20+
- Piper TTS (`pip install piper-tts`)
- OpenClaw CLI

## License

Private project.
