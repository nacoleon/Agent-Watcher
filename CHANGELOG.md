# Changelog

All notable changes to the PokéWatcher firmware will be documented in this file.

## [Unreleased]

### Fixed
- **#1 Dual queue consumers**: mood_engine_task and coordinator_task both consumed from the same FreeRTOS queue, causing ~50% of events to be delivered to the wrong consumer. Removed coordinator_task; mood engine now dispatches all event types via callbacks.
- **#2 Renderer thread safety**: sprite data, animation state, and frame buffer were accessed from multiple tasks without synchronization, causing use-after-free crashes. Added a mutex to serialize all renderer state access.
- **#3 Sprite frame OOB read**: frame extraction had no bounds checking on sheet coordinates — bad frames.json would read past the sprite sheet buffer. Added clamping.
- **#4 NULL deref in sprite loader**: missing `frame_width`, `frame_height`, `x`, or `y` keys in frames.json would crash. Added NULL checks.
- **#18 Unsigned enum comparison**: `mood < 0` was always false for unsigned enum. Replaced with `mood >= 6`.
- **#5 NULL deref in web server**: `handle_api_roster_active` didn't check if `cJSON_Parse` returned NULL before accessing fields. Added NULL check.
- **#6 Use-after-free in web server**: `handle_api_roster_active` read `id->valuestring` after `cJSON_Delete(root)` freed it. Moved `cJSON_Delete` after last use.
- **#11 Path traversal in web API**: Pokemon IDs from HTTP requests were used directly in file paths. Added `is_valid_pokemon_id()` to reject anything not `[a-z0-9_-]`.
- **#7 NVS blob versioning**: Roster and LLM config blobs loaded from NVS without size validation — struct layout changes between firmware versions would produce garbage. Added size checks; mismatches reset to defaults.
