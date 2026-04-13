# Building and Flashing PokéWatcher Firmware

Complete guide for building the firmware from source and flashing it to the SenseCAP Watcher. Documents every issue we hit and the workarounds required.

## Prerequisites

### ESP-IDF v5.2.1
```bash
mkdir -p ~/esp && cd ~/esp
git clone -b v5.2.1 --recursive https://github.com/espressif/esp-idf.git esp-idf
cd esp-idf && ./install.sh esp32s3
```

### SenseCAP Watcher SDK
The firmware depends on the Watcher BSP (board support package) for hardware drivers. Clone it to `/tmp/` — **not** inside the project directory (see "Space in Path" below).

```bash
git clone https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware /tmp/SenseCAP-Watcher-Firmware
```

The `pokewatcher/CMakeLists.txt` points to `/tmp/SenseCAP-Watcher-Firmware/components` via `EXTRA_COMPONENT_DIRS`.

### BSP Patch Required
The BSP has a Kconfig dependency issue when touch panel is disabled. Apply this patch to `/tmp/SenseCAP-Watcher-Firmware/components/sensecap-watcher/sensecap-watcher.c`:

Line 765 — change:
```c
.sensitivity = CONFIG_LVGL_INPUT_DEVICE_SENSITIVITY,
```
to:
```c
.sensitivity = 50,
```

This is needed because `CONFIG_LVGL_INPUT_DEVICE_SENSITIVITY` is only defined when `CONFIG_LVGL_INPUT_DEVICE_USE_TP=y`, but we disable the touch panel.

## The Space-in-Path Problem

The project lives at `/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/` — the space in `SenseCap Watcher` breaks the ESP-IDF linker. The linker interprets the path as two arguments at the space.

### Workaround
Build from a copy at a space-free path:

```bash
# Copy project to /tmp (exclude build artifacts)
rsync -a --exclude=build --exclude=.cache --exclude=sdkconfig \
  "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher/" \
  /tmp/pokewatcher-build/

# The SDK at the project root also causes issues — move it aside during builds
mv "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/SenseCAP-Watcher-Firmware" \
   "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/_SDK_BAK"
```

The IDF component manager resolves dependencies through the project-root SDK clone. If that clone has a space in its path, the linker script path (`rlottie.expmap`) breaks. Moving the project-root SDK aside forces resolution through `/tmp/SenseCAP-Watcher-Firmware` only.

**After building, restore it:**
```bash
mv "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/_SDK_BAK" \
   "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/SenseCAP-Watcher-Firmware"
```

### Syncing Source Changes
Before each build, copy modified source files to the build directory:
```bash
cp "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher/main/app_main.c" \
   /tmp/pokewatcher-build/main/app_main.c
# ... repeat for any changed files
```

Or rsync everything:
```bash
rsync -a --exclude=build --exclude=.cache --exclude=sdkconfig \
  "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher/" \
  /tmp/pokewatcher-build/
```

## Build Commands

```bash
# Source ESP-IDF environment (must do this in every new shell)
export IDF_PATH="/Users/nacoleon/esp/esp-idf"
. "$IDF_PATH/export.sh"

# Build
cd /tmp/pokewatcher-build
idf.py build

# Flash (app partition only — protects nvsfactory)
idf.py -p /dev/cu.usbmodem5A8A0533623 app-flash
```

### Full Rebuild (after sdkconfig.defaults changes)
When you change `sdkconfig.defaults`, the existing `sdkconfig` takes precedence and your changes are ignored. You must delete it:

```bash
rm -rf /tmp/pokewatcher-build/build /tmp/pokewatcher-build/sdkconfig
# Also delete managed_components and dependencies.lock if component issues
rm -rf /tmp/pokewatcher-build/managed_components /tmp/pokewatcher-build/dependencies.lock
idf.py build
```

**This is the #1 gotcha.** If you add a config like `CONFIG_LV_COLOR_16_SWAP=y` to `sdkconfig.defaults` but don't delete `sdkconfig`, the build silently uses the old value.

### One-Liner Build and Flash
```bash
bash -c 'export IDF_PATH="/Users/nacoleon/esp/esp-idf" && . "$IDF_PATH/export.sh" 2>/dev/null && cd /tmp/pokewatcher-build && idf.py build 2>&1 | tail -5 && idf.py -p /dev/cu.usbmodem5A8A0533623 app-flash 2>&1 | tail -5'
```

## Serial Port

The Watcher exposes two USB serial ports when connected via the bottom USB-C port:
- `/dev/cu.usbmodem5A8A0533621` — does NOT work for flashing
- `/dev/cu.usbmodem5A8A0533623` — use this one

The port numbers may change. Find them with:
```bash
ls /dev/cu.usbmodem*
```

### Serial Monitor
The ESP-IDF monitor tool requires a TTY and doesn't work well from non-interactive shells. Use raw serial reading instead:

```bash
python3 -c "
import serial, time, sys
port = serial.Serial('/dev/cu.usbmodem5A8A0533623', 115200, timeout=1)
# Reset device
port.setDTR(False)
port.setRTS(True)
time.sleep(0.1)
port.setRTS(False)
time.sleep(5)
# Read output
start = time.time()
while time.time() - start < 15:
    data = port.read(4096)
    if data:
        sys.stdout.write(data.decode('utf-8', errors='replace'))
        sys.stdout.flush()
port.close()
"
```

## First-Time Setup

### 1. Back Up nvsfactory Partition
**Do this BEFORE flashing anything.** Contains device EUI and SenseCraft credentials. Irreplaceable.

```bash
# Read the partition table first to find nvsfactory offset/size
esptool.py --chip esp32s3 -p /dev/cu.usbmodem5A8A0533623 \
  read_flash 0x8000 0x1000 /tmp/partition_table_dump.bin

python3 $IDF_PATH/components/partition_table/gen_esp32part.py /tmp/partition_table_dump.bin
# Look for nvsfactory — typically at 0x9000, size 0x32000 (200KB)

# Back up
esptool.py --chip esp32s3 -p /dev/cu.usbmodem5A8A0533623 \
  read_flash 0x9000 0x32000 backups/nvsfactory_backup.bin
```

Our device's partition table (stock firmware):
| Partition | Offset | Size |
|-----------|--------|------|
| nvsfactory | 0x9000 | 200KB |
| nvs | 0x3b000 | 840KB |
| otadata | 0x10d000 | 8KB |
| phy_init | 0x10f000 | 4KB |
| ota_0 | 0x110000 | 12MB |
| ota_1 | 0xd10000 | 12MB |
| model | 0x1910000 | 1MB |
| storage | 0x1a10000 | 6MB |

### 2. Flash with app-flash
Always use `app-flash` instead of `flash` — it only writes the app partition and leaves nvsfactory, NVS, and partition table untouched.

```bash
idf.py -p /dev/cu.usbmodem5A8A0533623 app-flash
```

### 3. Restoring Stock Firmware
Re-flash the original SenseCAP Watcher firmware from their GitHub repo. The nvsfactory partition is preserved by `app-flash`.

## Hardware Init Sequence

The boot order matters. Here's what we learned through trial and error:

### What the BSP Initializes
- `bsp_io_expander_init()` — PCA95xx I2C IO expander, controls power to LCD, SD card, AI chip
- `bsp_spi_bus_init()` — SPI2 bus (shared by SD card + Himax AI chip) and SPI3/QSPI bus (LCD)
- `bsp_lvgl_init()` — Full display pipeline: SPI bus, LCD panel, backlight, LVGL port, knob input

### Correct Init Order
```
1. NVS flash init
2. Event queue
3. Roster (from NVS)
4. Mood engine
5. LLM engine
6. IO expander init + 100ms delay (powers LCD)
7. Renderer (calls bsp_lvgl_init — inits SPI, LCD, backlight, LVGL)
8. SD card mount (SPI bus already set up by step 7)
9. Load active Pokemon sprites from SD
10. WiFi + web server
11. Start tasks (Himax, mood engine, renderer, LLM)
```

### Critical Lessons

**IO expander must init before display.** The IO expander controls `BSP_PWR_LCD`. If the LCD has no power when `bsp_lvgl_init()` sends SPI commands to the panel controller, those commands are lost and the display stays black.

**Do NOT call `bsp_spi_bus_init()` before `bsp_lvgl_init()`.** The BSP's SPI init has an idempotent guard (`static bool initialized`). If you call it early, it runs once. When `bsp_lvgl_init()` calls it internally, it skips (already initialized). But the internal call happens at a specific point in the LCD init sequence — calling it early can break the ordering.

**SD card init must come AFTER `bsp_lvgl_init()`.** The SPI2 bus is shared between the SD card and the Himax AI chip. `bsp_lvgl_init()` sets up the SPI bus correctly; the SD card mount adds its device to the existing bus.

**Touch panel must be disabled.** The SPD2010 touch controller init (`bsp_touch_indev_init`) uses `ESP_ERROR_CHECK` which aborts on failure. If the I2C communication fails (which it does on some units), the device boot-loops. Disabled via `CONFIG_LVGL_INPUT_DEVICE_USE_TP=n`.

## SD Card

### Hardware
- microSD slot, SPI mode (not SDMMC)
- SPI2 bus: GPIO 4 (CLK), GPIO 5 (MOSI), GPIO 6 (MISO), GPIO 46 (CS)
- Power controlled by IO expander pin 8 (`BSP_PWR_SDCARD`)

### Why We Don't Use `bsp_sdcard_init_default()`
The BSP function uses `ESP_ERROR_CHECK` on `esp_vfs_fat_sdspi_mount()`. If no SD card is inserted, it aborts and boot-loops. We replicate the same SPI mount logic but with a graceful error return.

### SD Card Detection Pin
`bsp_sdcard_is_inserted()` checks IO expander pin 4, but it's unreliable — returns false even with a card inserted. We skip the check and just try to mount.

### Format Requirements
- **FAT32** with **4096-byte allocation units**
- Windows may format small cards (256MB) in a way ESP32 can't read (`FR_NO_FILESYSTEM` error 13). If this happens, set `format_if_mount_failed = true` once to let ESP32 reformat, then copy files back.
- Enable `CONFIG_FATFS_LFN_HEAP=y` and `CONFIG_FATFS_CASE_INSENSITIVE=y` — Windows creates uppercase 8.3 filenames, but our code uses lowercase paths.

### Slow SPI Clock for Old Cards
256MB SD v1 cards need `host.max_freq_khz = 400` (400kHz). Default is too fast and causes timeouts. Larger SDHC cards work at higher speeds.

### 500ms Power-Up Delay
Add `vTaskDelay(pdMS_TO_TICKS(500))` before mounting. The IO expander enables power to the SD slot, but the card needs time to stabilize before SPI communication starts.

## sdkconfig.defaults Reference

```ini
# Target
CONFIG_IDF_TARGET="esp32s3"

# Flash
CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y

# PSRAM
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y

# CPU
CONFIG_ESP_DEFAULT_CPU_FREQ_240=y

# Partitions (uses stock Watcher layout)
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"

# Display
CONFIG_LV_COLOR_DEPTH_16=y
CONFIG_LV_COLOR_16_SWAP=y                    # Required for SPD2010 byte order
CONFIG_LV_HOR_RES_MAX=412
CONFIG_LV_VER_RES_MAX=412
CONFIG_LV_COLOR_CHROMA_KEY_HEX=0xFF00FF      # Magenta (not used with TRUE_COLOR_ALPHA, but set correctly anyway)

# Input — touch panel DISABLED (aborts on I2C failure)
CONFIG_LVGL_INPUT_DEVICE_USE_TP=n

# SD card filesystem
CONFIG_FATFS_LFN_HEAP=y                      # Long filename support
CONFIG_FATFS_MAX_LFN=255
CONFIG_FATFS_API_ENCODING_UTF_8=y
CONFIG_FATFS_CASE_INSENSITIVE=y              # Windows formats as uppercase

# Network
CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024
CONFIG_HTTPD_MAX_URI_LEN=512
CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y
CONFIG_LWIP_MAX_SOCKETS=16
```

## Display Transparency

### The Problem We Solved
Sprites rendered with a colored rectangle behind them instead of being transparent.

### Root Causes (multiple, all needed fixing)
1. **Wrong image format:** `LV_IMG_CF_TRUE_COLOR` renders every pixel opaque. Must use `LV_IMG_CF_TRUE_COLOR_ALPHA` (3 bytes per pixel: color_low, color_high, alpha).

2. **LVGL widget background:** `lv_img` objects inherit an opaque background from the default theme. Call `lv_obj_remove_style_all(sprite_img)` after creation.

3. **Image descriptor header:** `always_zero` and `reserved` fields must be explicitly zeroed. Uninitialized bits cause LVGL to misidentify the image source and skip alpha processing.

4. **Image cache:** Since the frame descriptor pointer is reused every frame, LVGL's image cache serves stale data. Call `lv_img_cache_invalidate_src(&frame_dsc)` before `lv_img_set_src()`.

5. **Source .raw files:** The RGB565 files were converted from PNGs that still had the green background (before transparency was added). The magenta color key `0xF81F` was never present. **Always remove the PNG background BEFORE converting to .raw.**

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| Boot loops at step 2 (SD card) | `bsp_sdcard_init_default()` aborts on missing card | Use manual SPI mount with error handling |
| Boot loops at step 7 (renderer) | Touch panel I2C init aborts | `CONFIG_LVGL_INPUT_DEVICE_USE_TP=n` |
| Display stays black | IO expander not initialized before LCD | Call `bsp_io_expander_init()` + delay before `pw_renderer_init()` |
| Display stays black | `bsp_spi_bus_init()` called before `bsp_lvgl_init()` | Remove early SPI init, let BSP handle it |
| SD card timeout (0x107) | Card not inserted, or needs power-up delay | Add 500ms delay, check card is seated |
| SD card invalid response (0x108) | Card not inserted (CMD8 fails with no card) | Insert the card |
| SD card no filesystem (error 13) | Windows FAT32 format incompatible | Let ESP32 reformat with `format_if_mount_failed=true` |
| `Cannot open pokemon.json` | Uppercase filenames on FAT32 | `CONFIG_FATFS_CASE_INSENSITIVE=y` |
| Linker error "cannot open SenseCap" | Space in project path breaks linker | Build from `/tmp/pokewatcher-build/`, move project-root SDK aside |
| sdkconfig changes ignored | Old `sdkconfig` takes precedence | Delete `sdkconfig` before rebuild |
| Green/colored rectangle around sprite | `.raw` converted before PNG background removed | Remove PNG background, reconvert `.raw` |
| Config build error `CONFIG_LVGL_INPUT_DEVICE_SENSITIVITY` | Kconfig dependency when touch disabled | Hardcode `.sensitivity = 50` in BSP source |
