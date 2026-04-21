# Dead Code Cleanup — Pokemon → Agent Watcher

## Context

This project started as "PokéWatcher" — a Pokemon AI companion for the SenseCap Watcher device. It was pivoted to "Agent Watcher" (aka "Zidane Watcher") using Final Fantasy IX sprites and OpenClaw integration. The Pokemon concept is fully abandoned but remnants exist throughout the codebase.

The C firmware files (`pokewatcher/main/*.c`, `*.h`) were already refactored during the pivot — they use generic naming (agent_state, sprite_loader, renderer) and contain no Pokemon-specific code. **Do not touch these files.**

## Your Task

Audit the entire repository for dead code, unused files, and stale references from the old Pokemon concept. For each finding, categorize it as **DELETE** (remove entirely), **UPDATE** (rename/rewrite to match current project), or **KEEP** (still useful, leave it).

Then execute the cleanup: delete dead files, update stale references, and commit after each logical group of changes.

## Known Dead Code Locations

### 1. Pokemon SD Card Assets (DELETE)
- `pokewatcher/sdcard/pokemon/` — 9 Pokemon directories (pikachu, charmander, charmeleon, bulbasaur, eevee, ivysaur, raichu, squirtle, wartortle) each with `pokemon.json`, `overworld.png`, `overworld.raw`, `frames.json`
- These are never loaded by the current firmware (it loads from `sdcard_prep/characters/zidane/`)

### 2. Legacy Web Pages (EVALUATE)
- `pokewatcher/main/web/roster.html` — Pokemon roster management (add/remove/select characters). Title says "PokéWatcher - Roster". References `pokemon-card`, `addPokemon()`, `removePokemon()`, `data-pokemon`. Check if ANY firmware endpoint still serves roster API routes (`/api/roster`, `/api/roster/active`). If no backend exists, DELETE this file.
- `pokewatcher/main/web/settings.html` — LLM config (endpoint, API key, model) and mood timers (curious, lonely, excited, overjoyed). Title says "PokéWatcher - Settings". Check if ANY firmware endpoint still serves settings API routes (`/api/settings`). If no backend exists, DELETE this file.
- `pokewatcher/main/web/style.css` — Contains mood badge classes (`.mood-excited`, `.mood-overjoyed`, `.mood-happy`, `.mood-curious`, `.mood-lonely`, `.mood-sleepy`) and `.pokemon-card` styling. If roster.html and settings.html are deleted, clean up or DELETE this file.
- `pokewatcher/main/web/app.js` — Comment says "Shared utilities for PokéWatcher dashboard", body is empty. DELETE if unused.

### 3. Legacy Config Defines (EVALUATE)
- `pokewatcher/main/config.h` — `PW_LLM_MAX_RESPONSE_LEN 512` is marked "kept for potential future use". Check if ANY code references it. If not, DELETE.
- `pokewatcher/main/config.h` — `PW_SD_CHARACTER_DIR "/sdcard/characters"` — verify this is still used by the current firmware (it should be, for loading Zidane sprites).

### 4. Legacy Documentation (DELETE)
- `docs/superpowers/specs/2026-04-12-pokewatcher-design.md` — Original Pokemon design spec (353 lines). Historical artifact, no longer relevant.
- `docs/superpowers/plans/2026-04-12-pokewatcher-plan.md` — Original Pokemon implementation plan. Historical artifact.
- `docs/knowledgebase/creating-pokemon-sprites.md` — Pokemon sprite conversion guide. The sprite pipeline is still relevant for Zidane but this doc references Pokemon specifically. Either UPDATE to be generic or DELETE if the Detailed Guide already covers sprite creation.

### 5. Brainstorm Artifacts (DELETE)
- `.superpowers/brainstorm/` — Old brainstorm sessions with Pokemon architecture diagrams. Internal artifacts, not useful.

### 6. Bridge Directory (EVALUATE)
- `bridge/` — Deprecated Express HTTP bridge (replaced by MCP server). Contains `src/`, `dist/`, `config.json`, `ai.openclaw.watcher-bridge.plist`. CHANGELOG says "LaunchAgent `ai.openclaw.watcher-bridge` retired." If truly deprecated, DELETE the entire directory.

### 7. Firmware Binary Archives (EVALUATE)
- `firmware/` — Pre-built Seeed Studio firmware images (`himax_firmware_20240816.img`, `sensecap_watcher_20241106.img`, `sensecap_watcher_20250102.img`). These are stock Seeed images, not custom builds. Check if any build or setup step references them. If not, DELETE (the Himax firmware lives in `sdcard_prep/himax/` which is the canonical location).
- `sdcard_prep/sensecap_watcher_20250102.img` — Duplicate of `firmware/sensecap_watcher_20250102.img`. DELETE the duplicate.

### 8. Stale References in Active Code (UPDATE)
- Search ALL tracked files for: `pokemon`, `pokewatcher`, `PokéWatcher`, `roster`, `evolv`, `mood`, `curious`, `lonely`, `excited`, `overjoyed`. Update or remove any that reference the old concept.
- Check `CHANGELOG.md` — it may contain Pokemon-era entries. These are historical and should be KEPT (don't rewrite history).

## Rules

1. **Read before deleting** — verify each file is truly unused before removing it
2. **Check for backend routes** — before deleting web pages, grep `web_server.c` for the corresponding API endpoints
3. **Commit after each logical group** — e.g., "remove Pokemon SD card assets", "remove legacy web pages", "remove deprecated bridge"
4. **Don't touch** `pokewatcher/main/*.c` or `*.h` files unless removing a clearly dead `#define`
5. **Don't touch** `CHANGELOG.md` entries (historical record)
6. **Don't touch** `watcher-mcp/` (actively maintained MCP server)
7. **Don't touch** `zidane-dashboard/` (actively maintained dev dashboard)
8. **Don't touch** `sdcard_prep/characters/zidane/` (active sprite assets)
9. After all cleanup, run `git status` and list what was removed/changed as a summary
