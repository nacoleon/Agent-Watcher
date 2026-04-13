# Zidane FFRK Sprite Catalog

Sprite sheet source: Final Fantasy Record Keeper — Zidane Tribal
File: `zidane-dashboard/zidane_spritesheet.png` (384x256 RGBA)
Left half = normal Zidane, right half = Trance (pink) — do not use Trance frames.

## Frame Reference

All coordinates are (x, y) from top-left. Standard frame size is 16x24 unless noted.

### Front-Facing (y=14) — Walking toward camera

| ID | Position | Description |
|----|----------|-------------|
| front1 | (1, 14) | Standing, arms at sides |
| front2 | (18, 14) | Standing, slight weight shift |
| front3 | (35, 14) | Walk cycle, left foot forward |
| front4 | (52, 14) | Walk cycle, right foot forward |
| front5 | (69, 14) | Standing, relaxed pose |

### Back-Facing (y=39) — Walking away from camera

| ID | Position | Description |
|----|----------|-------------|
| back1 | (1, 39) | Standing back |
| back2 | (18, 39) | Standing back, shift |
| back3 | (35, 39) | Walk cycle back, left foot |
| back4 | (52, 39) | Walk cycle back, right foot |
| back5 | (69, 39) | Standing back, relaxed |

### Side-Facing (y=64) — Walking sideways

| ID | Position | Status | Description |
|----|----------|--------|-------------|
| ~~side_bad1~~ | (1, 64) | WHITE/GHOST | Do not use |
| ~~side_bad2~~ | (18, 64) | WHITE/GHOST | Do not use |
| side1 | (35, 64) | OK | Side stand/walk frame 1 |
| side2 | (52, 64) | OK | Side stand/walk frame 2 |
| side3 | (69, 64) | OK | Side walk frame 3 |

### Side-Right Alternate (y=76)

| ID | Position | Description |
|----|----------|-------------|
| sideR1 | (101, 76) | Side-facing alternate 1 |
| sideR2 | (129, 76) | Side-facing alternate 2 |

### Action Row 1 (y=89) — Running/Dashing

| ID | Position | Status | Description |
|----|----------|--------|-------------|
| act1_1 | (1, 89) | OK | Dash/run frame 1 |
| act1_2 | (18, 89) | OK | Dash/run frame 2 |
| act1_3 | (35, 89) | OK | Dash/run frame 3 |
| ~~act1_4~~ | (52, 89) | WHITE | Do not use |

### Action Row 2 (y=101) — Combat Stance / Blade Draw

| ID | Position | Description |
|----|----------|-------------|
| act2_1 | (98, 101) | Blade draw / ready stance |
| act2_2 | (129, 101) | Combat ready |

### Action Row 3 (y=114) — Slashing / Attack

| ID | Position | Status | Description |
|----|----------|--------|-------------|
| act3_1 | (1, 114) | OK | Attack wind-up |
| act3_2 | (18, 114) | OK | Slash frame 1 |
| act3_3 | (35, 114) | OK | Slash frame 2 |
| act3_4 | (52, 114) | OK | Slash follow-through |
| ~~act3_5~~ | (69, 114) | WHITE | Do not use |

### Action Row 4 (y=126) — Petrify/Dark

| ID | Position | Status | Description |
|----|----------|--------|-------------|
| ~~act4_1~~ | (129, 126) | GRAY/DARK | Petrify effect — do not use |

### Action Row 5 (y=139) — Dynamic Running (side view)

| ID | Position | Description |
|----|----------|-------------|
| act5_1 | (1, 139) | Run side frame 1 |
| act5_2 | (18, 139) | Run side frame 2 |
| act5_3 | (35, 139) | Run side frame 3 |
| act5_4 | (52, 139) | Run side frame 4 |
| act5_5 | (69, 139) | Run side frame 5 |

### Action Row 6 (y=151-155) — Jumping / Aerial

| ID | Position | Description |
|----|----------|-------------|
| act6_1 | (86, 151) | Jump/aerial frame 1 |
| act6_2 | (103, 155) | Jump/aerial frame 2 |
| act6_3 | (129, 151) | Jump/aerial frame 3 |

### Action Row 7 (y=163-164) — Attack Follow-Through

| ID | Position | Description |
|----|----------|-------------|
| act7_1 | (1, 164) | Follow-through frame 1 |
| act7_2 | (18, 164) | Follow-through frame 2 |
| act7_3 | (35, 163) | Follow-through frame 3 |
| act7_4 | (52, 163) | Follow-through frame 4 |
| act7_5 | (69, 163) | Follow-through frame 5 |

### Action Row 8 (y=175-176) — Landing / Recovery

| ID | Position | Description |
|----|----------|-------------|
| act8_1 | (86, 175) | Landing frame |
| act8_2 | (129, 176) | Recovery stance |

### Special Poses (y=200+) — KO / Hurt / Victory

| ID | Position | Size | Description |
|----|----------|------|-------------|
| spc1 | (1, 200) | 15x24 | Eyes closed, slumped — **sleeping** |
| spc2 | (17, 200) | 15x23 | Eyes closed variant |
| spc3 | (50, 200) | 16x20 | Kneeling |
| spc4 | (129, 201) | 16x24 | Standing tired (Trance) |
| spc5 | (1, 225) | 15x23 | Fallen/collapsed |
| spc6 | (34, 221) | 16x20 | Kneeling variant 1 |
| spc7 | (51, 221) | 16x20 | Kneeling variant 2 |
| spc8 | (129, 226) | 16x24 | Victory/celebration |

### Small Sprites — Status Icons / Miniatures

Located at x=86-115, these are tiny chibi Zidane with status overlays. Not standalone icons.

| ID | Position | Size | Description |
|----|----------|------|-------------|
| small1 | (86, 14) | 14x16 | Mini Zidane with blue aura |
| small2 | (86, 31) | 14x16 | Mini Zidane with white bubble |
| small3 | (86, 48) | 14x16 | Mini Zidane petrify/gray |
| small4 | (86, 65) | 14x15 | Mini Zidane with white cloak |
| small5 | (86, 81) | 12x11 | Tiny Zidane |
| small6 | (86, 93) | 11x11 | Tiny Zidane variant |

### Top Row — Hair/Head Only

| ID | Position | Size | Description |
|----|----------|------|-------------|
| top1 | (51, 1) | 24x12 | Hair/head sprite front |
| top2 | (76, 1) | 24x12 | Hair/head sprite variant |

## Frames to Avoid

These are white/ghost/petrify sprites — zero saturation, high brightness:

- `(1, 64)` and `(18, 64)` — ghost side-facing
- `(52, 89)` — ghost action
- `(69, 114)` — ghost slash
- `(101, 51)`, `(129, 51)` — white standing
- `(129, 126)` — gray petrify
- `(1, 1)`, `(26, 1)`, `(146, 1)`, `(171, 1)` — white head sprites
- `(129, 2)` — white standing small

## Current State Mapping

| State | Frames | Notes |
|-------|--------|-------|
| IDLE | front1, front2 | Gentle sway |
| WORKING | act5_1, act5_2, act5_3 | Dynamic side running |
| WAITING | front1, front5 | Patient, still |
| ALERT | act2_1, act2_2, act3_1, act3_2 | Combat stance + "!" bubble |
| GREETING | front1, front3, front4, front2 | Bouncy approach + "!" bubble |
| SLEEPING | spc1, spc2 (15x24) | Eyes closed + code-drawn ZZZ |
| REPORTING | front1, front5 | Facing viewer, delivering info |

## Sprite Sheet Visual

See `zidane-dashboard/sprite_catalog.png` for a 6x scaled visual grid of all sprites.
See `zidane-dashboard/sleep_detail.png` for zoomed views of sleeping/icon candidates.
