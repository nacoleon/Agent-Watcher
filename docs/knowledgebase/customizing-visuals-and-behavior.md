# Customizing Visuals and Behavior

How to change backgrounds, swap Pokemon, adjust movement speed and patterns, and tune the mood system. All changes are in the firmware C code unless noted otherwise.

## Background Colors

### Where
`pokewatcher/main/renderer.c` — the `MOOD_BG_COLORS` array:

```c
static const uint32_t MOOD_BG_COLORS[] = {
    [PW_MOOD_EXCITED]   = 0xFDE8C8,   // warm gold
    [PW_MOOD_HAPPY]     = 0xFDE8C8,   // warm gold
    [PW_MOOD_CURIOUS]   = 0xE8F0E8,   // soft green
    [PW_MOOD_LONELY]    = 0xC8D8F0,   // cool blue
    [PW_MOOD_SLEEPY]    = 0x404060,   // dark purple
    [PW_MOOD_OVERJOYED] = 0xFFF0C0,   // bright yellow
};
```

### How to Change
Values are 24-bit hex RGB (same format as CSS). Replace any value:
- `0xFF0000` = red
- `0x00FF00` = green
- `0x000000` = black
- `0xFFFFFF` = white

The background fills the entire circular display. LVGL converts the 24-bit color to RGB565 internally.

### Web Preview
The web preview has its own background colors in `pokewatcher/main/web/index.html` — search for `bgColors` in the JavaScript. Update both to keep them in sync.

## Switching the Active Pokemon

### On First Boot (No Roster)
`pokewatcher/main/app_main.c` auto-adds Pikachu if the roster is empty:

```c
if (!active) {
    if (pw_roster_add("pikachu")) {
        active = pw_roster_get_active_id();
    }
}
```

Change `"pikachu"` to any Pokemon ID that has files on the SD card (e.g., `"charmander"`, `"eevee"`).

### Available Pokemon
These are on the SD card and ready to use:
`pikachu`, `raichu`, `charmander`, `charmeleon`, `bulbasaur`, `ivysaur`, `squirtle`, `wartortle`, `eevee`

### Clearing the Roster (Force Re-Add)
The roster is stored in NVS. To reset it, erase the NVS partition or add this before the auto-add:

```c
// Temporary: clear roster to switch Pokemon
nvs_handle_t h;
if (nvs_open("pokewatcher", NVS_READWRITE, &h) == ESP_OK) {
    nvs_erase_key(h, "roster");
    nvs_commit(h);
    nvs_close(h);
}
pw_roster_init();  // Reload (now empty)
```

Remove this code after the first boot with the new Pokemon.

### Via Web Dashboard (Future)
Once WiFi is configured, the web dashboard at `pokewatcher.local/roster` allows adding, removing, and switching Pokemon without firmware changes.

## Movement Behavior

### Where
`pokewatcher/main/renderer.c` — the `MOOD_BEHAVIORS` array controls how the Pokemon moves for each mood:

```c
static const mood_behavior_t MOOD_BEHAVIORS[] = {
    [PW_MOOD_EXCITED]   = { .walk_chance = 60, .turn_chance = 40, .walk_steps_min = 8,  .walk_steps_max = 20, .speed_x10 = 25, .bounce_amp = 3, .pause_min = 10,  .pause_max = 30  },
    [PW_MOOD_HAPPY]     = { .walk_chance = 30, .turn_chance = 20, .walk_steps_min = 6,  .walk_steps_max = 14, .speed_x10 = 15, .bounce_amp = 0, .pause_min = 30,  .pause_max = 80  },
    [PW_MOOD_CURIOUS]   = { .walk_chance = 20, .turn_chance = 60, .walk_steps_min = 4,  .walk_steps_max = 10, .speed_x10 = 10, .bounce_amp = 0, .pause_min = 20,  .pause_max = 50  },
    [PW_MOOD_LONELY]    = { .walk_chance = 10, .turn_chance = 10, .walk_steps_min = 3,  .walk_steps_max = 8,  .speed_x10 = 8,  .bounce_amp = 0, .pause_min = 60,  .pause_max = 150 },
    [PW_MOOD_SLEEPY]    = { .walk_chance = 3,  .turn_chance = 5,  .walk_steps_min = 2,  .walk_steps_max = 5,  .speed_x10 = 5,  .bounce_amp = 0, .pause_min = 100, .pause_max = 250 },
    [PW_MOOD_OVERJOYED] = { .walk_chance = 80, .turn_chance = 50, .walk_steps_min = 10, .walk_steps_max = 25, .speed_x10 = 30, .bounce_amp = 4, .pause_min = 5,   .pause_max = 15  },
};
```

### Parameter Reference

| Parameter | Unit | Description |
|-----------|------|-------------|
| `walk_chance` | per 1000 per tick | Probability of starting a walk each tick (10 ticks/sec). 60 = 6% per tick. |
| `turn_chance` | per 1000 per tick | Probability of turning to face a random direction. Checked before walk_chance. |
| `walk_steps_min` | ticks | Minimum walk duration. At 10 FPS, 8 ticks = 0.8 seconds. |
| `walk_steps_max` | ticks | Maximum walk duration. Random value between min and max. |
| `speed_x10` | pixels * 10 per tick | Movement speed. 25 = 2.5 pixels per tick. Higher = faster. |
| `bounce_amp` | pixels | Vertical bounce amplitude for excited moods. 0 = no bounce. |
| `pause_min` | ticks | Minimum pause after walking or turning. |
| `pause_max` | ticks | Maximum pause. Random value between min and max. |

### Tuning Tips

**Make Pokemon move more often:** Increase `walk_chance` and decrease `pause_min`/`pause_max`.

**Make Pokemon move faster:** Increase `speed_x10`. Values above 40 look unnaturally fast.

**Make Pokemon fidgety (look around a lot):** Increase `turn_chance`, decrease `walk_chance`.

**Make Pokemon mostly still:** Set `walk_chance` to 1-5, `turn_chance` to 1-5, `pause_min`/`pause_max` to 200+.

**Add bounce to a mood:** Set `bounce_amp` to 2-5. The bounce uses `sinf()` for smooth up/down motion.

### The State Machine
Each tick (10 per second), the Pokemon is in one of three states:

```
IDLE → (roll walk_chance) → WALKING → (steps run out or hits edge) → PAUSING → (timer expires) → IDLE
  ↓
  (roll turn_chance) → face random direction → PAUSING
```

- **IDLE:** Standing still, checking if it should walk or turn
- **WALKING:** Moving in `s_facing` direction at `speed_x10` rate, counting down `walk_steps_left`
- **PAUSING:** Standing still for `pause_target` ticks (set once when entering state)

### Movement Boundaries
The Pokemon stays within a circular area defined by:

```c
#define MOVE_RADIUS (CENTER_X - SPRITE_HALF - 10)
```

This keeps the sprite fully inside the round display with 10px padding. The `clamp_pos()` function enforces a circular boundary (not a square), and `pick_walk_dir()` biases toward the center when the Pokemon is far from it (60% chance to walk toward center when distance > 40px).

## Sprite Scale

### Where
`pokewatcher/main/config.h`:

```c
#define PW_SPRITE_SCALE       4          // 32px * 4 = 128px on screen
#define PW_SPRITE_SRC_SIZE    32         // source frame size
#define PW_SPRITE_DST_SIZE    (PW_SPRITE_SRC_SIZE * PW_SPRITE_SCALE)  // 128
```

### Changing Scale
- **Scale 3** (96px): smaller Pokemon, more room to walk
- **Scale 4** (128px): current default, good balance
- **Scale 5** (160px): big Pokemon, less walking room
- **Scale 6** (192px): fills most of the 412px display

Larger scale = more PSRAM per frame (scale^2 * 32^2 * 3 bytes). Scale 4 = 49KB per frame. Scale 6 = 110KB.

The web preview uses `DRAW_SCALE = 4` in `index.html` — update both to match.

## Animation Speed

### Frame Rate
`pokewatcher/main/config.h`:
```c
#define PW_ANIM_FPS           10
```

This is the renderer task tick rate. The behavior state machine runs once per tick.

### Sprite Frame Cycle
`pokewatcher/main/renderer.c` in `update_frame()`:
```c
if (s_frame_tick % 3 == 0) {
    s_current_frame++;
```

The sprite frame advances every 3 ticks = 300ms per frame at 10 FPS. With 3 walk frames (stand, step, stand), one full walk cycle = 900ms.

- Change `3` to `2` for faster animation (200ms per frame, snappier walk)
- Change `3` to `5` for slower animation (500ms per frame, lazy waddle)

## Mood Timers

### Where
`pokewatcher/main/config.h`:

```c
#define PW_EXCITED_DURATION_MS     10000   // 10 seconds in EXCITED before → HAPPY
#define PW_OVERJOYED_DURATION_MS   15000   // 15 seconds in OVERJOYED before → HAPPY
#define PW_CURIOUS_TIMEOUT_MS      300000  // 5 minutes without person → LONELY
#define PW_LONELY_TIMEOUT_MS       900000  // 15 minutes without person → SLEEPY
```

These are defaults — the web dashboard settings page can override them at runtime (saved to NVS).

### Mood Flow
```
Person detected (first time) → EXCITED (10s) → HAPPY
Person leaves → CURIOUS (5min) → LONELY (15min) → SLEEPY
Person returns from SLEEPY/LONELY → OVERJOYED (15s) → HAPPY
Person returns from CURIOUS → HAPPY (short absence, skip overjoyed)
```

### Testing a Specific Mood
To force a mood for testing, edit `pokewatcher/main/mood_engine.c`:

```c
s_state = (pw_mood_state_t){
    .current_mood = PW_MOOD_EXCITED,   // Change this
    .previous_mood = PW_MOOD_EXCITED,  // Match this
    ...
};
```

And match it in `pokewatcher/main/renderer.c`:
```c
static pw_mood_t s_current_mood = PW_MOOD_EXCITED;  // Match mood_engine
```

**Remember to revert both to `PW_MOOD_SLEEPY` for production.**

## Adding a Background Image (Future)

Currently the background is a solid color. The design spec calls for a pixel art environment (grass, flowers, rocks). To add one:

1. Create a 412x412 pixel art background as PNG
2. Convert to RGB565 .raw (same as sprites but no transparency needed)
3. Load in renderer as a static `lv_img` behind the sprite
4. Swap background image based on mood (or tint the same image)

The `s_screen` object in `renderer.c` already has `lv_obj_set_style_bg_opa(LV_OPA_COVER)` — to show a background image instead of solid color, create an `lv_img` child of `s_screen` and position it at (0,0) before the sprite image.

## Web Preview Sync

The web preview (`pokewatcher/tools/preview_server.py` on port 8090) has its own copy of the behavior parameters in `pokewatcher/main/web/index.html`. If you change movement behavior on the firmware side, update the JavaScript `MOOD_BEHAVIOR` object to match:

```javascript
const MOOD_BEHAVIOR = {
    excited:   { walkChance: 0.06, turnChance: 0.04, walkSteps: [8,20],  speed: 2.5, bounceAmp: 3, pauseTicks: [10,30] },
    happy:     { walkChance: 0.03, turnChance: 0.02, walkSteps: [6,14],  speed: 1.5, bounceAmp: 0, pauseTicks: [30,80] },
    // ...
};
```

The JS values are floats (0.06 = 60/1000), and speed is in canvas pixels (not x10 fixed point).

## Quick Change Recipes

### "I want Pikachu to move around more"
In `renderer.c`, change EXCITED behavior:
```c
[PW_MOOD_EXCITED] = { .walk_chance = 100, .turn_chance = 30, .walk_steps_min = 10, .walk_steps_max = 25, .speed_x10 = 30, .bounce_amp = 3, .pause_min = 5, .pause_max = 15 },
```

### "I want a calmer, slower pet"
```c
[PW_MOOD_HAPPY] = { .walk_chance = 15, .turn_chance = 10, .walk_steps_min = 4, .walk_steps_max = 8, .speed_x10 = 8, .bounce_amp = 0, .pause_min = 60, .pause_max = 120 },
```

### "I want a darker/night theme"
```c
static const uint32_t MOOD_BG_COLORS[] = {
    [PW_MOOD_EXCITED]   = 0x2A1A0A,
    [PW_MOOD_HAPPY]     = 0x1A1A2E,
    [PW_MOOD_CURIOUS]   = 0x1A2A1A,
    [PW_MOOD_LONELY]    = 0x0A1A2A,
    [PW_MOOD_SLEEPY]    = 0x0A0A15,
    [PW_MOOD_OVERJOYED] = 0x2A2A0A,
};
```

### "I want to switch to Charmander"
In `app_main.c`, change the auto-add:
```c
if (pw_roster_add("charmander")) {
```
Then erase the NVS roster key (see "Clearing the Roster" above) so it picks up the change on next boot.

### "I want the Pokemon bigger on screen"
In `config.h`:
```c
#define PW_SPRITE_SCALE  5   // 160px instead of 128px
```
Note: `MOVE_RADIUS` adjusts automatically since it's derived from `SPRITE_HALF`.
