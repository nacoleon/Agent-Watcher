# Push-to-Talk Enhancements Design

**Date**: 2026-04-20
**Status**: Approved

## Summary

Three enhancements to the Watcher's push-to-talk voice system:

1. **Early stop** — single knob press stops recording immediately (instead of waiting for timeout)
2. **90-second timeout** — increase max recording from 10s to 90s (PSRAM ceiling)
3. **Response mode setting** — configurable `both` / `voice_only` / `text_only` for voice-triggered replies, adjustable from Web UI, persisted in NVS across restarts

## Decisions

| Decision | Choice | Rationale |
|---|---|---|
| Stop recording gesture | Single knob press | Natural walkie-talkie UX; double-click starts, single press stops |
| Recording timeout | 90s fixed constant | Practical PSRAM max (~2.88MB of 6MB free). Not user-adjustable. |
| Response mode default | `both` (text + voice) | User's stated preference |
| Response mode scope | Voice replies only | Proactive messages (greetings, alerts) stay under OpenClaw's control |
| Response mode enforcement | MCP tool auto-pairing | Tools check setting and auto-call the complementary tool |
| Recording animation state | `PW_STATE_REPORTING` | Existing state, better fit than `working` |
| Web UI placement | Voice card | Single dropdown added below volume slider |

## 1. Early Stop Recording

### Current Behavior

- Double-click knob → `voice_record_task` runs for full `PW_VOICE_RECORD_MS` (10s)
- No way to interrupt recording
- Single knob press during recording is ignored (consumed as stale by `pw_dialog_tick`)

### New Behavior

- Double-click knob → recording starts (pulsing blue LED, `PW_STATE_REPORTING`)
- Single knob press during recording → sets `s_stop_requested = true`
- Recording loop in `voice_record_task` checks this flag each iteration and breaks early
- Minimum 0.5s of audio still enforced (shorter recordings discarded with red flash)
- If no press, auto-stops at 90s timeout as before

### Implementation

**voice_input.c**:
- Add `static volatile bool s_stop_requested = false;`
- Add `void pw_voice_request_stop(void)` — sets the flag
- Add `bool pw_voice_is_recording(void)` — returns `s_recording`
- Recording loop condition becomes: `while (!s_stop_requested && esp_timer_get_time() < end_time && total_read < PW_VOICE_BUF_SIZE)`
- Reset `s_stop_requested = false` at the start of `voice_record_task`
- Change `PW_STATE_WORKING` to `PW_STATE_REPORTING`

**voice_input.h**:
- Declare `pw_voice_is_recording()` and `pw_voice_request_stop()`

**dialog.c**:
- In `knob_btn_cb`: if `pw_voice_is_recording()` is true, call `pw_voice_request_stop()` and return early (don't set `s_knob_pressed` for dialog dismiss)
- Note: `knob_btn_cb` runs in button task context (not true ISR). Both `pw_voice_is_recording()` and `pw_voice_request_stop()` only read/write volatile bools, so this is safe without locks.

## 2. Recording Timeout — 90 Seconds

### Changes

**config.h** (line 62):
```c
#define PW_VOICE_RECORD_MS     90000          // 90 seconds recording (PSRAM max)
```

`PW_VOICE_BUF_SIZE` auto-calculates: `16000 * 2 * 90 = 2,880,000 bytes` (~2.88MB).

### Memory Budget

- ESP32-S3 Octal PSRAM: 8MB total
- Estimated used by LVGL, backgrounds, fonts, DMA: ~1-2MB
- Audio buffer: 2.88MB
- Remaining: ~3-4MB headroom

The PSRAM allocation happens only during recording and is freed after the daemon picks up the audio via `GET /api/audio` + `DELETE /api/audio`.

## 3. Response Mode Setting

### NVS Persistence

- **Namespace**: `pokewatcher` (existing)
- **Key**: `"resp_mode"` (string)
- **Values**: `"both"`, `"voice_only"`, `"text_only"`
- **Default**: `"both"`

### Firmware API

**web_server.c**:

New static variable:
```c
static char s_response_mode[16] = "both";
```

Loaded/saved alongside voice config in `load_voice_config()` / `save_voice_config()`:
```c
nvs_get_str(nvs, "resp_mode", s_response_mode, &len);
nvs_set_str(nvs, "resp_mode", s_response_mode);
```

Exposed in existing endpoints:
- `GET /api/status` → includes `"response_mode": "both"`
- `GET /api/voice` → includes `"response_mode": "both"`
- `PUT /api/voice` → accepts optional `"response_mode"` field, validates against allowed values, persists to NVS

### Web UI

Add a dropdown to the Voice card in `index.html`, below the volume slider:

```html
<div class="stat">
    <span class="label">Reply Mode</span>
    <select id="resp-mode" onchange="setVoice()" style="...">
        <option value="both">Both (Voice + Text)</option>
        <option value="voice_only">Voice Only</option>
        <option value="text_only">Text Only</option>
    </select>
</div>
```

The existing `setVoice()` function is extended to include `response_mode` in the `PUT /api/voice` body. The `poll()` function reads it from `/api/status` and updates the dropdown.

## 4. Response Mode Enforcement (MCP Server)

### Voice Context Tracking

The daemon knows when a voice input is being processed (it's the one that transcribes and sends to OpenClaw). It needs to signal this to the MCP tools.

**daemon.ts** additions:
- New in-memory flag: `let voiceContextActive = false`
- After transcription + `sendToZidane()`, set `voiceContextActive = true`
- Auto-clear after 60 seconds (safety timeout)
- New daemon HTTP API endpoints:
  - `GET /voice-context` → `{ active: true/false }`
  - `DELETE /voice-context` → clears the flag

### MCP Tool Auto-Pairing

**tools.ts** — `speak` tool enhancement:
1. Check `GET http://127.0.0.1:8378/voice-context` — if not active, behave normally
2. If active, read `GET /api/voice` from Watcher to get `response_mode`
3. If `response_mode` is `both`: do TTS as normal, then ALSO call the daemon queue `POST /queue` with the same text (to display on screen)
4. If `response_mode` is `text_only`: skip TTS entirely, only call daemon queue
5. After enforcement, `DELETE /voice-context` to clear the flag

**tools.ts** — `display_message` tool enhancement:
1. Same voice-context check
2. If `response_mode` is `both`: display as normal, then ALSO call `textToSpeech()` + `playAudio()`
3. If `response_mode` is `voice_only`: skip display, only do TTS
4. Clear voice context after enforcement

**Loop guard**: Each tool passes an internal `_paired: true` flag when calling the complementary action, so the auto-paired call doesn't trigger another pairing. Since the auto-paired calls go directly to Watcher APIs (not through MCP tools), there's no recursion risk — but the voice-context clear after first enforcement is the primary guard.

### Sequence Diagram — Voice Reply with `response_mode: both`

```
User          Watcher         Daemon          OpenClaw        MCP Server
  |  double-click  |              |               |               |
  |--------------->|  record...   |               |               |
  |  (knob press)  |  stop early  |               |               |
  |--------------->|              |               |               |
  |                |  audio_ready |               |               |
  |                |<-------------|  GET /audio   |               |
  |                |  WAV data    |               |               |
  |                |------------->|  transcribe   |               |
  |                |              |  voiceCtx=true|               |
  |                |              |--openclaw agent -->            |
  |                |              |               |  speak("Hi")  |
  |                |              |               |-------------->|
  |                |              |  GET /voice-context           |
  |                |              |<------------------------------|
  |                |              |  { active: true }             |
  |                |              |------------------------------->|
  |                |              |               |  GET /api/voice
  |                |<--------------------------------------------|
  |                |  { response_mode: "both" }                  |
  |                |-------------------------------------------->|
  |                |              |               |  TTS + play  |
  |                |<--------------------------------------------|
  |                |  (speaker)   |               |               |
  |                |              |  POST /queue (display)        |
  |                |              |<------------------------------|
  |                |  (screen)    |               |               |
  |                |              |  DEL /voice-context           |
  |                |              |<------------------------------|
```

## 5. Files Changed

| File | Change |
|---|---|
| `pokewatcher/main/config.h` | `PW_VOICE_RECORD_MS` 10000 → 90000 |
| `pokewatcher/main/voice_input.c` | Early stop flag, `pw_voice_is_recording()`, `pw_voice_request_stop()`, use `PW_STATE_REPORTING` |
| `pokewatcher/main/voice_input.h` | Declare new public functions |
| `pokewatcher/main/dialog.c` | Route knob press to `pw_voice_request_stop()` when recording is active |
| `pokewatcher/main/web_server.c` | `response_mode` NVS load/save, expose in `GET /api/status`, `GET /api/voice`, accept in `PUT /api/voice` |
| `pokewatcher/main/web/index.html` | Response mode dropdown in Voice card, update `setVoice()` and `poll()` |
| `watcher-mcp/src/tools.ts` | Auto-pair `speak` ↔ `display_message` based on response_mode + voice context |
| `watcher-mcp/src/daemon.ts` | Voice context flag, `GET /voice-context`, `DELETE /voice-context` endpoints |

## 6. Edge Cases

- **Recording stopped before 0.5s**: Discarded with red LED flash (existing behavior, unchanged)
- **Speaker busy during auto-paired speak**: Existing retry logic (1s wait + retry) handles this
- **Voice context expires (60s timeout)**: If OpenClaw is slow to respond, the auto-pairing won't fire. This is acceptable — 60s is generous for a voice reply.
- **Both tools called by OpenClaw**: If OpenClaw independently calls both `speak` and `display_message` for a voice reply, and response_mode is `both`, we need to avoid double-display/double-speak. The voice-context flag is cleared after the first tool enforces, so the second tool sees no active context and behaves normally. Net result: each action happens exactly once.
- **NVS full**: Extremely unlikely with 16KB partition and few keys. If `nvs_set_str` fails, the setting reverts to default on next boot.
