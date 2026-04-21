# Push-to-Talk Enhancements Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add early-stop recording via knob press, increase timeout to 90s, and add a persistent response_mode setting (both/voice_only/text_only) configurable from the Web UI that controls whether voice replies include text, voice, or both.

**Architecture:** Firmware changes (early stop + NVS setting) are self-contained in 4 files. MCP server changes (voice context tracking + tool auto-pairing) are in 2 files. The daemon sets a voice-context flag after transcription; MCP tools check it and the Watcher's response_mode to auto-pair speak↔display_message.

**Tech Stack:** ESP-IDF (C), LVGL, NVS, Node.js/TypeScript (MCP server)

**Spec:** `docs/superpowers/specs/2026-04-20-push-to-talk-enhancements-design.md`

---

## File Map

| File | Action | Responsibility |
|---|---|---|
| `pokewatcher/main/config.h` | Modify | Change `PW_VOICE_RECORD_MS` to 90000 |
| `pokewatcher/main/voice_input.h` | Modify | Declare `pw_voice_is_recording()` and `pw_voice_request_stop()` |
| `pokewatcher/main/voice_input.c` | Modify | Early stop flag, expose recording state, use `PW_STATE_REPORTING` |
| `pokewatcher/main/dialog.c` | Modify | Route knob press to stop recording when active |
| `pokewatcher/main/web_server.c` | Modify | `response_mode` NVS load/save, expose in API |
| `pokewatcher/main/web/index.html` | Modify | Response mode dropdown in Voice card |
| `watcher-mcp/src/daemon.ts` | Modify | Voice context flag + HTTP endpoints |
| `watcher-mcp/src/tools.ts` | Modify | Auto-pair speak↔display_message |

---

### Task 1: Increase Recording Timeout to 90 Seconds

**Files:**
- Modify: `pokewatcher/main/config.h:62`

- [ ] **Step 1: Change the timeout constant**

In `pokewatcher/main/config.h`, change line 62 from:
```c
#define PW_VOICE_RECORD_MS     10000          // 10 seconds recording
```
to:
```c
#define PW_VOICE_RECORD_MS     90000          // 90 seconds recording (PSRAM max)
```

`PW_VOICE_BUF_SIZE` on line 65 auto-calculates via the macro: `16000 * 2 * 90 = 2,880,000 bytes`.

- [ ] **Step 2: Commit**

```bash
git add pokewatcher/main/config.h
git commit -m "feat(voice): increase recording timeout to 90 seconds"
```

---

### Task 2: Add Early Stop to Recording

**Files:**
- Modify: `pokewatcher/main/voice_input.h`
- Modify: `pokewatcher/main/voice_input.c`
- Modify: `pokewatcher/main/dialog.c`

- [ ] **Step 1: Add new function declarations to voice_input.h**

In `pokewatcher/main/voice_input.h`, add before the closing `#endif`:

```c
// Returns true if currently recording audio
bool pw_voice_is_recording(void);

// Request early stop of current recording (safe to call from button callback)
void pw_voice_request_stop(void);
```

- [ ] **Step 2: Add early stop flag and new functions to voice_input.c**

In `pokewatcher/main/voice_input.c`, add a new volatile flag after line 33 (`static volatile bool s_recording = false;`):

```c
static volatile bool s_stop_requested = false;
```

Add two new public functions after `pw_voice_init()` (after line 208):

```c
bool pw_voice_is_recording(void)
{
    return s_recording;
}

void pw_voice_request_stop(void)
{
    s_stop_requested = true;
}
```

- [ ] **Step 3: Reset stop flag at recording start and check it in the loop**

In `voice_record_task()` in `pokewatcher/main/voice_input.c`, add at the start of the function (after line 58, the opening brace):

```c
    s_stop_requested = false;
```

Change the recording loop condition on line 98 from:
```c
    while (esp_timer_get_time() < end_time && total_read < PW_VOICE_BUF_SIZE) {
```
to:
```c
    while (!s_stop_requested && esp_timer_get_time() < end_time && total_read < PW_VOICE_BUF_SIZE) {
```

- [ ] **Step 4: Change recording state from WORKING to REPORTING**

In `voice_record_task()` in `pokewatcher/main/voice_input.c`, change line 67 from:
```c
    pw_agent_state_set(PW_STATE_WORKING);
```
to:
```c
    pw_agent_state_set(PW_STATE_REPORTING);
```

- [ ] **Step 5: Route knob press to stop recording in dialog.c**

In `pokewatcher/main/dialog.c`, add the include at the top (after `#include "config.h"` on line 3):

```c
#include "voice_input.h"
```

Then modify the `knob_btn_cb` function (lines 102-106) from:
```c
static void knob_btn_cb(void *arg, void *data)
{
    s_knob_pressed = true;
    s_btn_wake_requested = true;
}
```
to:
```c
static void knob_btn_cb(void *arg, void *data)
{
    if (pw_voice_is_recording()) {
        pw_voice_request_stop();
        return;
    }
    s_knob_pressed = true;
    s_btn_wake_requested = true;
}
```

- [ ] **Step 6: Build firmware to verify compilation**

```bash
cd /tmp/pokewatcher-build && idf.py build
```

Expected: Build succeeds with no errors.

- [ ] **Step 7: Commit**

```bash
git add pokewatcher/main/voice_input.h pokewatcher/main/voice_input.c pokewatcher/main/dialog.c
git commit -m "feat(voice): early stop recording via knob press

Double-click starts recording, single press stops it early.
Uses PW_STATE_REPORTING instead of PW_STATE_WORKING during recording."
```

---

### Task 3: Add response_mode NVS Setting and API

**Files:**
- Modify: `pokewatcher/main/web_server.c`

- [ ] **Step 1: Add response_mode static variable**

In `pokewatcher/main/web_server.c`, add after line 30 (`static int s_speaker_volume = 90;`):

```c
static char s_response_mode[16] = "both";
```

- [ ] **Step 2: Load response_mode from NVS**

In the `load_voice_config()` function in `pokewatcher/main/web_server.c`, add after line 38 (`nvs_get_i32(nvs, "volume", (int32_t *)&s_speaker_volume);`):

```c
        size_t rm_len = sizeof(s_response_mode);
        nvs_get_str(nvs, "resp_mode", s_response_mode, &rm_len);
```

Note: `nvs_get_str` silently fails if the key doesn't exist, leaving the default "both" intact.

- [ ] **Step 3: Save response_mode to NVS**

In the `save_voice_config()` function in `pokewatcher/main/web_server.c`, add after line 48 (`nvs_set_i32(nvs, "volume", s_speaker_volume);`):

```c
        nvs_set_str(nvs, "resp_mode", s_response_mode);
```

- [ ] **Step 4: Expose response_mode in GET /api/status**

In `handle_api_status()` in `pokewatcher/main/web_server.c`, add after line 147 (`cJSON_AddNumberToObject(root, "speaker_volume", s_speaker_volume);`):

```c
    cJSON_AddStringToObject(root, "response_mode", s_response_mode);
```

- [ ] **Step 5: Expose response_mode in GET /api/voice**

In `handle_api_voice_get()` in `pokewatcher/main/web_server.c`, add after line 460 (`cJSON_AddNumberToObject(root, "volume", s_speaker_volume);`):

```c
    cJSON_AddStringToObject(root, "response_mode", s_response_mode);
```

- [ ] **Step 6: Accept response_mode in PUT /api/voice**

In `handle_api_voice_put()` in `pokewatcher/main/web_server.c`, add after the volume handling block (after line 495, `if (s_speaker_volume > 95) s_speaker_volume = 95;`):

```c
    cJSON *resp_mode = cJSON_GetObjectItem(root, "response_mode");
    if (resp_mode && cJSON_IsString(resp_mode)) {
        const char *rm = resp_mode->valuestring;
        if (strcmp(rm, "both") == 0 || strcmp(rm, "voice_only") == 0 || strcmp(rm, "text_only") == 0) {
            strncpy(s_response_mode, rm, sizeof(s_response_mode) - 1);
            s_response_mode[sizeof(s_response_mode) - 1] = '\0';
        }
    }
```

- [ ] **Step 7: Update log line to include response_mode**

In `handle_api_voice_put()`, change the log line (line 500) from:
```c
    ESP_LOGI(TAG, "Voice config updated: voice=%s volume=%d", s_voice_name, s_speaker_volume);
```
to:
```c
    ESP_LOGI(TAG, "Voice config updated: voice=%s volume=%d response_mode=%s", s_voice_name, s_speaker_volume, s_response_mode);
```

- [ ] **Step 8: Build firmware to verify compilation**

```bash
cd /tmp/pokewatcher-build && idf.py build
```

Expected: Build succeeds with no errors.

- [ ] **Step 9: Commit**

```bash
git add pokewatcher/main/web_server.c
git commit -m "feat(voice): add response_mode setting with NVS persistence

New setting: both/voice_only/text_only. Default: both.
Persisted in NVS, exposed in GET /api/status, GET /api/voice,
accepted in PUT /api/voice."
```

---

### Task 4: Add Response Mode Dropdown to Web UI

**Files:**
- Modify: `pokewatcher/main/web/index.html`

- [ ] **Step 1: Add the response mode dropdown to the Voice card**

In `pokewatcher/main/web/index.html`, add after the volume stat div (after line 105, the closing `</div>` of the volume row):

```html
        <div class="stat">
            <span class="label">Reply Mode</span>
            <select id="resp-mode" onchange="setVoice()" style="flex:1;padding:5px;background:#1a1a4e;border:1px solid #2a2a5e;border-radius:6px;color:#e0e0e0;font-family:inherit;font-size:12px;">
                <option value="both">Both (Voice + Text)</option>
                <option value="voice_only">Voice Only</option>
                <option value="text_only">Text Only</option>
            </select>
        </div>
```

- [ ] **Step 2: Include response_mode in the setVoice() function**

In `pokewatcher/main/web/index.html`, modify the `setVoice()` function (around line 195) from:
```javascript
        async function setVoice() {
            const voice = document.getElementById('voice-select').value;
            const volume = parseInt(document.getElementById('vol-slider').value);
            document.getElementById('vol-val').textContent = volume;
            await fetch('/api/voice', { method:'PUT', headers:{'Content-Type':'application/json'}, body:JSON.stringify({voice, volume}) });
        }
```
to:
```javascript
        async function setVoice() {
            const voice = document.getElementById('voice-select').value;
            const volume = parseInt(document.getElementById('vol-slider').value);
            const response_mode = document.getElementById('resp-mode').value;
            document.getElementById('vol-val').textContent = volume;
            await fetch('/api/voice', { method:'PUT', headers:{'Content-Type':'application/json'}, body:JSON.stringify({voice, volume, response_mode}) });
        }
```

- [ ] **Step 3: Update poll() to sync the response mode dropdown**

In `pokewatcher/main/web/index.html`, in the `poll()` function, add after the volume sync block (after line 258, `document.getElementById('vol-val').textContent = d.speaker_volume;`):

```javascript
                if (d.response_mode) {
                    document.getElementById('resp-mode').value = d.response_mode;
                }
```

- [ ] **Step 4: Build firmware to verify compilation (HTML is embedded)**

```bash
cd /tmp/pokewatcher-build && idf.py build
```

Expected: Build succeeds.

- [ ] **Step 5: Commit**

```bash
git add pokewatcher/main/web/index.html
git commit -m "feat(webui): add reply mode dropdown to voice settings card"
```

---

### Task 5: Update WatcherStatus Type for response_mode

**Files:**
- Modify: `watcher-mcp/src/watcher-client.ts`

- [ ] **Step 1: Add response_mode to WatcherStatus interface**

In `watcher-mcp/src/watcher-client.ts`, add to the `WatcherStatus` interface (after line 12, `audio_ready?: boolean;`):

```typescript
  response_mode?: string;
```

- [ ] **Step 2: Add response_mode to getVoiceConfig return type**

In `watcher-mcp/src/watcher-client.ts`, update the `getVoiceConfig` return type (line 101-104) from:

```typescript
export async function getVoiceConfig(): Promise<{
  voice: string;
  volume: number;
}> {
```

to:

```typescript
export async function getVoiceConfig(): Promise<{
  voice: string;
  volume: number;
  response_mode?: string;
}> {
```

- [ ] **Step 3: Build and commit**

```bash
cd watcher-mcp && npm run build
```

Expected: Build succeeds.

```bash
git add watcher-mcp/src/watcher-client.ts
git commit -m "feat(mcp): add response_mode to WatcherStatus and voice config types"
```

---

### Task 6: Add Voice Context Tracking to Daemon

**Files:**
- Modify: `watcher-mcp/src/daemon.ts`

- [ ] **Step 1: Add voice context state and timeout**

In `watcher-mcp/src/daemon.ts`, add after the config section (after line 27, `const DAEMON_API_PORT = 8378;`):

```typescript
// --- Voice context (signals MCP tools that current response is to voice input) ---
let voiceContextActive = false;
let voiceContextTimer: ReturnType<typeof setTimeout> | null = null;
const VOICE_CONTEXT_TIMEOUT_MS = 60000;

function setVoiceContext(): void {
  voiceContextActive = true;
  if (voiceContextTimer) clearTimeout(voiceContextTimer);
  voiceContextTimer = setTimeout(() => {
    voiceContextActive = false;
    voiceContextTimer = null;
    log("voice-ctx", "Voice context expired (60s timeout)");
  }, VOICE_CONTEXT_TIMEOUT_MS);
  log("voice-ctx", "Voice context activated");
}

function clearVoiceContext(): void {
  voiceContextActive = false;
  if (voiceContextTimer) {
    clearTimeout(voiceContextTimer);
    voiceContextTimer = null;
  }
  log("voice-ctx", "Voice context cleared");
}
```

- [ ] **Step 2: Set voice context after transcription**

In the voice audio pickup section of the `poll()` function in `watcher-mcp/src/daemon.ts`, add `setVoiceContext()` right before `sendToZidane()`. Change the block (around lines 308-309) from:

```typescript
            if (text) {
              sendToZidane(`[Voice from Watcher] ${text}`);
            }
```
to:
```typescript
            if (text) {
              setVoiceContext();
              sendToZidane(`[Voice from Watcher] ${text}`);
            }
```

- [ ] **Step 3: Add voice-context HTTP endpoints to the daemon API server**

In `watcher-mcp/src/daemon.ts`, in the `apiServer` request handler, add new routes after the existing `GET /queue` handler (after the closing brace of the `else if (req.method === "GET" && req.url === "/queue")` block, around line 386):

```typescript
    } else if (req.method === "GET" && req.url === "/voice-context") {
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ active: voiceContextActive }));
    } else if (req.method === "DELETE" && req.url === "/voice-context") {
      clearVoiceContext();
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(JSON.stringify({ ok: true }));
```

- [ ] **Step 4: Build MCP server to verify compilation**

```bash
cd watcher-mcp && npm run build
```

Expected: Build succeeds with no TypeScript errors.

- [ ] **Step 5: Commit**

```bash
git add watcher-mcp/src/daemon.ts
git commit -m "feat(daemon): add voice context tracking for response mode enforcement

Sets a flag after voice transcription, auto-clears after 60s.
Exposes GET/DELETE /voice-context endpoints for MCP tools."
```

---

### Task 7: Add Response Mode Enforcement to MCP Tools

**Files:**
- Modify: `watcher-mcp/src/tools.ts`

- [ ] **Step 1: Add daemon helper and voice context checker**

In `watcher-mcp/src/tools.ts`, add after the `ok()` helper function (after line 57):

```typescript
// --- Voice context + response mode helpers ---

async function getVoiceContext(): Promise<boolean> {
  try {
    const result = await daemonRequest("GET", "/voice-context");
    return result?.active === true;
  } catch {
    return false;
  }
}

async function clearVoiceContext(): Promise<void> {
  try {
    await daemonRequest("DELETE", "/voice-context");
  } catch {}
}

async function getResponseMode(): Promise<string> {
  try {
    const config = await watcher.getVoiceConfig();
    return config.response_mode || "both";
  } catch {
    return "both";
  }
}
```

- [ ] **Step 2: Update the speak tool to auto-pair with display_message**

In `watcher-mcp/src/tools.ts`, in the `speak` tool handler (the async function starting around line 211), replace the entire handler body. Change from:

```typescript
    async ({ text, voice }: { text: string; voice?: string }) => {
      if (!text.trim()) {
        return error("No text to speak");
      }
      log("tool", "speak", { voice, text: text.slice(0, 80) });
      try {
        if (!voice) {
          const config = await watcher.getVoiceConfig();
          voice = config.voice;
        }

        const pcm = await textToSpeech(text, voice!);
        log(
          "tts",
          `Generated ${pcm.length} bytes of PCM (${(pcm.length / 32000).toFixed(1)}s)`
        );

        const result = await watcher.playAudio(pcm);

        if (result?.error === "speaker busy") {
          log("tts", "Speaker busy, retrying in 1s");
          await new Promise((r) => setTimeout(r, 1000));
          await watcher.playAudio(pcm);
        }

        return ok({
          spoke: text,
          voice,
          duration_s: +(pcm.length / 32000).toFixed(1),
        });
      } catch (err: any) {
        log("error", "speak failed", { error: err.message });
        return error(`Speak failed: ${err.message}`);
      }
    }
```

to:

```typescript
    async ({ text, voice }: { text: string; voice?: string }) => {
      if (!text.trim()) {
        return error("No text to speak");
      }
      log("tool", "speak", { voice, text: text.slice(0, 80) });
      try {
        const isVoiceReply = await getVoiceContext();
        const responseMode = isVoiceReply ? await getResponseMode() : "voice_only";

        if (!voice) {
          const config = await watcher.getVoiceConfig();
          voice = config.voice;
        }

        let durationS = 0;

        // Do TTS unless response_mode is text_only
        if (responseMode !== "text_only") {
          const pcm = await textToSpeech(text, voice!);
          durationS = +(pcm.length / 32000).toFixed(1);
          log("tts", `Generated ${pcm.length} bytes of PCM (${durationS}s)`);

          const result = await watcher.playAudio(pcm);

          if (result?.error === "speaker busy") {
            log("tts", "Speaker busy, retrying in 1s");
            await new Promise((r) => setTimeout(r, 1000));
            await watcher.playAudio(pcm);
          }
        }

        // Auto-pair: also display on screen if response_mode requires it
        if (isVoiceReply && (responseMode === "both" || responseMode === "text_only")) {
          log("tool", "speak auto-pairing with display_message", { responseMode });
          try {
            await daemonRequest("POST", "/queue", {
              text,
              level: "info",
              state: "reporting",
            });
          } catch (err: any) {
            log("error", "auto-pair display failed", { error: err.message });
          }
        }

        if (isVoiceReply) await clearVoiceContext();

        return ok({
          spoke: text,
          voice,
          response_mode: responseMode,
          auto_paired: isVoiceReply && responseMode !== "voice_only",
          duration_s: durationS,
        });
      } catch (err: any) {
        log("error", "speak failed", { error: err.message });
        return error(`Speak failed: ${err.message}`);
      }
    }
```

- [ ] **Step 3: Update the display_message tool to auto-pair with speak**

In `watcher-mcp/src/tools.ts`, in the `display_message` tool handler (the async function starting around line 83), replace the handler body. Change from:

```typescript
    async ({ text, state, level }: { text: string; state: string; level: string }) => {
      log("tool", "display_message", { state, level, text: text.slice(0, 80) });
      try {
        const result = await daemonRequest("POST", "/queue", { text, level, state });
        log("tool", "display_message result", result);
        return ok(result);
      } catch (err: any) {
        log("error", "display_message failed", { error: err.message });
        return error(err.message);
      }
    }
```

to:

```typescript
    async ({ text, state, level }: { text: string; state: string; level: string }) => {
      log("tool", "display_message", { state, level, text: text.slice(0, 80) });
      try {
        const isVoiceReply = await getVoiceContext();
        const responseMode = isVoiceReply ? await getResponseMode() : "text_only";

        // Display on screen unless response_mode is voice_only
        let result: any = {};
        if (responseMode !== "voice_only") {
          result = await daemonRequest("POST", "/queue", { text, level, state });
          log("tool", "display_message result", result);
        }

        // Auto-pair: also speak if response_mode requires it
        if (isVoiceReply && (responseMode === "both" || responseMode === "voice_only")) {
          log("tool", "display_message auto-pairing with speak", { responseMode });
          try {
            let voice: string | undefined;
            const config = await watcher.getVoiceConfig();
            voice = config.voice;

            const pcm = await textToSpeech(text, voice);
            const playResult = await watcher.playAudio(pcm);

            if (playResult?.error === "speaker busy") {
              await new Promise((r) => setTimeout(r, 1000));
              await watcher.playAudio(pcm);
            }
          } catch (err: any) {
            log("error", "auto-pair speak failed", { error: err.message });
          }
        }

        if (isVoiceReply) await clearVoiceContext();

        return ok(result);
      } catch (err: any) {
        log("error", "display_message failed", { error: err.message });
        return error(err.message);
      }
    }
```

- [ ] **Step 4: Build MCP server to verify compilation**

```bash
cd watcher-mcp && npm run build
```

Expected: Build succeeds with no TypeScript errors.

- [ ] **Step 5: Commit**

```bash
git add watcher-mcp/src/tools.ts
git commit -m "feat(mcp): auto-pair speak and display_message based on response_mode

When voice context is active, speak tool also displays text (if mode
is both/text_only), and display_message tool also speaks (if mode is
both/voice_only). Voice context cleared after first enforcement."
```

---

### Task 8: Full Integration Smoke Test

- [ ] **Step 1: Build firmware from /tmp**

```bash
cd /tmp/pokewatcher-build && idf.py build
```

Expected: Clean build, no warnings related to our changes.

- [ ] **Step 2: Flash firmware (app-only to preserve NVS)**

```bash
idf.py -p /dev/cu.usbmodem5A8A0533623 app-flash
```

- [ ] **Step 3: Build MCP server**

```bash
cd watcher-mcp && npm run build
```

- [ ] **Step 4: Test recording early stop**

1. Double-click knob → blue pulsing LED, Zidane enters reporting state
2. Wait 2-3 seconds, then single-press knob → recording stops, green flash
3. Verify audio_ready=true in `GET /api/status`

- [ ] **Step 5: Test response_mode API**

```bash
# Check default
curl http://zidane.local/api/voice
# Expected: {"voice":"en_US-bryce-medium","volume":90,"response_mode":"both"}

# Change to voice_only
curl -X PUT http://zidane.local/api/voice -H 'Content-Type: application/json' -d '{"response_mode":"voice_only"}'

# Verify persisted
curl http://zidane.local/api/voice
# Expected: response_mode: "voice_only"

# Reboot and verify persistence
curl -X POST http://zidane.local/api/reboot
# Wait 10s, then:
curl http://zidane.local/api/voice
# Expected: response_mode: "voice_only" (survived reboot)

# Reset to both
curl -X PUT http://zidane.local/api/voice -H 'Content-Type: application/json' -d '{"response_mode":"both"}'
```

- [ ] **Step 6: Test Web UI**

1. Open http://zidane.local in browser
2. Scroll to Voice card → verify "Reply Mode" dropdown shows "Both (Voice + Text)"
3. Change to "Voice Only" → refresh page → verify selection persists
4. Change back to "Both"

- [ ] **Step 7: Test voice reply auto-pairing**

1. Set response_mode to "both" via Web UI
2. Double-click knob, say something, press knob to stop
3. Wait for OpenClaw to respond
4. Verify: both speaker plays audio AND text appears on LCD screen

- [ ] **Step 8: Commit any fixes, then final commit**

If any fixes were needed during testing, commit them. Then:

```bash
git add -A
git commit -m "test: verify push-to-talk enhancements end-to-end"
```
