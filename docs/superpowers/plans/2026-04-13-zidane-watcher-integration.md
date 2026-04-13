# Zidane × Watcher Integration — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Transform the SenseCap Watcher from a Pokémon desk pet into Zidane's physical desk companion — driven by OpenClaw agent state via an HTTP bridge, with FF9-style dialog rendering.

**Architecture:** Three components: (1) firmware refactored from Pokémon moods to agent states with new API endpoints and FF dialog renderer, (2) Node.js bridge on Mac that proxies Zidane tool calls to Watcher HTTP API and polls presence, (3) Zidane tool registration in OpenClaw workspace files.

**Tech Stack:** ESP-IDF 5.2.1 (C), LVGL 8.x, Node.js/TypeScript, Express, ESP HTTP server, cJSON

**Spec:** `docs/superpowers/specs/2026-04-13-zidane-watcher-integration-design.md`

**Build workflow:** Must build from `/tmp/pokewatcher-build/` due to space in project path. See `docs/knowledgebase/building-and-flashing-firmware.md`.

---

## File Map

### Firmware — New Files
| File | Responsibility |
|------|---------------|
| `pokewatcher/main/agent_state.h` | Agent state enum, config struct, public API |
| `pokewatcher/main/agent_state.c` | Agent state engine (replaces mood_engine for state management) |
| `pokewatcher/main/dialog.h` | FF9 dialog box renderer API |
| `pokewatcher/main/dialog.c` | LVGL dialog box widget — gradient bg, portrait, text, auto-dismiss |

### Firmware — Modified Files
| File | Changes |
|------|---------|
| `pokewatcher/main/event_queue.h` | Add agent state enum, new event types |
| `pokewatcher/main/config.h` | Replace mood timers with agent state config, rename SD paths |
| `pokewatcher/main/renderer.h` | Replace mood API with agent state API, add dialog methods |
| `pokewatcher/main/renderer.c` | Replace mood behavior table with agent state behaviors, integrate dialog |
| `pokewatcher/main/web_server.c` | Add `PUT /api/agent-state`, `POST /api/message`, update status endpoint, update mDNS to `zidane.local` |
| `pokewatcher/main/app_main.c` | Replace mood engine init/callbacks with agent state, remove roster/LLM callbacks |
| `pokewatcher/main/sprite_loader.h` | Update path from pokemon to characters |
| `pokewatcher/main/sprite_loader.c` | Update path from pokemon to characters |
| `pokewatcher/main/CMakeLists.txt` | Add agent_state.c, dialog.c to SRCS |

### Firmware — Removed/Gutted Files
| File | Action |
|------|--------|
| `pokewatcher/main/mood_engine.h` | Remove (replaced by agent_state) |
| `pokewatcher/main/mood_engine.c` | Remove (replaced by agent_state) |
| `pokewatcher/main/llm_task.h` | Remove (Zidane handles LLM now) |
| `pokewatcher/main/llm_task.c` | Remove (Zidane handles LLM now) |
| `pokewatcher/main/roster.h` | Remove (single character, no roster) |
| `pokewatcher/main/roster.c` | Remove (single character, no roster) |

### Bridge — New Files
| File | Responsibility |
|------|---------------|
| `bridge/package.json` | Node.js project config |
| `bridge/tsconfig.json` | TypeScript config |
| `bridge/src/index.ts` | Express server entry point |
| `bridge/src/watcher-client.ts` | HTTP client for Watcher API |
| `bridge/src/presence.ts` | Poller + debounced change detection |
| `bridge/src/events.ts` | Event/context file writer |
| `bridge/src/config.ts` | Config loader |
| `bridge/config.json` | Runtime config |

### OpenClaw — Modified Files
| File | Changes |
|------|---------|
| `~/clawd/TOOLS.md` | Add watcher.* tool definitions |
| `~/clawd/HEARTBEAT.md` | Add desk context section |

---

## Task 1: Replace Event Queue Enums (agent state + new events)

**Files:**
- Modify: `pokewatcher/main/event_queue.h`

- [ ] **Step 1: Replace pw_mood_t with pw_agent_state_t and add new event types**

In `pokewatcher/main/event_queue.h`, replace the mood enum and add agent state event:

```c
#ifndef POKEWATCHER_EVENT_QUEUE_H
#define POKEWATCHER_EVENT_QUEUE_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "config.h"

typedef enum {
    PW_EVENT_PERSON_DETECTED,
    PW_EVENT_PERSON_LEFT,
    PW_EVENT_AGENT_STATE_CHANGED,
    PW_EVENT_MESSAGE_RECEIVED,
} pw_event_type_t;

typedef enum {
    PW_STATE_IDLE,
    PW_STATE_WORKING,
    PW_STATE_WAITING,
    PW_STATE_ALERT,
    PW_STATE_GREETING,
    PW_STATE_SLEEPING,
    PW_STATE_REPORTING,
    PW_STATE_COUNT,
} pw_agent_state_t;

typedef enum {
    PW_MSG_LEVEL_INFO,
    PW_MSG_LEVEL_SUCCESS,
    PW_MSG_LEVEL_WARNING,
    PW_MSG_LEVEL_ERROR,
} pw_msg_level_t;

typedef struct {
    pw_event_type_t type;
    union {
        struct {
            pw_agent_state_t new_state;
            pw_agent_state_t old_state;
        } state;
        struct {
            char text[81];
            pw_msg_level_t level;
        } message;
    } data;
} pw_event_t;

void pw_event_queue_init(void);
bool pw_event_send(const pw_event_t *event);
bool pw_event_receive(pw_event_t *event, uint32_t timeout_ms);
QueueHandle_t pw_event_queue_handle(void);

#endif
```

- [ ] **Step 2: Verify event_queue.c compiles (no changes needed)**

`event_queue.c` only uses `pw_event_t` by size — no field access. It should compile unchanged. Verify by checking it only does `xQueueSend`/`xQueueReceive` on the struct.

- [ ] **Step 3: Commit**

```bash
git add pokewatcher/main/event_queue.h
git commit -m "refactor: replace mood enum with agent state enum in event queue"
```

---

## Task 2: Update config.h (agent state config, character paths)

**Files:**
- Modify: `pokewatcher/main/config.h`

- [ ] **Step 1: Replace mood timers and pokemon paths with agent state config**

Replace the full content of `config.h`:

```c
#ifndef POKEWATCHER_CONFIG_H
#define POKEWATCHER_CONFIG_H

// Display
#define PW_DISPLAY_WIDTH      412
#define PW_DISPLAY_HEIGHT     412
#define PW_SPRITE_SCALE       4
#define PW_SPRITE_SRC_SIZE    32
#define PW_SPRITE_DST_SIZE    (PW_SPRITE_SRC_SIZE * PW_SPRITE_SCALE)  // 128
#define PW_ANIM_FPS           10

// Agent state timeouts (auto-revert to IDLE)
#define PW_GREETING_TIMEOUT_MS   10000   // 10 seconds
#define PW_ALERT_TIMEOUT_MS      60000   // 60 seconds

// Dialog
#define PW_DIALOG_MAX_TEXT     81
#define PW_DIALOG_DISPLAY_MS   10000   // 10 seconds
#define PW_DIALOG_FADE_MS      500

// Event queue
#define PW_EVENT_QUEUE_SIZE    16

// Web server
#define PW_WEB_SERVER_PORT     80

// SD card paths
#define PW_SD_MOUNT_POINT      "/sdcard"
#define PW_SD_CHARACTER_DIR    "/sdcard/characters"

// NVS namespace
#define PW_NVS_NAMESPACE       "pokewatcher"

// Display sleep
#define PW_DISPLAY_SLEEP_TIMEOUT_MS 300000  // 5 minutes

// WiFi defaults (NVS overrides at runtime)
#define PW_WIFI_SSID_DEFAULT       "YOUR_WIFI_SSID"
#define PW_WIFI_PASSWORD_DEFAULT   "YOUR_WIFI_PASSWORD"

// LLM (kept for potential future use)
#define PW_LLM_MAX_RESPONSE_LEN   512

#endif
```

- [ ] **Step 2: Commit**

```bash
git add pokewatcher/main/config.h
git commit -m "refactor: update config for agent states, character paths, dialog settings"
```

---

## Task 3: Create agent_state module

**Files:**
- Create: `pokewatcher/main/agent_state.h`
- Create: `pokewatcher/main/agent_state.c`

- [ ] **Step 1: Create agent_state.h**

```c
#ifndef POKEWATCHER_AGENT_STATE_H
#define POKEWATCHER_AGENT_STATE_H

#include "event_queue.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    pw_agent_state_t current_state;
    pw_agent_state_t previous_state;
    int64_t state_entered_at_ms;
    bool person_present;
} pw_agent_state_data_t;

typedef void (*pw_state_change_cb_t)(pw_agent_state_t old_state, pw_agent_state_t new_state);

void pw_agent_state_init(void);
void pw_agent_state_set(pw_agent_state_t state);
pw_agent_state_data_t pw_agent_state_get(void);
const char *pw_agent_state_to_string(pw_agent_state_t state);
pw_agent_state_t pw_agent_state_from_string(const char *str);
void pw_agent_state_set_person_present(bool present);
void pw_agent_state_tick(void);
void pw_agent_state_set_change_cb(pw_state_change_cb_t cb);
void pw_agent_state_task_start(void);

#endif
```

- [ ] **Step 2: Create agent_state.c**

```c
#include "agent_state.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "pw_agent";

static pw_agent_state_data_t s_state;
static pw_state_change_cb_t s_change_cb = NULL;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static const char *STATE_STRINGS[] = {
    [PW_STATE_IDLE]      = "idle",
    [PW_STATE_WORKING]   = "working",
    [PW_STATE_WAITING]   = "waiting",
    [PW_STATE_ALERT]     = "alert",
    [PW_STATE_GREETING]  = "greeting",
    [PW_STATE_SLEEPING]  = "sleeping",
    [PW_STATE_REPORTING] = "reporting",
};

void pw_agent_state_init(void)
{
    s_state.current_state = PW_STATE_IDLE;
    s_state.previous_state = PW_STATE_IDLE;
    s_state.state_entered_at_ms = now_ms();
    s_state.person_present = false;
    ESP_LOGI(TAG, "Agent state engine initialized (starting IDLE)");
}

void pw_agent_state_set(pw_agent_state_t state)
{
    if (state >= PW_STATE_COUNT) return;
    if (state == s_state.current_state) return;

    pw_agent_state_t old = s_state.current_state;
    s_state.previous_state = old;
    s_state.current_state = state;
    s_state.state_entered_at_ms = now_ms();

    ESP_LOGI(TAG, "State: %s -> %s", STATE_STRINGS[old], STATE_STRINGS[state]);

    if (s_change_cb) {
        s_change_cb(old, state);
    }
}

pw_agent_state_data_t pw_agent_state_get(void)
{
    return s_state;
}

const char *pw_agent_state_to_string(pw_agent_state_t state)
{
    if (state < PW_STATE_COUNT) return STATE_STRINGS[state];
    return "unknown";
}

pw_agent_state_t pw_agent_state_from_string(const char *str)
{
    for (int i = 0; i < PW_STATE_COUNT; i++) {
        if (strcmp(STATE_STRINGS[i], str) == 0) return (pw_agent_state_t)i;
    }
    return PW_STATE_IDLE;
}

void pw_agent_state_set_person_present(bool present)
{
    s_state.person_present = present;
}

void pw_agent_state_tick(void)
{
    int64_t elapsed = now_ms() - s_state.state_entered_at_ms;

    if (s_state.current_state == PW_STATE_GREETING && elapsed > PW_GREETING_TIMEOUT_MS) {
        pw_agent_state_set(PW_STATE_IDLE);
    }
    if (s_state.current_state == PW_STATE_ALERT && elapsed > PW_ALERT_TIMEOUT_MS) {
        pw_agent_state_set(PW_STATE_IDLE);
    }
}

void pw_agent_state_set_change_cb(pw_state_change_cb_t cb)
{
    s_change_cb = cb;
}

static void agent_state_task(void *arg)
{
    ESP_LOGI(TAG, "Agent state task started");
    while (1) {
        pw_event_t event;
        if (pw_event_receive(&event, 1000)) {
            switch (event.type) {
                case PW_EVENT_PERSON_DETECTED:
                    pw_agent_state_set_person_present(true);
                    break;
                case PW_EVENT_PERSON_LEFT:
                    pw_agent_state_set_person_present(false);
                    break;
                default:
                    break;
            }
        }
        pw_agent_state_tick();
    }
}

void pw_agent_state_task_start(void)
{
    xTaskCreate(agent_state_task, "agent_state", 4096, NULL, 5, NULL);
}
```

- [ ] **Step 3: Commit**

```bash
git add pokewatcher/main/agent_state.h pokewatcher/main/agent_state.c
git commit -m "feat: add agent state engine replacing mood engine"
```

---

## Task 4: Create dialog module (FF9-style dialog box)

**Files:**
- Create: `pokewatcher/main/dialog.h`
- Create: `pokewatcher/main/dialog.c`

- [ ] **Step 1: Create dialog.h**

```c
#ifndef POKEWATCHER_DIALOG_H
#define POKEWATCHER_DIALOG_H

#include "event_queue.h"
#include "lvgl.h"
#include <stdbool.h>

void pw_dialog_init(lv_obj_t *parent);
void pw_dialog_show(const char *text, pw_msg_level_t level);
void pw_dialog_hide(void);
bool pw_dialog_is_visible(void);
void pw_dialog_tick(void);
const char *pw_dialog_get_last_text(void);

#endif
```

- [ ] **Step 2: Create dialog.c**

```c
#include "dialog.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>

static const char *TAG = "pw_dialog";

static lv_obj_t *s_dialog_container = NULL;
static lv_obj_t *s_dialog_label = NULL;
static lv_obj_t *s_name_label = NULL;
static bool s_visible = false;
static int64_t s_show_time_ms = 0;
static char s_last_text[PW_DIALOG_MAX_TEXT] = "";

// Border colors per message level
static const lv_color_t LEVEL_COLORS[] = {
    [PW_MSG_LEVEL_INFO]    = LV_COLOR_MAKE(0x55, 0x66, 0xAA),
    [PW_MSG_LEVEL_SUCCESS] = LV_COLOR_MAKE(0x55, 0xAA, 0x66),
    [PW_MSG_LEVEL_WARNING] = LV_COLOR_MAKE(0xAA, 0xAA, 0x55),
    [PW_MSG_LEVEL_ERROR]   = LV_COLOR_MAKE(0xAA, 0x55, 0x55),
};

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

void pw_dialog_init(lv_obj_t *parent)
{
    // Container: positioned within circular safe zone
    // 18% from bottom = 74px, 15% from sides = 62px on 412px display
    s_dialog_container = lv_obj_create(parent);
    lv_obj_set_size(s_dialog_container, 288, 70);
    lv_obj_align(s_dialog_container, LV_ALIGN_BOTTOM_MID, 0, -74);
    lv_obj_set_style_pad_all(s_dialog_container, 8, 0);
    lv_obj_set_style_radius(s_dialog_container, 8, 0);

    // Background: dark blue gradient
    lv_obj_set_style_bg_color(s_dialog_container, LV_COLOR_MAKE(0x0A, 0x0A, 0x2E), 0);
    lv_obj_set_style_bg_opa(s_dialog_container, LV_OPA_COVER, 0);

    // Border: default blue, 2px
    lv_obj_set_style_border_color(s_dialog_container, LV_COLOR_MAKE(0x55, 0x66, 0xAA), 0);
    lv_obj_set_style_border_width(s_dialog_container, 2, 0);

    // No scrollbar
    lv_obj_clear_flag(s_dialog_container, LV_OBJ_FLAG_SCROLLABLE);

    // Name label: "Zidane:" in green
    s_name_label = lv_label_create(s_dialog_container);
    lv_label_set_text(s_name_label, "Zidane:");
    lv_obj_set_style_text_color(s_name_label, LV_COLOR_MAKE(0x7B, 0xE8, 0x7B), 0);
    lv_obj_set_style_text_font(s_name_label, &lv_font_montserrat_14, 0);
    lv_obj_align(s_name_label, LV_ALIGN_TOP_LEFT, 4, 0);

    // Message text label: white
    s_dialog_label = lv_label_create(s_dialog_container);
    lv_label_set_text(s_dialog_label, "");
    lv_obj_set_style_text_color(s_dialog_label, LV_COLOR_MAKE(0xFF, 0xFF, 0xFF), 0);
    lv_obj_set_style_text_font(s_dialog_label, &lv_font_montserrat_12, 0);
    lv_obj_set_width(s_dialog_label, 260);
    lv_label_set_long_mode(s_dialog_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_dialog_label, LV_ALIGN_TOP_LEFT, 4, 20);

    // Start hidden
    lv_obj_add_flag(s_dialog_container, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;

    ESP_LOGI(TAG, "Dialog renderer initialized");
}

void pw_dialog_show(const char *text, pw_msg_level_t level)
{
    if (!s_dialog_container) return;

    strncpy(s_last_text, text, PW_DIALOG_MAX_TEXT - 1);
    s_last_text[PW_DIALOG_MAX_TEXT - 1] = '\0';

    lv_label_set_text(s_dialog_label, text);

    // Set border color per level
    if (level < 4) {
        lv_obj_set_style_border_color(s_dialog_container, LEVEL_COLORS[level], 0);
    }

    lv_obj_set_style_opa(s_dialog_container, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_dialog_container, LV_OBJ_FLAG_HIDDEN);
    s_visible = true;
    s_show_time_ms = now_ms();

    ESP_LOGI(TAG, "Dialog: [%s] %s",
        level == PW_MSG_LEVEL_SUCCESS ? "success" :
        level == PW_MSG_LEVEL_WARNING ? "warning" :
        level == PW_MSG_LEVEL_ERROR   ? "error"   : "info",
        text);
}

void pw_dialog_hide(void)
{
    if (!s_dialog_container || !s_visible) return;
    lv_obj_add_flag(s_dialog_container, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
}

bool pw_dialog_is_visible(void)
{
    return s_visible;
}

void pw_dialog_tick(void)
{
    if (!s_visible) return;

    int64_t elapsed = now_ms() - s_show_time_ms;

    if (elapsed > PW_DIALOG_DISPLAY_MS + PW_DIALOG_FADE_MS) {
        pw_dialog_hide();
    } else if (elapsed > PW_DIALOG_DISPLAY_MS) {
        // Fade out over PW_DIALOG_FADE_MS
        int64_t fade_elapsed = elapsed - PW_DIALOG_DISPLAY_MS;
        uint8_t opa = (uint8_t)(255 - (fade_elapsed * 255 / PW_DIALOG_FADE_MS));
        lv_obj_set_style_opa(s_dialog_container, opa, 0);
    }
}

const char *pw_dialog_get_last_text(void)
{
    return s_last_text;
}
```

- [ ] **Step 3: Commit**

```bash
git add pokewatcher/main/dialog.h pokewatcher/main/dialog.c
git commit -m "feat: add FF9-style dialog box renderer"
```

---

## Task 5: Update sprite_loader paths (pokemon → characters)

**Files:**
- Modify: `pokewatcher/main/sprite_loader.h`
- Modify: `pokewatcher/main/sprite_loader.c`

- [ ] **Step 1: Update sprite_loader.h**

Replace the `mood_anim_names` field (line 29) with agent state animation names:

```c
// In pw_sprite_data_t struct, replace:
    char mood_anim_names[6][PW_ANIM_NAME_LEN];
// with:
    char state_anim_names[PW_STATE_COUNT][PW_ANIM_NAME_LEN];
```

Update the function signature (line 37):

```c
// Replace:
const pw_animation_t *pw_sprite_get_mood_anim(const pw_sprite_data_t *sprite, pw_mood_t mood);
// with:
const pw_animation_t *pw_sprite_get_state_anim(const pw_sprite_data_t *sprite, pw_agent_state_t state);
```

- [ ] **Step 2: Update sprite_loader.c path references**

Replace all occurrences of `PW_SD_POKEMON_DIR` with `PW_SD_CHARACTER_DIR`. The path format changes from:
```
/sdcard/pokemon/{id}/frames.json
/sdcard/pokemon/{id}/sprite_sheet.raw
```
to:
```
/sdcard/characters/{id}/frames.json
/sdcard/characters/{id}/sprite_sheet.raw
```

Also update `pw_sprite_get_mood_anim` to `pw_sprite_get_state_anim` — same lookup logic, just references `state_anim_names` instead of `mood_anim_names`, and takes `pw_agent_state_t` instead of `pw_mood_t`.

Update the mood animation name mapping in the JSON parsing to map agent states instead:
```c
// In load_frame_manifest, replace mood_anim_map entries with:
static const char *state_anim_map[] = {
    [PW_STATE_IDLE]      = "idle_down",
    [PW_STATE_WORKING]   = "walk_down",
    [PW_STATE_WAITING]   = "idle_down",
    [PW_STATE_ALERT]     = "walk_down",
    [PW_STATE_GREETING]  = "greeting",
    [PW_STATE_SLEEPING]  = "sleeping",
    [PW_STATE_REPORTING] = "idle_down",
};
for (int i = 0; i < PW_STATE_COUNT; i++) {
    strncpy(sprite->state_anim_names[i], state_anim_map[i], PW_ANIM_NAME_LEN - 1);
}
```

- [ ] **Step 3: Commit**

```bash
git add pokewatcher/main/sprite_loader.h pokewatcher/main/sprite_loader.c
git commit -m "refactor: update sprite loader for agent states and character paths"
```

---

## Task 6: Refactor renderer (agent states + dialog integration)

**Files:**
- Modify: `pokewatcher/main/renderer.h`
- Modify: `pokewatcher/main/renderer.c`

- [ ] **Step 1: Update renderer.h**

```c
#ifndef POKEWATCHER_RENDERER_H
#define POKEWATCHER_RENDERER_H

#include "event_queue.h"
#include "sprite_loader.h"
#include <stdbool.h>

void pw_renderer_init(void);
bool pw_renderer_load_character(const char *character_id);
void pw_renderer_set_state(pw_agent_state_t state);
void pw_renderer_show_message(const char *text, pw_msg_level_t level);
void pw_renderer_wake_display(void);
void pw_renderer_task_start(void);

#endif
```

- [ ] **Step 2: Update renderer.c — replace mood tables with agent state tables**

Replace `MOOD_BG_COLORS` array (lines 31-38) with:

```c
static const uint32_t STATE_BG_COLORS[] = {
    [PW_STATE_IDLE]      = 0xFDE8C8,
    [PW_STATE_WORKING]   = 0xE8F0E8,
    [PW_STATE_WAITING]   = 0xE8E0F0,
    [PW_STATE_ALERT]     = 0x402020,
    [PW_STATE_GREETING]  = 0xFFF0C0,
    [PW_STATE_SLEEPING]  = 0x404060,
    [PW_STATE_REPORTING] = 0xFDE8C8,
};
```

Replace `MOOD_BEHAVIORS` array (lines 71-78) with:

```c
static const mood_behavior_t STATE_BEHAVIORS[] = {
    [PW_STATE_IDLE]      = { .walk_chance = 30, .turn_chance = 20, .walk_steps_min = 6,  .walk_steps_max = 14, .speed_x10 = 15, .bounce_amp = 0, .pause_min = 30,  .pause_max = 80  },
    [PW_STATE_WORKING]   = { .walk_chance = 50, .turn_chance = 30, .walk_steps_min = 8,  .walk_steps_max = 16, .speed_x10 = 15, .bounce_amp = 0, .pause_min = 20,  .pause_max = 50  },
    [PW_STATE_WAITING]   = { .walk_chance = 5,  .turn_chance = 15, .walk_steps_min = 2,  .walk_steps_max = 5,  .speed_x10 = 8,  .bounce_amp = 0, .pause_min = 80,  .pause_max = 200 },
    [PW_STATE_ALERT]     = { .walk_chance = 80, .turn_chance = 50, .walk_steps_min = 10, .walk_steps_max = 25, .speed_x10 = 30, .bounce_amp = 4, .pause_min = 5,   .pause_max = 15  },
    [PW_STATE_GREETING]  = { .walk_chance = 60, .turn_chance = 40, .walk_steps_min = 8,  .walk_steps_max = 20, .speed_x10 = 25, .bounce_amp = 3, .pause_min = 10,  .pause_max = 30  },
    [PW_STATE_SLEEPING]  = { .walk_chance = 3,  .turn_chance = 5,  .walk_steps_min = 2,  .walk_steps_max = 5,  .speed_x10 = 5,  .bounce_amp = 0, .pause_min = 100, .pause_max = 250 },
    [PW_STATE_REPORTING] = { .walk_chance = 10, .turn_chance = 10, .walk_steps_min = 3,  .walk_steps_max = 6,  .speed_x10 = 10, .bounce_amp = 0, .pause_min = 50,  .pause_max = 100 },
};
```

- [ ] **Step 3: Update renderer functions**

Replace `pw_renderer_set_mood(pw_mood_t mood)` with `pw_renderer_set_state(pw_agent_state_t state)`. Same logic — updates background color from `STATE_BG_COLORS`, updates current behavior from `STATE_BEHAVIORS`, sets animation from `pw_sprite_get_state_anim`.

Replace `pw_renderer_load_pokemon` with `pw_renderer_load_character`. Same logic, just renamed.

Remove `pw_renderer_play_evolution` (no evolution system).

Add `pw_renderer_show_message`:
```c
void pw_renderer_show_message(const char *text, pw_msg_level_t level)
{
    if (xSemaphoreTake(s_render_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        pw_dialog_show(text, level);
        xSemaphoreGive(s_render_mutex);
    }
}
```

- [ ] **Step 4: Add dialog tick to render loop**

In `renderer_task()`, inside the render loop, add `pw_dialog_tick()` call after `update_frame()`:

```c
// In renderer_task while loop, after update_frame():
pw_dialog_tick();
```

- [ ] **Step 5: Initialize dialog in pw_renderer_init**

After creating the LVGL screen objects, add:
```c
#include "dialog.h"

// At end of pw_renderer_init(), before ESP_LOGI:
pw_dialog_init(lv_scr_act());
```

- [ ] **Step 6: Commit**

```bash
git add pokewatcher/main/renderer.h pokewatcher/main/renderer.c
git commit -m "refactor: renderer uses agent states, integrates FF9 dialog"
```

---

## Task 7: Update web_server (new endpoints + mDNS rename)

**Files:**
- Modify: `pokewatcher/main/web_server.c`

- [ ] **Step 1: Update handle_api_status to return agent state**

Replace the current `handle_api_status` function body with:

```c
static esp_err_t handle_api_status(httpd_req_t *req)
{
    pw_agent_state_data_t state = pw_agent_state_get();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "agent_state", pw_agent_state_to_string(state.current_state));
    cJSON_AddBoolToObject(root, "person_present", state.person_present);
    cJSON_AddStringToObject(root, "last_message", pw_dialog_get_last_text());
    cJSON_AddNumberToObject(root, "uptime_seconds", (double)(esp_timer_get_time() / 1000000));

    // WiFi RSSI
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        cJSON_AddNumberToObject(root, "wifi_rssi", ap_info.rssi);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}
```

Add include at top of file:
```c
#include "agent_state.h"
#include "dialog.h"
#include "esp_wifi.h"
```

- [ ] **Step 2: Add PUT /api/agent-state handler**

```c
static esp_err_t handle_api_agent_state(httpd_req_t *req)
{
    char body[128];
    int ret = recv_full_body(req, body, sizeof(body));
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *state_json = cJSON_GetObjectItem(root, "state");
    if (!state_json || !cJSON_IsString(state_json)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'state' field");
        return ESP_FAIL;
    }

    pw_agent_state_t state = pw_agent_state_from_string(state_json->valuestring);
    pw_agent_state_set(state);
    cJSON_Delete(root);

    // Wake display on any state change
    pw_renderer_wake_display();

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "state", pw_agent_state_to_string(state));
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}
```

- [ ] **Step 3: Add POST /api/message handler**

```c
static esp_err_t handle_api_message(httpd_req_t *req)
{
    char body[256];
    int ret = recv_full_body(req, body, sizeof(body));
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing body");
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(body);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *text_json = cJSON_GetObjectItem(root, "text");
    if (!text_json || !cJSON_IsString(text_json)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'text' field");
        return ESP_FAIL;
    }

    pw_msg_level_t level = PW_MSG_LEVEL_INFO;
    cJSON *level_json = cJSON_GetObjectItem(root, "level");
    if (level_json && cJSON_IsString(level_json)) {
        const char *lvl = level_json->valuestring;
        if (strcmp(lvl, "success") == 0) level = PW_MSG_LEVEL_SUCCESS;
        else if (strcmp(lvl, "warning") == 0) level = PW_MSG_LEVEL_WARNING;
        else if (strcmp(lvl, "error") == 0) level = PW_MSG_LEVEL_ERROR;
    }

    pw_renderer_show_message(text_json->valuestring, level);
    pw_renderer_wake_display();
    cJSON_Delete(root);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}
```

- [ ] **Step 4: Update route registration**

In `register_routes()`, remove roster/settings/timeline routes and add the new ones:

```c
// Remove these routes:
//   /roster, /settings (pages)
//   /api/roster (GET, POST, DELETE)
//   /api/roster/active (PUT)
//   /api/settings (GET, PUT)
//   /api/timeline (GET)

// Add these routes:
httpd_uri_t agent_state_uri = { .uri = "/api/agent-state", .method = HTTP_PUT, .handler = handle_api_agent_state };
httpd_register_uri_handler(server, &agent_state_uri);

httpd_uri_t message_uri = { .uri = "/api/message", .method = HTTP_POST, .handler = handle_api_message };
httpd_register_uri_handler(server, &message_uri);
```

Keep: `/` (index), `/style.css`, `/app.js`, `/api/status`.

- [ ] **Step 5: Update mDNS hostname**

In `init_mdns()`, change:
```c
// Replace:
mdns_hostname_set("pokewatcher");
// With:
mdns_hostname_set("zidane");

// Replace:
mdns_instance_name_set("PokéWatcher Dashboard");
// With:
mdns_instance_name_set("Zidane Watcher");
```

- [ ] **Step 6: Remove unused includes**

Remove includes for `roster.h`, `llm_task.h`, `mood_engine.h`. Add `agent_state.h`, `dialog.h`.

- [ ] **Step 7: Commit**

```bash
git add pokewatcher/main/web_server.c
git commit -m "feat: add agent-state and message API endpoints, rename mDNS to zidane"
```

---

## Task 8: Update app_main.c (new init flow)

**Files:**
- Modify: `pokewatcher/main/app_main.c`

- [ ] **Step 1: Replace includes and callbacks**

Replace imports:
```c
// Remove:
#include "mood_engine.h"
#include "roster.h"
#include "llm_task.h"

// Add:
#include "agent_state.h"
```

Replace `on_mood_changed` callback with:
```c
static void on_state_changed(pw_agent_state_t old_state, pw_agent_state_t new_state)
{
    pw_renderer_set_state(new_state);
    pw_renderer_wake_display();
    ESP_LOGI(TAG, "State changed: %s -> %s",
             pw_agent_state_to_string(old_state),
             pw_agent_state_to_string(new_state));
}
```

Remove `on_roster_change` and `on_evolution_triggered` callbacks entirely.

- [ ] **Step 2: Update app_main init sequence**

The new sequence is simpler:

```c
void app_main(void)
{
    ESP_LOGI(TAG, "=== Zidane Watcher v2 starting ===");

    // [1/7] NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "[1/7] NVS initialized");

    // [2/7] Event queue
    pw_event_queue_init();
    ESP_LOGI(TAG, "[2/7] Event queue initialized");

    // [3/7] Agent state engine
    pw_agent_state_init();
    ESP_LOGI(TAG, "[3/7] Agent state initialized");

    // [4/7] IO expander + renderer
    ESP_ERROR_CHECK(bsp_io_expander_init());
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(TAG, "[4/7] IO expander ready, LCD powered");
    pw_renderer_init();
    ESP_LOGI(TAG, "[4/7] Renderer initialized");

    // [5/7] SD card
    init_sdcard();
    ESP_LOGI(TAG, "[5/7] SD card initialized");

    // [6/7] Load Zidane character
    if (!pw_renderer_load_character("zidane")) {
        ESP_LOGW(TAG, "Failed to load Zidane sprites from SD card");
    }

    // [7/7] WiFi + web server
    init_wifi();
    pw_web_server_start();
    ESP_LOGI(TAG, "[7/7] WiFi + web server initialized");

    // Register callbacks
    pw_agent_state_set_change_cb(on_state_changed);

    // Start tasks
    pw_himax_task_start();
    pw_agent_state_task_start();
    pw_renderer_task_start();

    ESP_LOGI(TAG, "=== Zidane Watcher v2 running ===");
    ESP_LOGI(TAG, "Dashboard: http://zidane.local");
}
```

- [ ] **Step 3: Commit**

```bash
git add pokewatcher/main/app_main.c
git commit -m "refactor: app_main uses agent state engine, loads Zidane character"
```

---

## Task 9: Update CMakeLists.txt and remove old files

**Files:**
- Modify: `pokewatcher/main/CMakeLists.txt`
- Delete: `pokewatcher/main/mood_engine.h`, `pokewatcher/main/mood_engine.c`
- Delete: `pokewatcher/main/llm_task.h`, `pokewatcher/main/llm_task.c`
- Delete: `pokewatcher/main/roster.h`, `pokewatcher/main/roster.c`

- [ ] **Step 1: Update CMakeLists.txt SRCS**

```cmake
idf_component_register(
    SRCS
        "app_main.c"
        "event_queue.c"
        "himax_task.c"
        "agent_state.c"
        "renderer.c"
        "sprite_loader.c"
        "dialog.c"
        "web_server.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_http_server
        esp_http_client
        esp_lcd_touch_chsc6x
        esp_lvgl_port
        lvgl
        sscma_client
        esp_jpeg_simd
        json
        fatfs
        sdmmc
        mdns
        sensecap-watcher
        esp_io_expander_pca95xx_16bit
        esp_timer
        driver
    EMBED_FILES
        "web/index.html"
        "web/style.css"
        "web/app.js"
)
```

Note: Removed `mood_engine.c`, `llm_task.c`, `roster.c` from SRCS. Removed `web/roster.html` and `web/settings.html` from EMBED_FILES.

- [ ] **Step 2: Delete old files**

```bash
git rm pokewatcher/main/mood_engine.h pokewatcher/main/mood_engine.c
git rm pokewatcher/main/llm_task.h pokewatcher/main/llm_task.c
git rm pokewatcher/main/roster.h pokewatcher/main/roster.c
```

- [ ] **Step 3: Commit**

```bash
git add pokewatcher/main/CMakeLists.txt
git commit -m "refactor: remove mood engine, LLM task, roster — replaced by agent state"
```

---

## Task 10: Prepare SD card with Zidane character placeholder

**Files:**
- Create: `assets/characters/zidane/frames.json` (template for SD card)

- [ ] **Step 1: Create placeholder frame manifest**

Create `assets/characters/zidane/frames.json` in the repo (copied to SD card manually):

```json
{
  "frame_width": 32,
  "frame_height": 32,
  "animations": {
    "idle_down":  { "frames": [{"x": 0, "y": 0}, {"x": 32, "y": 0}], "loop": true },
    "idle_up":    { "frames": [{"x": 64, "y": 0}, {"x": 96, "y": 0}], "loop": true },
    "idle_left":  { "frames": [{"x": 128, "y": 0}, {"x": 160, "y": 0}], "loop": true },
    "idle_right": { "frames": [{"x": 192, "y": 0}, {"x": 224, "y": 0}], "loop": true },
    "walk_down":  { "frames": [{"x": 0, "y": 32}, {"x": 32, "y": 32}, {"x": 64, "y": 32}, {"x": 96, "y": 32}], "loop": true },
    "walk_up":    { "frames": [{"x": 0, "y": 64}, {"x": 32, "y": 64}, {"x": 64, "y": 64}, {"x": 96, "y": 64}], "loop": true },
    "walk_left":  { "frames": [{"x": 0, "y": 96}, {"x": 32, "y": 96}, {"x": 64, "y": 96}, {"x": 96, "y": 96}], "loop": true },
    "walk_right": { "frames": [{"x": 0, "y": 128}, {"x": 32, "y": 128}, {"x": 64, "y": 128}, {"x": 96, "y": 128}], "loop": true },
    "greeting":   { "frames": [{"x": 0, "y": 160}, {"x": 32, "y": 160}, {"x": 64, "y": 160}, {"x": 96, "y": 160}], "loop": false },
    "sleeping":   { "frames": [{"x": 0, "y": 192}, {"x": 32, "y": 192}], "loop": true }
  }
}
```

This will be populated with real sprite data once we have the Zidane sprite sheet. For now, the Pikachu sprite can be copied as a stand-in:

```bash
# On SD card: copy pikachu as placeholder
mkdir -p /sdcard/characters/zidane
cp /sdcard/pokemon/pikachu/sprite_sheet.raw /sdcard/characters/zidane/
cp /sdcard/pokemon/pikachu/frames.json /sdcard/characters/zidane/
```

- [ ] **Step 2: Commit**

```bash
git add assets/characters/zidane/frames.json
git commit -m "feat: add Zidane character frame manifest template"
```

---

## Task 11: Build and flash firmware

- [ ] **Step 1: Sync to build directory**

```bash
rsync -a --exclude=build --exclude=.cache --exclude=sdkconfig \
  "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher/" \
  /tmp/pokewatcher-build/
```

- [ ] **Step 2: Clean build (sdkconfig changed)**

```bash
rm -rf /tmp/pokewatcher-build/build /tmp/pokewatcher-build/sdkconfig
```

- [ ] **Step 3: Build**

```bash
export IDF_PATH="/Users/nacoleon/esp/esp-idf"
. "$IDF_PATH/export.sh"
cd /tmp/pokewatcher-build
idf.py build
```

Fix any compilation errors — the most likely issues:
- Missing includes (check each new file includes what it uses)
- Type mismatches (old `pw_mood_t` references remaining)
- Missing function declarations

- [ ] **Step 4: Prepare SD card**

Copy Pikachu sprite as Zidane placeholder:
```bash
# Mount SD card or prepare files to copy
mkdir -p /Volumes/SDCARD/characters/zidane
cp /Volumes/SDCARD/pokemon/pikachu/* /Volumes/SDCARD/characters/zidane/
```

- [ ] **Step 5: Flash**

```bash
idf.py -p /dev/cu.usbmodem5A8A0533623 app-flash
```

- [ ] **Step 6: Verify boot via serial**

```bash
python3 -c "
import serial, time, sys
port = serial.Serial('/dev/cu.usbmodem5A8A0533623', 115200, timeout=1)
port.setDTR(False); port.setRTS(True); time.sleep(0.1); port.setRTS(False); time.sleep(5)
start = time.time()
while time.time() - start < 20:
    data = port.read(4096)
    if data: sys.stdout.write(data.decode('utf-8', errors='replace')); sys.stdout.flush()
port.close()
"
```

Expected: `=== Zidane Watcher v2 running ===`, WiFi connected, `http://zidane.local`.

- [ ] **Step 7: Test new endpoints**

```bash
# Test status
curl http://zidane.local/api/status

# Test set state
curl -X PUT http://zidane.local/api/agent-state -H "Content-Type: application/json" -d '{"state":"working"}'

# Test message
curl -X POST http://zidane.local/api/message -H "Content-Type: application/json" -d '{"text":"Hello from the bridge!","level":"success"}'
```

- [ ] **Step 8: Commit any build fixes**

```bash
git add -A
git commit -m "fix: resolve build issues from firmware refactor"
```

---

## Task 12: Create Watcher Bridge (Node.js)

**Files:**
- Create: `bridge/package.json`
- Create: `bridge/tsconfig.json`
- Create: `bridge/config.json`
- Create: `bridge/src/config.ts`
- Create: `bridge/src/watcher-client.ts`
- Create: `bridge/src/presence.ts`
- Create: `bridge/src/events.ts`
- Create: `bridge/src/index.ts`

- [ ] **Step 1: Create package.json**

```json
{
  "name": "watcher-bridge",
  "version": "1.0.0",
  "description": "Bridge between OpenClaw Zidane agent and SenseCap Watcher",
  "main": "dist/index.js",
  "scripts": {
    "build": "tsc",
    "start": "node dist/index.js",
    "dev": "npx tsx src/index.ts"
  },
  "dependencies": {
    "express": "^4.18.0"
  },
  "devDependencies": {
    "@types/express": "^4.17.0",
    "@types/node": "^20.0.0",
    "typescript": "^5.0.0",
    "tsx": "^4.0.0"
  }
}
```

- [ ] **Step 2: Create tsconfig.json**

```json
{
  "compilerOptions": {
    "target": "ES2022",
    "module": "commonjs",
    "outDir": "dist",
    "rootDir": "src",
    "strict": true,
    "esModuleInterop": true,
    "resolveJsonModule": true
  },
  "include": ["src/**/*"]
}
```

- [ ] **Step 3: Create config.json**

```json
{
  "watcher_url": "http://zidane.local",
  "bridge_port": 3847,
  "poll_interval_ms": 5000,
  "debounce_count": 2,
  "events_file": "~/.openclaw/watcher-events.json",
  "context_file": "~/.openclaw/watcher-context.json"
}
```

- [ ] **Step 4: Create src/config.ts**

```typescript
import { readFileSync } from "fs";
import { resolve } from "path";
import { homedir } from "os";

export interface BridgeConfig {
  watcher_url: string;
  bridge_port: number;
  poll_interval_ms: number;
  debounce_count: number;
  events_file: string;
  context_file: string;
}

export function loadConfig(): BridgeConfig {
  const raw = readFileSync(resolve(__dirname, "../config.json"), "utf-8");
  const config: BridgeConfig = JSON.parse(raw);
  // Expand ~ to home directory
  config.events_file = config.events_file.replace("~", homedir());
  config.context_file = config.context_file.replace("~", homedir());
  return config;
}
```

- [ ] **Step 5: Create src/watcher-client.ts**

```typescript
import http from "http";

export interface WatcherStatus {
  agent_state: string;
  person_present: boolean;
  last_message: string;
  uptime_seconds: number;
  wifi_rssi?: number;
}

export class WatcherClient {
  constructor(private baseUrl: string) {}

  private request(method: string, path: string, body?: object): Promise<any> {
    return new Promise((resolve, reject) => {
      const url = new URL(path, this.baseUrl);
      const data = body ? JSON.stringify(body) : undefined;

      const req = http.request(
        {
          hostname: url.hostname,
          port: url.port || 80,
          path: url.pathname,
          method,
          headers: data
            ? { "Content-Type": "application/json", "Content-Length": Buffer.byteLength(data) }
            : {},
          timeout: 5000,
        },
        (res) => {
          let chunks: Buffer[] = [];
          res.on("data", (chunk) => chunks.push(chunk));
          res.on("end", () => {
            const text = Buffer.concat(chunks).toString();
            try {
              resolve(JSON.parse(text));
            } catch {
              resolve(text);
            }
          });
        }
      );
      req.on("error", reject);
      req.on("timeout", () => { req.destroy(); reject(new Error("timeout")); });
      if (data) req.write(data);
      req.end();
    });
  }

  async getStatus(): Promise<WatcherStatus> {
    return this.request("GET", "/api/status");
  }

  async setState(state: string): Promise<any> {
    return this.request("PUT", "/api/agent-state", { state });
  }

  async sendMessage(text: string, level: string = "info"): Promise<any> {
    return this.request("POST", "/api/message", { text, level });
  }
}
```

- [ ] **Step 6: Create src/events.ts**

```typescript
import { appendFileSync, writeFileSync, mkdirSync } from "fs";
import { dirname } from "path";

export class EventWriter {
  constructor(
    private eventsFile: string,
    private contextFile: string
  ) {
    mkdirSync(dirname(eventsFile), { recursive: true });
    mkdirSync(dirname(contextFile), { recursive: true });
  }

  writeEvent(type: string, data: Record<string, any>): void {
    const event = { type, ...data, timestamp: new Date().toISOString() };
    appendFileSync(this.eventsFile, JSON.stringify(event) + "\n");
  }

  writeContext(context: Record<string, any>): void {
    const data = { ...context, updated_at: new Date().toISOString() };
    writeFileSync(this.contextFile, JSON.stringify(data, null, 2));
  }
}
```

- [ ] **Step 7: Create src/presence.ts**

```typescript
import { WatcherClient, WatcherStatus } from "./watcher-client";
import { EventWriter } from "./events";

export class PresencePoller {
  private lastPresent: boolean | null = null;
  private debounceCounter = 0;
  private interval: NodeJS.Timeout | null = null;
  private startTime = Date.now();

  constructor(
    private client: WatcherClient,
    private events: EventWriter,
    private pollIntervalMs: number,
    private debounceCount: number
  ) {}

  start(): void {
    this.interval = setInterval(() => this.poll(), this.pollIntervalMs);
    console.log(`[presence] Polling every ${this.pollIntervalMs}ms`);
  }

  stop(): void {
    if (this.interval) clearInterval(this.interval);
  }

  private async poll(): Promise<void> {
    try {
      const status: WatcherStatus = await this.client.getStatus();

      // Update context file every cycle
      this.events.writeContext({
        person_present: status.person_present,
        agent_state: status.agent_state,
        last_message: status.last_message,
        uptime_seconds: status.uptime_seconds,
        wifi_rssi: status.wifi_rssi,
        bridge_uptime_seconds: Math.floor((Date.now() - this.startTime) / 1000),
      });

      // Debounced presence change detection
      if (this.lastPresent === null) {
        this.lastPresent = status.person_present;
        return;
      }

      if (status.person_present !== this.lastPresent) {
        this.debounceCounter++;
        if (this.debounceCounter >= this.debounceCount) {
          this.lastPresent = status.person_present;
          this.debounceCounter = 0;
          this.events.writeEvent("presence_changed", { present: status.person_present });
          console.log(`[presence] Changed: person_present=${status.person_present}`);
        }
      } else {
        this.debounceCounter = 0;
      }
    } catch (err: any) {
      console.error(`[presence] Poll failed: ${err.message}`);
    }
  }
}
```

- [ ] **Step 8: Create src/index.ts**

```typescript
import express from "express";
import { loadConfig } from "./config";
import { WatcherClient } from "./watcher-client";
import { EventWriter } from "./events";
import { PresencePoller } from "./presence";

const config = loadConfig();
const client = new WatcherClient(config.watcher_url);
const events = new EventWriter(config.events_file, config.context_file);
const poller = new PresencePoller(client, events, config.poll_interval_ms, config.debounce_count);

const app = express();
app.use(express.json());

// Tool endpoints for Zidane

app.post("/tools/display_message", async (req, res) => {
  try {
    const { text, level } = req.body;
    if (!text) return res.status(400).json({ error: "Missing 'text'" });
    const result = await client.sendMessage(text, level || "info");
    res.json(result);
  } catch (err: any) {
    res.status(502).json({ error: err.message });
  }
});

app.post("/tools/set_state", async (req, res) => {
  try {
    const { state } = req.body;
    if (!state) return res.status(400).json({ error: "Missing 'state'" });
    const result = await client.setState(state);
    res.json(result);
  } catch (err: any) {
    res.status(502).json({ error: err.message });
  }
});

app.get("/tools/get_status", async (_req, res) => {
  try {
    const status = await client.getStatus();
    res.json(status);
  } catch (err: any) {
    res.status(502).json({ error: err.message });
  }
});

app.post("/tools/notify", async (req, res) => {
  try {
    const { text, level, state } = req.body;
    if (!text) return res.status(400).json({ error: "Missing 'text'" });
    if (state) await client.setState(state);
    const result = await client.sendMessage(text, level || "info");
    res.json(result);
  } catch (err: any) {
    res.status(502).json({ error: err.message });
  }
});

// Health check
app.get("/health", (_req, res) => res.json({ ok: true }));

// Start
app.listen(config.bridge_port, () => {
  console.log(`[bridge] Watcher Bridge running on port ${config.bridge_port}`);
  console.log(`[bridge] Watcher URL: ${config.watcher_url}`);
  poller.start();
});
```

- [ ] **Step 9: Install dependencies and test**

```bash
cd bridge
npm install
npx tsx src/index.ts
```

In another terminal:
```bash
# Test bridge → watcher
curl http://localhost:3847/tools/get_status
curl -X POST http://localhost:3847/tools/set_state -H "Content-Type: application/json" -d '{"state":"working"}'
curl -X POST http://localhost:3847/tools/display_message -H "Content-Type: application/json" -d '{"text":"Bridge test!","level":"success"}'
curl -X POST http://localhost:3847/tools/notify -H "Content-Type: application/json" -d '{"text":"Task done","level":"success","state":"reporting"}'

# Check context file
cat ~/.openclaw/watcher-context.json

# Check events file
cat ~/.openclaw/watcher-events.json
```

- [ ] **Step 10: Commit**

```bash
git add bridge/
git commit -m "feat: add Watcher Bridge — Node.js tool API + presence poller"
```

---

## Task 13: Register Zidane tools in OpenClaw workspace

**Files:**
- Modify: `~/clawd/TOOLS.md`
- Modify: `~/clawd/HEARTBEAT.md`

- [ ] **Step 1: Add watcher tools to TOOLS.md**

Append to `~/clawd/TOOLS.md`:

```markdown
## Watcher (Desk Companion)

Physical desk device running your avatar on a circular LCD. Controls via bridge at localhost:3847.

### watcher.display_message
Show a FF9-style dialog box on the Watcher screen. Auto-dismisses after 10s.
- POST http://localhost:3847/tools/display_message
- Body: {"text": "string (max 80 chars)", "level": "info|success|warning|error"}
- Use for: task completions, status updates, greetings

### watcher.set_state
Change your visual state on the Watcher.
- POST http://localhost:3847/tools/set_state
- Body: {"state": "idle|working|waiting|alert|greeting|sleeping|reporting"}
- Use for: reflecting what you're currently doing

### watcher.get_status
Read the Watcher's current state and sensor data.
- GET http://localhost:3847/tools/get_status
- Returns: {person_present, agent_state, last_message, uptime_seconds, wifi_rssi}

### watcher.notify
Convenience: set state + show message in one call.
- POST http://localhost:3847/tools/notify
- Body: {"text": "string", "level": "info|success|warning|error", "state": "reporting"}
```

- [ ] **Step 2: Add desk context to HEARTBEAT.md**

Append to `~/clawd/HEARTBEAT.md`:

```markdown
## Desk Context

Read ~/.openclaw/watcher-context.json for physical desk state.

- If person_present is true, Manny is at his desk — prefer watcher.notify for updates.
- If person_present is false, skip watcher messages (he won't see them).
- Check ~/.openclaw/watcher-events.json for presence changes since last heartbeat. Clear after reading.

On person arrival:
  1. watcher.set_state("greeting")
  2. watcher.display_message with a short greeting + summary of recent work
  3. After 10s, watcher.set_state("idle") or "reporting" if there's news

On person departure:
  1. watcher.set_state("sleeping")

During active work:
  - watcher.set_state("working") when executing tasks
  - watcher.set_state("idle") when between tasks
  - watcher.notify on task completion (level: "success")
  - watcher.notify on failures (level: "error", state: "alert")

Message discipline:
  - Max 1 message per 5 minutes unless ALERT
  - Keep messages to 1 line when possible
  - Only use ALERT for failures or things needing immediate attention
```

- [ ] **Step 3: Commit** (in clawd repo)

```bash
cd ~/clawd
git add TOOLS.md HEARTBEAT.md
git commit -m "feat: add Watcher desk companion tool definitions"
```

---

## Task 14: Create LaunchAgent for bridge

**Files:**
- Create: `bridge/ai.openclaw.watcher-bridge.plist`

- [ ] **Step 1: Create LaunchAgent plist**

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>ai.openclaw.watcher-bridge</string>
    <key>ProgramArguments</key>
    <array>
        <string>/opt/homebrew/bin/node</string>
        <string>/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/bridge/dist/index.js</string>
    </array>
    <key>WorkingDirectory</key>
    <string>/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/bridge</string>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>/Users/nacoleon/.openclaw/logs/watcher-bridge.log</string>
    <key>StandardErrorPath</key>
    <string>/Users/nacoleon/.openclaw/logs/watcher-bridge.log</string>
</dict>
</plist>
```

- [ ] **Step 2: Build and install**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/bridge"
npm run build
mkdir -p ~/.openclaw/logs
cp ai.openclaw.watcher-bridge.plist ~/Library/LaunchAgents/
launchctl load ~/Library/LaunchAgents/ai.openclaw.watcher-bridge.plist
```

- [ ] **Step 3: Verify running**

```bash
launchctl list | grep watcher-bridge
curl http://localhost:3847/health
tail -20 ~/.openclaw/logs/watcher-bridge.log
```

- [ ] **Step 4: Commit**

```bash
git add bridge/ai.openclaw.watcher-bridge.plist
git commit -m "feat: add LaunchAgent plist for bridge auto-start"
```

---

## Task 15: End-to-end integration test

- [ ] **Step 1: Verify full pipeline**

```bash
# 1. Check Watcher is online
curl http://zidane.local/api/status

# 2. Check bridge is running
curl http://localhost:3847/health

# 3. Set state via bridge
curl -X POST http://localhost:3847/tools/set_state -H "Content-Type: application/json" -d '{"state":"working"}'
# Verify Watcher display changes to mint green background

# 4. Send message via bridge
curl -X POST http://localhost:3847/tools/display_message -H "Content-Type: application/json" -d '{"text":"Integration test passed!","level":"success"}'
# Verify FF dialog appears on Watcher screen for 10s

# 5. Check presence context
cat ~/.openclaw/watcher-context.json
# Should show person_present, agent_state, etc.

# 6. Test notify (combined)
curl -X POST http://localhost:3847/tools/notify -H "Content-Type: application/json" -d '{"text":"All systems go","level":"info","state":"idle"}'

# 7. Test all states
for state in idle working waiting alert greeting sleeping reporting; do
  curl -s -X POST http://localhost:3847/tools/set_state -H "Content-Type: application/json" -d "{\"state\":\"$state\"}"
  sleep 3
done
```

- [ ] **Step 2: Verify person detection flow**

Walk in front of the camera, then walk away. Check:
```bash
# After a few seconds, events file should have entries
cat ~/.openclaw/watcher-events.json
# Should see presence_changed events
```

- [ ] **Step 3: Final commit if any fixes needed**

```bash
git add -A
git commit -m "fix: integration test fixes"
```
