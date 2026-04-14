# Changelog

All notable changes to the PokéWatcher firmware will be documented in this file.

## [Unreleased]

### Added
- **"down" state**: New `PW_STATE_DOWN` for when OpenClaw is offline — laying-down sprite pose, dark gray background
- **Sprite mirror support**: `mirror: true` flag in frames.json flips left-facing sprites for right-facing animations
- **Per-frame custom sizes**: frames.json supports `w`/`h` overrides per frame (e.g., 25x13 "down" pose)
- **Anti-reversal walking**: Idle walking no longer picks the opposite direction (no up→down→up jitter)
- **Sprite catalog page**: `localhost:8091/sprites.html` shows all frames, animations, and coordinates with hover tooltips
- **Himax pause/resume API**: `pw_himax_pause()`/`pw_himax_resume()` for SPI bus arbitration

### Changed
- **Backgrounds decoupled from states**: Single pool of 72 curated backgrounds rotates every 5 minutes, same for all states
- **States only control sprite animation**: No more per-state background colors — states are sprite behavior triggered by OpenClaw
- **Per-state animations corrected**: Each state now uses its dedicated animation (alert=combat, working=focused, sleeping=still, etc.)
- **Fixed-position states**: Alert, greeting, sleeping, waiting, reporting, working lock sprite to center
- **Idle walk tuning**: walk_chance 30→60, pause 30-80→20-40 (more active movement)
- **State change uses dedicated animation**: Fixed bug where state change always set `idle_down` instead of the state's animation

### Fixed
- **#23 Display freeze on dialog**: `pw_dialog_tick()` called `lv_obj_set_style_opa()` every frame on a 288x70 container, overwhelming the LCD SPI transfer queue until the flush callback hung permanently. Removed per-frame opacity fade — dialog now shows/hides with simple timeout. See `docs/knowledgebase/display-freeze-root-cause.md`
- **SPI collision was misdiagnosed**: The display freeze was NOT caused by Himax SPI collision as previously documented. It was LVGL's per-frame style changes on large objects. Updated `docs/knowledgebase/spi-bus-conflict.md`

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
