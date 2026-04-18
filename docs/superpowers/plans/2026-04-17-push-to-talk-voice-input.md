# Push-to-Talk Voice Input Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Double-click the knob to record 5 seconds of audio, POST it to the MCP server, transcribe with whisper-node, and deliver the text to OpenClaw.

**Architecture:** ESP32 records 16kHz/16-bit mono PCM on double-click, wraps in a WAV header, HTTP POSTs to the MCP server's Express endpoint on port 3848. The MCP server saves to a temp file, transcribes via whisper-node (whisper.cpp compiled at install time), and sends the text to OpenClaw as a `sendLoggingMessage`. OpenClaw responds through existing MCP tools.

**Tech Stack:** ESP-IDF (C), Node.js/TypeScript, whisper-node, Express, esp_http_client

**Spec:** `docs/superpowers/specs/2026-04-17-push-to-talk-whisper-design.md`

---

## File Map

### New Files
| File | Responsibility |
|------|---------------|
| `watcher-mcp/src/audio-server.ts` | Express HTTP server on :3848, receives WAV, transcribes via whisper-node, calls onTranscription callback |
| `pokewatcher/main/voice_input.c` | Recording task: double-click handler, bsp_i2s_read loop, WAV header, HTTP POST |
| `pokewatcher/main/voice_input.h` | Public API: `pw_voice_init()` |

### Modified Files
| File | Change |
|------|--------|
| `watcher-mcp/package.json` | Add `whisper-node`, `express`, `@types/express` dependencies |
| `watcher-mcp/src/config.ts` | Add `AUDIO_PORT`, `WHISPER_MODEL` constants |
| `watcher-mcp/src/index.ts` | Import + start audio server, wire transcription to `sendLoggingMessage` |
| `pokewatcher/main/dialog.c` | Add `BUTTON_DOUBLE_CLICK` callback + volatile flag |
| `pokewatcher/main/dialog.h` | Add `pw_dialog_consume_double_click()` declaration |
| `pokewatcher/main/config.h` | Add MCP server IP/port, voice recording constants |
| `pokewatcher/main/app_main.c` | Add `pw_voice_init()` call after codec init |
| `pokewatcher/main/CMakeLists.txt` | Add `voice_input.c` to SRCS |

---

## Task 1: MCP Server — Add Dependencies

**Files:**
- Modify: `watcher-mcp/package.json`

- [ ] **Step 1: Install whisper-node, express, and types**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/watcher-mcp"
npm install whisper-node express
npm install --save-dev @types/express
```

This will compile whisper.cpp from source (~2-3 minutes on first install). Verify it completes without errors.

Expected: `package.json` updated with:
```json
"dependencies": {
  "@modelcontextprotocol/sdk": "^1.29.0",
  "express": "^4.21.0",
  "whisper-node": "^1.1.0",
  "zod": "^3.24.0"
},
"devDependencies": {
  "@types/express": "^4.17.0",
  "@types/node": "^20.0.0",
  "typescript": "^5.0.0"
}
```

- [ ] **Step 2: Commit**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher"
git add watcher-mcp/package.json watcher-mcp/package-lock.json
git commit -m "chore(mcp): add whisper-node and express dependencies"
```

---

## Task 2: MCP Server — Config Constants

**Files:**
- Modify: `watcher-mcp/src/config.ts`

- [ ] **Step 1: Add audio constants to config.ts**

Add these two lines at the end of the file, before the closing `AgentState` type export (keep existing content unchanged):

```typescript
export const AUDIO_PORT = 3848;
export const WHISPER_MODEL = "base.en";
```

The full file should now be:

```typescript
export const WATCHER_URL = "http://10.0.0.40";
export const POLL_INTERVAL_MS = 5000;
export const DEBOUNCE_COUNT = 2;
export const REQUEST_TIMEOUT_MS = 5000;

export const VALID_STATES = [
  "idle", "working", "waiting", "alert",
  "greeting", "sleeping", "reporting", "down",
] as const;

export type AgentState = typeof VALID_STATES[number];

export const AUDIO_PORT = 3848;
export const WHISPER_MODEL = "base.en";
```

- [ ] **Step 2: Commit**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher"
git add watcher-mcp/src/config.ts
git commit -m "feat(mcp): add AUDIO_PORT and WHISPER_MODEL config constants"
```

---

## Task 3: MCP Server — Audio Server Module

**Files:**
- Create: `watcher-mcp/src/audio-server.ts`

- [ ] **Step 1: Create audio-server.ts**

```typescript
import express from "express";
import { writeFileSync, unlinkSync, mkdirSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { whisper } from "whisper-node";
import { AUDIO_PORT, WHISPER_MODEL } from "./config.js";
import { log } from "./logger.js";

export function startAudioServer(
  onTranscription: (text: string) => void
): void {
  const app = express();

  // Accept raw binary body (WAV file from ESP32)
  app.use(
    "/audio",
    express.raw({ type: "application/octet-stream", limit: "1mb" })
  );

  app.post("/audio", async (req, res) => {
    const audioBuffer = req.body as Buffer;
    if (!audioBuffer || audioBuffer.length < 44) {
      res.status(400).json({ error: "No audio data or too short" });
      return;
    }

    log("audio", `Received ${audioBuffer.length} bytes`);

    const tmpDir = join(tmpdir(), "watcher-audio");
    mkdirSync(tmpDir, { recursive: true });
    const tmpFile = join(tmpDir, `voice-${Date.now()}.wav`);

    try {
      writeFileSync(tmpFile, audioBuffer);

      const result = await whisper(tmpFile, {
        modelName: WHISPER_MODEL,
        whisperOptions: { language: "en" },
      });
      const text = result
        .map((r: any) => r.speech)
        .join(" ")
        .trim();

      log("audio", `Transcribed: "${text}"`);

      if (text) {
        onTranscription(text);
      }

      res.json({ ok: true, text });
    } catch (err: any) {
      log("error", "Transcription failed", { error: err.message });
      res.status(500).json({ error: err.message });
    } finally {
      try {
        unlinkSync(tmpFile);
      } catch {}
    }
  });

  app.get("/audio/health", (_req, res) => {
    res.json({ ok: true, model: WHISPER_MODEL });
  });

  app.listen(AUDIO_PORT, () => {
    log("audio", `Audio server listening on :${AUDIO_PORT}`);
  });
}
```

- [ ] **Step 2: Verify TypeScript compiles**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/watcher-mcp"
npx tsc --noEmit
```

Expected: No errors. If `whisper-node` has no types, you may need to add a declaration. If so, create `watcher-mcp/src/whisper-node.d.ts`:

```typescript
declare module "whisper-node" {
  interface WhisperResult {
    start: string;
    end: string;
    speech: string;
  }
  interface WhisperOptions {
    modelName: string;
    whisperOptions?: { language?: string };
  }
  export function whisper(
    filePath: string,
    options: WhisperOptions
  ): Promise<WhisperResult[]>;
}
```

- [ ] **Step 3: Commit**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher"
git add watcher-mcp/src/audio-server.ts
# Also add whisper-node.d.ts if it was created:
git add watcher-mcp/src/whisper-node.d.ts 2>/dev/null; true
git commit -m "feat(mcp): add audio-server module with whisper-node transcription"
```

---

## Task 4: MCP Server — Wire Audio Server into Index

**Files:**
- Modify: `watcher-mcp/src/index.ts`

- [ ] **Step 1: Add audio server import and startup**

The full updated `index.ts`:

```typescript
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { registerTools } from "./tools.js";
import { registerResources } from "./resources.js";
import { startPresencePoller } from "./presence.js";
import { initQueue } from "./queue.js";
import { initLogger } from "./logger.js";
import { startAudioServer } from "./audio-server.js";

initLogger();

const server = new McpServer(
  {
    name: "watcher",
    version: "1.2.0",
  },
  {
    capabilities: {
      logging: {},
    },
  }
);

initQueue(server);
registerTools(server);
registerResources(server);

const transport = new StdioServerTransport();
await server.connect(transport);

startPresencePoller(server);

// Start audio receiver — transcriptions go to OpenClaw as logging messages
startAudioServer(async (text: string) => {
  await server.sendLoggingMessage({
    level: "info",
    logger: "voice",
    data: `voice_input: ${text}`,
  });
});
```

Changes from original:
- Added `import { startAudioServer }` line
- Bumped version from `"1.1.0"` to `"1.2.0"`
- Added `startAudioServer(...)` block at the end

- [ ] **Step 2: Build and verify**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/watcher-mcp"
npm run build
```

Expected: `dist/` updated with compiled JS, no errors.

- [ ] **Step 3: Commit**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher"
git add watcher-mcp/src/index.ts
git commit -m "feat(mcp): wire audio server into MCP index, bump version to 1.2.0"
```

---

## Task 5: MCP Server — End-to-End Test with curl

- [ ] **Step 1: Download whisper model (first-time only)**

The model downloads automatically on first transcription, but you can pre-download:

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/watcher-mcp"
npx whisper-node download
```

Select `base.en` (~140MB). If this command doesn't exist in the whisper-node version, skip — it will download on first use.

- [ ] **Step 2: Create a test WAV file**

```bash
# Generate 2 seconds of silence as a test WAV (16kHz, 16-bit, mono)
python3 -c "
import struct, wave
with wave.open('/tmp/test-voice.wav', 'w') as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(16000)
    w.writeframes(b'\x00\x00' * 32000)
print('Created /tmp/test-voice.wav')
"
```

For a real test, record yourself saying something:
```bash
# On macOS with sox installed (brew install sox):
sox -d -r 16000 -b 16 -c 1 /tmp/test-voice.wav trim 0 3
```

- [ ] **Step 3: Start the MCP server and test**

In one terminal:
```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/watcher-mcp"
node dist/index.js
```

In another terminal:
```bash
# Health check
curl http://localhost:3848/audio/health
# Expected: {"ok":true,"model":"base.en"}

# Send test audio
curl -X POST http://localhost:3848/audio \
  -H "Content-Type: application/octet-stream" \
  --data-binary @/tmp/test-voice.wav
# Expected: {"ok":true,"text":"..."} (may be empty for silence)
```

Check the log file at `~/.openclaw/watcher-mcp-logs/` for `[audio] Received ... bytes` and `[audio] Transcribed: "..."` entries.

- [ ] **Step 4: Verify sendLoggingMessage fires**

The `sendLoggingMessage` call requires an active MCP connection (stdio transport). When running standalone with `node dist/index.js`, the MCP transport may not be connected. The test in Step 3 verifies the HTTP + transcription pipeline works. The full integration (MCP logging) will be verified in Task 9.

---

## Task 6: Firmware — Config Constants

**Files:**
- Modify: `pokewatcher/main/config.h`

- [ ] **Step 1: Add voice input constants to config.h**

Add before the closing `#endif`:

```c
// Voice input (push-to-talk)
#define PW_MCP_SERVER_IP       "10.0.0.42"   // Mac's IP on local network
#define PW_MCP_SERVER_PORT     3848
#define PW_VOICE_RECORD_MS     5000           // 5 seconds recording
#define PW_VOICE_SAMPLE_RATE   16000
#define PW_VOICE_SAMPLE_BITS   16
#define PW_VOICE_BUF_SIZE      (PW_VOICE_SAMPLE_RATE * (PW_VOICE_SAMPLE_BITS / 8) * (PW_VOICE_RECORD_MS / 1000))  // 160000 bytes
```

**IMPORTANT:** Replace `10.0.0.42` with the actual IP of the Mac running the MCP server. Find it with `ifconfig en0 | grep "inet "` on the Mac.

- [ ] **Step 2: Commit**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher"
git add pokewatcher/main/config.h
git commit -m "feat(firmware): add push-to-talk voice input config constants"
```

---

## Task 7: Firmware — Double-Click Handler in dialog.c

**Files:**
- Modify: `pokewatcher/main/dialog.c`
- Modify: `pokewatcher/main/dialog.h`

- [ ] **Step 1: Add volatile flag and callback in dialog.c**

After the existing `s_knob_long_pressed` declaration (line 96), add:

```c
static volatile bool s_double_click = false;
```

After the `knob_btn_long_cb` function (after line 109), add:

```c
static void knob_btn_double_cb(void *arg, void *data)
{
    s_double_click = true;
}
```

- [ ] **Step 2: Register the double-click callback**

In the `pw_dialog_init` function, after the existing `iot_button_register_cb` calls (after line 234), add:

```c
        iot_button_register_cb(s_btn_handle, BUTTON_DOUBLE_CLICK, knob_btn_double_cb, NULL);
```

Update the log line on line 235 to:
```c
        ESP_LOGI(TAG, "Knob button registered (press=dismiss, long=reboot, double=voice)");
```

- [ ] **Step 3: Add consume function**

After the existing `pw_dialog_consume_knob_press` function (after line 358), add:

```c
bool pw_dialog_consume_double_click(void)
{
    if (s_double_click) {
        s_double_click = false;
        return true;
    }
    return false;
}
```

- [ ] **Step 4: Add declaration to dialog.h**

Add before the closing `#endif` in `dialog.h`:

```c
bool pw_dialog_consume_double_click(void);
```

- [ ] **Step 5: Commit**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher"
git add pokewatcher/main/dialog.c pokewatcher/main/dialog.h
git commit -m "feat(firmware): add knob double-click detection for voice input"
```

---

## Task 8: Firmware — Voice Input Module

**Files:**
- Create: `pokewatcher/main/voice_input.h`
- Create: `pokewatcher/main/voice_input.c`
- Modify: `pokewatcher/main/CMakeLists.txt`
- Modify: `pokewatcher/main/app_main.c`

- [ ] **Step 1: Create voice_input.h**

```c
#ifndef POKEWATCHER_VOICE_INPUT_H
#define POKEWATCHER_VOICE_INPUT_H

// Initialize voice input (register double-click polling in renderer loop)
void pw_voice_init(void);

// Call from renderer loop to check for double-click and start recording
void pw_voice_tick(void);

#endif
```

- [ ] **Step 2: Create voice_input.c**

```c
#include "voice_input.h"
#include "config.h"
#include "dialog.h"
#include "agent_state.h"
#include "sensecap-watcher.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "pw_voice";

// WAV header for 16kHz 16-bit mono PCM
typedef struct __attribute__((packed)) {
    char     riff[4];
    uint32_t file_size;
    char     wave[4];
    char     fmt[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t num_channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char     data_tag[4];
    uint32_t data_size;
} wav_header_t;

static volatile bool s_recording = false;

static void build_wav_header(wav_header_t *hdr, uint32_t data_size)
{
    memcpy(hdr->riff, "RIFF", 4);
    hdr->file_size = data_size + 36;
    memcpy(hdr->wave, "WAVE", 4);
    memcpy(hdr->fmt, "fmt ", 4);
    hdr->fmt_size = 16;
    hdr->audio_format = 1;  // PCM
    hdr->num_channels = 1;
    hdr->sample_rate = PW_VOICE_SAMPLE_RATE;
    hdr->byte_rate = PW_VOICE_SAMPLE_RATE * 1 * (PW_VOICE_SAMPLE_BITS / 8);
    hdr->block_align = 1 * (PW_VOICE_SAMPLE_BITS / 8);
    hdr->bits_per_sample = PW_VOICE_SAMPLE_BITS;
    memcpy(hdr->data_tag, "data", 4);
    hdr->data_size = data_size;
}

static void voice_record_task(void *arg)
{
    ESP_LOGI(TAG, "Recording started (%d ms)", PW_VOICE_RECORD_MS);

    // Save current state to restore later
    pw_agent_state_data_t state_data = pw_agent_state_get();
    pw_agent_state_t prev_state = state_data.current_state;

    // Visual feedback: blue LED + working state
    bsp_rgb_set(0, 0, 26);  // blue
    pw_agent_state_set(PW_STATE_WORKING);

    // Allocate audio buffer in PSRAM
    uint8_t *audio_buf = heap_caps_malloc(PW_VOICE_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!audio_buf) {
        ESP_LOGE(TAG, "Failed to allocate %d bytes in PSRAM for audio", PW_VOICE_BUF_SIZE);
        bsp_rgb_set(26, 0, 0);  // red = error
        vTaskDelay(pdMS_TO_TICKS(500));
        bsp_rgb_set(0, 0, 0);
        s_recording = false;
        vTaskDelete(NULL);
        return;
    }

    // Record audio via I2S
    size_t total_read = 0;
    size_t chunk_size = 1024;
    int64_t start_time = esp_timer_get_time();
    int64_t end_time = start_time + (int64_t)PW_VOICE_RECORD_MS * 1000;

    while (esp_timer_get_time() < end_time && total_read < PW_VOICE_BUF_SIZE) {
        size_t bytes_read = 0;
        size_t to_read = chunk_size;
        if (total_read + to_read > PW_VOICE_BUF_SIZE) {
            to_read = PW_VOICE_BUF_SIZE - total_read;
        }
        esp_err_t ret = bsp_i2s_read(audio_buf + total_read, to_read, &bytes_read, 100);
        if (ret == ESP_OK && bytes_read > 0) {
            total_read += bytes_read;
        }

        // Pulse blue LED (toggle brightness every 500ms)
        int64_t elapsed = (esp_timer_get_time() - start_time) / 1000;
        if ((elapsed / 500) % 2 == 0) {
            bsp_rgb_set(0, 0, 26);
        } else {
            bsp_rgb_set(0, 0, 10);
        }
    }

    ESP_LOGI(TAG, "Recording complete: %zu bytes captured", total_read);

    if (total_read < 16000) {  // less than 0.5 seconds
        ESP_LOGW(TAG, "Audio too short (%zu bytes), skipping upload", total_read);
        heap_caps_free(audio_buf);
        bsp_rgb_set(26, 0, 0);  // red flash
        vTaskDelay(pdMS_TO_TICKS(500));
        bsp_rgb_set(0, 0, 0);
        pw_agent_state_set(prev_state);
        s_recording = false;
        vTaskDelete(NULL);
        return;
    }

    // Build WAV header
    wav_header_t hdr;
    build_wav_header(&hdr, total_read);

    // LED yellow = uploading
    bsp_rgb_set(26, 18, 0);

    // HTTP POST to MCP server
    char url[64];
    snprintf(url, sizeof(url), "http://%s:%d/audio", PW_MCP_SERVER_IP, PW_MCP_SERVER_PORT);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 30000,  // whisper transcription can take a few seconds
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_http_client_set_header(client, "Content-Type", "application/octet-stream");

    // Write WAV header + PCM data
    int content_length = sizeof(wav_header_t) + total_read;
    esp_http_client_open(client, content_length);
    esp_http_client_write(client, (const char *)&hdr, sizeof(wav_header_t));
    esp_http_client_write(client, (const char *)audio_buf, total_read);

    int status = esp_http_client_fetch_headers(client);
    int http_status = esp_http_client_get_status_code(client);

    if (status >= 0 && http_status == 200) {
        ESP_LOGI(TAG, "Audio uploaded successfully (HTTP %d)", http_status);
        // Green flash = success
        bsp_rgb_set(0, 26, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
    } else {
        ESP_LOGE(TAG, "Audio upload failed (HTTP %d, fetch=%d)", http_status, status);
        // Red flash x3 = error
        for (int i = 0; i < 3; i++) {
            bsp_rgb_set(26, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
            bsp_rgb_set(0, 0, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    heap_caps_free(audio_buf);

    bsp_rgb_set(0, 0, 0);  // LED off
    pw_agent_state_set(prev_state);  // restore state
    s_recording = false;

    ESP_LOGI(TAG, "Voice input task complete");
    vTaskDelete(NULL);
}

void pw_voice_tick(void)
{
    if (s_recording) return;  // already recording

    if (pw_dialog_consume_double_click()) {
        s_recording = true;
        ESP_LOGI(TAG, "Double-click detected — starting voice recording");
        xTaskCreate(voice_record_task, "voice_rec", 4096, NULL, 5, NULL);
    }
}

void pw_voice_init(void)
{
    ESP_LOGI(TAG, "Voice input initialized (double-click knob to record)");
}
```

- [ ] **Step 3: Add voice_input.c to CMakeLists.txt**

In `pokewatcher/main/CMakeLists.txt`, add `"voice_input.c"` to the SRCS list after `"web_server.c"`:

```
    SRCS
        "app_main.c"
        "event_queue.c"
        "himax_task.c"
        "agent_state.c"
        "renderer.c"
        "sprite_loader.c"
        "dialog.c"
        "web_server.c"
        "voice_input.c"
```

- [ ] **Step 4: Add pw_voice_init() and pw_voice_tick() calls**

In `pokewatcher/main/app_main.c`, add the include at the top with other includes:

```c
#include "voice_input.h"
```

Add the init call after `bsp_codec_mute_set(true);` (after line 154):

```c
    pw_voice_init();
```

- [ ] **Step 5: Wire pw_voice_tick() into the renderer loop**

The renderer loop in `renderer.c` is the main polling loop. Add the include at the top:

```c
#include "voice_input.h"
```

In the renderer task loop (in the function `renderer_task`), add `pw_voice_tick()` at the start of the loop, before the LVGL lock. Find the comment about processing flags and add it nearby. Place it right after the wake/display-off checks but before `prepare_frame()`:

```c
            // Check for voice recording double-click
            pw_voice_tick();
```

- [ ] **Step 6: Commit**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher"
git add pokewatcher/main/voice_input.h pokewatcher/main/voice_input.c pokewatcher/main/CMakeLists.txt pokewatcher/main/app_main.c pokewatcher/main/renderer.c
git commit -m "feat(firmware): add push-to-talk voice recording with HTTP upload"
```

---

## Task 9: Build, Flash, and End-to-End Test

**CRITICAL:** Firmware must be built from `/tmp`. See `docs/knowledgebase/building-and-flashing-firmware.md`.

- [ ] **Step 1: Sync source to /tmp build directory**

```bash
rsync -av --delete \
  --exclude='build' \
  --exclude='managed_components' \
  --exclude='dependencies.lock' \
  --exclude='sdkconfig' \
  "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher/" \
  /tmp/pokewatcher-build/
```

- [ ] **Step 2: Delete sdkconfig to pick up any new defaults**

```bash
rm -f /tmp/pokewatcher-build/sdkconfig
```

- [ ] **Step 3: Build**

```bash
cd /tmp/pokewatcher-build
source ~/esp/esp-idf/export.sh
idf.py build
```

Expected: Successful build. Watch for any compilation errors in `voice_input.c`.

- [ ] **Step 4: Flash (app only — preserves NVS)**

```bash
cd /tmp/pokewatcher-build
idf.py -p /dev/cu.usbmodem5A8A0533623 app-flash
```

- [ ] **Step 5: Start MCP server**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/watcher-mcp"
npm run build && node dist/index.js
```

- [ ] **Step 6: Test double-click**

1. Open serial monitor: `idf.py -p /dev/cu.usbmodem5A8A0533623 monitor`
2. Double-click the knob button on the Watcher
3. Expected serial output:
   ```
   I (xxxxx) pw_voice: Double-click detected — starting voice recording
   I (xxxxx) pw_voice: Recording started (5000 ms)
   I (xxxxx) pw_voice: Recording complete: 160000 bytes captured
   I (xxxxx) pw_voice: Audio uploaded successfully (HTTP 200)
   I (xxxxx) pw_voice: Voice input task complete
   ```
4. Expected LED sequence: blue (recording) → yellow (uploading) → green flash (success) → off
5. Check MCP server logs at `~/.openclaw/watcher-mcp-logs/` for `[audio] Received ... bytes` and `[audio] Transcribed: "..."`

- [ ] **Step 7: Test with speech**

Say something clearly while the blue LED is on. Check that the MCP server log shows a non-empty transcription. Verify the `sendLoggingMessage` with `voice_input: <your words>` appears.

- [ ] **Step 8: Test error cases**

1. **MCP server not running**: Double-click, expect red LED flash × 3, serial log shows HTTP error
2. **Very short audio**: Double-click and immediately unplug mic (if possible) — expect red flash and "Audio too short" log

---

## Task 10: Commit Final State and Update Docs

- [ ] **Step 1: Update project-status.md**

In `docs/knowledgebase/project-status.md`, add to the "What's Done / Firmware" section:

```markdown
- Push-to-talk voice input: double-click knob → 5s recording → HTTP POST WAV to MCP server → whisper-node transcription → OpenClaw via sendLoggingMessage
```

Move the audio/speaker line from "Future Features" to a partially-done state or update it:

```markdown
- [ ] Audio/speaker output — codec initialized and muted; mic input working for push-to-talk (speaker playback TBD)
```

- [ ] **Step 2: Update the MCP section in project-status.md**

Add to the MCP server "What's Done" section:

```markdown
- Audio receiver: Express HTTP server on :3848, whisper-node transcription, voice_input logging messages to OpenClaw
```

- [ ] **Step 3: Commit docs**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher"
git add docs/knowledgebase/project-status.md
git commit -m "docs: update project status with push-to-talk voice input"
```
