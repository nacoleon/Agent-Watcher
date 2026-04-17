# Handoff: Gesture Actions + MCP Notifications

**Date:** 2026-04-17
**Previous agent session:** Fixed Himax camera, built gesture detection, AI model switching UI

---

## Your Two Tasks

### Task 1: Gesture Actions — Make Zidane React to Gestures

**Current state:** The camera detects Rock/Paper/Scissors and logs them. `PW_EVENT_GESTURE_DETECTED` fires in `agent_state.c` (line ~178). But Zidane doesn't visually react.

**What to build:**
- When a gesture is confirmed, Zidane should do something visible — play an animation, show a dialog, change state, or some combination
- The user hasn't specified exactly WHAT reaction they want — ask them before implementing
- Possible ideas: greeting animation on any gesture, different dialog per gesture ("Rock? Let's battle!"), state change to alert/greeting

**Key files:**
- `pokewatcher/main/agent_state.c` — gesture event handler (line ~178), currently just logs
- `pokewatcher/main/himax_task.c` — gesture detection logic, `on_event` callback
- `pokewatcher/main/renderer.c` — animation system, `pw_renderer_show_message()`, state transitions
- `pokewatcher/main/event_queue.h` — event types including `PW_EVENT_GESTURE_DETECTED`

**Gesture event data:**
```c
event.data.gesture.gesture  // "Rock", "Paper", "Scissors"
event.data.gesture.score    // 85-100
event.data.gesture.box_w    // bounding box width
event.data.gesture.box_h    // bounding box height
```

### Task 2: MCP Person/Gesture Notifications to OpenClaw

**Current state:** The MCP server (`watcher-mcp/`) has a presence poller that polls `GET /api/status` every 5 seconds. It sends `person_arrived` and `person_left` MCP notifications. But it reads `person_present` from the API which is only set by person detection (model 1). When the camera auto-switches to gesture mode (model 3), person presence isn't tracked.

**What to build:**
- The MCP server should send gesture notifications to OpenClaw when gestures are detected
- Add `gesture_log` to what the MCP poller checks — when new gestures appear, send MCP notifications
- The API already returns `gesture_log`, `active_model`, and `model_name` in `GET /api/status`

**Key files:**
- `watcher-mcp/src/index.ts` — MCP server, presence poller, notification system
- `pokewatcher/main/web_server.c` — API already serves gesture_log in status

---

## Critical Build Notes

**sdkconfig MUST have:**
```
CONFIG_FREERTOS_HZ=1000
```
Without this, the Himax camera cannot communicate (SPI transport's 2ms delay rounds to 0 at 100Hz).

**Build from /tmp:**
```bash
bash /tmp/idf_build.sh /tmp/pokewatcher-build build
bash /tmp/idf_build.sh /tmp/pokewatcher-build -p /dev/cu.usbmodem5A8A0533623 app-flash
```

**After editing source files, copy to build dir:**
```bash
cp "pokewatcher/main/<file>" /tmp/pokewatcher-build/main/
cp "pokewatcher/main/web/index.html" /tmp/pokewatcher-build/main/web/
```

**sdkconfig keeps reverting to 100Hz** — the build regenerates it. Must `sed -i '' 's/CONFIG_FREERTOS_HZ=100/CONFIG_FREERTOS_HZ=1000/' /tmp/pokewatcher-build/sdkconfig` after any clean rebuild. It's in sdkconfig.defaults but cmake sometimes ignores it.

**Serial monitor resets the device** — opening the serial port with DTR/RTS toggles causes a reboot. Use the web API (`curl http://10.0.0.40/api/status`) for debugging instead.

## Current Detection Behavior

- **Boot:** Person detection (model 1)
- **Person arrives:** Auto-switches to Gesture mode (model 3)
- **Gesture mode:** Rock (85%, 4 frames, box≥130px) / Paper (85%, 4 frames) / Scissors (85%, 4 frames)
- **Same gesture re-logs after 6 seconds**
- **Object left after 3 minutes no detection**
- **20 minutes idle in gesture mode:** Auto-switches back to Person mode
- **Purple double LED flash** on confirmed gesture
- **Display wakes** on gesture detection

## What NOT to Change

- Don't touch `sscma_client_ops.c` — it has critical buffer fixes and OOB write fix
- Don't change CONFIG_FREERTOS_HZ — must stay at 1000
- Don't re-initialize the SD card after boot — it's powered off for Himax SPI
- Don't add delays between camera AT commands — they work fine at 1000Hz
