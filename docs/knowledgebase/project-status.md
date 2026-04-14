# Project Status — Zidane Watcher

Last updated: 2026-04-13

## What's Done

### Firmware (pokewatcher/)
- Agent state engine: 7 states (idle, working, waiting, alert, greeting, sleeping, reporting)
- FF9 dialog box renderer (LVGL, grainy gray texture)
- API endpoints: GET /api/status, PUT /api/agent-state, POST /api/message, POST /api/reboot
- Sprite system using FFRK Zidane (16x24 at 4x scale)
- Web UI at http://10.0.0.40 with state controls, message input, reboot button
- WiFi auto-connect (YOUR_WIFI_SSID, IP: 10.0.0.40, mDNS: zidane.local)
- Person detection via Himax camera
- Display sleep after 5 min idle, wake on API calls
- SPI-safe renderer: prepare/commit split, single LVGL lock per frame, dirty flags

### Bridge (bridge/)
- Node.js Express on port 3847, runs as LaunchAgent
- Tool endpoints: /tools/display_message, /tools/set_state, /tools/get_status, /tools/notify
- Presence poller (5s, debounced) writes to ~/.openclaw/watcher-events.json
- Context file at ~/.openclaw/watcher-context.json

### OpenClaw Integration
- watcher.* tools registered in ~/clawd/TOOLS.md
- Desk context in ~/clawd/HEARTBEAT.md
- LaunchAgent: ai.openclaw.watcher-bridge

### Dashboard Preview (zidane-dashboard/)
- Runs on localhost:8091 (python3 zidane-dashboard/server.py)
- FFRK Zidane sprite with all animations
- 84 Pictlogica FF backgrounds (12 per state, curated)
- FF9 dialog box with grainy gray, speech tail, dynamic height
- Matches Watcher display at 412x412

### SD Card (sdcard_prep/characters/zidane/)
- overworld.raw — sprite sheet (RGB565)
- frames.json — 14 animations
- bg/ — 72 background tiles (.raw, unused by firmware currently)

## What Needs To Be Done

### Immediate
- [ ] **Revert IDLE bg color** — Change `0xFF0000` (test red) back to `0xFDE8C8` (warm peach) in renderer.c STATE_BG_COLORS
- [ ] **Test state changes from web UI** — Verify bg colors change for all 7 states without freeze
- [ ] **End-to-end bridge test** — Verify Zidane agent can call bridge tools and Watcher responds

### Background Images on Firmware
- [ ] Background tile loading is disabled (#if 0 in renderer.c) because SD card reads at 400kHz cause SPI collision with Himax
- [ ] **Option A**: Pre-cache backgrounds in PSRAM at boot (before Himax starts). ~340KB per scaled tile, could cache 1 per state = 2.4MB of 8MB PSRAM
- [ ] **Option B**: Increase SD card SPI clock (needs faster card, currently 400kHz for old 256MB card)
- [ ] **Option C**: Dedicated background task with SPI mutex arbitration

### Sprite Improvements
- [ ] Custom Zidane pixel art (currently using FFRK rips from Spriters Resource)
- [ ] Portrait sprite for dialog box (spec calls for face next to text)
- [ ] More animation variety (current side-facing reuses same 2 frames)

### Firmware Stability
- [ ] **SPI flush intermittent failure** — `spi_device_queue_trans` returns `ESP_ERR_INVALID_STATE` (0x101) intermittently on large dirty regions. Happens on ~60-80% of boots when background image is displayed. Error means "polling transaction not terminated." Diagnostic logging added to SPD2010 driver. Tested: Himax disable, LVGL core pinning, BSP flush recovery — none solved root cause. See `docs/knowledgebase/display-freeze-root-cause.md`. Boot-dependent, possibly related to DMA channel assignment or WiFi init timing.
- [ ] Remove debug logging from prepare_frame/commit_frame (s_prepare_call_count, s_commit_call_count)
- [ ] Add null checks in renderer for edge cases (no sprite loaded, no animation)

### Bridge Improvements
- [ ] Add /tools/reboot endpoint to bridge (currently only direct Watcher API)
- [ ] Health monitoring — restart bridge if Watcher goes offline
- [ ] mDNS resolution too slow for Node.js — using direct IP (10.0.0.40), fragile with DHCP

### Dashboard
- [ ] Dashboard (8091) should proxy to real Watcher (10.0.0.40) instead of mock data
- [ ] Sync dashboard state controls with actual Watcher state

### Match Firmware to Dashboard (Priority)

The web dashboard at localhost:8091 (`zidane-dashboard/index.html`) is the design reference. The firmware renderer needs to match it. Here's what's different and how to fix each:

#### 1. Per-State Animations in sprite_loader.c

**Dashboard has**: alert=combat stance, greeting=walk cycle, sleeping=still front, working=side-run
**Firmware has**: everything maps to `idle_down` or `walk_down`

Fix: Update `state_anim_map[]` in `pokewatcher/main/sprite_loader.c` (line ~77):
```c
static const char *state_anim_map[] = {
    [PW_STATE_IDLE]      = "idle_down",
    [PW_STATE_WORKING]   = "working",      // was "walk_down"
    [PW_STATE_WAITING]   = "waiting",      // was "idle_down"
    [PW_STATE_ALERT]     = "alert",        // was "walk_down"
    [PW_STATE_GREETING]  = "greeting",     // already correct
    [PW_STATE_SLEEPING]  = "sleeping",     // already correct
    [PW_STATE_REPORTING] = "reporting",    // was "idle_down"
};
```
These animation names match the `frames.json` on the SD card which already has all 14 animations defined.

#### 2. Fixed-Position States in renderer.c

**Dashboard has**: alert, greeting, sleeping, waiting, reporting lock sprite to center (no wandering)
**Firmware has**: all states wander based on behavior params

Fix: In `prepare_frame()` in `renderer.c`, add a check before `behavior_tick()`:
```c
// States that stay centered — no wandering
static const bool FIXED_STATES[] = {
    [PW_STATE_IDLE] = false,
    [PW_STATE_WORKING] = false,    // working still wanders
    [PW_STATE_WAITING] = true,
    [PW_STATE_ALERT] = true,
    [PW_STATE_GREETING] = true,
    [PW_STATE_SLEEPING] = true,
    [PW_STATE_REPORTING] = true,
};

if (FIXED_STATES[s_current_state]) {
    s_pos_x10 = CENTER_X * 10;
    s_pos_y10 = CENTER_Y * 10;
    // Still advance animation frames but don't move
} else {
    behavior_tick();
}
```
For fixed states, still cycle animation frames (the sprite animates in place) but skip the behavior walk/turn/pause logic.

#### 3. ZZZ Overlay for Sleeping

**Dashboard has**: floating z's drifting upward above Zidane
**Firmware has**: nothing

Fix: In `commit_frame()` (inside the lvgl_port_lock section), after the sprite is drawn, check if sleeping and draw z's using an LVGL label:
- Create an `lv_label` during init with "z Z z" text, hidden by default
- In commit_frame, if state==SLEEPING, show it positioned above sprite with a slow Y offset animation
- Keep it simple — a static "z Z z" label above the sprite is fine, don't need floating animation initially
- **WARNING**: Only set label text/position, don't create/destroy objects per frame (SPI conflict risk)

#### 4. Behavior Params

**Dashboard has** (in STATE_BEHAVIOR):
```
alert:     walkChance=0, speed=0, bounceAmp=0      (completely still)
greeting:  walkChance=0, speed=0, bounceAmp=0      (completely still)
waiting:   walkChance=0.005, speed=0.8              (barely moves)
sleeping:  walkChance=0.003, speed=0.5              (barely moves)
reporting: walkChance=0.01, speed=1.0               (barely moves)
```

**Firmware has** (in STATE_BEHAVIORS):
```
alert:     walkChance=0, speed=0, bounceAmp=0       ✓ matches
greeting:  walkChance=0, speed=0, bounceAmp=0       ✓ matches
waiting:   walkChance=5, speed=8                     ✗ too active
sleeping:  walkChance=3, speed=5                     ✗ too active
reporting: walkChance=10, speed=10                   ✗ too active
```

Fix: Update STATE_BEHAVIORS in renderer.c to use 0 walk/turn chance for fixed states. The fixed-position check (item 2 above) makes this redundant but update the values anyway for consistency.

#### 5. FF9 Dialog Box for Alert/Greeting/Reporting

**Dashboard has**: When alert, greeting, or reporting is active, Zidane shifts down and an FF9-style dialog box appears ABOVE him with:
- Grainy gray background (deterministic black dot pattern, NOT random per frame)
- Light gray border (#888888)
- "Zidane" name in white, bold 13px monospace
- Message text in white, 12px monospace, word-wrapped at 36 chars/line
- Dynamic height (grows up to 8 lines)
- Sharp speech tail triangle at 75% from left, pointing down toward Zidane
- Default messages: alert="Something needs your attention!", greeting="Morning! Ready to get to work.", reporting="Here's what happened while you were away."
- When a message is sent via API, it replaces the default text

**Firmware has**: FF9 dialog box exists (`dialog.c`) but only shows when `POST /api/message` is called. It does NOT auto-show for alert/greeting/reporting states. No speech tail. No sprite repositioning.

Fix — two parts:

**Part A**: Auto-show dialog for certain states. In the render loop (renderer.c), when state changes to alert/greeting/reporting AND no custom message is pending, auto-show a default message:
```c
if (s_state_changed) {
    // ... existing state change code ...

    // Auto-show dialog for dialog states
    const char *auto_msgs[] = {
        [PW_STATE_ALERT]     = "Something needs your attention!",
        [PW_STATE_GREETING]  = "Morning! Ready to get to work.",
        [PW_STATE_REPORTING] = "Here's what happened.",
    };
    if (state == PW_STATE_ALERT || state == PW_STATE_GREETING || state == PW_STATE_REPORTING) {
        if (!s_msg_pending) {
            pw_dialog_show(auto_msgs[state], PW_MSG_LEVEL_INFO);
        }
    } else {
        pw_dialog_hide();
    }
}
```
Note: `pw_dialog_show` and `pw_dialog_hide` must be called inside the `lvgl_port_lock` section.

**Part B**: Shift sprite down when dialog is showing. In `prepare_frame()`, offset `s_next_screen_y` down when dialog is visible:
```c
// After calculating s_next_screen_y:
if (pw_dialog_is_visible()) {
    s_next_screen_y += 95;  // shift sprite down, dialog goes above
}
```

**Part C** (optional, cosmetic): The speech tail triangle and grainy texture are in the dashboard's canvas drawing code. For the firmware's LVGL dialog, the current solid dark bg with border is fine — the grainy texture would require a custom LVGL draw callback which is complex. The speech tail could be approximated with an LVGL triangle object but is low priority.

#### 6. Background Images (Deferred)

**Dashboard has**: 84 Pictlogica FF backgrounds cycling per state
**Firmware has**: solid colors only (bg tile loading disabled due to SPI conflict)

This is the hardest to fix. Best approach: at boot (before Himax starts), pre-load one background per state into PSRAM (~340KB each × 7 states = 2.4MB). Then on state change, just swap the LVGL image source pointer — no file I/O needed during runtime. This avoids the SPI conflict entirely.

#### Reference: Dashboard Animation Names → frames.json
| Dashboard anim | frames.json key | Frames used |
|---------------|----------------|-------------|
| idle (down/up/left/right) | idle_down, idle_up, idle_left, idle_right | front/back/side standing |
| walk (directions) | walk_down, walk_up, walk_left, walk_right | walk cycles |
| greeting | greeting | front walk cycle (bouncy) |
| sleeping | sleeping | front1 still frame |
| working | working | side-run (act1 row) |
| alert | alert | combat stance (act2+act3 rows) |
| reporting | reporting | front standing (front1, front5) |
| waiting | waiting | front standing (front1, front5) |

### Future Features (from design spec)
- [ ] Audio/speaker output (hardware ready)
- [ ] RGB LED integration (hardware ready)
- [ ] BLE phone connectivity
- [ ] ZZZ overlay on Watcher display for sleeping state
- [ ] Touch screen interaction

## Critical Knowledge for Future Agents

1. **READ `docs/knowledgebase/spi-bus-conflict.md` BEFORE touching renderer.c** — SPI collision between LCD and Himax will cause permanent freeze if you call LVGL from wrong thread or too frequently
2. **Build from /tmp** — Space in project path breaks linker. See `docs/knowledgebase/building-and-flashing-firmware.md`
3. **Move SDK aside during build** — `SenseCAP-Watcher-Firmware/` in project root causes linker path issues
4. **Delete sdkconfig when changing defaults** — Old sdkconfig overrides sdkconfig.defaults silently
5. **Serial port is /dev/cu.usbmodem5A8A0533623** — The other port (621) doesn't work for flashing
6. **Flash with app-flash only** — Preserves NVS and nvsfactory partitions
7. **Renderer is single-threaded** — All LVGL calls happen in renderer_task. Other threads set volatile flags only.
