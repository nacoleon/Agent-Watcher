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

Copy the example config and fill in your credentials:

```bash
cp pokewatcher/main/config.local.h.example pokewatcher/main/config.local.h
```

Edit `pokewatcher/main/config.local.h`:

```c
#define PW_WIFI_SSID_DEFAULT       "YourNetworkName"
#define PW_WIFI_PASSWORD_DEFAULT   "YourPassword"
```

This file is gitignored so your credentials are never committed. The firmware auto-includes it if it exists. The Watcher gets a DHCP address — note the IP from the serial console after boot (you'll need it for MCP config).

## Step 4: Get the SDK Components

The firmware depends on Seeed Studio's SenseCAP Watcher SDK. Clone it to `/tmp`:

```bash
git clone https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware.git /tmp/SenseCAP-Watcher-Firmware
```

The build expects components at `/tmp/SenseCAP-Watcher-Firmware/components` (referenced in `pokewatcher/CMakeLists.txt`).

## Step 5: Build & Flash Firmware

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

# 4. Find your serial port
ls /dev/cu.usb*
# Look for something like /dev/cu.usbmodemXXXXX

# 5. Flash (app partition only — preserves NVS settings)
idf.py -p /dev/cu.usbmodemXXXXX app-flash

# 6. Monitor serial output to confirm boot
idf.py -p /dev/cu.usbmodemXXXXX monitor
# (Ctrl+] to exit monitor)
```

You should see `[BOOT]` messages 1/7 through 7/7, then Zidane appears on the 412×412 round LCD walking to his idle position.

## Step 6: Verify Watcher is Online

Open a browser to `http://<WATCHER_IP>` (e.g., `http://10.0.0.40`). You should see the Watcher's built-in web dashboard with:

- Live status (state, person detection, uptime, WiFi signal)
- State control buttons (click to change Zidane's animation)
- Message input (type and send dialog messages)
- Background controls (cycle tiles, toggle auto-rotation)
- Heartbeat monitor, AI model selector, gesture/presence logs
- Voice config (voice model dropdown + volume slider)

Or from terminal:
```bash
curl http://10.0.0.40/api/status
```

Expected: JSON with `agent_state`, `person_present`, `uptime_seconds`, etc.

## Step 7: Build the MCP Server

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

## Step 8: Configure MCP in OpenClaw

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

## Step 9: Start the Daemon

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

## Step 10: Install Piper TTS

```bash
pip install piper-tts
# Verify:
piper --help
```

Voice models are auto-downloaded on first use to `/tmp/piper-voices/`. The default voice is `en_US-bryce-medium`.

## Step 11: Test Everything

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
