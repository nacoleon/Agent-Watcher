# Creating Pokemon Sprites for PokéWatcher

Complete guide for adding new Pokemon to the PokéWatcher firmware. Covers the full pipeline from downloading sprites to seeing them on the Watcher's display.

## Overview

Each Pokemon needs 4 files in `pokewatcher/sdcard/pokemon/<name>/`:

| File | Purpose |
|------|---------|
| `pokemon.json` | Identity, name, evolution chain |
| `frames.json` | Animation definitions, frame coordinates |
| `overworld.png` | Transparent PNG sprite sheet (for web preview) |
| `overworld.raw` | RGB565 binary sprite sheet (for ESP32 hardware) |

## Step 1: Download Sprite Sheet

### Source
Pokemon HeartGold/SoulSilver overworld sprites from [The Spriters Resource](https://www.spriters-resource.com/ds_dsi/pokemonheartgoldsoulsilver/).

### What You Get
A compilation sheet with all Pokemon. Each Pokemon occupies a cell with:
- 4 rows: down, up, left, right facing directions
- 2-3 columns: walk cycle frames per direction
- ~32x32 pixels per frame
- Solid colored background (NOT transparent)

### Extracting Individual Pokemon
Crop each Pokemon's cell from the compilation sheet. The final sprite sheet should be:
- **96x128 pixels** (3 columns x 4 rows of 32x32 frames)
- If the source only has 2 unique frames per direction, duplicate frame 0 as frame 2 (stand, step, stand)

## Step 2: Remove Background Color

**This is critical.** The source sprites have a solid background color (typically green `rgb(160, 176, 128)`) that must be made transparent. If you skip this step, the Pokemon will have a colored rectangle around it on both the web preview AND the hardware display.

### Python Script
```python
from PIL import Image

img = Image.open('overworld.png').convert('RGBA')

# Sample background color from top-left pixel (always background)
bg = img.getpixel((0, 0))

# Make all matching pixels transparent
data = img.getdata()
new_data = []
for p in data:
    if p[:3] == bg[:3]:
        new_data.append((0, 0, 0, 0))  # Fully transparent
    else:
        new_data.append(p)

img.putdata(new_data)
img.save('overworld.png')
```

### Check for Artifacts
Some sprites have stray pixels from the extraction process (e.g., bright blue `(0, 128, 255)` pixels in corners). Remove them:

```python
from PIL import Image

img = Image.open('overworld.png').convert('RGBA')
data = list(img.getdata())
for i, p in enumerate(data):
    if p[3] > 0 and p[0] == 0 and p[1] == 128 and p[2] == 255:
        data[i] = (0, 0, 0, 0)
img.putdata(data)
img.save('overworld.png')
```

### Verify Transparency
```python
from PIL import Image
img = Image.open('overworld.png')
alphas = set(p[3] for p in img.getdata())
print(f"Alpha values: {sorted(alphas)}")
# Should show [0, 255] — 0 for transparent, 255 for opaque
# If you see only [255], the background removal failed
```

## Step 3: Convert PNG to RGB565 (.raw)

**Order matters: remove the background from the PNG FIRST, then convert to .raw.** The converter maps transparent pixels to magenta `0xF81F`, which is the color key the ESP32 renderer uses for alpha transparency.

### Using the Converter
```bash
python3 pokewatcher/tools/convert_sprites.py overworld.png overworld.raw
```

### What the Converter Does
1. Reads the PNG with alpha channel
2. For each pixel:
   - If alpha = 0 (transparent): writes magenta `0xF81F` in RGB565
   - If alpha > 0 (opaque): converts RGB888 to RGB565
3. Output format: 4-byte header (uint16 width, uint16 height, little-endian) + pixel data

### Verify the .raw File
```python
import struct
with open('overworld.raw', 'rb') as f:
    w, h = struct.unpack('<HH', f.read(4))
    data = f.read()
    pixels = struct.unpack(f'<{w*h}H', data)
    magenta = sum(1 for p in pixels if p == 0xF81F)
    print(f"Size: {w}x{h}")
    print(f"Magenta (transparent) pixels: {magenta} ({100*magenta/len(pixels):.1f}%)")
    # Should be 70-85% for typical overworld sprites
```

## Step 4: Create pokemon.json

```json
{
  "id": "pikachu",
  "name": "Pikachu",
  "sprite_sheet": "overworld.png",
  "frame_manifest": "frames.json",
  "evolves_to": "raichu",
  "evolution_hours": 24
}
```

| Field | Required | Description |
|-------|----------|-------------|
| `id` | Yes | Lowercase, alphanumeric + underscore/hyphen only |
| `name` | Yes | Display name |
| `sprite_sheet` | Yes | Always `"overworld.png"` |
| `frame_manifest` | Yes | Always `"frames.json"` |
| `evolves_to` | No | ID of evolution target (omit for final forms) |
| `evolution_hours` | No | Hours as active companion before evolution (default: 24) |

## Step 5: Create frames.json

```json
{
  "frame_width": 32,
  "frame_height": 32,
  "animations": {
    "idle_down":    { "frames": [{ "x": 0,  "y": 0 }], "loop": true },
    "idle_up":      { "frames": [{ "x": 0,  "y": 32 }], "loop": true },
    "idle_left":    { "frames": [{ "x": 0,  "y": 64 }], "loop": true },
    "idle_right":   { "frames": [{ "x": 0,  "y": 96 }], "loop": true },
    "walk_down":    { "frames": [{ "x": 0,  "y": 0 }, { "x": 32, "y": 0 }, { "x": 64, "y": 0 }], "loop": true },
    "walk_up":      { "frames": [{ "x": 0,  "y": 32 }, { "x": 32, "y": 32 }, { "x": 64, "y": 32 }], "loop": true },
    "walk_left":    { "frames": [{ "x": 0,  "y": 64 }, { "x": 32, "y": 64 }, { "x": 64, "y": 64 }], "loop": true },
    "walk_right":   { "frames": [{ "x": 0,  "y": 96 }, { "x": 32, "y": 96 }, { "x": 64, "y": 96 }], "loop": true }
  },
  "mood_animations": {
    "excited":   "walk_down",
    "happy":     "idle_down",
    "curious":   "idle_left",
    "lonely":    "idle_down",
    "sleepy":    "idle_down",
    "overjoyed": "walk_down"
  }
}
```

### Sprite Sheet Layout (96x128)
```
        Col 0      Col 1      Col 2
        x=0        x=32       x=64
Row 0   stand      step       stand     y=0   (facing down)
Row 1   stand      step       stand     y=32  (facing up)
Row 2   stand      step       stand     y=64  (facing left)
Row 3   stand      step       stand     y=96  (facing right)
```

### Frame Coordinates
- `x` and `y` are the top-left pixel of the frame within the sprite sheet
- Each frame is `frame_width` x `frame_height` pixels (always 32x32 for HGSS)
- Frame 0 and frame 2 are typically identical (standing pose)
- Frame 1 is the step pose

## Step 6: Copy to SD Card

### SD Card Requirements
- **Format:** FAT32 with 4096-byte allocation units
- **Max size:** 32GB (ESP32 fatfs limitation)
- **Filesystem:** Must be formatted in a way ESP32 can read — if Windows formats it and ESP32 can't mount, use `format_if_mount_failed = true` in the firmware once to let ESP32 reformat, then copy files back
- **Case sensitivity:** The firmware uses `CONFIG_FATFS_CASE_INSENSITIVE=y`, so filenames can be uppercase or lowercase

### Directory Structure on SD Card
```
/pokemon/
  pikachu/
    pokemon.json
    frames.json
    overworld.raw
  charmander/
    pokemon.json
    frames.json
    overworld.raw
  ...
```

The `.png` files are NOT needed on the SD card (only for the web preview server). Only `.json` and `.raw` files are read by the firmware.

## How Transparency Works on the Hardware

### The Pipeline
1. `.raw` file has transparent pixels stored as magenta `0xF81F` (RGB565)
2. `sprite_loader.c` reads the raw data and extracts a frame at 4x scale
3. For each pixel, it checks `if (px == 0xF81F)`:
   - Transparent: writes 3 bytes `[0x00, 0x00, 0x00]` (color=black, alpha=0)
   - Opaque: writes 3 bytes `[color_low, color_high, 0xFF]` (byte-swapped RGB565, alpha=255)
4. `renderer.c` sets `s_frame_dsc.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA`
5. LVGL reads the alpha byte and skips transparent pixels, showing the background through

### Why LV_IMG_CF_TRUE_COLOR_ALPHA (not CHROMA_KEYED)
We tried `LV_IMG_CF_TRUE_COLOR_CHROMA_KEYED` first but it failed due to byte-order mismatches between `LV_COLOR_16_SWAP` and the chroma key comparison. `TRUE_COLOR_ALPHA` with an explicit alpha byte per pixel is more reliable — no color matching needed, just alpha=0 for transparent.

### Why LV_COLOR_16_SWAP Matters
The SPD2010 display expects bytes in big-endian order, but ESP32 is little-endian. `CONFIG_LV_COLOR_16_SWAP=y` tells LVGL to swap bytes. The sprite loader must also swap: `(px >> 8) | (px << 8)`.

### LVGL Widget Background Gotcha
`lv_img` objects inherit a default background style from the LVGL theme. This shows as an opaque rectangle behind the image even when alpha pixels are correct. Fix: call `lv_obj_remove_style_all(sprite_img)` after creating the image widget.

## Common Mistakes

| Mistake | Symptom | Fix |
|---------|---------|-----|
| Convert PNG to .raw BEFORE removing background | Light green rectangle around sprite | Remove background from PNG first, then reconvert |
| Green background not fully removed from PNG | Faint green border around sprite | Check top-left pixel color, use it as background key |
| Stray blue pixels in sprite | Blue dots visible at 4x scale | Scan for `(0, 128, 255)` and remove |
| .raw file has wrong transparency color | Colored rectangle on hardware | Verify .raw has 0xF81F for transparent pixels |
| sdkconfig not regenerated after changing defaults | Config changes don't take effect | Delete `sdkconfig` and rebuild |
| SD card formatted by Windows with wrong FAT type | `FR_NO_FILESYSTEM` error | Format FAT32 with 4096-byte allocation units |
| Filenames uppercase on FAT32 | `Cannot open pokemon.json` | Enable `CONFIG_FATFS_CASE_INSENSITIVE=y` |

## Quick Add Checklist

1. Download/extract 96x128 sprite sheet PNG
2. Remove background color → transparent
3. Remove any artifact pixels (blue, etc.)
4. Verify PNG has alpha channel with 0 and 255 values
5. Convert PNG to .raw: `python3 tools/convert_sprites.py input.png output.raw`
6. Verify .raw has ~80% magenta (0xF81F) pixels
7. Create `pokemon.json` with ID, name, evolution info
8. Create `frames.json` with frame coordinates and mood mappings
9. Copy folder to SD card under `/pokemon/<name>/`
10. Reboot Watcher — add via web dashboard or auto-add on empty roster
