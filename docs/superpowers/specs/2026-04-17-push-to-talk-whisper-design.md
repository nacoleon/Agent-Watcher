# Push-to-Talk Voice Input with Embedded Whisper — Design Spec

## Overview

Add voice input to the Zidane Watcher via push-to-talk (double-click knob). The ESP32 records audio, HTTP POSTs it to the MCP server on Mac, which transcribes it locally using whisper-node (bundled dependency — installs with `npm install`), and delivers the text to OpenClaw as a logging message. No separate Whisper server process required.

## Architecture

```
[Double-click knob on Watcher]
    → ESP32 records 5s of 16kHz 16-bit mono PCM
    → Prepends 44-byte WAV header
    → HTTP POST to Mac (watcher-mcp :3848/audio)
    → whisper-node transcribes in-process
    → MCP server sends logging message to OpenClaw: "voice_input: <transcribed text>"
    → OpenClaw responds via existing MCP tools (display_message, set_state, etc.)
```

```
                                    ┌─────────────────────────────────────────┐
                                    │         watcher-mcp (Node.js)          │
ESP32 ──HTTP POST /audio──────────► │  Express :3848  →  whisper-node        │
       (WAV binary body)            │       ↓                ↓               │
                                    │  save temp.wav → transcribe → text     │
                                    │       ↓                                │
                                    │  server.sendLoggingMessage()           │
                                    │       ↓                                │
                                    │  stdio transport ← → OpenClaw          │
                                    └─────────────────────────────────────────┘
```

## Why This Design

- **Bundled dependency**: `whisper-node` compiles whisper.cpp during `npm install`. No separate server, no Docker, no Python. One `package.json` dependency.
- **Apple Silicon optimized**: whisper.cpp compiles with Metal/Accelerate on macOS automatically.
- **Zero extra processes**: The Express HTTP listener coexists with the stdio MCP transport in the same Node.js process.
- **Fastest path to OpenClaw**: Audio arrives → transcribes → `sendLoggingMessage()` fires immediately. No inter-process hops, no polling.

## Firmware Changes (ESP32-S3)

### 1. New file: `pokewatcher/main/voice_input.c` + `.h`

Voice recording task triggered by double-click.

#### Public API

```c
void pw_voice_init(void);  // Register double-click callback + allocate buffers
```

#### Internal flow

1. **Double-click callback** — registered on the existing knob button handle in `dialog.c`
2. **Start recording task** — `xTaskCreate` with ~4KB stack on PSRAM-capable thread
3. **Visual feedback** — flash RGB LED blue while recording, set agent state to "working"
4. **Record loop** — call `bsp_i2s_read()` in a loop for 5 seconds, filling a PSRAM buffer
5. **Build WAV** — prepend 44-byte WAV header to the PCM data
6. **HTTP POST** — send to `http://<MCP_SERVER_IP>:3848/audio` using `esp_http_client`
7. **Cleanup** — free buffer, restore previous agent state, LED off

#### Memory budget

| Allocation | Size | Where |
|-----------|------|-------|
| 5s audio buffer | 160,000 bytes (16000 Hz × 2 bytes × 5s) | PSRAM (`MALLOC_CAP_SPIRAM`) |
| WAV header | 44 bytes | Stack |
| HTTP client buffer | ~512 bytes | Stack |
| Task stack | 4096 bytes | Internal SRAM |
| **Total** | **~160 KB PSRAM + 4.5 KB internal** | Fits easily |

#### Key BSP functions to use

```c
// Already initialized in app_main.c (bsp_codec_init())
esp_err_t bsp_i2s_read(void *audio_buffer, size_t len, size_t *bytes_read, uint32_t timeout_ms);
esp_err_t bsp_codec_mute_set(bool enable);  // unmute mic if needed
esp_err_t bsp_codec_set_fs(uint32_t rate, uint32_t bits_cfg, i2s_slot_mode_t ch);
```

Codec is already initialized at 16kHz/16-bit in `app_main.c`. The `bsp_i2s_read()` is mutex-protected (`codec_mutex`) so it's safe to call from the voice task while the speaker might be playing.

#### WAV header format

```c
// 44-byte RIFF WAV header for 16kHz 16-bit mono PCM
typedef struct __attribute__((packed)) {
    char     riff[4];        // "RIFF"
    uint32_t file_size;      // data_size + 36
    char     wave[4];        // "WAVE"
    char     fmt[4];         // "fmt "
    uint32_t fmt_size;       // 16
    uint16_t audio_format;   // 1 (PCM)
    uint16_t num_channels;   // 1
    uint32_t sample_rate;    // 16000
    uint32_t byte_rate;      // 32000 (16000 * 1 * 2)
    uint16_t block_align;    // 2 (1 * 16/8)
    uint16_t bits_per_sample;// 16
    char     data[4];        // "data"
    uint32_t data_size;      // num_samples * 2
} wav_header_t;
```

### 2. Modify: `pokewatcher/main/dialog.c`

Add `BUTTON_DOUBLE_CLICK` callback registration alongside existing single-press and long-press.

```c
// In pw_dialog_init(), after existing button registrations:
iot_button_register_cb(s_btn_handle, BUTTON_DOUBLE_CLICK, knob_btn_double_cb, NULL);
```

The double-click callback calls `pw_voice_start_recording()` which spawns the recording task.

**Important**: The `iot_button` library (already a dependency) supports `BUTTON_DOUBLE_CLICK` natively. Default double-click window is ~300ms — no configuration needed.

### 3. Modify: `pokewatcher/main/config.h`

Add MCP server IP/port config:

```c
#define PW_MCP_SERVER_IP    "10.0.0.X"   // Mac's IP — discover via status endpoint or hardcode
#define PW_MCP_SERVER_PORT  3848
#define PW_VOICE_RECORD_MS  5000         // Recording duration
#define PW_VOICE_SAMPLE_RATE 16000
#define PW_VOICE_SAMPLE_BITS 16
```

### 4. Modify: `pokewatcher/main/app_main.c`

Add `pw_voice_init()` call after codec init:

```c
// After bsp_codec_init() / bsp_codec_mute_set(true):
pw_voice_init();
```

### 5. Modify: `pokewatcher/main/CMakeLists.txt`

Add `voice_input.c` to SRCS list.

## MCP Server Changes (Node.js)

### 1. Add dependency: `whisper-node`

```json
{
  "dependencies": {
    "@modelcontextprotocol/sdk": "^1.29.0",
    "zod": "^3.24.0",
    "whisper-node": "^1.1.0"
  }
}
```

`npm install` will compile whisper.cpp from source with Metal/Accelerate support on macOS. First install takes ~2-3 minutes for the C++ compilation.

**Model download**: whisper-node includes a helper to download models. Use `base.en` (~140MB) for English-only, fast transcription. The model downloads on first use and caches locally.

### 2. New file: `watcher-mcp/src/audio-server.ts`

Express HTTP server that receives audio from the Watcher and transcribes it.

```typescript
import express from "express";
import { writeFileSync, unlinkSync, mkdirSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { whisper } from "whisper-node";
import { log } from "./logger.js";

const AUDIO_PORT = 3848;
const MODEL = "base.en";  // ~140MB, fast, English-only

export function startAudioServer(onTranscription: (text: string) => void): void {
  const app = express();

  // Accept raw binary body (WAV file from ESP32)
  app.use("/audio", express.raw({ type: "application/octet-stream", limit: "1mb" }));

  app.post("/audio", async (req, res) => {
    const audioBuffer = req.body as Buffer;
    if (!audioBuffer || audioBuffer.length < 44) {
      res.status(400).json({ error: "No audio data" });
      return;
    }

    log("audio", `Received ${audioBuffer.length} bytes`);

    // Save to temp file (whisper-node needs a file path)
    const tmpDir = join(tmpdir(), "watcher-audio");
    mkdirSync(tmpDir, { recursive: true });
    const tmpFile = join(tmpDir, `voice-${Date.now()}.wav`);

    try {
      writeFileSync(tmpFile, audioBuffer);

      // Transcribe
      const result = await whisper(tmpFile, { modelName: MODEL, whisperOptions: { language: "en" } });
      const text = result.map(r => r.speech).join(" ").trim();

      log("audio", `Transcribed: "${text}"`);

      if (text) {
        onTranscription(text);
      }

      res.json({ ok: true, text });
    } catch (err: any) {
      log("error", "Transcription failed", { error: err.message });
      res.status(500).json({ error: err.message });
    } finally {
      try { unlinkSync(tmpFile); } catch {}
    }
  });

  // Health check
  app.get("/audio/health", (_req, res) => {
    res.json({ ok: true, model: MODEL });
  });

  app.listen(AUDIO_PORT, () => {
    log("audio", `Audio server listening on :${AUDIO_PORT}`);
  });
}
```

### 3. Modify: `watcher-mcp/src/index.ts`

Wire up the audio server and connect transcriptions to MCP logging.

```typescript
import { startAudioServer } from "./audio-server.js";

// ... existing setup ...

// Start audio receiver alongside MCP stdio transport
startAudioServer(async (text: string) => {
  // Send transcription to OpenClaw as a logging message
  await server.sendLoggingMessage({
    level: "info",
    logger: "voice",
    data: `voice_input: ${text}`,
  });
});
```

### 4. Add dependency: `express` + `@types/express`

```json
{
  "dependencies": {
    "express": "^4.21.0"
  },
  "devDependencies": {
    "@types/express": "^4.17.0"
  }
}
```

### 5. Modify: `watcher-mcp/src/config.ts`

```typescript
export const AUDIO_PORT = 3848;
export const WHISPER_MODEL = "base.en";
```

## MCP Server IP Discovery

The ESP32 needs to know the Mac's IP to POST audio. Options (implement in order of preference):

1. **Hardcode in config.h** — simplest for now. The Mac is always at a known IP on the local network.
2. **Broadcast via mDNS** — the MCP server could register `_watcher-mcp._tcp` via Bonjour. ESP32 discovers via mDNS query. Nice but more work.
3. **Return in heartbeat response** — the firmware already calls `/api/heartbeat` which the MCP initiates. Could reverse: MCP server announces its IP via a new status field.

**Recommendation**: Start with hardcode. Add mDNS later if needed.

## Visual/Audio Feedback on Watcher

| Phase | Visual | Audio |
|-------|--------|-------|
| Double-click detected | RGB LED → solid blue | Short beep (optional, phase 2) |
| Recording (5s) | RGB LED → pulsing blue | — |
| Uploading | RGB LED → pulsing yellow | — |
| Transcription received | RGB LED → green flash, then off | — |
| Error | RGB LED → red flash × 3, then off | — |

Agent state changes during recording:
- Set to `working` when recording starts
- Restore to previous state when complete

## Error Handling

| Scenario | Firmware behavior | MCP behavior |
|----------|------------------|--------------|
| WiFi down | Skip POST, flash red LED, log error | — |
| MCP server unreachable | Retry once after 1s, then give up, flash red | — |
| Whisper fails | — | Return 500, log error |
| Empty transcription | — | Don't send logging message, return `{ ok: true, text: "" }` |
| Audio too short (<0.5s) | Don't POST, flash red | Reject with 400 |
| PSRAM alloc fails | Don't record, flash red, log error | — |

## Testing Plan

### Phase 1: MCP server audio endpoint
1. `npm install` in watcher-mcp — verify whisper-node compiles
2. Start MCP server, POST a test WAV to `:3848/audio` via curl
3. Verify transcription appears in logs
4. Verify `sendLoggingMessage` fires (check via MCP inspector or OpenClaw logs)

```bash
# Test with a sample WAV file
curl -X POST http://localhost:3848/audio \
  -H "Content-Type: application/octet-stream" \
  --data-binary @test.wav
```

### Phase 2: Firmware double-click + recording
1. Add double-click handler, verify it fires (serial log)
2. Record 5s audio, dump hex to serial log to verify PCM data
3. POST to MCP server, verify end-to-end transcription

### Phase 3: Integration
1. Double-click → record → transcribe → OpenClaw receives text
2. OpenClaw responds → display_message shows response on Watcher
3. Test with background noise, different distances from mic

## Implementation Order

### Step 1: MCP server (Mac side) — do this first, testable independently
1. `npm install whisper-node express @types/express`
2. Create `audio-server.ts`
3. Wire into `index.ts`
4. Update `config.ts` with audio port/model constants
5. Test with curl + sample WAV

### Step 2: Firmware voice recording (ESP32 side)
1. Create `voice_input.c` + `voice_input.h`
2. Add double-click registration in `dialog.c`
3. Add config constants in `config.h`
4. Add `pw_voice_init()` call in `app_main.c`
5. Update `CMakeLists.txt`
6. Build from `/tmp` and flash

### Step 3: Integration + polish
1. End-to-end test
2. Tune recording duration (5s default, could make configurable)
3. Add RGB LED feedback
4. Add confirmation chime via speaker (optional)

## Files Changed Summary

### New files
| File | Side | Purpose |
|------|------|---------|
| `pokewatcher/main/voice_input.c` | Firmware | Recording task, WAV builder, HTTP POST |
| `pokewatcher/main/voice_input.h` | Firmware | Public API (`pw_voice_init`) |
| `watcher-mcp/src/audio-server.ts` | MCP | Express server + whisper-node transcription |

### Modified files
| File | Side | Change |
|------|------|--------|
| `pokewatcher/main/dialog.c` | Firmware | Add `BUTTON_DOUBLE_CLICK` callback |
| `pokewatcher/main/config.h` | Firmware | Add MCP server IP/port, voice constants |
| `pokewatcher/main/app_main.c` | Firmware | Add `pw_voice_init()` call |
| `pokewatcher/main/CMakeLists.txt` | Firmware | Add `voice_input.c` to SRCS |
| `watcher-mcp/src/index.ts` | MCP | Import + start audio server |
| `watcher-mcp/src/config.ts` | MCP | Add AUDIO_PORT, WHISPER_MODEL |
| `watcher-mcp/package.json` | MCP | Add whisper-node, express deps |

## Critical Gotchas for the Implementing Agent

1. **Build from /tmp**: Firmware MUST be built from `/tmp/pokewatcher-build`, not the repo directory. See `docs/knowledgebase/building-and-flashing-firmware.md`.
2. **SPI bus conflict**: The I2S audio bus is separate from SPI — no conflict with LCD or Himax camera. Safe to read mic while camera is running.
3. **Codec already initialized**: `bsp_codec_init()` runs in `app_main.c`. Do NOT reinitialize. Just call `bsp_i2s_read()` directly.
4. **Speaker is muted at boot**: The mic works independently of the speaker mute state. No need to unmute for recording.
5. **PSRAM allocation**: Use `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` for the audio buffer. Never use internal SRAM for 160KB — it won't fit.
6. **whisper-node first run**: First `npm install` compiles C++ (~2-3 min). First transcription downloads the model (~140MB). Subsequent runs are fast.
7. **sdkconfig**: Do NOT modify `sdkconfig` directly. Put new Kconfig options in `sdkconfig.defaults`. See feedback memory.
8. **Thread safety**: `bsp_i2s_read()` uses `codec_mutex` internally — safe to call from any task.
9. **Double-click vs single-click**: The `iot_button` library handles timing internally. A double-click will NOT trigger two single-press events — the library debounces correctly.
10. **HTTP POST from ESP32**: Use `esp_http_client` (already available in ESP-IDF). Set `content_type` to `application/octet-stream` and write the WAV binary directly.
