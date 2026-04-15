# Changelog

All notable changes to the PokéWatcher firmware will be documented in this file.

## [Unreleased]

### Added
- **Wakeup animation**: 7-frame non-looping sequence (laying down → kneeling → standing) plays automatically when display wakes from off before any queued state applies
- **PW_STATE_WAKEUP**: New agent state with wakeup button in web UI, purple badge, mapped to "wakeup" animation in frames.json
- **Wakeup state queue**: When display wakes from off, pending state changes are queued and applied after wakeup animation completes
- **Slower wakeup frame rate**: Wakeup animation runs at 2x slower speed (every 6 ticks vs 3) for a natural getting-up feel
- **Dialog pagination**: Messages over 80 chars split into pages, scroll with physical knob wheel. Page indicator (e.g., "1/3") at bottom-right. 1000 char message limit.
- **Knob button controls**: Short press dismisses dialog (only when visible), long press (6s) hard reboots. Button press while display is off triggers wakeup animation.
- **Character counter in web UI**: Live count (0/1000) with yellow at 800, red at limit
- **SPI flush retry**: SPD2010 driver retries full CASET+RASET+RAMWR sequence on SPI stall with 10ms recovery delay. LVGL flush callback signals flush_ready on retry failure to prevent permanent mutex deadlock.

### Changed
- **Transition walks generalized**: Sprite now walks between ANY two fixed-position states (not just from idle). Prevents large SPI dirty regions that caused display freezes
- **LVGL lock timeout**: `lvgl_port_lock` now uses 500ms timeout instead of wait-forever, allowing renderer to recover from SPI flush stalls
- **Dialog box fixed at 288x88**: No per-frame size changes — per-frame lv_obj_set_height/lv_label_set_text triggers the SPI flush race condition permanently
- **Sprites.html improvements**: Down/KO frame (25x13) added to frame grid, animation preview resizes per-frame for mixed-size animations, per-animation speed support

### Fixed
- **Display freeze on non-idle→sleeping transition**: Position snap from bottom-center to sleep position created large SPI dirty region. Fixed by generalizing transition walks to all state changes
- **Display freeze on state change during sleep window**: Same root cause — no transition walk when leaving sleeping state
- **LVGL flush deadlock root cause**: `lvgl_port_flush_callback` never called `lv_disp_flush_ready()` when `esp_lcd_panel_draw_bitmap` failed — LVGL waited forever for flush completion
- **Web server buffer overflow**: 256-byte body buffer overflowed with long messages in JSON wrapper. Increased to 1280 bytes.
- **Dead wake flag**: `s_pre_sleep_triggered` never reset after first sleep cycle because `s_wake_requested` was consumed before the reset check. Fixed with `woke_this_frame` flag

### Previous
- **"down" state**: New `PW_STATE_DOWN` for when OpenClaw is offline — laying-down sprite pose, dark gray background
- **Sprite mirror support**: `mirror: true` flag in frames.json flips left-facing sprites for right-facing animations
- **Per-frame custom sizes**: frames.json supports `w`/`h` overrides per frame (e.g., 25x13 "down" pose)
- **Anti-reversal walking**: Idle walking no longer picks the opposite direction (no up→down→up jitter)
- **Sprite catalog page**: `localhost:8091/sprites.html` shows all frames, animations, and coordinates with hover tooltips
- **Himax pause/resume API**: `pw_himax_pause()`/`pw_himax_resume()` for SPI bus arbitration
- **Background image loading**: Pre-load background tile from SD card at boot, scaled to 412x412 in PSRAM
- **Background API**: PUT /api/background with web UI prev/next/set controls
- **Smooth state transitions**: Sprite walks from idle position to target before entering state animation
- **Per-state sprite positions**: Alert/reporting/greeting/working/waiting at bottom-center, sleeping/down below center
- **ZZZ animation**: Cycling "z Z z" text in Montserrat 28, follows sprite position dynamically
- **Display sleep with pre-sleep**: 3min idle → sleeping state → 15s later display off. Also sleeps 15s after sleeping state set via API
- **Speaker mute at boot**: Codec initialized and muted to prevent idle amp pops (unmute with `bsp_codec_mute_set(false)`)
- **Hardware-like reboot**: Web UI reboot power cycles LCD and AI chip before restart
- **SPI diagnostic logging**: SPD2010 driver logs error code, area, task, and core on flush failure

### Changed
- **Backgrounds decoupled from states**: Single pool of 72 curated backgrounds rotates every 5 minutes, same for all states
- **States only control sprite animation**: No more per-state background colors — states are sprite behavior triggered by OpenClaw
- **Per-state animations corrected**: Each state now uses its dedicated animation (alert=combat, working=focused, sleeping=still, etc.)
- **Fixed-position states**: Alert, greeting, sleeping, waiting, reporting, working, down lock sprite to fixed positions
- **Idle walk tuning**: walk_chance 30→60, pause 30-80→20-40 (more active movement)
- **State change uses dedicated animation**: Fixed bug where state change always set `idle_down` instead of the state's animation
- **Dialog box moved to top**: Dialog now shows at top of screen instead of bottom
- **Himax disabled**: Person detection disabled since OpenClaw controls states (SPI collision confirmed)
- **Frame corrections**: Updated idle_down, idle_left/right, walk_down/up/left/right, alert, working animations with correct sprite frames

### Fixed
- **#23 Display freeze on dialog**: `pw_dialog_tick()` called `lv_obj_set_style_opa()` every frame on a 288x70 container, overwhelming the LCD SPI transfer queue until the flush callback hung permanently. Removed per-frame opacity fade — dialog now shows/hides with simple timeout. See `docs/knowledgebase/display-freeze-root-cause.md`
- **SPI collision was misdiagnosed**: The display freeze was NOT caused by Himax SPI collision as previously documented. It was LVGL's per-frame style changes on large objects. Updated `docs/knowledgebase/spi-bus-conflict.md`
- **ZZZ not showing after transition**: ZZZ overlay only appeared for direct state changes, not after walk-to-position transitions. Fixed by re-triggering visuals dirty flag on transition complete.
- **Background byte-swap**: Background tiles were showing as color negative — added RGB565 byte-swap for LV_COLOR_16_SWAP
- **Speaker pop noise**: Idle amp produced periodic pops. Fixed by initializing codec and muting at boot.

### Previous Fixes
- **#1 Dual queue consumers**: mood_engine_task and coordinator_task both consumed from the same FreeRTOS queue, causing ~50% of events to be delivered to the wrong consumer. Removed coordinator_task; mood engine now dispatches all event types via callbacks.
- **#2 Renderer thread safety**: sprite data, animation state, and frame buffer were accessed from multiple tasks without synchronization, causing use-after-free crashes. Added a mutex to serialize all renderer state access.
- **#3 Sprite frame OOB read**: frame extraction had no bounds checking on sheet coordinates — bad frames.json would read past the sprite sheet buffer. Added clamping.
- **#4 NULL deref in sprite loader**: missing `frame_width`, `frame_height`, `x`, or `y` keys in frames.json would crash. Added NULL checks.
- **#18 Unsigned enum comparison**: `mood < 0` was always false for unsigned enum. Replaced with `mood >= 6`.
- **#5 NULL deref in web server**: `handle_api_roster_active` didn't check if `cJSON_Parse` returned NULL before accessing fields. Added NULL check.
- **#6 Use-after-free in web server**: `handle_api_roster_active` read `id->valuestring` after `cJSON_Delete(root)` freed it. Moved `cJSON_Delete` after last use.
- **#11 Path traversal in web API**: Pokemon IDs from HTTP requests were used directly in file paths. Added `is_valid_pokemon_id()` to reject anything not `[a-z0-9_-]`.
- **#7 NVS blob versioning**: Roster and LLM config blobs loaded from NVS without size validation — struct layout changes between firmware versions would produce garbage. Added size checks; mismatches reset to defaults.
- **#8 Mood config not persisted**: Mood timer settings (curious/lonely/excited/overjoyed durations) were only stored in memory. Rebooting reset them to compile-time defaults. Now saved to NVS on change and loaded on boot.
- **#9 Evolution progress not persisted**: `pw_roster_update_evolution()` was never called — evolution seconds tracked by mood engine were lost on reboot. Now syncs to roster NVS every 60 seconds and restores on boot.
- **#10 First detection goes OVERJOYED**: On first boot (no person ever seen), detecting a person triggered OVERJOYED instead of EXCITED. Now checks `last_person_seen_ms == 0` to use EXCITED for the very first detection.
- **#12 Partial HTTP reads**: `httpd_req_recv` may return fewer bytes than content-length. All three handler call sites now use `recv_full_body()` which loops until the full body is received.
- **#14 Unused LLM queue**: `s_llm_queue` was created but never read from or written to. Removed dead code and unused include.
