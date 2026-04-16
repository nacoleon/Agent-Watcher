# Agent Instructions: Himax Camera SPI2 Bus Conflict Investigation

## Your Mission

Find which specific feature of the pokewatcher firmware breaks the Himax camera when the SD card is also mounted on SPI2. The camera works in the sscma_client_monitor SDK example (with SD card + Himax on same SPI2 bus) but fails in our pokewatcher firmware. You will progressively add pokewatcher features to the working monitor example until it breaks.

## Critical Context — Read These First

1. **`docs/knowledgebase/himax-camera-debugging.md`** — Full investigation log. 29 attempts. Root cause: SD card on SPI2 breaks Himax, but only in our firmware. The monitor example works with both.
2. **`docs/knowledgebase/himax-progressive-test-plan.md`** — Detailed step-by-step test plan. Follow this exactly.
3. **`docs/knowledgebase/building-and-flashing-firmware.md`** — Build from /tmp, serial port, sdkconfig rules.
4. **`docs/knowledgebase/spi-bus-conflict.md`** — SPI bus architecture.

## What We Know

- **Camera works:** stock firmware, sscma_client_monitor example, our firmware WITHOUT SD card
- **Camera fails:** our firmware WITH SD card — any init order, any config, any sdkconfig
- The monitor example at `/tmp/sscma_monitor_build/` was built and confirmed working (person detection at 10 FPS, SD card mounted, LCD running)
- When camera works, you see: `on_connect: Himax connected!` and `rx buffer is full`
- When camera fails: AT commands timeout after 20s, no callbacks fire
- The SPI MISO line returns 0xFF (all ones) when the failure occurs — the chip's slave hardware clocks but no software drives real data

## Your Approach

Follow the test plan in `docs/knowledgebase/himax-progressive-test-plan.md`. In summary:

1. **Step 0:** Confirm the monitor example at `/tmp/sscma_monitor_build/` still works. Flash it, check serial output for person detection logs. If it doesn't work (e.g., wrong partition table from a previous pokewatcher flash), rebuild it from `/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/SenseCAP-Watcher-Firmware/examples/sscma_client_monitor/` following the test plan instructions.

2. **Step 1-4:** Copy the monitor build to `/tmp/himax_progressive_test/`. Add one pokewatcher feature at a time (WiFi, event queue, renderer, himax task). After each addition, build, flash, and check if the camera still works.

3. **When it breaks:** You found the culprit. Document it, then do sub-tests to narrow down the exact code within that feature.

## Build Environment

```bash
export IDF_PATH="/Users/nacoleon/esp/esp-idf"
. "$IDF_PATH/export.sh"
```

- Serial port: `/dev/cu.usbmodem5A8A0533623`
- Flash command: `idf.py -p /dev/cu.usbmodem5A8A0533623 app-flash`
- The SDK components are at `/tmp/SenseCAP-Watcher-Firmware/components/`
- The BSP patch (sensitivity = 50) is already applied in the /tmp copy
- Our pokewatcher source is at `/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher/main/`

## Important Gotchas

1. **Partition table mismatch:** The monitor example and pokewatcher have different partition tables. When switching between them, use `idf.py flash` (full flash) the first time to update the partition table, then `app-flash` for subsequent builds of the same project.

2. **SD card must be in the slot** for the test to be valid. The whole point is testing SPI2 bus sharing between SD card and Himax.

3. **The monitor example's `bsp_sdcard_init_default()` uses `ESP_ERROR_CHECK`** — it will crash if SD card isn't inserted. Our pokewatcher has graceful error handling. When adding code to the monitor, keep the ESP_ERROR_CHECK or add your own error handling.

4. **`bsp_spi_bus_init()` has a static guard** — it only initializes once. The first caller wins. Both `bsp_lvgl_init()` and `bsp_sscma_client_init()` call it internally.

5. **The Himax may be auto-running inference from a previous session.** After hardware reset, send `sscma_client_break()` and flush the RX buffer before sending AT commands. See `himax_task.c` for the flush logic.

6. **FREERTOS_HZ:** The monitor uses 1000Hz, our firmware defaults to 100Hz. This is an untested difference and could be relevant. If all 4 steps pass, try changing FREERTOS_HZ back to 100 as a test.

## Serial Monitor

Use this to check camera status after each flash:

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
            if any(k in line.lower() for k in ['himax', 'sscma', 'info', 'model', 'person', 'detection', 'invoke', 'running', 'connect', 'break', 'rx buffer', 'on_connect']):
                sys.stdout.write(line + '\n')
                sys.stdout.flush()
port.close()
"
```

**Camera working** = you see `on_connect` or `Person detection running`
**Camera broken** = you see `request model failed` or `request id failed` after 20s timeouts

## Deliverables

1. A results table showing which step broke the camera (or if all pass)
2. If a step fails, sub-test results narrowing it down further
3. Update `docs/knowledgebase/himax-camera-debugging.md` with a new "## Phase 8: Progressive Feature Addition" section containing your results
4. If you find the root cause and can fix it, implement the fix and verify camera + SD card + all features work together
5. Commit all changes with a descriptive commit message

## After the Investigation

Once you've identified the culprit:
- If it's the **renderer**: investigate DMA contention between SPI3 (LCD) and SPI2 (Himax/SD). Look at LVGL flush timing, SPI transaction queue depth, DMA channel allocation.
- If it's **WiFi**: investigate WiFi DMA buffer allocation vs SPI DMA memory pool.
- If it's a **combination**: document the exact combination and investigate shared resource contention.
- If **all steps pass** (unlikely): the issue is in the interaction between ALL features running simultaneously. Try the FREERTOS_HZ test and combination tests from the test plan.

After finding and fixing the issue:
1. Re-enable `pw_himax_task_start()` in `pokewatcher/main/app_main.c`
2. Verify person detection works with sprites + WiFi + heartbeat + everything
3. Update `docs/knowledgebase/himax-camera-debugging.md` with the fix
4. Update `docs/knowledgebase/project-status.md` to mark Himax as resolved
5. Commit and update CHANGELOG.md
