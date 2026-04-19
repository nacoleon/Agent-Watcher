# TTS Speaker Output Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add voice output to the Watcher — OpenClaw calls `watcher__speak`, MCP server runs Piper TTS, streams resampled PCM to the Watcher which plays it through the speaker.

**Architecture:** MCP `speak` tool spawns Piper TTS on the Mac, resamples 22050→16000 Hz in Node.js, HTTP POSTs raw PCM to the Watcher's `POST /api/audio/play` endpoint which writes chunks to the speaker via `bsp_i2s_write()`. Voice selection stored in NVS, configurable from web UI.

**Tech Stack:** ESP-IDF (C), Node.js/TypeScript, Piper TTS (pip), `bsp_i2s_write`, NVS

**Spec:** `docs/superpowers/specs/2026-04-18-tts-speaker-output-design.md`

---

## File Map

### New Files
| File | Responsibility |
|------|---------------|
| `watcher-mcp/src/tts.ts` | Piper TTS runner: spawn piper process, resample 22050→16000 Hz, return PCM buffer |

### Modified Files
| File | Change |
|------|--------|
| `pokewatcher/main/web_server.c` | Add `POST /api/audio/play`, `GET /PUT /api/voice` endpoints, NVS voice config, speaker playing flag |
| `pokewatcher/main/web/index.html` | Add voice settings section (dropdown + volume slider) |
| `watcher-mcp/src/tools.ts` | Register `speak` tool |
| `watcher-mcp/src/watcher-client.ts` | Add `getVoiceConfig()`, `playAudio()` functions |

---

## Task 1: Install Piper TTS on Mac

- [ ] **Step 1: Install piper-tts**

```bash
pip install piper-tts
```

This compiles from source on Apple Silicon (~5-10 minutes). Verify it works:

```bash
echo "Hello world" | piper --model en_US-amy-medium --output-file /tmp/piper-test.wav
```

First run downloads the `en_US-amy-medium` model (~50MB). Verify the WAV file plays:

```bash
afplay /tmp/piper-test.wav
```

- [ ] **Step 2: Verify raw output mode and sample rate**

```bash
echo "Hello world" | piper --model en_US-amy-medium --output-raw > /tmp/piper-test.raw
python3 -c "
import os
size = os.path.getsize('/tmp/piper-test.raw')
duration = size / (22050 * 2)  # 16-bit mono at 22050 Hz
print(f'Size: {size} bytes, Duration: {duration:.2f}s at 22050 Hz')
"
```

Expected: ~1-2 seconds of audio, file size ~44-88KB.

- [ ] **Step 3: Verify piper is in PATH for the MCP server**

```bash
which piper
```

If it's in a virtualenv, note the full path — the MCP server will need it.

---

## Task 2: Firmware — `POST /api/audio/play` Endpoint

**Files:**
- Modify: `pokewatcher/main/web_server.c`

- [ ] **Step 1: Add speaker state variables and NVS voice config**

At the top of `web_server.c`, after the existing `#include` and static variables (after the `s_heartbeat_log` section around line 22), add:

```c
// Speaker / voice config
static volatile bool s_playing = false;
static char s_voice_name[64] = "en_US-amy-medium";
static int s_speaker_volume = 70;

static void load_voice_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(PW_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(s_voice_name);
        nvs_get_str(nvs, "voice", s_voice_name, &len);
        nvs_get_i32(nvs, "volume", (int32_t *)&s_speaker_volume);
        nvs_close(nvs);
    }
}

static void save_voice_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open(PW_NVS_NAMESPACE, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, "voice", s_voice_name);
        nvs_set_i32(nvs, "volume", s_speaker_volume);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
}
```

Also add the NVS include at the top if not already present:

```c
#include "nvs_flash.h"
#include "nvs.h"
```

- [ ] **Step 2: Add audio play handler**

Before `register_routes`, add:

```c
static esp_err_t handle_api_audio_play(httpd_req_t *req)
{
    if (s_playing) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"speaker busy\"}");
        return ESP_OK;
    }
    s_playing = true;

    ESP_LOGI(TAG, "Playing audio: %d bytes, volume=%d", req->content_len, s_speaker_volume);

    // Unmute and set volume
    bsp_codec_mute_set(false);
    bsp_codec_volume_set(s_speaker_volume, NULL);

    // Stream chunks from HTTP body directly to I2S
    char buf[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining < (int)sizeof(buf) ? remaining : (int)sizeof(buf);
        int received = httpd_req_recv(req, buf, to_read);
        if (received <= 0) {
            ESP_LOGE(TAG, "Audio recv error: %d", received);
            break;
        }
        size_t written = 0;
        bsp_i2s_write(buf, received, &written, 1000);
        remaining -= received;
    }

    // Mute when done
    bsp_codec_mute_set(true);
    s_playing = false;

    ESP_LOGI(TAG, "Audio playback complete");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}
```

- [ ] **Step 3: Add voice config GET/PUT handlers**

Before `register_routes`, add:

```c
static esp_err_t handle_api_voice_get(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "voice", s_voice_name);
    cJSON_AddNumberToObject(root, "volume", s_speaker_volume);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t handle_api_voice_put(httpd_req_t *req)
{
    char body[256];
    int len = recv_full_body(req, body, sizeof(body));
    if (len < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_OK;
    }
    body[len] = '\0';
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_OK;
    }

    cJSON *voice = cJSON_GetObjectItem(root, "voice");
    cJSON *volume = cJSON_GetObjectItem(root, "volume");

    if (voice && cJSON_IsString(voice)) {
        strncpy(s_voice_name, voice->valuestring, sizeof(s_voice_name) - 1);
        s_voice_name[sizeof(s_voice_name) - 1] = '\0';
    }
    if (volume && cJSON_IsNumber(volume)) {
        s_speaker_volume = (int)volume->valuedouble;
        if (s_speaker_volume < 0) s_speaker_volume = 0;
        if (s_speaker_volume > 95) s_speaker_volume = 95;
    }

    save_voice_config();
    cJSON_Delete(root);

    ESP_LOGI(TAG, "Voice config updated: voice=%s volume=%d", s_voice_name, s_speaker_volume);
    return handle_api_voice_get(req);
}
```

- [ ] **Step 4: Add voice and speaker_volume to status response**

In `handle_api_status`, before the `wifi_ap_record_t` line, add:

```c
    // Voice config
    cJSON_AddStringToObject(root, "voice", s_voice_name);
    cJSON_AddNumberToObject(root, "speaker_volume", s_speaker_volume);
```

- [ ] **Step 5: Register the new routes**

In `register_routes`, after the audio DELETE handler registration, add:

```c
    httpd_uri_t audio_play_uri = { .uri = "/api/audio/play", .method = HTTP_POST, .handler = handle_api_audio_play };
    httpd_register_uri_handler(server, &audio_play_uri);

    httpd_uri_t voice_get_uri = { .uri = "/api/voice", .method = HTTP_GET, .handler = handle_api_voice_get };
    httpd_register_uri_handler(server, &voice_get_uri);

    httpd_uri_t voice_put_uri = { .uri = "/api/voice", .method = HTTP_PUT, .handler = handle_api_voice_put };
    httpd_register_uri_handler(server, &voice_put_uri);
```

Also increase `max_uri_handlers` from 16 to 20 in `pw_web_server_start`:

```c
    config.max_uri_handlers = 20;
```

- [ ] **Step 6: Load voice config at startup**

In `pw_web_server_start`, before `init_mdns()`, add:

```c
    load_voice_config();
    ESP_LOGI(TAG, "Voice config: voice=%s volume=%d", s_voice_name, s_speaker_volume);
```

- [ ] **Step 7: Commit**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher"
git add pokewatcher/main/web_server.c
git commit -m "feat(firmware): add /api/audio/play and /api/voice endpoints with NVS config"
```

---

## Task 3: Firmware — Web UI Voice Settings

**Files:**
- Modify: `pokewatcher/main/web/index.html`

- [ ] **Step 1: Add voice settings HTML section**

In `index.html`, before the Reboot button (`<button class="reboot"`), add:

```html
    <h2>Voice: <span id="voice-current">en_US-amy-medium</span></h2>
    <div class="card" style="font-size:12px;">
        <div class="stat">
            <span class="label">Voice</span>
            <select id="voice-select" onchange="setVoice()" style="flex:1;padding:5px;background:#1a1a4e;border:1px solid #2a2a5e;border-radius:6px;color:#e0e0e0;font-family:inherit;font-size:12px;">
                <option value="en_US-amy-medium">Amy (US, medium)</option>
                <option value="en_US-arctic-medium">Arctic (US, medium)</option>
                <option value="en_US-bryce-medium">Bryce (US, medium)</option>
                <option value="en_US-danny-low">Danny (US, low)</option>
                <option value="en_US-hfc_female-medium">HFC Female (US, medium)</option>
                <option value="en_US-hfc_male-medium">HFC Male (US, medium)</option>
                <option value="en_US-joe-medium">Joe (US, medium)</option>
                <option value="en_US-john-medium">John (US, medium)</option>
                <option value="en_US-kathleen-low">Kathleen (US, low)</option>
                <option value="en_US-kristin-medium">Kristin (US, medium)</option>
                <option value="en_US-kusal-medium">Kusal (US, medium)</option>
                <option value="en_GB-alan-medium">Alan (GB, medium)</option>
                <option value="en_GB-alba-medium">Alba (GB, medium)</option>
                <option value="en_GB-jenny_dioco-medium">Jenny (GB, medium)</option>
                <option value="en_GB-northern_english_male-medium">Northern Male (GB, medium)</option>
            </select>
        </div>
        <div class="stat">
            <span class="label">Volume</span>
            <input id="vol-slider" type="range" min="0" max="95" value="70" onchange="setVoice()" style="flex:1;">
            <span id="vol-val" style="min-width:30px;text-align:right;">70</span>
        </div>
    </div>
```

- [ ] **Step 2: Add JavaScript for voice settings**

In the `<script>` section, before the `poll()` function definition, add:

```javascript
        async function setVoice() {
            const voice = document.getElementById('voice-select').value;
            const volume = parseInt(document.getElementById('vol-slider').value);
            document.getElementById('vol-val').textContent = volume;
            await fetch('/api/voice', { method:'PUT', headers:{'Content-Type':'application/json'}, body:JSON.stringify({voice, volume}) });
        }
        document.getElementById('vol-slider').addEventListener('input', function() {
            document.getElementById('vol-val').textContent = this.value;
        });
```

- [ ] **Step 3: Update poll() to sync voice settings from status**

In the `poll()` function, after the AI model section (after the `curModel` assignment), add:

```javascript
                // Voice config
                if (d.voice) {
                    document.getElementById('voice-current').textContent = d.voice;
                    document.getElementById('voice-select').value = d.voice;
                }
                if (d.speaker_volume !== undefined) {
                    document.getElementById('vol-slider').value = d.speaker_volume;
                    document.getElementById('vol-val').textContent = d.speaker_volume;
                }
```

- [ ] **Step 4: Commit**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher"
git add pokewatcher/main/web/index.html
git commit -m "feat(firmware): add voice settings UI with dropdown and volume slider"
```

---

## Task 4: MCP Server — TTS Module

**Files:**
- Create: `watcher-mcp/src/tts.ts`

- [ ] **Step 1: Create tts.ts**

```typescript
import { spawn } from "node:child_process";
import { log } from "./logger.js";

/**
 * Run Piper TTS and return 16kHz 16-bit mono PCM buffer.
 * Piper outputs 22050 Hz — we resample to 16000 Hz in-process.
 */
export async function textToSpeech(
  text: string,
  voice: string
): Promise<Buffer> {
  const raw22k = await runPiper(text, voice);
  return resample(raw22k, 22050, 16000);
}

function runPiper(text: string, voice: string): Promise<Buffer> {
  return new Promise((resolve, reject) => {
    const proc = spawn("piper", ["--model", voice, "--output-raw"], {
      stdio: ["pipe", "pipe", "pipe"],
    });

    const chunks: Buffer[] = [];

    proc.stdout.on("data", (chunk: Buffer) => chunks.push(chunk));

    proc.stderr.on("data", (data: Buffer) => {
      // Piper logs to stderr — ignore unless it's an error
      const msg = data.toString().trim();
      if (msg && !msg.startsWith("[") && !msg.includes("Real-time factor")) {
        log("tts", `piper stderr: ${msg}`);
      }
    });

    proc.on("close", (code) => {
      if (code !== 0) {
        reject(new Error(`piper exited with code ${code}`));
        return;
      }
      resolve(Buffer.concat(chunks));
    });

    proc.on("error", (err) => {
      reject(new Error(`piper not found — install with: pip install piper-tts (${err.message})`));
    });

    proc.stdin.write(text);
    proc.stdin.end();
  });
}

/**
 * Resample 16-bit mono PCM from srcRate to dstRate using linear interpolation.
 */
function resample(input: Buffer, srcRate: number, dstRate: number): Buffer {
  const srcSamples = input.length / 2;
  const dstSamples = Math.floor((srcSamples * dstRate) / srcRate);
  const output = Buffer.alloc(dstSamples * 2);

  for (let i = 0; i < dstSamples; i++) {
    const srcPos = (i * srcRate) / dstRate;
    const srcIdx = Math.floor(srcPos);
    const frac = srcPos - srcIdx;

    const s0 = input.readInt16LE(srcIdx * 2);
    const s1 =
      srcIdx + 1 < srcSamples ? input.readInt16LE((srcIdx + 1) * 2) : s0;

    const sample = Math.round(s0 + frac * (s1 - s0));
    output.writeInt16LE(
      Math.max(-32768, Math.min(32767, sample)),
      i * 2
    );
  }

  return output;
}
```

- [ ] **Step 2: Verify TypeScript compiles**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/watcher-mcp"
npx tsc --noEmit
```

- [ ] **Step 3: Commit**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher"
git add watcher-mcp/src/tts.ts
git commit -m "feat(mcp): add TTS module — Piper runner + 22050→16000 Hz resampler"
```

---

## Task 5: MCP Server — `speak` Tool + Watcher Client

**Files:**
- Modify: `watcher-mcp/src/watcher-client.ts`
- Modify: `watcher-mcp/src/tools.ts`

- [ ] **Step 1: Add getVoiceConfig and playAudio to watcher-client.ts**

At the end of `watcher-client.ts`, after the `clearAudio` function, add:

```typescript
export async function getVoiceConfig(): Promise<{
  voice: string;
  volume: number;
}> {
  return request("GET", "/api/voice");
}

export function playAudio(pcm: Buffer): Promise<any> {
  return new Promise((resolve, reject) => {
    const url = new URL("/api/audio/play", WATCHER_URL);
    const req = http.request(
      {
        hostname: url.hostname,
        port: url.port || 80,
        path: url.pathname,
        method: "POST",
        headers: {
          "Content-Type": "application/octet-stream",
          "Content-Length": pcm.length,
        },
        timeout: 30000,
      },
      (res) => {
        const chunks: Buffer[] = [];
        res.on("data", (chunk) => chunks.push(chunk));
        res.on("end", () => {
          const text = Buffer.concat(chunks).toString();
          try {
            resolve(JSON.parse(text));
          } catch {
            resolve(text);
          }
        });
      }
    );
    req.on("error", reject);
    req.on("timeout", () => {
      req.destroy();
      reject(new Error("timeout"));
    });
    req.write(pcm);
    req.end();
  });
}
```

- [ ] **Step 2: Register speak tool in tools.ts**

At the top of `tools.ts`, add the import:

```typescript
import { textToSpeech } from "./tts.js";
```

At the end of `registerTools`, before the closing `}`, add:

```typescript
  server.registerTool(
    "speak",
    {
      title: "Speak (TTS)",
      description:
        "Speak text aloud through the Watcher's speaker using text-to-speech. " +
        "Use this for responding to voice input from the Watcher. " +
        "Keep responses short and conversational (1-2 sentences). " +
        "The voice parameter is optional — defaults to the voice configured on the device.",
      inputSchema: {
        text: z.string().describe("Text to speak aloud"),
        voice: z
          .string()
          .optional()
          .describe("Piper voice model (e.g. en_US-amy-medium). Omit to use device default."),
      },
    },
    async ({ text, voice }: { text: string; voice?: string }) => {
      if (!text.trim()) {
        return error("No text to speak");
      }
      log("tool", "speak", { voice, text: text.slice(0, 80) });
      try {
        // Get default voice from Watcher if not specified
        if (!voice) {
          const config = await watcher.getVoiceConfig();
          voice = config.voice;
        }

        // Run TTS
        const pcm = await textToSpeech(text, voice);
        log("tts", `Generated ${pcm.length} bytes of PCM (${(pcm.length / 32000).toFixed(1)}s)`);

        // Send to Watcher speaker
        const result = await watcher.playAudio(pcm);

        if (result?.error === "speaker busy") {
          // Retry once after 1 second
          log("tts", "Speaker busy, retrying in 1s");
          await new Promise((r) => setTimeout(r, 1000));
          await watcher.playAudio(pcm);
        }

        return ok({ spoke: text, voice, duration_s: +(pcm.length / 32000).toFixed(1) });
      } catch (err: any) {
        log("error", "speak failed", { error: err.message });
        return error(`Speak failed: ${err.message}`);
      }
    }
  );
```

- [ ] **Step 3: Build and verify**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/watcher-mcp"
npm run build
```

- [ ] **Step 4: Commit**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher"
git add watcher-mcp/src/tools.ts watcher-mcp/src/watcher-client.ts
git commit -m "feat(mcp): add speak tool with Piper TTS and audio streaming to Watcher"
```

---

## Task 6: Build, Flash, and Test

**CRITICAL:** Build from `/tmp`. Move SDK aside during build.

- [ ] **Step 1: Sync and build firmware**

```bash
rsync -av --delete \
  --exclude='build' \
  --exclude='managed_components' \
  --exclude='dependencies.lock' \
  --exclude='sdkconfig' \
  "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher/" \
  /tmp/pokewatcher-build/

mv "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/SenseCAP-Watcher-Firmware" \
   "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/_SDK_BAK"

cd /tmp/pokewatcher-build
source ~/esp/esp-idf/export.sh
idf.py build

mv "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/_SDK_BAK" \
   "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/SenseCAP-Watcher-Firmware"
```

- [ ] **Step 2: Flash**

```bash
cd /tmp/pokewatcher-build
idf.py -p /dev/cu.usbmodem5A8A0533623 app-flash
```

- [ ] **Step 3: Test voice config API**

```bash
# Get default voice config
curl -s http://10.0.0.40/api/voice

# Set voice and volume
curl -s -X PUT http://10.0.0.40/api/voice \
  -H "Content-Type: application/json" \
  -d '{"voice":"en_US-danny-low","volume":50}'

# Verify in status
curl -s http://10.0.0.40/api/status | python3 -c "import sys,json; d=json.load(sys.stdin); print('voice:', d.get('voice'), 'vol:', d.get('speaker_volume'))"
```

- [ ] **Step 4: Test audio playback with a test file**

Generate a test PCM file and send it:

```bash
# Generate 1 second of 440Hz sine wave at 16kHz 16-bit mono
python3 -c "
import struct, math
samples = [int(16000 * math.sin(2 * math.pi * 440 * i / 16000)) for i in range(16000)]
with open('/tmp/test-tone.raw', 'wb') as f:
    for s in samples:
        f.write(struct.pack('<h', s))
print('Created 1s 440Hz tone')
"

# Play it on the Watcher
curl -X POST http://10.0.0.40/api/audio/play \
  -H "Content-Type: application/octet-stream" \
  --data-binary @/tmp/test-tone.raw
```

Expected: Watcher speaker plays a 1-second 440Hz tone.

- [ ] **Step 5: Test Piper TTS end-to-end**

```bash
# Generate TTS and resample
echo "Hello, I am Zidane" | piper --model en_US-amy-medium --output-raw > /tmp/tts-22k.raw
sox -r 22050 -b 16 -c 1 -e signed -t raw /tmp/tts-22k.raw -r 16000 -t raw /tmp/tts-16k.raw

# Play on Watcher
curl -X POST http://10.0.0.40/api/audio/play \
  -H "Content-Type: application/octet-stream" \
  --data-binary @/tmp/tts-16k.raw
```

Expected: Watcher speaker says "Hello, I am Zidane".

- [ ] **Step 6: Test MCP speak tool**

Restart the OpenClaw gateway to pick up new MCP code:

```bash
openclaw gateway restart
```

Then test the speak tool:

```bash
openclaw agent --agent main -m "Use the watcher__speak tool to say 'Testing voice output'" --timeout 60
```

Expected: Watcher speaker plays TTS audio of the text.

---

## Task 7: Update Zidane's Instructions

**Files:**
- Modify: `/Users/nacoleon/clawd/TOOLS.md`

- [ ] **Step 1: Add voice output section to TOOLS.md**

In the Watcher section of `TOOLS.md`, after the "Watcher Events" subsection, add:

```markdown
### Voice Output (Speaker)
You have a `watcher__speak` tool that plays text through the Watcher's speaker using TTS.
- For push-to-talk responses (`[Voice from Watcher]` messages): use `watcher__speak` instead of `watcher__display_message`
- You can specify a voice or let it use the default configured on the device
- Keep spoken responses conversational and concise (1-2 sentences)
- If the user explicitly asks you to "send a message" or "show on screen", use `watcher__display_message` instead
```

- [ ] **Step 2: Update project-status.md**

In `docs/knowledgebase/project-status.md`, add to the Firmware "What's Done" section:

```markdown
- TTS speaker output: watcher__speak MCP tool, Piper TTS on Mac, 22050→16000 Hz resampling, streaming PCM to speaker. Voice selection in web UI with NVS persistence.
```

Update the Future Features audio line:

```markdown
- [ ] Audio/speaker output — mic input (push-to-talk) and speaker output (TTS) both working. Codec fully utilized.
```

- [ ] **Step 3: Commit**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher"
git add docs/knowledgebase/project-status.md
git commit -m "docs: update project status with TTS speaker output"
```
