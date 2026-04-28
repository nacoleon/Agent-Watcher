# Changelog

All notable changes to Agent Watcher will be documented in this file.

## [Unreleased]

### Added
- **MCP idle timeout**: MCP server self-terminates after 5 minutes of no stdin activity, preventing orphaned "zombie" processes from accumulating when OpenClaw gateway doesn't close the stdio pipe.

### Changed
- **DOWN state persists through sleep/wake**: DOWN state no longer gets overwritten by the pre-sleep SLEEPING transition. Display still turns off on idle timeout, but agent state stays DOWN. Waking the display (button, person detection, gesture) goes straight to the DOWN animation — no wakeup animation plays.
- **LED suppressed while display sleeps**: RGB LED blink is fully dark when the display is off. LED activates when the display wakes back up.
- **Any OpenClaw API call recovers from DOWN**: Previously only heartbeat recovered from DOWN → IDLE. Now any write endpoint (message, state change, background, model, audio play, voice config) also recovers, since it proves the agent is alive.
- **Person/gesture detection preserves DOWN**: Person detection and gesture detection wake the display but no longer override DOWN → IDLE. Only OpenClaw API calls can clear DOWN state.
- **Poll interval reduced to 1s**: Device polling dropped from 5s to 1s, cutting voice message detection latency by ~4s on average. Periodic status log adjusted from every 60 polls to every 300 to maintain ~5min cadence.
- **WiFi credentials externalized**: Moved SSID/password to `config.local.h` (gitignored) with `config.local.h.example` template. `config.h` uses placeholders and auto-includes the local override via `__has_include`. Git history scrubbed of prior credentials.
- **WATCHER_URL configurable**: MCP server and daemon read `WATCHER_URL` from environment variable with fallback to `http://10.0.0.40`. No longer requires editing source to change device IP.

### Fixed
- **Dialog dismiss did not sync agent_state**: When the user dismissed a greeting/alert/reporting dialog by pressing the knob, the renderer animation switched to idle but `pw_agent_state` stayed on the previous state — so the Web UI kept reporting the old state until the next OpenClaw event. Dismiss path now calls `pw_agent_state_set(PW_STATE_IDLE)` alongside the renderer reset, matching the pattern used in `voice_input.c` and `web_server.c`.
- **MCP zombie accumulation**: OpenClaw gateway spawns stdio MCP server processes that never exit after tool calls complete. Root cause: gateway doesn't close stdin pipe. Mitigated with 5-minute idle timeout auto-exit.
- **Stale web embed references**: Removed orphaned `style.css` and `app.js` references from CMakeLists.txt and web_server.c (files were deleted in a prior commit but references remained).

### Documentation
- **README rewritten**: Added repository structure, fixed quick start snippet (set-target, flash vs app-flash), updated MCP tools with auto-pairing, corrected requirements (Python 3, OpenClaw gateway).
- **Quick Start guide**: Full 12-step zero-to-running setup for brand-new device + fresh Mac.
- **Detailed Guide**: Complete feature reference covering all firmware, MCP, daemon, and Web UI features.
- **EHOSTUNREACH daemon knowledgebase**: `docs/knowledgebase/ehostunreach-daemon-bug.md` documents the daemon-stuck-on-EHOSTUNREACH bug (likely ExpressVPN NKE), kickstart workaround, and investigation notes.
- **DOWN state sleep/wake plan**: `docs/superpowers/plans/2026-04-21-down-state-sleep-wake.md` archives the implementation plan behind the DOWN-state persistence fixes shipped this release.

---

## [Previous Unreleased]

### Added
- **Early stop recording**: Single knob press stops voice recording immediately instead of waiting for the full timeout. Double-click starts, single press stops.
- **90-second recording timeout**: Increased from 10s to 90s (~2.88MB PSRAM buffer). Practical ceiling given available memory.
- **Response mode setting**: New `response_mode` setting (`both`/`voice_only`/`text_only`, default: `both`) controls whether voice replies include text, voice, or both. Persisted in NVS across reboots. Configurable from Web UI "Reply Mode" dropdown in the Voice card.
- **Voice reply auto-pairing**: When `response_mode` is `both`, the `speak` MCP tool automatically displays the response text on the LCD, and `display_message` automatically speaks it. Only applies to voice-triggered replies — proactive messages from OpenClaw remain under its control.
- **Voice context tracking**: Daemon sets a 120-second voice context flag after transcription. MCP tools check this to determine if the current response is to a voice input.
- **Direct gateway WebSocket client**: Daemon connects to OpenClaw gateway via persistent WebSocket (`ws://127.0.0.1:18789`) with Ed25519 device identity signing. Replaces `openclaw agent` CLI spawn. Message delivery dropped from ~40s to <1s.
- **Push-to-talk voice input**: Double-click knob to record up to 90s of 16kHz audio, HTTP POST WAV to MCP server on port 3848, whisper-node transcribes locally, text delivered to OpenClaw via gateway WebSocket. RGB LED feedback: blue pulsing (recording), green (success), red (error).
- **MCP audio server**: Express HTTP endpoint at `:3848/audio` receives WAV from Watcher, transcribes via whisper-node (whisper.cpp, Apple Silicon optimized), sends `voice_input: <text>` logging message to OpenClaw.
- **RGB LED state blink**: WS2812 LED blinks every 10 seconds at 10% brightness with state-specific color — red (alert/down), orange (waiting), pink (greeting), green (reporting). LED turns off on knob dismiss and display sleep.
- **Background auto-rotate toggle**: Web UI button and API field (`auto_rotate`) to pause/resume the 5-minute background rotation.
- **OpenClaw heartbeat**: `POST /api/heartbeat` endpoint and `watcher__heartbeat` MCP tool. Watcher switches to "down" state if no heartbeat received for 1.5 hours, auto-recovers to idle on next beat. Web UI shows heartbeat status and last 5 heartbeat timestamps.
- **Presence logging**: Agent state tracks presence events (arrived/left) with timestamps, exposed in `/api/status` and web UI.
- **Himax person detection infrastructure**: 60s time-based debounce, wake display on person arrive, sleep on person leave (if idle + no dialog), presence event log in web UI. Camera disabled pending SD card SPI2 bus conflict resolution — see `docs/knowledgebase/himax-camera-debugging.md`.
- **Himax auto-flash from SD card**: OTA flash logic reads firmware.img + person.tflite from `/sdcard/himax/` if chip doesn't respond. SD card files prepared in `sdcard_prep/himax/`.
- **CPU frequency fix**: Corrected sdkconfig key from `CONFIG_ESP_DEFAULT_CPU_FREQ_240` to `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240` — was silently running at 160MHz instead of 240MHz.
- **SD card init switched to BSP**: Uses `bsp_sdcard_init_default()` (20MHz, same as stock) instead of custom init at 400kHz.

### Changed
- **Dialog box redesigned**: Larger (340×170 max), auto-heights to content, Montserrat 22 font (was 14), 95 chars per page (was 80). Page indicator gets reserved space at bottom to prevent text overlap.
- **Unicode text sanitizer**: Em dashes, curly quotes, ellipsis, and emoji in messages are replaced with ASCII equivalents to prevent rectangle glyphs in the font.
- **Walk speed increased**: Sprite walks at 2.5 px/frame (was 1.5) for more natural RPG-like movement.
- **Sleep/down states don't wake display**: External state changes to sleeping or down no longer wake the display. Prevents spurious wake after sleep timeout.
- **Button wake doesn't dismiss dialog**: Pressing the knob while display is off now only wakes the screen. A second press is needed to dismiss the dialog.
- **Sleeping position with dialog**: When a dialog is open, sleeping/down states position the sprite at bottom-center (visible below dialog) instead of center.
- **sdkconfig simplified**: Removed DMA bounce buffer, removed SPI_MASTER_IN_IRAM, removed SSCMA task overrides, removed LVGL affinity settings. Minimal config matching monitor example + memory reserves.

### Changed
- **Recording animation state**: Uses `PW_STATE_REPORTING` (was `PW_STATE_WORKING`) during voice recording for a better visual fit.
- **Text before voice on replies**: When auto-pairing is active, the text message displays on screen before TTS audio plays, so Zidane shows reporting animation while speaking.

### Fixed
- **State animation not triggering via API**: `PUT /api/agent-state` was only updating the state data struct (`pw_agent_state_set`) without calling `pw_renderer_set_state`, so API-triggered state changes (including auto-paired display messages) never changed Zidane's animation. Now both are called.
- **Recording animation not triggering**: Same bug in `voice_input.c` — recording state change never reached the renderer. Fixed by adding `pw_renderer_set_state` calls alongside `pw_agent_state_set`.
- **SPI flush stall / fractal lines (root cause found and fixed)**: Error 0x101 was `ESP_ERR_NO_MEM` — LVGL draw buffers in PSRAM require DMA bounce buffers in internal SRAM. Fix: `CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=262144` reserves 256KB internal for DMA. DMA bounce buffer pre-allocation removed (stock BSP approach works).

### Previous Unreleased

### Added
- **Watcher MCP server**: Stdio MCP server replaces the Express HTTP bridge for OpenClaw integration. Zidane auto-discovers `watcher__*` tools (display_message, set_state, get_status, notify, reboot) via MCP protocol instead of manual TOOLS.md definitions.
- **MCP presence notifications**: 5-second poller with 2-poll debounce pushes `person_arrived`/`person_left` log messages to Zidane via MCP, replacing file-based polling (`watcher-context.json`/`watcher-events.json`).
- **MCP status resource**: `watcher://status` resource exposes live device state (agent_state, person_present, uptime, wifi_rssi).
- **MCP message queue**: Messages queue in the MCP server (FIFO, max 10) instead of overwriting. Next message sends after knob dismiss. Zidane receives `message_read`/`queue_empty` notifications. States are paired with their messages so the state only applies when that message reaches the screen.
- **Dismiss counter**: Firmware tracks `dismiss_count` in `/api/status` — increments on every knob press dismiss. MCP server uses counter comparison instead of `dialog_visible` transitions, eliminating polling gap bugs.
- **Knob dismiss resets to idle**: Pressing the knob to dismiss a dialog also sets agent state to idle, so Zidane doesn't stay stuck in alert/greeting pose after reading a message.
- **`get_queue` tool**: MCP tool to check pending messages and queue state.
- **Tool error handling**: All MCP tool handlers return descriptive `isError` responses when Watcher is offline. Notify tool reports partial failures (state set but message failed).
- **Background index in status API**: `/api/status` now returns current background index, web UI syncs the background picker from it.

### Changed
- **OpenClaw integration**: Moved from manual HTTP bridge (Express on port 3847) to native MCP server (stdio transport, spawned by OpenClaw gateway). LaunchAgent `ai.openclaw.watcher-bridge` retired.
- **TOOLS.md**: Removed manual watcher tool definitions — now auto-discovered via MCP.
- **HEARTBEAT.md**: Updated Desk Context section to use MCP presence notifications instead of file polling.
- **Dialog stays until dismissed**: Removed 10-second auto-dismiss timer. Dialog now persists until knob press.
- **Greeting/alert no auto-revert**: Removed 10s/60s timers that auto-reverted greeting and alert states to idle. States now persist until explicitly changed (same as reporting).

## [Previous Unreleased]

### Added
- **Wakeup animation**: 7-frame non-looping sequence (laying down → kneeling → standing) plays automatically when display wakes from off before any queued state applies
- **PW_STATE_WAKEUP**: New agent state with wakeup button in web UI, purple badge, mapped to "wakeup" animation in frames.json
- **Wakeup state queue**: When display wakes from off, pending state changes are queued and applied after wakeup animation completes
- **Slower wakeup frame rate**: Wakeup animation runs at 2x slower speed (every 6 ticks vs 3) for a natural getting-up feel
- **Dialog pagination**: Messages over 80 chars split into pages, scroll with physical knob wheel. Page indicator (e.g., "1/3") at bottom-right. 1000 char message limit.
- **Knob button controls**: Short press dismisses dialog (only when visible), long press (6s) hard reboots. Button press while display is off triggers wakeup animation.
- **Character counter in web UI**: Live count (0/1000) with yellow at 800, red at limit
- **Background auto-rotation**: 72 curated tiles rotate every 5 minutes with strip wipe transition (20 rows/frame, ~2s top-to-bottom wipe). Double-buffered staging in PSRAM for SPI-safe transitions.
- **Wakeup from knob button**: Press wheel button while display is off to trigger wakeup animation
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
