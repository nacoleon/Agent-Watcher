# Himax Camera Investigation Report — Full Results

**Date:** 2026-04-16
**Investigator:** Claude Opus 4.6 agent session
**Commit:** `4b006ae` (fix(firmware): SD card MISO contention root cause found)

---

## Executive Summary

Two bugs were found blocking Himax camera person detection:

1. **SPI2 MISO contention (SOLVED):** The physical SD card doesn't release the shared data wire after being initialized, blocking the camera. Fix: power off the SD card after loading sprites.

2. **SSCMA library heap crash (OPEN):** The camera connects successfully, but the third-party SSCMA library crashes when processing stale data from the camera's previous session. Crash is in `mapping_insert` at `tlsf.c:266` (heap allocator). Likely root cause: a buffer accounting bug in `sscma_client_ops.c` line 260-261.

---

## Part 1: SPI2 MISO Contention — SOLVED

### Background

The SenseCap Watcher has two devices sharing the SPI2 bus:
- **Himax WE2 camera** (CS = GPIO21, 12 MHz) — person detection AI chip
- **SD card** (CS = GPIO46, 20 MHz) — stores sprite images

Both use the same MOSI (GPIO5), MISO (GPIO6), and SCLK (GPIO4) wires.

### Root Cause

The physical SD card, once initialized into SPI mode by `esp_vfs_fat_sdspi_mount()`, does **not tri-state its MISO output** when its chip-select (GPIO46) goes high. It continues driving MISO high (0xFF), overpowering the Himax camera's responses on the same wire.

This is a known hardware behavior of some SD cards in SPI mode. The SD card specification technically requires tri-state when CS is high, but many cards don't comply.

### How I Proved It — 10 Experiments

Each experiment was a modified version of the `sscma_client_monitor` SDK example, built at `/tmp/himax_progressive_test/`, with one variable changed.

| # | What I Changed | Camera Works? | What It Proves |
|---|---------------|:---:|---|
| 1 | SD card physically in slot, never software-initialized, CS held high | YES | Physical presence alone doesn't matter |
| 2 | Full SD mount → full deinit (unmount + remove SPI device) → CS high | NO | Software teardown doesn't release MISO |
| 3 | Held Himax CS (GPIO21) HIGH before SD init | NO | Not a floating CS problem |
| 4 | Registered a dummy SPI device on SPI2 (no actual SD communication) | YES | Having 2 SPI devices on the bus is fine |
| 5 | Sent CMD0 to SD card via raw SPI, then removed device | YES | Simple SPI traffic doesn't break things |
| 6 | Full SD mount → deinit → sent 80 clock pulses with CS high | NO | Clock flushing doesn't release MISO |
| 7 | Full SD mount → deinit → power cycled Himax chip | NO | Not a Himax state problem |
| 8 | Full SD mount → deinit → freed entire SPI2 bus → reinit from scratch → power cycled Himax | NO | Not ESP32 SPI subsystem state |
| 9 | Started camera first (working at 10 FPS), then mounted SD card at runtime | Images stopped instantly | Proves SD mount disrupts bus in real-time |
| **10** | **Full SD mount → deinit → POWER OFF SD card via IO expander** | **YES** | **Cutting power releases MISO** |

### The Fix

In `pokewatcher/main/app_main.c`, after loading sprites (step 5b):

```c
// Unmount SD card filesystem and remove SPI device
bsp_sdcard_deinit_default();

// Kill SD card power — this physically releases MISO
esp_io_expander_set_level(io_exp, BSP_PWR_SDCARD, 0);
vTaskDelay(pdMS_TO_TICKS(100));

// Hold CS high to prevent stray activity
gpio_set_level(BSP_SD_SPI_CS, 1);

// Power cycle Himax to clear stale inference data
esp_io_expander_set_level(io_exp, BSP_PWR_AI_CHIP, 0);
vTaskDelay(pdMS_TO_TICKS(200));
esp_io_expander_set_level(io_exp, BSP_PWR_AI_CHIP, 1);
vTaskDelay(pdMS_TO_TICKS(500));
```

The SD card is only needed for ~500ms at boot to load sprite images into SPIRAM. After that, it's powered off permanently for the session.

### Verification

With this fix:
- `sscma_client_init: 0x0 (ESP_OK)` — camera init succeeds
- `on_connect: Himax connected!` — camera connected
- `rx buffer is full` — data flowing from camera
- Sprites, WiFi, web server, renderer, agent state all running simultaneously

### Important Hardware Details

- SD card power: IO expander pin 8 (`BSP_PWR_SDCARD`)
- Himax power: IO expander pin 11 (`BSP_PWR_AI_CHIP`)
- Himax reset: IO expander pin 7 (`BSP_SSCMA_CLIENT_RST`)
- SD card CS: GPIO46 (`BSP_SD_SPI_CS`)
- Himax CS: GPIO21 (`BSP_SSCMA_CLIENT_SPI_CS`)
- SPI2 bus: MOSI=GPIO5, MISO=GPIO6, SCLK=GPIO4
- `bsp_spi_bus_init()` has a static guard — only initializes once

---

## Part 2: SSCMA Library Heap Crash — OPEN

### Symptom

After the SPI2 fix, the camera connects and communicates. But ~20-25 seconds after boot, the firmware crashes:

```
W (28039) sscma_client: request not found: MODEL
W (28039) sscma_client: request not found: MODEL
... (dozens of these)
Guru Meditation Error: Core 0 panic'ed (LoadProhibited). Exception was unhandled.
PC: 0x420ee9af
```

Decoded crash address: `mapping_insert` in `tlsf.c:266` — the TLSF heap allocator. This is **heap corruption**, not a simple NULL pointer.

### Why It Happens

1. The Himax chip has person detection firmware stored in its internal flash
2. After power-on, it **auto-starts inference** — it immediately begins sending detection events over SPI
3. When `bsp_sscma_client_init()` is called, it creates two internal FreeRTOS tasks:
   - `sscma_client_process` — parses replies from the RX buffer
   - `sscma_client_monitor` — calls `on_connect`, `on_event`, `on_log` callbacks
4. The process task immediately starts receiving unsolicited inference events
5. These events contain large base64-encoded JPEG images (~8KB each) and bounding box data
6. The process task tries to match each event to a pending request — there are none
7. "request not found: MODEL" warnings flood the log
8. The sustained allocation/deallocation of reply buffers corrupts the heap

### Likely Root Cause: Buffer Accounting Bug

In `sscma_client_ops.c`, the `sscma_client_process` function (line 206) has a buffer management issue at **line 260-261**:

```c
// Line 260: memmove remaining data to buffer start
memmove(client->rx_buffer.data,
        suffix + RESPONSE_SUFFIX_LEN,
        client->rx_buffer.pos - (suffix - client->rx_buffer.data) - RESPONSE_PREFIX_LEN);

// Line 261: adjust position
client->rx_buffer.pos -= len;  // len = suffix - prefix + RESPONSE_SUFFIX_LEN
```

**The bug:** When there's garbage data before the `\r{` prefix marker (common with stale inference data), the `memmove` copies remaining data to `buffer[0]`, discarding both the reply AND the garbage prefix. But `pos -= len` only subtracts the reply length, not the garbage length. This leaves `pos` larger than the actual data, causing subsequent iterations to read beyond valid buffer contents.

Example walkthrough:
```
Buffer: [GARBAGE_7_BYTES]\r{...reply_100_bytes...}\nMORE_DATA_20_BYTES
pos = 129, prefix at offset 7, len = 102

memmove: copies 20 bytes of MORE_DATA to buffer[0]  ← correct
pos -= 102 → pos = 27  ← WRONG! Should be 20 (7 bytes of garbage also removed)

Next iteration: reads 7 bytes of uninitialized memory as if it were valid data
```

Over time, parsing garbage memory corrupts cJSON allocations, which corrupts the TLSF heap.

**Note:** `RESPONSE_PREFIX_LEN` and `RESPONSE_SUFFIX_LEN` are both 2 (`"\r{"` and `"}\n"`), so the prefix/suffix confusion doesn't matter numerically. The bug is that garbage bytes before the prefix aren't accounted for in the `pos` adjustment.

### What I Already Tried

| Approach | Result |
|----------|--------|
| `s_himax_ready` guard in `on_event` callback | Prevents our callback crash, but SSCMA internal crash remains |
| Deferred callback registration (register `on_event` only after model set) | Same crash — it's in the library's process task, not our callback |
| Himax power cycle before SSCMA init (clear stale data) | Same crash — chip auto-starts inference after power-on |
| Match monitor example exactly (no delays, same call sequence) | Same crash |
| 3-second delay before `sscma_client_init()` | Same crash |

### Files Involved

- **SSCMA library source:** `/tmp/SenseCAP-Watcher-Firmware/components/sscma_client/src/sscma_client_ops.c`
  - `sscma_client_process()` — line 206-370, the crash-causing function
  - `sscma_client_new()` — line ~490, creates the process and monitor tasks
  - `sscma_client_init()` — line ~740, does hardware reset (suspends process task temporarily)
- **SSCMA commands:** `/tmp/SenseCAP-Watcher-Firmware/components/sscma_client/include/sscma_client_commands.h`
- **BSP init:** `/tmp/SenseCAP-Watcher-Firmware/components/sensecap-watcher/sensecap-watcher.c`
  - `bsp_sscma_client_init()` — line 1241

---

## Instructions for the Next Agent

### Your Mission

Fix the SSCMA library heap crash so that `pw_himax_task_start()` can be uncommented and person detection runs stably alongside sprites + WiFi + renderer.

### Build Environment

```bash
# Build helper script (handles IDF paths):
bash /tmp/idf_build.sh /tmp/pokewatcher-build build
bash /tmp/idf_build.sh /tmp/pokewatcher-build -p /dev/cu.usbmodem5A8A0533623 app-flash

# Serial monitor:
python3 -c "
import serial, time, sys
port = serial.Serial('/dev/cu.usbmodem5A8A0533623', 115200, timeout=1)
port.setDTR(False); port.setRTS(True); time.sleep(0.1); port.setRTS(False); time.sleep(0.5)
start = time.time()
while time.time() - start < 60:
    data = port.read(4096)
    if data:
        sys.stdout.write(data.decode('utf-8', errors='replace'))
        sys.stdout.flush()
port.close()
"
```

**CRITICAL:** The pokewatcher build at `/tmp/pokewatcher-build/` uses COPIES of the source files, not symlinks. After editing files in the project directory, you MUST copy them:
```bash
cp "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher/main/app_main.c" /tmp/pokewatcher-build/main/
cp "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher/main/himax_task.c" /tmp/pokewatcher-build/main/
```

### Recommended Approach: Fix the SSCMA Library Directly

The cleanest fix is to patch `sscma_client_ops.c` in the SDK:

**Option A — Fix the buffer accounting bug (line 260-261):**

```c
// BEFORE (buggy):
memmove(client->rx_buffer.data, suffix + RESPONSE_SUFFIX_LEN,
        client->rx_buffer.pos - (suffix - client->rx_buffer.data) - RESPONSE_PREFIX_LEN);
client->rx_buffer.pos -= len;

// AFTER (fixed):
int bytes_after_reply = client->rx_buffer.pos - (suffix - client->rx_buffer.data + RESPONSE_SUFFIX_LEN);
if (bytes_after_reply < 0) bytes_after_reply = 0;
memmove(client->rx_buffer.data, suffix + RESPONSE_SUFFIX_LEN, bytes_after_reply);
client->rx_buffer.pos = bytes_after_reply;
```

This same fix needs to be applied at line 426 as well (the `sscma_client_monitor` function has the same pattern).

**Option B — Prevent stale data from reaching the parser:**

In `sscma_client_process()`, after line 216 (`if (client->inited == false) continue;`), add a drain mode that discards all data until the first legitimate request is sent. The `inited` flag is set by `sscma_client_init()` which does a hardware reset. Any data received before that is stale.

**Option C — Combine A + B for maximum safety.**

### After Patching

1. Copy the patched file to the build SDK:
   ```bash
   cp /path/to/patched/sscma_client_ops.c /tmp/SenseCAP-Watcher-Firmware/components/sscma_client/src/
   ```

2. Uncomment `pw_himax_task_start()` in `pokewatcher/main/app_main.c` (line 206)

3. Copy updated files to build directory:
   ```bash
   cp "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher/main/app_main.c" /tmp/pokewatcher-build/main/
   ```

4. Build and flash:
   ```bash
   bash /tmp/idf_build.sh /tmp/pokewatcher-build build
   bash /tmp/idf_build.sh /tmp/pokewatcher-build -p /dev/cu.usbmodem5A8A0533623 app-flash
   ```

5. Monitor for 60+ seconds — success looks like:
   ```
   I (xxxx) pw_himax: on_connect: Himax connected!
   I (xxxx) pw_himax: Himax: id=360779f5 name=SenseCAP Watcher fw=2024.08.16
   I (xxxx) pw_himax: Person detection running
   I (xxxx) pw_himax: Person detected
   ```

   Failure looks like: `Guru Meditation Error` or `request not found: MODEL` flooding.

6. Once stable, verify no crash for 5+ minutes, then commit.

### What NOT to Do

- **Don't re-investigate the SPI2/SD card issue** — it's solved. The SD card power-off fix works.
- **Don't add delays or retries to himax_task.c** — the crash is in the SSCMA library's internal process task, not in our code. Delays just change the timing of the crash.
- **Don't increase buffer/stack sizes** — the crash is heap corruption from wrong buffer arithmetic, not an overflow.
- **Don't try to drain the RX buffer from our code** — the SSCMA library's internal tasks are already reading from SPI before our code gets a chance. The library starts its tasks in `bsp_sscma_client_init()`.

### Alternative Approach: SPIFFS Instead of SD Card

If the SSCMA library crash proves too hard to fix, a nuclear option is to move sprites from the SD card to a SPIFFS partition in flash. This eliminates the need for SPI2 bus sharing entirely:

1. Add a `spiffs` partition to the partition table (~256KB for sprites)
2. Use `esp_vfs_spiffs_register()` instead of SD card mount
3. Pack sprites into the SPIFFS image at build time
4. Remove all SD card code
5. The Himax can be initialized immediately without the power-off sequence

The sprites are small (~200KB total) and would fit in a flash partition. This is the safest long-term solution.

### Key File Locations

| File | Purpose |
|------|---------|
| `pokewatcher/main/app_main.c` | Boot sequence, SD power-off fix, himax task start |
| `pokewatcher/main/himax_task.c` | Camera task, person detection logic |
| `/tmp/SenseCAP-Watcher-Firmware/components/sscma_client/src/sscma_client_ops.c` | SSCMA library — the crash source |
| `/tmp/SenseCAP-Watcher-Firmware/components/sensecap-watcher/sensecap-watcher.c` | BSP — SD card init, Himax init, SPI bus init |
| `docs/knowledgebase/himax-camera-debugging.md` | Full investigation history (40 attempts) |
| `docs/knowledgebase/spi-bus-conflict.md` | SPI architecture and safe patterns |
| `/tmp/idf_build.sh` | Build helper script (sets up ESP-IDF paths) |

### Serial Port

`/dev/cu.usbmodem5A8A0533623` — 115200 baud, ESP32-S3 USB serial

### Success Criteria

1. No `Guru Meditation Error` for 5+ minutes after boot
2. `Person detection running` log message appears
3. `Person detected` / `Person left` events fire when someone walks past the camera
4. Renderer continues at 10 FPS (check `ALIVE frame=xxx` logs)
5. WiFi stays connected
6. Web dashboard at http://zidane.local responds
