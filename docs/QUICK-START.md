# Zidane Watcher — Quick Start Guide

Get your SenseCap Watcher running as an OpenClaw AI desk companion. This guide assumes a brand-new device and a fresh Mac.

---

## Step 1: Install Prerequisites

### Xcode Command Line Tools
```bash
xcode-select --install
```

### Homebrew (if not installed)
```bash
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
```

### Node.js 20+
```bash
brew install node
```

### Python 3 (usually pre-installed on macOS)
```bash
python3 --version
# If missing: brew install python
```

### Piper TTS (text-to-speech engine)
```bash
pip3 install piper-tts
piper --help  # verify it works
```

### ESP-IDF 5.2.1 (ESP32-S3 firmware toolchain)
```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.2.1 --recursive https://github.com/espressif/esp-idf.git esp-idf
cd esp-idf && ./install.sh esp32s3
```

This takes 10-15 minutes. After install, you'll need to source the environment in every new terminal:
```bash
source ~/esp/esp-idf/export.sh
```

### OpenClaw
OpenClaw gateway must be running on `ws://127.0.0.1:18789` with device identity configured in `~/.openclaw/identity/`. See the OpenClaw documentation for setup.

---

## Step 2: Clone the Repository

```bash
git clone https://github.com/nacoleon/Agent-Watcher.git
cd Agent-Watcher
```

---

## Step 3: Clone the SDK

The firmware depends on Seeed Studio's SenseCAP Watcher SDK for hardware drivers. Clone it to `/tmp`:

```bash
git clone https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware.git /tmp/SenseCAP-Watcher-Firmware
```

### Apply the BSP Patch (Required)

The SDK has a build error when the touch panel is disabled. Fix it:

```bash
# Edit /tmp/SenseCAP-Watcher-Firmware/components/sensecap-watcher/sensecap-watcher.c
# Find line ~765:
#   .sensitivity = CONFIG_LVGL_INPUT_DEVICE_SENSITIVITY,
# Change to:
#   .sensitivity = 50,
```

This is needed because the config variable is only defined when touch is enabled, which we disable.

---

## Step 4: Prepare the SD Card

The Watcher loads character sprites from the SD card at boot.

### Format the SD Card
- Use **Disk Utility** on macOS: select the SD card → Erase → Format: **MS-DOS (FAT)** → Erase
- Or from terminal: `diskutil eraseDisk FAT32 WATCHER MBRFormat /dev/diskN` (replace `diskN` with your SD card)

### Copy Sprite Files to Root

```bash
# Copy these two files to the SD card root:
cp sdcard_prep/characters/zidane/overworld.raw /Volumes/WATCHER/
cp sdcard_prep/characters/zidane/frames.json /Volumes/WATCHER/
```

That's it — just the sprite sheet and frame definitions. Insert the SD card into the Watcher's microSD slot.

> **Note:** The Himax AI camera chip comes pre-flashed from the factory. The `sdcard_prep/himax/` directory contains firmware for re-flashing if needed — see the [Detailed Guide](DETAILED-GUIDE.md#himax-firmware-one-time-recovery-only).

---

## Step 5: Configure WiFi

```bash
cp pokewatcher/main/config.local.h.example pokewatcher/main/config.local.h
```

Edit `pokewatcher/main/config.local.h` with your WiFi credentials:

```c
#undef PW_WIFI_SSID_DEFAULT
#undef PW_WIFI_PASSWORD_DEFAULT

#define PW_WIFI_SSID_DEFAULT       "YourNetworkName"
#define PW_WIFI_PASSWORD_DEFAULT   "YourPassword"
```

This file is gitignored — your credentials are never committed.

---

## Step 6: Back Up the Factory Partition (First-Time Only)

**Do this BEFORE flashing.** The `nvsfactory` partition contains the device's unique EUI and SenseCraft credentials. It's irreplaceable.

```bash
source ~/esp/esp-idf/export.sh

# Connect the Watcher via USB-C and find the serial port:
ls /dev/cu.usbmodem*
# You'll see two ports — use the higher-numbered one (e.g., ...623 not ...621)

# Back up nvsfactory (200KB at offset 0x9000):
mkdir -p backups
esptool.py --chip esp32s3 -p /dev/cu.usbmodemXXXXX \
  read_flash 0x9000 0x32000 backups/nvsfactory_backup.bin
```

Keep this backup safe. You can restore stock firmware anytime, but you can't recreate the factory credentials.

---

## Step 7: Build & Flash Firmware

**Critical:** The project path may contain spaces, which breaks the ESP-IDF linker. Always build from `/tmp`.

```bash
# 1. Source ESP-IDF environment
source ~/esp/esp-idf/export.sh

# 2. Copy firmware source to /tmp (no spaces in path)
rm -rf /tmp/pokewatcher-build
cp -r pokewatcher /tmp/pokewatcher-build

# 3. Build
cd /tmp/pokewatcher-build
idf.py set-target esp32s3
idf.py build
```

The first build takes several minutes. Subsequent builds are faster (incremental).

### Flash to the Watcher

```bash
# Find your serial port (use the higher-numbered one):
ls /dev/cu.usbmodem*

# First-time flash — use 'flash' to write app + partition table:
idf.py -p /dev/cu.usbmodemXXXXX flash

# For all future flashes — use 'app-flash' to preserve NVS/WiFi settings:
idf.py -p /dev/cu.usbmodemXXXXX app-flash
```

### Verify Boot

```bash
idf.py -p /dev/cu.usbmodemXXXXX monitor
# (Ctrl+] to exit)
```

You should see `[BOOT]` messages 1/7 through 7/7, then:
```
=== Zidane Watcher v2 running ===
Dashboard: http://zidane.local
```

Zidane appears on the 412×412 round LCD, walking to his idle position with a background image. Note the IP address printed in the WiFi connect log — you'll need it next.

---

## Step 8: Verify Watcher is Online

Open a browser to `http://<WATCHER_IP>` (the IP from the serial log). You should see the web dashboard with:

- Live status (state, person detection, uptime, WiFi signal)
- State control buttons (click to change Zidane's animation)
- Message input (type and send dialog messages)
- Background controls (cycle tiles, toggle auto-rotation)
- Heartbeat monitor, AI model selector, gesture/presence logs
- Voice config (voice model dropdown, volume slider, reply mode)

Or from terminal:
```bash
curl http://<WATCHER_IP>/api/status
```

> **Tip:** Assign a static IP to your Watcher in your router's DHCP settings (using the Watcher's MAC address). This prevents the IP from changing after a reboot. Look for "DHCP reservations" or "static leases" in your router's admin page.

---

## Step 9: Build the MCP Server

```bash
cd watcher-mcp
npm install
npm run build
```

This compiles TypeScript to `dist/` and builds the whisper.cpp native binary.

### Verify whisper.cpp built correctly:
```bash
ls node_modules/whisper-node/lib/whisper.cpp/main
# Should show the binary
```

### Download the Whisper speech-to-text model:
```bash
cd node_modules/whisper-node/lib/whisper.cpp
bash models/download-ggml-model.sh base.en
cd ../../../../..
# Back to project root
```

---

## Step 10: Configure MCP in OpenClaw

Add the Watcher MCP server to your OpenClaw configuration:

```json
{
  "mcpServers": {
    "watcher": {
      "command": "node",
      "args": ["/absolute/path/to/Agent-Watcher/watcher-mcp/dist/index.js"],
      "transportType": "stdio",
      "env": {
        "WATCHER_URL": "http://YOUR_WATCHER_IP"
      }
    }
  }
}
```

Replace `/absolute/path/to/Agent-Watcher` with your actual clone path, and `YOUR_WATCHER_IP` with the Watcher's IP address.

---

## Step 11: Start the Daemon

The daemon runs 24/7, polling the Watcher for voice audio, presence changes, and message dismissals.

### Test it manually first:
```bash
WATCHER_URL="http://YOUR_WATCHER_IP" node watcher-mcp/dist/daemon.js
```

You should see:
```
[system] Watcher daemon started
[system] Polling http://YOUR_WATCHER_IP every 5000ms
[poll] alive — uptime=...
```

Press `Ctrl+C` to stop once verified.

### Run permanently as a macOS LaunchAgent:

```bash
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
        <string>/absolute/path/to/Agent-Watcher/watcher-mcp/dist/daemon.js</string>
    </array>
    <key>WorkingDirectory</key>
    <string>/absolute/path/to/Agent-Watcher/watcher-mcp</string>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>EnvironmentVariables</key>
    <dict>
        <key>WATCHER_URL</key>
        <string>http://YOUR_WATCHER_IP</string>
    </dict>
    <key>StandardOutPath</key>
    <string>/tmp/watcher-daemon.log</string>
    <key>StandardErrorPath</key>
    <string>/tmp/watcher-daemon.log</string>
</dict>
</plist>
EOF
```

**Edit the plist** — replace `/absolute/path/to/Agent-Watcher` and `YOUR_WATCHER_IP` with your actual values.

Then load it:
```bash
launchctl load ~/Library/LaunchAgents/ai.openclaw.watcher-daemon.plist
launchctl list | grep watcher
# Should show the daemon with a PID
```

---

## Step 12: Test Everything

### Send a test message:
```bash
curl -X POST http://YOUR_WATCHER_IP/api/message \
  -H "Content-Type: application/json" \
  -d '{"text": "Hello from Quick Start!", "level": "info"}'
```

Zidane should show the FF9 dialog box with your message. Press the knob to dismiss.

### Test voice input:
1. **Double-click the knob** — LED turns blue, Zidane enters reporting animation
2. Speak your message (up to 90 seconds)
3. **Single-press the knob** to stop recording (or wait for auto-stop)
4. LED turns green — daemon picks up audio, transcribes, sends to OpenClaw
5. OpenClaw's reply auto-pairs voice + text based on the Reply Mode setting

### Test state changes:
```bash
curl -X PUT http://YOUR_WATCHER_IP/api/agent-state \
  -H "Content-Type: application/json" \
  -d '{"state": "greeting"}'
```

---

## Optional: Sprite & Background Planning Dashboard

Preview and plan custom sprites and backgrounds without hardware:

```bash
python3 zidane-dashboard/server.py
# Open http://localhost:8091
```

- **Main preview** (`/`) — See how your character looks on the 412×412 circular display
- **Sprite catalog** (`/sprites.html`) — Frame-by-frame reference with coordinates and animation previews
- **Background catalog** (`/backgrounds.html`) — Toggle which tiles to include in the rotation pool

See the [Detailed Guide](DETAILED-GUIDE.md#sprite--background-planning-dashboard) for details.

---

## You're Done!

Zidane is now running on your Watcher, connected to OpenClaw via MCP. The daemon handles voice transcription, presence detection, and message queuing automatically.

**Next:** Read the [Detailed Guide](DETAILED-GUIDE.md) for the complete feature reference, API documentation, and troubleshooting.
