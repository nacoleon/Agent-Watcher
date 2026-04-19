# TTS Speaker Output — Design Spec

## Overview

Add voice output to the Zidane Watcher via Piper TTS. OpenClaw calls a new `watcher__speak` MCP tool with text, the MCP server runs Piper locally on the Mac, resamples the audio to 16kHz, and streams the PCM to the Watcher which plays it through the speaker. Voice selection is configurable from the Watcher web UI with a default voice, and Zidane can optionally override per-message.

## Architecture

```
OpenClaw calls watcher__speak(text, voice?)
  → MCP tool handler in watcher-mcp:
    1. If no voice param, fetch default from GET /api/voice on Watcher
    2. Spawn: echo "text" | piper --model <voice> --output-raw
    3. Resample piper stdout from 22050 Hz → 16000 Hz (linear interpolation in Node.js)
    4. HTTP POST resampled PCM to http://10.0.0.40/api/audio/play
  → Watcher firmware:
    1. Receive PCM chunks in HTTP body
    2. Unmute speaker, set volume
    3. bsp_i2s_write() each chunk as it arrives
    4. Mute speaker when done
```

```
┌──────────────────────────────────────────────────────┐
│                  Mac (MCP Server)                    │
│                                                      │
│  watcher__speak(text, voice?)                        │
│       │                                              │
│       ▼                                              │
│  echo text | piper --model voice --output-raw        │
│       │  (22050 Hz 16-bit mono PCM)                  │
│       ▼                                              │
│  Resample 22050 → 16000 Hz (Node.js)                 │
│       │                                              │
│       ▼                                              │
│  HTTP POST /api/audio/play (raw PCM body)            │
└──────────────────────┬───────────────────────────────┘
                       │
                       ▼
┌──────────────────────────────────────────────────────┐
│              Watcher (ESP32-S3)                      │
│                                                      │
│  POST /api/audio/play handler:                       │
│    1. bsp_codec_mute_set(false)  — unmute            │
│    2. bsp_codec_volume_set(volume)                   │
│    3. bsp_i2s_write(chunk) in recv loop              │
│    4. bsp_codec_mute_set(true)   — mute when done   │
└──────────────────────────────────────────────────────┘
```

## Why This Design

- **Piper runs on Mac** — ESP32 can't run TTS locally. Mac has the CPU and Piper is ~5x real-time.
- **Resampling on Mac** — Piper outputs 22050 Hz, Watcher codec is 16000 Hz. Cheaper to resample on Mac than add complexity to firmware.
- **Streaming, not buffering** — PCM streams through the HTTP body. Watcher plays chunks as they arrive via `bsp_i2s_write()`. No need to buffer the full audio in PSRAM.
- **MCP server handles TTS, not daemon** — The `speak` tool is called by OpenClaw during a conversation. The MCP server already has HTTP access to the Watcher. No daemon changes needed.
- **OpenClaw decides voice vs text** — `speak` for voice responses, `display_message` for text. Zidane's instructions tell him to use `speak` for push-to-talk responses.

## Firmware Changes

### 1. New endpoint: `POST /api/audio/play`

In `web_server.c`. Receives raw 16-bit mono 16kHz PCM in the HTTP body. Plays through speaker.

```c
static esp_err_t handle_api_audio_play(httpd_req_t *req)
{
    // Unmute + set volume
    bsp_codec_mute_set(false);
    bsp_codec_volume_set(s_speaker_volume, NULL);

    // Stream chunks from HTTP body directly to I2S
    uint8_t buf[1024];
    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = remaining < sizeof(buf) ? remaining : sizeof(buf);
        int received = httpd_req_recv(req, (char *)buf, to_read);
        if (received <= 0) break;
        size_t written = 0;
        bsp_i2s_write(buf, received, &written, 1000);
        remaining -= received;
    }

    // Mute when done
    bsp_codec_mute_set(true);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}
```

No PSRAM allocation needed — 1KB stack buffer, write-through.

**Busy detection:** If a `s_playing` flag is set, return 409 Conflict. Set flag on entry, clear on exit.

### 2. New endpoints: `GET /PUT /api/voice`

Voice config stored in NVS (`pw_voice_name`, `pw_speaker_volume`).

```c
// GET /api/voice
{"voice": "en_US-amy-medium", "volume": 70}

// PUT /api/voice
{"voice": "en_US-danny-low", "volume": 50}
```

Defaults: voice = `"en_US-amy-medium"`, volume = `70`.

### 3. Web UI additions

- **Voice dropdown** — hardcoded list of Piper English voices:
  - en_US-amy-medium, en_US-arctic-medium, en_US-bryce-medium, en_US-danny-low, en_US-hfc_female-medium, en_US-hfc_male-medium, en_US-joe-medium, en_US-john-medium, en_US-kathleen-low, en_US-kristin-medium, en_US-kusal-medium
  - en_GB-alan-medium, en_GB-alba-medium, en_GB-jenny_dioco-medium, en_GB-northern_english_male-medium
- **Volume slider** — 0 to 95
- **Test button** — sends PUT to set voice, then calls a test endpoint or just plays a sample

### 4. Status API update

Add `voice` and `speaker_volume` to `GET /api/status` response so the MCP server can read the default voice.

### 5. Modified files summary

| File | Change |
|------|--------|
| `pokewatcher/main/web_server.c` | Add `/api/audio/play`, `/api/voice` endpoints, NVS voice config |
| `pokewatcher/main/web/app.js` | Voice dropdown + volume slider in web UI |
| `pokewatcher/main/web/index.html` | Voice settings section |

## MCP Server Changes

### 1. New tool: `speak`

In `tools.ts`. Registered as `speak` (becomes `watcher__speak` via MCP).

Parameters:
- `text` (string, required) — text to speak
- `voice` (string, optional) — Piper voice model name, defaults to Watcher's configured voice

```typescript
server.registerTool("speak", {
  description: "Speak text aloud through the Watcher's speaker using TTS",
  inputSchema: {
    text: z.string().describe("Text to speak"),
    voice: z.string().optional().describe("Piper voice model (e.g. en_US-amy-medium)"),
  },
}, async ({ text, voice }) => {
  // 1. Get default voice from Watcher if not provided
  if (!voice) {
    const config = await watcher.getVoiceConfig();
    voice = config.voice;
  }

  // 2. Run Piper TTS
  const pcm = await runPiper(text, voice);  // spawns piper, returns 16kHz PCM buffer

  // 3. POST to Watcher
  await watcher.playAudio(pcm);

  return { content: [{ type: "text", text: `Spoke: "${text}" (voice: ${voice})` }] };
});
```

### 2. New file: `tts.ts`

Piper TTS runner + resampler.

```typescript
import { spawn } from "node:child_process";

export async function runPiper(text: string, voice: string): Promise<Buffer> {
  // Spawn piper, pipe text to stdin, collect raw PCM from stdout
  // Resample 22050 → 16000 Hz
  // Return 16kHz 16-bit mono PCM buffer
}
```

**Resampling:** Linear interpolation in Node.js. For every 22050 input samples, produce 16000 output samples. Simple ratio: `outputIndex / 16000 * 22050 = inputIndex`. Interpolate between adjacent samples.

### 3. New watcher-client functions

```typescript
export async function getVoiceConfig(): Promise<{ voice: string; volume: number }>;
export async function playAudio(pcm: Buffer): Promise<any>;  // POST /api/audio/play
```

### 4. Modified files summary

| File | Change |
|------|--------|
| `watcher-mcp/src/tools.ts` | Add `speak` tool registration |
| `watcher-mcp/src/tts.ts` | New — Piper runner + resampler |
| `watcher-mcp/src/watcher-client.ts` | Add `getVoiceConfig()`, `playAudio()` |

## Zidane's Instructions

Update `TOOLS.md` Watcher section:

```markdown
### Voice Output (Speaker)
You have a `watcher__speak` tool that plays text through the Watcher's speaker using TTS.
- For push-to-talk responses (`[Voice from Watcher]` messages): use `watcher__speak` instead of `watcher__display_message`
- You can specify a voice or let it use the default configured on the device
- Keep spoken responses conversational and concise (1-2 sentences)
- If the user explicitly asks you to "send a message" or "show on screen", use `watcher__display_message` instead
```

## Dependencies

- **Piper TTS** — `pip install piper-tts` on the Mac. First run downloads voice models automatically.
- **No new firmware dependencies** — uses existing `bsp_i2s_write`, `bsp_codec_mute_set`, `bsp_codec_volume_set`, NVS.
- **No new npm dependencies** — uses Node.js `child_process.spawn` for Piper.

## Error Handling

| Scenario | Behavior |
|----------|----------|
| Piper not installed | Tool returns error: "piper not found — install with pip install piper-tts" |
| Voice model not found | Piper auto-downloads on first use. If download fails, tool returns error |
| Watcher unreachable | Tool returns error: "Watcher unreachable" |
| Speaker busy (already playing) | Watcher returns 409. MCP retries once after 1s, then returns error |
| Empty text | Tool returns error: "No text to speak" |

## Testing Plan

### Phase 1: Piper on Mac
1. `pip install piper-tts`
2. `echo "Hello world" | piper --model en_US-amy-medium --output-raw > /tmp/test.raw`
3. Verify raw PCM file is created (~22050 Hz)

### Phase 2: Firmware audio playback
1. Resample test.raw to 16kHz: `sox -r 22050 -b 16 -c 1 -e signed -t raw /tmp/test.raw -r 16000 -t raw /tmp/test16k.raw`
2. `curl -X POST http://10.0.0.40/api/audio/play -H "Content-Type: application/octet-stream" --data-binary @/tmp/test16k.raw`
3. Verify speaker plays audio

### Phase 3: MCP tool integration
1. Build MCP server, verify `speak` tool appears in tool list
2. Call `watcher__speak` with test text
3. Verify end-to-end: text → Piper → resample → Watcher speaker

### Phase 4: Voice selection
1. Web UI: select different voice from dropdown
2. Verify NVS persistence across reboots
3. Verify MCP tool uses configured voice when no override provided

## Implementation Order

1. **Install Piper** on Mac, verify it works
2. **Firmware: `/api/audio/play`** endpoint — receive PCM, play through speaker
3. **Firmware: `/api/voice`** endpoints + NVS + web UI
4. **MCP: `tts.ts`** — Piper runner + resampler
5. **MCP: `speak` tool** in tools.ts + watcher-client additions
6. **Zidane instructions** — update TOOLS.md
7. **End-to-end test**

## Critical Gotchas

1. **Speaker is muted at boot** — must call `bsp_codec_mute_set(false)` before playing, `true` after.
2. **Volume clamped to 95** — `bsp_codec_volume_set` clamps internally. Don't pass higher values.
3. **I2S is separate from SPI** — no conflict with LCD or Himax camera. Safe to play audio anytime.
4. **Codec is thread-safe** — `bsp_i2s_write` uses `codec_mutex` internally.
5. **Piper outputs 22050 Hz** for medium/high quality voices — must resample to 16000 Hz before sending to Watcher.
6. **Build from /tmp** — firmware must be built from `/tmp/pokewatcher-build`.
7. **HTTP body streaming** — the `httpd_req_recv` loop reads chunks as they arrive. No need to buffer the full PCM.
8. **Don't modify sdkconfig directly** — only `sdkconfig.defaults`.
9. **Full duplex** — mic and speaker can work simultaneously. Recording and playback don't conflict.
10. **Piper auto-downloads models** — first use of a voice downloads the .onnx model. May take a minute.
