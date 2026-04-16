# Himax Camera — Progressive Feature Addition Test Plan

## Goal

Find which specific piece of the pokewatcher firmware breaks Himax camera SPI2 communication when SD card is also on SPI2. The sscma_client_monitor example works with both. Our firmware doesn't. We need to progressively add our features to the monitor example until it breaks.

## Prerequisites

- Read `docs/knowledgebase/himax-camera-debugging.md` FIRST — contains 29 prior attempts and all technical context
- The monitor example build exists at `/tmp/sscma_monitor_build/` — it was previously built and confirmed working with person detection at 10 FPS
- Our pokewatcher build is at `/tmp/pokewatcher-build/`
- Serial port: `/dev/cu.usbmodem5A8A0533623`
- Build from /tmp (space in project path breaks linker)
- The SDK at `/tmp/SenseCAP-Watcher-Firmware/` has the BSP sensitivity patch applied (line 765: `.sensitivity = 50`)
- Flash with `app-flash` only — preserves NVS/nvsfactory

## How to Detect Success vs Failure

**Success (camera works):**
```
I (xxxx) pw_himax: on_connect: Himax connected!
W (xxxx) sscma_client: rx buffer is full     ← data flowing
```
OR:
```
I (xxxx) pw_himax: Himax: id=360779f5 name=SenseCAP Watcher fw=2024.08.16
I (xxxx) pw_himax: Person detection running
```

**Failure (camera broken):**
```
E (xxxx) sscma_client: sscma_client_set_model(1053): request model failed
E (xxxx) sscma_client: sscma_client_get_info(896): request id failed
```
No `on_connect` callback. AT commands timeout after 20s each.

## Serial Monitor Command

After each flash, reboot the device and capture logs:
```python
python3 -c "
import serial, time, sys
port = serial.Serial('/dev/cu.usbmodem5A8A0533623', 115200, timeout=1)
port.setDTR(False); port.setRTS(True); time.sleep(0.1); port.setRTS(False); time.sleep(0.5)
start = time.time()
while time.time() - start < 60:
    data = port.read(4096)
    if data:
        text = data.decode('utf-8', errors='replace')
        for line in text.split('\n'):
            if any(k in line.lower() for k in ['himax', 'sscma', 'info', 'model', 'person', 'detection', 'invoke', 'running', 'connect', 'break', 'rx buffer']):
                sys.stdout.write(line + '\n')
                sys.stdout.flush()
port.close()
"
```

## Test Steps

Work in a COPY of the monitor example at `/tmp/himax_progressive_test/`. Start from the working monitor example and add one pokewatcher feature at a time. After each addition:
1. Build: `cd /tmp/himax_progressive_test && idf.py build`
2. Flash: `idf.py -p /dev/cu.usbmodem5A8A0533623 app-flash`
3. Monitor serial for 60s
4. Record result: PASS (camera works) or FAIL (camera broken)
5. If FAIL: you found the culprit. Stop and document.

### Step 0: Baseline — Confirm monitor example still works
```bash
cp -r /tmp/sscma_monitor_build /tmp/himax_progressive_test
cd /tmp/himax_progressive_test
# Build and flash
export IDF_PATH="/Users/nacoleon/esp/esp-idf" && . "$IDF_PATH/export.sh"
idf.py build && idf.py -p /dev/cu.usbmodem5A8A0533623 app-flash
```
**Expected:** Camera works (person detection at 10 FPS). If this fails, the monitor example needs to be rebuilt.

**IMPORTANT NOTE:** The monitor example has a different partition table (factory at 0x290000). When switching between monitor and pokewatcher, you may need `idf.py flash` (full flash) instead of `app-flash` to update the partition table. Check boot logs for "no factory app" errors.

### Step 1: Add WiFi + web server
Copy these files from pokewatcher to the monitor example:
- `pokewatcher/main/web_server.c` → `main/web_server.c`
- `pokewatcher/main/web_server.h` → `main/web_server.h`
- `pokewatcher/main/web/index.html` → `main/web/index.html`
- `pokewatcher/main/web/style.css` → `main/web/style.css` (if exists)
- `pokewatcher/main/web/app.js` → `main/web/app.js` (if exists)
- `pokewatcher/main/config.h` → `main/config.h`

Add to the monitor's `app_main()` after existing init:
```c
#include "web_server.h"
// ... in app_main after WiFi init:
init_wifi();  // Copy wifi init code from pokewatcher app_main.c
pw_web_server_start();
```

Update `main/CMakeLists.txt` to include the new source files and embed web assets.

Add WiFi + HTTP sdkconfig entries to `sdkconfig.defaults`:
```
CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024
CONFIG_HTTPD_MAX_URI_LEN=512
CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y
CONFIG_LWIP_MAX_SOCKETS=16
```

Build, flash, test. **Record: PASS or FAIL.**

### Step 2: Add event queue + agent state
Copy:
- `pokewatcher/main/event_queue.c` + `.h`
- `pokewatcher/main/agent_state.c` + `.h`

Add to app_main:
```c
pw_event_queue_init();
pw_agent_state_init();
pw_agent_state_task_start();
```

Update CMakeLists.txt. Build, flash, test. **Record: PASS or FAIL.**

### Step 3: Add renderer (LVGL sprite animation)
This is the big one — the renderer does continuous LVGL rendering at 10 FPS.

Copy:
- `pokewatcher/main/renderer.c` + `.h`
- `pokewatcher/main/sprite_loader.c` + `.h`
- `pokewatcher/main/dialog.c` + `.h`

**NOTE:** The monitor example already calls `bsp_lvgl_init()`. Our `pw_renderer_init()` also calls it. Make sure it's not called twice — either remove the monitor's LVGL init or skip ours.

Add to app_main:
```c
pw_renderer_init();
pw_renderer_load_character("zidane");
pw_renderer_task_start();
```

Add font configs to sdkconfig.defaults:
```
CONFIG_LV_FONT_MONTSERRAT_20=y
CONFIG_LV_FONT_MONTSERRAT_22=y
CONFIG_LV_FONT_MONTSERRAT_28=y
CONFIG_LVGL_INPUT_DEVICE_USE_TP=n
```

Build, flash, test. **Record: PASS or FAIL.**

### Step 4: Add the himax_task with all our detection logic
Replace the monitor's camera code with our `himax_task.c`:
- `pokewatcher/main/himax_task.c` + `.h`

Call `pw_himax_task_start()` instead of the monitor's inline camera code.

Build, flash, test. **Record: PASS or FAIL.**

### If Step 3 Fails (Renderer is the Culprit)

The renderer is the most likely suspect because:
- It does continuous LVGL rendering → continuous SPI3 DMA transfers
- It runs at 10 FPS → every 100ms a full screen flush
- The background wipe transition temporarily increases SPI3 activity

Sub-tests:
- **3a:** Add renderer but DON'T call `pw_renderer_task_start()` — just init, no rendering loop
- **3b:** Start renderer task but with a very slow frame rate (1 FPS instead of 10)
- **3c:** Disable background loading (no SD card reads during runtime)

### If Step 1 Fails (WiFi/Web Server is the Culprit)

WiFi uses DMA for packet buffers. The HTTP server runs tasks that could interfere with SPI scheduling.

Sub-tests:
- **1a:** Init WiFi but don't start web server
- **1b:** Start web server but don't connect to WiFi (no AP configured)

### If No Single Step Fails

It might be a COMBINATION of features. Try:
- Steps 1+2 together (WiFi + agent state, no renderer)
- Steps 1+3 (WiFi + renderer, no agent state)
- Steps 2+3 (agent state + renderer, no WiFi)

## Build Notes

- The monitor example's `CMakeLists.txt` is at `main/CMakeLists.txt`
- Our pokewatcher's `CMakeLists.txt` includes `EXTRA_COMPONENT_DIRS` pointing to `/tmp/SenseCAP-Watcher-Firmware/components`
- The monitor example should already have this — check its top-level `CMakeLists.txt`
- Web assets (index.html etc.) need `EMBED_FILES` in CMakeLists.txt — copy from pokewatcher's `main/CMakeLists.txt`
- If you get linker errors about `_binary_index_html_start`, the embed isn't set up

## Recording Results

Document each test result in a table:

| Step | Feature Added | Result | Notes |
|------|--------------|--------|-------|
| 0 | Baseline monitor | PASS/FAIL | |
| 1 | WiFi + web server | PASS/FAIL | |
| 2 | Event queue + agent state | PASS/FAIL | |
| 3 | Renderer (LVGL sprites) | PASS/FAIL | |
| 4 | Himax task | PASS/FAIL | |

Add the results to `docs/knowledgebase/himax-camera-debugging.md` under a new "## Phase 8: Progressive Feature Addition" section.

## After Finding the Culprit

Once you identify which feature breaks Himax:
1. Document the exact feature and the minimal code that triggers the failure
2. Investigate WHY that feature affects SPI2 — likely through DMA contention, task scheduling, or interrupt priority
3. Look for a fix that allows both to coexist (timing, priorities, bus locking, etc.)
4. Update `himax-camera-debugging.md` with findings
