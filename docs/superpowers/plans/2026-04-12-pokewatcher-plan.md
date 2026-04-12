# PokéWatcher v1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an AI-driven Pokemon companion that lives on the SenseCAP Watcher's round display, reacting to person detection with mood-based animations and LLM-generated personality commentary.

**Architecture:** Multi-task event-driven FreeRTOS app forked from SenseCAP Watcher SDK. Five tasks (Himax, LLM, Mood Engine, Renderer, Web Server) communicate through a central event queue. Overworld sprites from HGSS rendered via LVGL on the 412×412 circular display.

**Tech Stack:** ESP-IDF v5.2.1, C, FreeRTOS, LVGL 8.3.3, sscma_client component, ESP HTTP server/client, NVS, FatFS/SDMMC

---

## File Structure

```
pokewatcher/                          # ESP-IDF project root
├── CMakeLists.txt                    # Top-level CMake (references components + main)
├── partitions.csv                    # Flash partition table (copied from factory_firmware)
├── sdkconfig.defaults                # SDK config defaults for ESP32-S3
├── main/
│   ├── CMakeLists.txt                # Main component build file
│   ├── app_main.c                    # Entry point: init hardware, start tasks
│   ├── event_queue.h                 # Mood event types and queue API
│   ├── event_queue.c                 # FreeRTOS queue implementation
│   ├── himax_task.h                  # Himax person detection task API
│   ├── himax_task.c                  # UART reading, event parsing, queue push
│   ├── mood_engine.h                 # Mood state machine API
│   ├── mood_engine.c                 # State transitions, timers, evolution tracking
│   ├── renderer.h                    # LVGL renderer API
│   ├── renderer.c                    # Sprite loading, animation, background, display
│   ├── sprite_loader.h              # Sprite sheet parsing API
│   ├── sprite_loader.c              # PNG decode, frame manifest JSON parse
│   ├── llm_task.h                    # LLM personality engine API
│   ├── llm_task.c                    # HTTP client, prompt composition, response parse
│   ├── web_server.h                  # Web dashboard API
│   ├── web_server.c                  # HTTP server, REST endpoints, static files
│   ├── roster.h                      # Pokemon roster management API
│   ├── roster.c                      # NVS persistence, SD card sprite discovery
│   ├── config.h                      # Global config constants and defaults
│   └── web/                          # Static web assets (embedded in flash)
│       ├── index.html                # Dashboard home/status page
│       ├── roster.html               # Roster management page
│       ├── settings.html             # Settings page
│       ├── style.css                 # Shared styles
│       └── app.js                    # Shared JS (fetch API, roster interactions)
├── sdcard/                           # Reference SD card layout (not flashed)
│   ├── pokemon/
│   │   ├── pikachu/
│   │   │   ├── pokemon.json          # Pokemon definition (name, evolves_to, etc.)
│   │   │   ├── overworld.png         # Sprite sheet PNG
│   │   │   └── frames.json           # Frame coordinates and animation sequences
│   │   ├── charmander/
│   │   │   ├── pokemon.json
│   │   │   ├── overworld.png
│   │   │   └── frames.json
│   │   └── ...
│   └── background/
│       └── grass_scene.png           # Background scene image (412×412)
└── test/                             # Host-based unit tests (Linux build)
    ├── CMakeLists.txt
    ├── test_event_queue.c
    ├── test_mood_engine.c
    └── test_roster.c
```

## Testing Strategy

ESP32 firmware testing uses a two-layer approach:

1. **Host-based unit tests** — Pure logic modules (event queue, mood engine, roster) compiled and tested on the host machine using ESP-IDF's Linux target or a simple test harness with `assert()`. No hardware required.
2. **On-device integration tests** — Flash to Watcher, verify via serial monitor (`idf.py monitor`). Check LVGL renders, Himax events arrive, web server responds, LLM calls succeed.

The plan uses host-based tests for logic and serial monitor verification for hardware integration.

---

### Task 1: Project Scaffold & Build System

**Files:**
- Create: `pokewatcher/CMakeLists.txt`
- Create: `pokewatcher/partitions.csv`
- Create: `pokewatcher/sdkconfig.defaults`
- Create: `pokewatcher/main/CMakeLists.txt`
- Create: `pokewatcher/main/app_main.c`
- Create: `pokewatcher/main/config.h`

- [ ] **Step 1: Create project directory structure**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher"
mkdir -p pokewatcher/main pokewatcher/test pokewatcher/sdcard/pokemon pokewatcher/sdcard/background pokewatcher/main/web
```

- [ ] **Step 2: Create top-level CMakeLists.txt**

Create `pokewatcher/CMakeLists.txt`:
```cmake
cmake_minimum_required(VERSION 3.16)

# Point to the Watcher SDK components
set(EXTRA_COMPONENT_DIRS
    "../SenseCAP-Watcher-Firmware/components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pokewatcher)
```

- [ ] **Step 3: Create partition table**

Create `pokewatcher/partitions.csv` (preserves nvsfactory at 0x9000):
```csv
# Name,       Type, SubType, Offset,     Size,       Flags
nvs,          data, nvs,     0x9000,     0x4000,
otadata,      data, ota,     0xd000,     0x2000,
phy_init,     data, phy,     0xf000,     0x1000,
nvsfactory,   data, nvs,     0x10000,    0x32000,
factory,      app,  factory, 0x110000,   0x300000,
storage,      data, spiffs,  0x410000,   0x100000,
```

- [ ] **Step 4: Create sdkconfig.defaults**

Create `pokewatcher/sdkconfig.defaults`:
```
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
CONFIG_ESP_DEFAULT_CPU_FREQ_240=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
CONFIG_LV_COLOR_DEPTH_16=y
CONFIG_LV_HOR_RES_MAX=412
CONFIG_LV_VER_RES_MAX=412
CONFIG_HTTPD_MAX_REQ_HDR_LEN=1024
CONFIG_HTTPD_MAX_URI_LEN=512
CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y
CONFIG_LWIP_MAX_SOCKETS=16
```

- [ ] **Step 5: Create config.h with project constants**

Create `pokewatcher/main/config.h`:
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

// Mood timers (milliseconds)
#define PW_EXCITED_DURATION_MS     10000   // 10 seconds
#define PW_OVERJOYED_DURATION_MS   15000   // 15 seconds
#define PW_CURIOUS_TIMEOUT_MS      300000  // 5 minutes
#define PW_LONELY_TIMEOUT_MS       900000  // 15 minutes

// Evolution
#define PW_DEFAULT_EVOLUTION_HOURS 24

// LLM
#define PW_LLM_MAX_RESPONSE_LEN   512

// Event queue
#define PW_EVENT_QUEUE_SIZE        16

// Web server
#define PW_WEB_SERVER_PORT         80

// SD card paths
#define PW_SD_MOUNT_POINT          "/sdcard"
#define PW_SD_POKEMON_DIR          "/sdcard/pokemon"
#define PW_SD_BACKGROUND_DIR       "/sdcard/background"

// NVS namespace
#define PW_NVS_NAMESPACE           "pokewatcher"

#endif
```

- [ ] **Step 6: Create main component CMakeLists.txt**

Create `pokewatcher/main/CMakeLists.txt`:
```cmake
idf_component_register(
    SRCS
        "app_main.c"
        "event_queue.c"
        "himax_task.c"
        "mood_engine.c"
        "renderer.c"
        "sprite_loader.c"
        "llm_task.c"
        "web_server.c"
        "roster.c"
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
    EMBED_FILES
        "web/index.html"
        "web/roster.html"
        "web/settings.html"
        "web/style.css"
        "web/app.js"
)
```

- [ ] **Step 7: Create minimal app_main.c stub**

Create `pokewatcher/main/app_main.c`:
```c
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "config.h"

static const char *TAG = "pokewatcher";

void app_main(void)
{
    ESP_LOGI(TAG, "PokéWatcher v1 starting...");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "NVS initialized");
    ESP_LOGI(TAG, "PokéWatcher scaffold running. All systems stub.");

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

- [ ] **Step 8: Verify the project compiles**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher"
idf.py set-target esp32s3
idf.py build
```

Expected: Build succeeds (or fails only on missing components from SDK — we'll fix paths). The goal is to verify CMake finds everything.

- [ ] **Step 9: Commit**

```bash
git init
git add CMakeLists.txt partitions.csv sdkconfig.defaults main/CMakeLists.txt main/app_main.c main/config.h
git commit -m "feat: project scaffold with build system, partition table, and config"
```

---

### Task 2: Event Queue System

**Files:**
- Create: `pokewatcher/main/event_queue.h`
- Create: `pokewatcher/main/event_queue.c`
- Create: `pokewatcher/test/test_event_queue.c`

- [ ] **Step 1: Write the event queue header**

Create `pokewatcher/main/event_queue.h`:
```c
#ifndef POKEWATCHER_EVENT_QUEUE_H
#define POKEWATCHER_EVENT_QUEUE_H

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "config.h"

typedef enum {
    PW_EVENT_PERSON_DETECTED,
    PW_EVENT_PERSON_LEFT,
    PW_EVENT_LLM_COMMENTARY,
    PW_EVENT_ROSTER_CHANGE,
    PW_EVENT_MOOD_CHANGED,
    PW_EVENT_EVOLUTION_TRIGGERED,
} pw_event_type_t;

typedef enum {
    PW_MOOD_EXCITED,
    PW_MOOD_HAPPY,
    PW_MOOD_CURIOUS,
    PW_MOOD_LONELY,
    PW_MOOD_SLEEPY,
    PW_MOOD_OVERJOYED,
} pw_mood_t;

typedef struct {
    pw_event_type_t type;
    union {
        struct {
            pw_mood_t new_mood;
            pw_mood_t old_mood;
        } mood;
        struct {
            char commentary[PW_LLM_MAX_RESPONSE_LEN];
        } llm;
        struct {
            char pokemon_id[32];
        } roster;
    } data;
} pw_event_t;

/**
 * Initialize the global event queue.
 * Must be called before any other event_queue function.
 */
void pw_event_queue_init(void);

/**
 * Send an event to the queue. Non-blocking (drops if full).
 * Returns true if sent, false if queue was full.
 */
bool pw_event_send(const pw_event_t *event);

/**
 * Receive an event from the queue. Blocks for up to timeout_ms.
 * Returns true if an event was received.
 */
bool pw_event_receive(pw_event_t *event, uint32_t timeout_ms);

/**
 * Get the queue handle (for tasks that need direct access).
 */
QueueHandle_t pw_event_queue_handle(void);

#endif
```

- [ ] **Step 2: Write the event queue implementation**

Create `pokewatcher/main/event_queue.c`:
```c
#include "event_queue.h"
#include "esp_log.h"

static const char *TAG = "pw_event_queue";
static QueueHandle_t s_event_queue = NULL;

void pw_event_queue_init(void)
{
    s_event_queue = xQueueCreate(PW_EVENT_QUEUE_SIZE, sizeof(pw_event_t));
    if (s_event_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create event queue");
    } else {
        ESP_LOGI(TAG, "Event queue created (depth=%d)", PW_EVENT_QUEUE_SIZE);
    }
}

bool pw_event_send(const pw_event_t *event)
{
    if (s_event_queue == NULL) {
        ESP_LOGE(TAG, "Queue not initialized");
        return false;
    }
    BaseType_t ret = xQueueSend(s_event_queue, event, 0);
    if (ret != pdTRUE) {
        ESP_LOGW(TAG, "Event queue full, dropping event type=%d", event->type);
        return false;
    }
    return true;
}

bool pw_event_receive(pw_event_t *event, uint32_t timeout_ms)
{
    if (s_event_queue == NULL) {
        return false;
    }
    TickType_t ticks = (timeout_ms == UINT32_MAX) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xQueueReceive(s_event_queue, event, ticks) == pdTRUE;
}

QueueHandle_t pw_event_queue_handle(void)
{
    return s_event_queue;
}
```

- [ ] **Step 3: Verify build compiles with event queue**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher"
idf.py build
```

Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add main/event_queue.h main/event_queue.c
git commit -m "feat: add event queue system for inter-task communication"
```

---

### Task 3: Mood Engine (State Machine)

**Files:**
- Create: `pokewatcher/main/mood_engine.h`
- Create: `pokewatcher/main/mood_engine.c`

- [ ] **Step 1: Write the mood engine header**

Create `pokewatcher/main/mood_engine.h`:
```c
#ifndef POKEWATCHER_MOOD_ENGINE_H
#define POKEWATCHER_MOOD_ENGINE_H

#include "event_queue.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    pw_mood_t current_mood;
    pw_mood_t previous_mood;
    int64_t mood_entered_at_ms;       // When current mood started
    int64_t last_person_seen_ms;      // Last time a person was detected
    bool person_present;              // Is someone currently detected
    uint32_t evolution_seconds;       // Cumulative seconds as active companion
    int64_t evolution_last_tick_ms;   // Last time evolution timer was incremented
} pw_mood_state_t;

typedef struct {
    uint32_t excited_duration_ms;
    uint32_t overjoyed_duration_ms;
    uint32_t curious_timeout_ms;
    uint32_t lonely_timeout_ms;
    uint32_t evolution_threshold_hours;
} pw_mood_config_t;

/**
 * Initialize mood engine with default config.
 * Starts in SLEEPY state (nobody around yet).
 */
void pw_mood_engine_init(void);

/**
 * Process an incoming event and update mood state.
 * Returns true if the mood changed.
 */
bool pw_mood_engine_process_event(const pw_event_t *event);

/**
 * Tick the mood engine timers. Call this periodically (~1s).
 * Handles auto-transitions (excited→happy, curious→lonely, etc.).
 * Returns true if mood changed.
 */
bool pw_mood_engine_tick(void);

/**
 * Get current mood state (read-only snapshot).
 */
pw_mood_state_t pw_mood_engine_get_state(void);

/**
 * Get current config.
 */
pw_mood_config_t pw_mood_engine_get_config(void);

/**
 * Update config (from web dashboard).
 */
void pw_mood_engine_set_config(const pw_mood_config_t *config);

/**
 * Get mood name as string.
 */
const char *pw_mood_to_string(pw_mood_t mood);

/**
 * Callback type for mood changes. Called from the mood engine task.
 */
typedef void (*pw_mood_change_cb_t)(pw_mood_t old_mood, pw_mood_t new_mood);

/**
 * Register a callback for mood changes (used by coordinator).
 */
void pw_mood_engine_set_change_cb(pw_mood_change_cb_t cb);

/**
 * Start the mood engine FreeRTOS task.
 * Consumes events from the input queue, updates state, calls change callback.
 */
void pw_mood_engine_task_start(void);

#endif
```

- [ ] **Step 2: Write the mood engine implementation**

Create `pokewatcher/main/mood_engine.c`:
```c
#include "mood_engine.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "pw_mood";

static pw_mood_state_t s_state;
static pw_mood_config_t s_config;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void set_mood(pw_mood_t new_mood)
{
    if (s_state.current_mood == new_mood) {
        return;
    }
    ESP_LOGI(TAG, "Mood: %s → %s", pw_mood_to_string(s_state.current_mood), pw_mood_to_string(new_mood));
    s_state.previous_mood = s_state.current_mood;
    s_state.current_mood = new_mood;
    s_state.mood_entered_at_ms = now_ms();
    // Mood change notification is handled by the task loop via callback
}

void pw_mood_engine_init(void)
{
    int64_t now = now_ms();

    s_state = (pw_mood_state_t){
        .current_mood = PW_MOOD_SLEEPY,
        .previous_mood = PW_MOOD_SLEEPY,
        .mood_entered_at_ms = now,
        .last_person_seen_ms = 0,
        .person_present = false,
        .evolution_seconds = 0,
        .evolution_last_tick_ms = now,
    };

    s_config = (pw_mood_config_t){
        .excited_duration_ms = PW_EXCITED_DURATION_MS,
        .overjoyed_duration_ms = PW_OVERJOYED_DURATION_MS,
        .curious_timeout_ms = PW_CURIOUS_TIMEOUT_MS,
        .lonely_timeout_ms = PW_LONELY_TIMEOUT_MS,
        .evolution_threshold_hours = PW_DEFAULT_EVOLUTION_HOURS,
    };

    ESP_LOGI(TAG, "Mood engine initialized (starting in SLEEPY)");
}

bool pw_mood_engine_process_event(const pw_event_t *event)
{
    pw_mood_t old_mood = s_state.current_mood;

    switch (event->type) {
    case PW_EVENT_PERSON_DETECTED:
        s_state.person_present = true;
        s_state.last_person_seen_ms = now_ms();

        switch (s_state.current_mood) {
        case PW_MOOD_SLEEPY:
        case PW_MOOD_LONELY:
            set_mood(PW_MOOD_OVERJOYED);
            break;
        case PW_MOOD_CURIOUS:
            set_mood(PW_MOOD_HAPPY);
            break;
        case PW_MOOD_HAPPY:
        case PW_MOOD_EXCITED:
        case PW_MOOD_OVERJOYED:
            // Already in a positive state, stay
            break;
        }
        break;

    case PW_EVENT_PERSON_LEFT:
        s_state.person_present = false;
        if (s_state.current_mood == PW_MOOD_HAPPY ||
            s_state.current_mood == PW_MOOD_EXCITED) {
            set_mood(PW_MOOD_CURIOUS);
        }
        break;

    default:
        break;
    }

    return s_state.current_mood != old_mood;
}

bool pw_mood_engine_tick(void)
{
    int64_t now = now_ms();
    int64_t in_mood_ms = now - s_state.mood_entered_at_ms;
    pw_mood_t old_mood = s_state.current_mood;

    // Auto-transitions based on time
    switch (s_state.current_mood) {
    case PW_MOOD_EXCITED:
        if (in_mood_ms >= s_config.excited_duration_ms) {
            set_mood(PW_MOOD_HAPPY);
        }
        break;

    case PW_MOOD_OVERJOYED:
        if (in_mood_ms >= s_config.overjoyed_duration_ms) {
            set_mood(PW_MOOD_HAPPY);
        }
        break;

    case PW_MOOD_CURIOUS:
        if (!s_state.person_present && in_mood_ms >= s_config.curious_timeout_ms) {
            set_mood(PW_MOOD_LONELY);
        }
        break;

    case PW_MOOD_LONELY:
        if (!s_state.person_present && in_mood_ms >= s_config.lonely_timeout_ms) {
            set_mood(PW_MOOD_SLEEPY);
        }
        break;

    case PW_MOOD_HAPPY:
    case PW_MOOD_SLEEPY:
        // No auto-transition
        break;
    }

    // Tick evolution timer (1 second granularity)
    if (now - s_state.evolution_last_tick_ms >= 1000) {
        uint32_t elapsed = (uint32_t)((now - s_state.evolution_last_tick_ms) / 1000);
        s_state.evolution_seconds += elapsed;
        s_state.evolution_last_tick_ms = now;

        // Check evolution threshold
        uint32_t threshold_seconds = s_config.evolution_threshold_hours * 3600;
        if (threshold_seconds > 0 && s_state.evolution_seconds >= threshold_seconds) {
            pw_event_t evt = { .type = PW_EVENT_EVOLUTION_TRIGGERED };
            pw_event_send(&evt);
            s_state.evolution_seconds = 0;
        }
    }

    return s_state.current_mood != old_mood;
}

pw_mood_state_t pw_mood_engine_get_state(void)
{
    return s_state;
}

pw_mood_config_t pw_mood_engine_get_config(void)
{
    return s_config;
}

void pw_mood_engine_set_config(const pw_mood_config_t *config)
{
    s_config = *config;
    ESP_LOGI(TAG, "Config updated: excited=%lums, curious=%lums, lonely=%lums",
             (unsigned long)s_config.excited_duration_ms,
             (unsigned long)s_config.curious_timeout_ms,
             (unsigned long)s_config.lonely_timeout_ms);
}

const char *pw_mood_to_string(pw_mood_t mood)
{
    switch (mood) {
    case PW_MOOD_EXCITED:   return "excited";
    case PW_MOOD_HAPPY:     return "happy";
    case PW_MOOD_CURIOUS:   return "curious";
    case PW_MOOD_LONELY:    return "lonely";
    case PW_MOOD_SLEEPY:    return "sleepy";
    case PW_MOOD_OVERJOYED: return "overjoyed";
    default:                return "unknown";
    }
}

// Callback for notifying other tasks of mood changes
static pw_mood_change_cb_t s_mood_change_cb = NULL;

void pw_mood_engine_set_change_cb(pw_mood_change_cb_t cb)
{
    s_mood_change_cb = cb;
}

static void notify_mood_change(pw_mood_t old_mood, pw_mood_t new_mood)
{
    if (s_mood_change_cb) {
        s_mood_change_cb(old_mood, new_mood);
    }
}

static void mood_engine_task(void *arg)
{
    ESP_LOGI(TAG, "Mood engine task started");
    pw_event_t event;

    while (1) {
        // Check for events with 1-second timeout (for tick)
        if (pw_event_receive(&event, 1000)) {
            pw_mood_t old = s_state.current_mood;
            pw_mood_engine_process_event(&event);
            if (s_state.current_mood != old) {
                notify_mood_change(old, s_state.current_mood);
            }
        }
        pw_mood_t old = s_state.current_mood;
        pw_mood_engine_tick();
        if (s_state.current_mood != old) {
            notify_mood_change(old, s_state.current_mood);
        }
    }
}

void pw_mood_engine_task_start(void)
{
    xTaskCreate(mood_engine_task, "mood_engine", 4096, NULL, 5, NULL);
}
```

- [ ] **Step 3: Verify build compiles**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher"
idf.py build
```

Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add main/mood_engine.h main/mood_engine.c
git commit -m "feat: add mood state machine with timer-based transitions and evolution tracking"
```

---

### Task 4: Roster Management & NVS Persistence

**Files:**
- Create: `pokewatcher/main/roster.h`
- Create: `pokewatcher/main/roster.c`

- [ ] **Step 1: Write the roster header**

Create `pokewatcher/main/roster.h`:
```c
#ifndef POKEWATCHER_ROSTER_H
#define POKEWATCHER_ROSTER_H

#include <stdint.h>
#include <stdbool.h>

#define PW_MAX_ROSTER_SIZE   20
#define PW_POKEMON_ID_LEN    32
#define PW_POKEMON_NAME_LEN  32

typedef struct {
    char id[PW_POKEMON_ID_LEN];            // e.g. "charmander"
    char name[PW_POKEMON_NAME_LEN];        // e.g. "Charmander"
    char sprite_sheet[64];                  // e.g. "overworld.png"
    char frame_manifest[64];               // e.g. "frames.json"
    char evolves_to[PW_POKEMON_ID_LEN];    // e.g. "charmeleon" or "" if final
    uint32_t evolution_hours;              // Hours needed to evolve
} pw_pokemon_def_t;

typedef struct {
    char id[PW_POKEMON_ID_LEN];
    uint32_t evolution_seconds;            // Cumulative seconds as active
} pw_roster_entry_t;

typedef struct {
    pw_roster_entry_t entries[PW_MAX_ROSTER_SIZE];
    int count;
    char active_id[PW_POKEMON_ID_LEN];
} pw_roster_t;

/**
 * Initialize roster: load from NVS, or create empty.
 */
void pw_roster_init(void);

/**
 * Get current roster (read-only snapshot).
 */
pw_roster_t pw_roster_get(void);

/**
 * Add a Pokemon to the roster by ID (must exist on SD card).
 * Returns true on success.
 */
bool pw_roster_add(const char *pokemon_id);

/**
 * Remove a Pokemon from the roster by ID.
 * Returns true on success.
 */
bool pw_roster_remove(const char *pokemon_id);

/**
 * Set the active Pokemon. Returns true on success.
 */
bool pw_roster_set_active(const char *pokemon_id);

/**
 * Get the active Pokemon's ID. Returns NULL if none set.
 */
const char *pw_roster_get_active_id(void);

/**
 * Update evolution seconds for the active Pokemon.
 */
void pw_roster_update_evolution(uint32_t total_seconds);

/**
 * Load a Pokemon definition from its JSON file on SD card.
 * Returns true on success.
 */
bool pw_pokemon_load_def(const char *pokemon_id, pw_pokemon_def_t *def);

/**
 * Scan SD card for available Pokemon (those with valid pokemon.json).
 * Fills ids array, returns count found.
 */
int pw_pokemon_scan_available(char ids[][PW_POKEMON_ID_LEN], int max_count);

/**
 * Handle evolution: replace current Pokemon with evolved form in roster.
 */
bool pw_roster_evolve_active(void);

/**
 * Save roster to NVS. Called automatically on changes.
 */
void pw_roster_save(void);

#endif
```

- [ ] **Step 2: Write the roster implementation**

Create `pokewatcher/main/roster.c`:
```c
#include "roster.h"
#include "config.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "cJSON.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static const char *TAG = "pw_roster";
static pw_roster_t s_roster;

// --- NVS Persistence ---

void pw_roster_save(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(PW_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
        return;
    }

    nvs_set_blob(handle, "roster", &s_roster, sizeof(pw_roster_t));
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Roster saved (count=%d, active=%s)", s_roster.count, s_roster.active_id);
}

static void roster_load(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(PW_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved roster found, starting fresh");
        memset(&s_roster, 0, sizeof(pw_roster_t));
        return;
    }

    size_t len = sizeof(pw_roster_t);
    err = nvs_get_blob(handle, "roster", &s_roster, &len);
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved roster found, starting fresh");
        memset(&s_roster, 0, sizeof(pw_roster_t));
    } else {
        ESP_LOGI(TAG, "Roster loaded (count=%d, active=%s)", s_roster.count, s_roster.active_id);
    }
}

// --- Pokemon Definition Loading ---

bool pw_pokemon_load_def(const char *pokemon_id, pw_pokemon_def_t *def)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/%s/pokemon.json", PW_SD_POKEMON_DIR, pokemon_id);

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize > 2048) {
        ESP_LOGE(TAG, "pokemon.json too large: %ld", fsize);
        fclose(f);
        return false;
    }

    char *buf = malloc(fsize + 1);
    if (!buf) {
        fclose(f);
        return false;
    }
    fread(buf, 1, fsize, f);
    buf[fsize] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse %s", path);
        return false;
    }

    memset(def, 0, sizeof(pw_pokemon_def_t));

    cJSON *id_j = cJSON_GetObjectItem(root, "id");
    cJSON *name_j = cJSON_GetObjectItem(root, "name");
    cJSON *sprite_j = cJSON_GetObjectItem(root, "sprite_sheet");
    cJSON *frames_j = cJSON_GetObjectItem(root, "frame_manifest");
    cJSON *evolves_j = cJSON_GetObjectItem(root, "evolves_to");
    cJSON *hours_j = cJSON_GetObjectItem(root, "evolution_hours");

    if (!id_j || !name_j || !sprite_j || !frames_j) {
        ESP_LOGE(TAG, "Missing required fields in %s", path);
        cJSON_Delete(root);
        return false;
    }

    strncpy(def->id, id_j->valuestring, PW_POKEMON_ID_LEN - 1);
    strncpy(def->name, name_j->valuestring, PW_POKEMON_NAME_LEN - 1);
    strncpy(def->sprite_sheet, sprite_j->valuestring, sizeof(def->sprite_sheet) - 1);
    strncpy(def->frame_manifest, frames_j->valuestring, sizeof(def->frame_manifest) - 1);

    if (evolves_j && cJSON_IsString(evolves_j)) {
        strncpy(def->evolves_to, evolves_j->valuestring, PW_POKEMON_ID_LEN - 1);
    }
    if (hours_j && cJSON_IsNumber(hours_j)) {
        def->evolution_hours = (uint32_t)hours_j->valuedouble;
    } else {
        def->evolution_hours = PW_DEFAULT_EVOLUTION_HOURS;
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded Pokemon def: %s (%s)", def->name, def->id);
    return true;
}

int pw_pokemon_scan_available(char ids[][PW_POKEMON_ID_LEN], int max_count)
{
    DIR *dir = opendir(PW_SD_POKEMON_DIR);
    if (!dir) {
        ESP_LOGE(TAG, "Cannot open pokemon directory: %s", PW_SD_POKEMON_DIR);
        return 0;
    }

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_count) {
        if (entry->d_type == DT_DIR && entry->d_name[0] != '.') {
            // Check if pokemon.json exists in this directory
            char check_path[128];
            snprintf(check_path, sizeof(check_path), "%s/%s/pokemon.json", PW_SD_POKEMON_DIR, entry->d_name);
            struct stat st;
            if (stat(check_path, &st) == 0) {
                strncpy(ids[count], entry->d_name, PW_POKEMON_ID_LEN - 1);
                ids[count][PW_POKEMON_ID_LEN - 1] = '\0';
                count++;
            }
        }
    }

    closedir(dir);
    ESP_LOGI(TAG, "Found %d Pokemon on SD card", count);
    return count;
}

// --- Roster Operations ---

void pw_roster_init(void)
{
    roster_load();
}

pw_roster_t pw_roster_get(void)
{
    return s_roster;
}

bool pw_roster_add(const char *pokemon_id)
{
    if (s_roster.count >= PW_MAX_ROSTER_SIZE) {
        ESP_LOGW(TAG, "Roster full");
        return false;
    }

    // Check not already in roster
    for (int i = 0; i < s_roster.count; i++) {
        if (strcmp(s_roster.entries[i].id, pokemon_id) == 0) {
            ESP_LOGW(TAG, "%s already in roster", pokemon_id);
            return false;
        }
    }

    // Verify exists on SD card
    pw_pokemon_def_t def;
    if (!pw_pokemon_load_def(pokemon_id, &def)) {
        return false;
    }

    pw_roster_entry_t *entry = &s_roster.entries[s_roster.count];
    strncpy(entry->id, pokemon_id, PW_POKEMON_ID_LEN - 1);
    entry->evolution_seconds = 0;
    s_roster.count++;

    // If first Pokemon, set as active
    if (s_roster.count == 1) {
        strncpy(s_roster.active_id, pokemon_id, PW_POKEMON_ID_LEN - 1);
    }

    pw_roster_save();
    ESP_LOGI(TAG, "Added %s to roster (count=%d)", pokemon_id, s_roster.count);
    return true;
}

bool pw_roster_remove(const char *pokemon_id)
{
    for (int i = 0; i < s_roster.count; i++) {
        if (strcmp(s_roster.entries[i].id, pokemon_id) == 0) {
            // Shift remaining entries down
            for (int j = i; j < s_roster.count - 1; j++) {
                s_roster.entries[j] = s_roster.entries[j + 1];
            }
            s_roster.count--;

            // Clear active if removed
            if (strcmp(s_roster.active_id, pokemon_id) == 0) {
                if (s_roster.count > 0) {
                    strncpy(s_roster.active_id, s_roster.entries[0].id, PW_POKEMON_ID_LEN - 1);
                } else {
                    s_roster.active_id[0] = '\0';
                }
            }

            pw_roster_save();
            ESP_LOGI(TAG, "Removed %s from roster", pokemon_id);
            return true;
        }
    }
    ESP_LOGW(TAG, "%s not found in roster", pokemon_id);
    return false;
}

bool pw_roster_set_active(const char *pokemon_id)
{
    for (int i = 0; i < s_roster.count; i++) {
        if (strcmp(s_roster.entries[i].id, pokemon_id) == 0) {
            strncpy(s_roster.active_id, pokemon_id, PW_POKEMON_ID_LEN - 1);
            pw_roster_save();
            ESP_LOGI(TAG, "Active Pokemon set to %s", pokemon_id);
            return true;
        }
    }
    ESP_LOGW(TAG, "%s not in roster", pokemon_id);
    return false;
}

const char *pw_roster_get_active_id(void)
{
    if (s_roster.active_id[0] == '\0') {
        return NULL;
    }
    return s_roster.active_id;
}

void pw_roster_update_evolution(uint32_t total_seconds)
{
    for (int i = 0; i < s_roster.count; i++) {
        if (strcmp(s_roster.entries[i].id, s_roster.active_id) == 0) {
            s_roster.entries[i].evolution_seconds = total_seconds;
            // Save periodically (caller decides frequency)
            return;
        }
    }
}

bool pw_roster_evolve_active(void)
{
    const char *active_id = pw_roster_get_active_id();
    if (!active_id) {
        return false;
    }

    pw_pokemon_def_t current_def;
    if (!pw_pokemon_load_def(active_id, &current_def)) {
        return false;
    }

    if (current_def.evolves_to[0] == '\0') {
        ESP_LOGI(TAG, "%s is a final form, no evolution", active_id);
        return false;
    }

    // Verify evolved form exists on SD
    pw_pokemon_def_t evolved_def;
    if (!pw_pokemon_load_def(current_def.evolves_to, &evolved_def)) {
        ESP_LOGE(TAG, "Evolved form %s not found on SD card", current_def.evolves_to);
        return false;
    }

    // Replace in roster
    for (int i = 0; i < s_roster.count; i++) {
        if (strcmp(s_roster.entries[i].id, active_id) == 0) {
            strncpy(s_roster.entries[i].id, current_def.evolves_to, PW_POKEMON_ID_LEN - 1);
            s_roster.entries[i].evolution_seconds = 0;
            strncpy(s_roster.active_id, current_def.evolves_to, PW_POKEMON_ID_LEN - 1);
            pw_roster_save();
            ESP_LOGI(TAG, "Evolution: %s → %s", active_id, current_def.evolves_to);
            return true;
        }
    }

    return false;
}
```

- [ ] **Step 3: Verify build compiles**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher"
idf.py build
```

Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add main/roster.h main/roster.c
git commit -m "feat: add roster management with NVS persistence and SD card Pokemon loading"
```

---

### Task 5: Sprite Loader

**Files:**
- Create: `pokewatcher/main/sprite_loader.h`
- Create: `pokewatcher/main/sprite_loader.c`
- Create: `pokewatcher/sdcard/pokemon/pikachu/pokemon.json` (example)
- Create: `pokewatcher/sdcard/pokemon/pikachu/frames.json` (example)

- [ ] **Step 1: Create example Pokemon data files**

Create `pokewatcher/sdcard/pokemon/pikachu/pokemon.json`:
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

Create `pokewatcher/sdcard/pokemon/pikachu/frames.json`:
```json
{
  "frame_width": 32,
  "frame_height": 32,
  "animations": {
    "idle_down":    { "frames": [{ "x": 0,  "y": 0 }, { "x": 32, "y": 0 }, { "x": 64, "y": 0 }], "loop": true },
    "idle_up":      { "frames": [{ "x": 0,  "y": 32 }, { "x": 32, "y": 32 }, { "x": 64, "y": 32 }], "loop": true },
    "idle_left":    { "frames": [{ "x": 0,  "y": 64 }, { "x": 32, "y": 64 }, { "x": 64, "y": 64 }], "loop": true },
    "idle_right":   { "frames": [{ "x": 0,  "y": 96 }, { "x": 32, "y": 96 }, { "x": 64, "y": 96 }], "loop": true },
    "walk_down":    { "frames": [{ "x": 0,  "y": 0 }, { "x": 32, "y": 0 }, { "x": 64, "y": 0 }, { "x": 96, "y": 0 }], "loop": true },
    "walk_up":      { "frames": [{ "x": 0,  "y": 32 }, { "x": 32, "y": 32 }, { "x": 64, "y": 32 }, { "x": 96, "y": 32 }], "loop": true },
    "walk_left":    { "frames": [{ "x": 0,  "y": 64 }, { "x": 32, "y": 64 }, { "x": 64, "y": 64 }, { "x": 96, "y": 64 }], "loop": true },
    "walk_right":   { "frames": [{ "x": 0,  "y": 96 }, { "x": 32, "y": 96 }, { "x": 64, "y": 96 }, { "x": 96, "y": 96 }], "loop": true }
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

- [ ] **Step 2: Write the sprite loader header**

Create `pokewatcher/main/sprite_loader.h`:
```c
#ifndef POKEWATCHER_SPRITE_LOADER_H
#define POKEWATCHER_SPRITE_LOADER_H

#include "event_queue.h"
#include <stdint.h>
#include <stdbool.h>

#define PW_MAX_FRAMES_PER_ANIM  8
#define PW_MAX_ANIMATIONS       16
#define PW_ANIM_NAME_LEN        32

typedef struct {
    uint16_t x;
    uint16_t y;
} pw_frame_coord_t;

typedef struct {
    char name[PW_ANIM_NAME_LEN];
    pw_frame_coord_t frames[PW_MAX_FRAMES_PER_ANIM];
    int frame_count;
    bool loop;
} pw_animation_t;

typedef struct {
    uint16_t frame_width;
    uint16_t frame_height;
    pw_animation_t animations[PW_MAX_ANIMATIONS];
    int animation_count;
    char mood_anim_names[6][PW_ANIM_NAME_LEN];  // Indexed by pw_mood_t
    uint8_t *sprite_sheet_data;                   // Raw decoded RGBA pixels
    uint32_t sheet_width;
    uint32_t sheet_height;
} pw_sprite_data_t;

/**
 * Load sprite data for a Pokemon from SD card.
 * Loads the PNG sprite sheet into PSRAM and parses the frame manifest.
 * Returns true on success.
 */
bool pw_sprite_load(const char *pokemon_id, pw_sprite_data_t *sprite);

/**
 * Free sprite data (releases PSRAM).
 */
void pw_sprite_free(pw_sprite_data_t *sprite);

/**
 * Get the animation for a given mood.
 * Returns NULL if no mapping found.
 */
const pw_animation_t *pw_sprite_get_mood_anim(const pw_sprite_data_t *sprite, pw_mood_t mood);

/**
 * Extract a single frame as scaled LVGL-compatible pixel buffer (RGB565).
 * Caller must free the returned buffer.
 * Returns NULL on failure.
 */
uint16_t *pw_sprite_extract_frame_scaled(const pw_sprite_data_t *sprite,
                                          const pw_frame_coord_t *coord,
                                          uint16_t scale);

#endif
```

- [ ] **Step 3: Write the sprite loader implementation**

Create `pokewatcher/main/sprite_loader.c`:
```c
#include "sprite_loader.h"
#include "config.h"
#include "esp_log.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "pw_sprite";

// Forward declarations for PNG decoding (uses esp_jpeg_simd or lodepng)
// For v1, we'll use pre-converted raw RGB565 files instead of runtime PNG decode
// to simplify. The sprite sheet on SD is stored as .raw (RGB565 little-endian).

static bool load_frame_manifest(const char *pokemon_id, pw_sprite_data_t *sprite)
{
    char path[128];
    snprintf(path, sizeof(path), "%s/%s/frames.json", PW_SD_POKEMON_DIR, pokemon_id);

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc(fsize + 1);
    if (!buf) { fclose(f); return false; }
    fread(buf, 1, fsize, f);
    buf[fsize] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGE(TAG, "Failed to parse frames.json");
        return false;
    }

    sprite->frame_width = (uint16_t)cJSON_GetObjectItem(root, "frame_width")->valuedouble;
    sprite->frame_height = (uint16_t)cJSON_GetObjectItem(root, "frame_height")->valuedouble;

    // Parse animations
    cJSON *anims = cJSON_GetObjectItem(root, "animations");
    sprite->animation_count = 0;
    cJSON *anim_item = NULL;
    cJSON_ArrayForEach(anim_item, anims) {
        if (sprite->animation_count >= PW_MAX_ANIMATIONS) break;

        pw_animation_t *anim = &sprite->animations[sprite->animation_count];
        strncpy(anim->name, anim_item->string, PW_ANIM_NAME_LEN - 1);

        cJSON *loop_j = cJSON_GetObjectItem(anim_item, "loop");
        anim->loop = loop_j ? cJSON_IsTrue(loop_j) : true;

        cJSON *frames_arr = cJSON_GetObjectItem(anim_item, "frames");
        anim->frame_count = 0;
        cJSON *frame_j = NULL;
        cJSON_ArrayForEach(frame_j, frames_arr) {
            if (anim->frame_count >= PW_MAX_FRAMES_PER_ANIM) break;
            anim->frames[anim->frame_count].x = (uint16_t)cJSON_GetObjectItem(frame_j, "x")->valuedouble;
            anim->frames[anim->frame_count].y = (uint16_t)cJSON_GetObjectItem(frame_j, "y")->valuedouble;
            anim->frame_count++;
        }
        sprite->animation_count++;
    }

    // Parse mood_animations mapping
    cJSON *mood_map = cJSON_GetObjectItem(root, "mood_animations");
    if (mood_map) {
        const char *mood_keys[] = {"excited", "happy", "curious", "lonely", "sleepy", "overjoyed"};
        for (int i = 0; i < 6; i++) {
            cJSON *val = cJSON_GetObjectItem(mood_map, mood_keys[i]);
            if (val && cJSON_IsString(val)) {
                strncpy(sprite->mood_anim_names[i], val->valuestring, PW_ANIM_NAME_LEN - 1);
            }
        }
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Loaded %d animations, frame size %dx%d",
             sprite->animation_count, sprite->frame_width, sprite->frame_height);
    return true;
}

static bool load_sprite_sheet(const char *pokemon_id, pw_sprite_data_t *sprite)
{
    // Load pre-converted RGB565 raw file from SD card
    // File format: first 4 bytes = width (uint16_t LE) + height (uint16_t LE)
    // followed by width*height*2 bytes of RGB565 pixel data
    char path[128];
    snprintf(path, sizeof(path), "%s/%s/overworld.raw", PW_SD_POKEMON_DIR, pokemon_id);

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open sprite sheet: %s", path);
        return false;
    }

    uint16_t dims[2];
    fread(dims, sizeof(uint16_t), 2, f);
    sprite->sheet_width = dims[0];
    sprite->sheet_height = dims[1];

    size_t data_size = sprite->sheet_width * sprite->sheet_height * 2; // RGB565
    sprite->sprite_sheet_data = heap_caps_malloc(data_size, MALLOC_CAP_SPIRAM);
    if (!sprite->sprite_sheet_data) {
        ESP_LOGE(TAG, "Failed to allocate %zu bytes in PSRAM for sprite sheet", data_size);
        fclose(f);
        return false;
    }

    size_t read = fread(sprite->sprite_sheet_data, 1, data_size, f);
    fclose(f);

    if (read != data_size) {
        ESP_LOGE(TAG, "Sprite sheet read incomplete: %zu/%zu", read, data_size);
        heap_caps_free(sprite->sprite_sheet_data);
        sprite->sprite_sheet_data = NULL;
        return false;
    }

    ESP_LOGI(TAG, "Loaded sprite sheet %ux%u (%zu bytes)", sprite->sheet_width, sprite->sheet_height, data_size);
    return true;
}

bool pw_sprite_load(const char *pokemon_id, pw_sprite_data_t *sprite)
{
    memset(sprite, 0, sizeof(pw_sprite_data_t));

    if (!load_frame_manifest(pokemon_id, sprite)) {
        return false;
    }
    if (!load_sprite_sheet(pokemon_id, sprite)) {
        return false;
    }
    return true;
}

void pw_sprite_free(pw_sprite_data_t *sprite)
{
    if (sprite->sprite_sheet_data) {
        heap_caps_free(sprite->sprite_sheet_data);
        sprite->sprite_sheet_data = NULL;
    }
}

const pw_animation_t *pw_sprite_get_mood_anim(const pw_sprite_data_t *sprite, pw_mood_t mood)
{
    if (mood < 0 || mood > 5) return NULL;

    const char *anim_name = sprite->mood_anim_names[mood];
    if (anim_name[0] == '\0') return NULL;

    for (int i = 0; i < sprite->animation_count; i++) {
        if (strcmp(sprite->animations[i].name, anim_name) == 0) {
            return &sprite->animations[i];
        }
    }
    return NULL;
}

uint16_t *pw_sprite_extract_frame_scaled(const pw_sprite_data_t *sprite,
                                          const pw_frame_coord_t *coord,
                                          uint16_t scale)
{
    if (!sprite->sprite_sheet_data) return NULL;

    uint16_t dst_w = sprite->frame_width * scale;
    uint16_t dst_h = sprite->frame_height * scale;
    size_t buf_size = dst_w * dst_h * sizeof(uint16_t);

    uint16_t *buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!buf) return NULL;

    const uint16_t *sheet = (const uint16_t *)sprite->sprite_sheet_data;

    // Nearest-neighbor scale
    for (uint16_t dy = 0; dy < dst_h; dy++) {
        uint16_t sy = coord->y + (dy / scale);
        for (uint16_t dx = 0; dx < dst_w; dx++) {
            uint16_t sx = coord->x + (dx / scale);
            buf[dy * dst_w + dx] = sheet[sy * sprite->sheet_width + sx];
        }
    }

    return buf;
}
```

- [ ] **Step 4: Verify build compiles**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher"
idf.py build
```

Expected: Build succeeds.

- [ ] **Step 5: Commit**

```bash
git add main/sprite_loader.h main/sprite_loader.c sdcard/pokemon/pikachu/
git commit -m "feat: add sprite loader with frame manifest parsing and nearest-neighbor scaling"
```

---

### Task 6: Himax Person Detection Task

**Files:**
- Create: `pokewatcher/main/himax_task.h`
- Create: `pokewatcher/main/himax_task.c`

- [ ] **Step 1: Write the Himax task header**

Create `pokewatcher/main/himax_task.h`:
```c
#ifndef POKEWATCHER_HIMAX_TASK_H
#define POKEWATCHER_HIMAX_TASK_H

/**
 * Start the Himax UART listener task.
 * Reads person detection events from the Himax WiseEye2 chip
 * and pushes PW_EVENT_PERSON_DETECTED / PW_EVENT_PERSON_LEFT
 * to the mood event queue.
 */
void pw_himax_task_start(void);

#endif
```

- [ ] **Step 2: Write the Himax task implementation**

Create `pokewatcher/main/himax_task.c`:
```c
#include "himax_task.h"
#include "event_queue.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sscma_client_io.h"
#include "sscma_client_ops.h"

static const char *TAG = "pw_himax";

// Debounce: require person absent for this many consecutive checks
// before sending PERSON_LEFT. Prevents flicker from momentary detection gaps.
#define PERSON_ABSENT_THRESHOLD  5  // 5 checks × ~500ms = 2.5 seconds

static bool s_person_present = false;
static int s_absent_count = 0;

static void process_detection(bool person_detected)
{
    if (person_detected) {
        s_absent_count = 0;
        if (!s_person_present) {
            s_person_present = true;
            pw_event_t evt = { .type = PW_EVENT_PERSON_DETECTED };
            pw_event_send(&evt);
            ESP_LOGI(TAG, "Person detected");
        }
    } else {
        if (s_person_present) {
            s_absent_count++;
            if (s_absent_count >= PERSON_ABSENT_THRESHOLD) {
                s_person_present = false;
                s_absent_count = 0;
                pw_event_t evt = { .type = PW_EVENT_PERSON_LEFT };
                pw_event_send(&evt);
                ESP_LOGI(TAG, "Person left");
            }
        }
    }
}

static void himax_task(void *arg)
{
    ESP_LOGI(TAG, "Himax task started, initializing SSCMA client...");

    // Initialize the SSCMA client for Himax communication
    // The sscma_client component from the Watcher SDK handles UART setup
    sscma_client_handle_t client = NULL;
    sscma_client_io_handle_t io = NULL;

    // UART config for Himax chip (pins and baud from Watcher SDK defaults)
    sscma_client_io_uart_config_t uart_config = {
        .port = UART_NUM_1,
        .baud_rate = 921600,
        .tx_pin = 21,    // ESP32-S3 TX to Himax RX
        .rx_pin = 20,    // ESP32-S3 RX from Himax TX
        .rx_buffer_size = 4096,
        .tx_buffer_size = 1024,
    };

    esp_err_t err = sscma_client_new_io_uart_bus(&uart_config, &io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create SSCMA UART IO: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    sscma_client_config_t client_config = SSCMA_CLIENT_CONFIG_DEFAULT();
    err = sscma_client_new(io, &client_config, &client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create SSCMA client: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    // Wait for Himax to boot (it outputs noise initially)
    vTaskDelay(pdMS_TO_TICKS(3000));

    // Configure Himax for person detection
    // The model should already be flashed on the Himax chip
    // We just need to start invoke and read results
    sscma_client_invoke(client, -1, false, false);

    ESP_LOGI(TAG, "Himax detection running");

    while (1) {
        sscma_client_reply_t reply = {};
        err = sscma_client_get_invoke(client, &reply, pdMS_TO_TICKS(500));

        if (err == ESP_OK && reply.boxes != NULL) {
            // Check if any detected box is a person (class 0 in COCO = person)
            bool found_person = false;
            for (int i = 0; i < reply.box_count; i++) {
                if (reply.boxes[i].target == 0 && reply.boxes[i].score > 60) {
                    found_person = true;
                    break;
                }
            }
            process_detection(found_person);
            sscma_client_reply_clear(&reply);
        } else {
            // No detection result in timeout, treat as no person
            process_detection(false);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void pw_himax_task_start(void)
{
    xTaskCreate(himax_task, "himax", 4096, NULL, 6, NULL);
}
```

- [ ] **Step 3: Verify build compiles**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher"
idf.py build
```

Expected: Build succeeds. Note: the exact SSCMA API may need adjustment based on the SDK header files — the struct/function names here are based on SDK exploration and may need minor tweaks when building against the actual SDK.

- [ ] **Step 4: Commit**

```bash
git add main/himax_task.h main/himax_task.c
git commit -m "feat: add Himax person detection task with SSCMA client and debounce"
```

---

### Task 7: LVGL Renderer

**Files:**
- Create: `pokewatcher/main/renderer.h`
- Create: `pokewatcher/main/renderer.c`

- [ ] **Step 1: Write the renderer header**

Create `pokewatcher/main/renderer.h`:
```c
#ifndef POKEWATCHER_RENDERER_H
#define POKEWATCHER_RENDERER_H

#include "event_queue.h"
#include "sprite_loader.h"
#include <stdbool.h>

/**
 * Initialize the LVGL display driver, create UI objects.
 * Must be called from app_main before starting the renderer task.
 */
void pw_renderer_init(void);

/**
 * Load sprites for a Pokemon and prepare for rendering.
 */
bool pw_renderer_load_pokemon(const char *pokemon_id);

/**
 * Set the current mood (triggers animation change).
 */
void pw_renderer_set_mood(pw_mood_t mood);

/**
 * Play the evolution animation, then load the new Pokemon.
 */
void pw_renderer_play_evolution(const char *new_pokemon_id);

/**
 * Start the renderer FreeRTOS task (LVGL tick + animation loop).
 */
void pw_renderer_task_start(void);

#endif
```

- [ ] **Step 2: Write the renderer implementation**

Create `pokewatcher/main/renderer.c`:
```c
#include "renderer.h"
#include "config.h"
#include "sprite_loader.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "pw_renderer";

// LVGL objects
static lv_obj_t *s_screen = NULL;
static lv_obj_t *s_bg_img = NULL;
static lv_obj_t *s_sprite_img = NULL;

// Sprite state
static pw_sprite_data_t s_sprite = {};
static pw_mood_t s_current_mood = PW_MOOD_SLEEPY;
static const pw_animation_t *s_current_anim = NULL;
static int s_current_frame = 0;
static uint16_t *s_frame_buf = NULL;  // Current scaled frame pixels
static lv_img_dsc_t s_frame_dsc = {};

// Background tint colors per mood (RGB565)
static const uint32_t MOOD_BG_COLORS[] = {
    [PW_MOOD_EXCITED]   = 0xFDE8C8,  // warm golden
    [PW_MOOD_HAPPY]     = 0xFDE8C8,  // warm golden
    [PW_MOOD_CURIOUS]   = 0xE8F0E8,  // neutral green
    [PW_MOOD_LONELY]    = 0xC8D8F0,  // cool blue
    [PW_MOOD_SLEEPY]    = 0x404060,  // dark night
    [PW_MOOD_OVERJOYED] = 0xFFF0C0,  // bright golden
};

static void update_frame(void)
{
    if (!s_current_anim || s_current_anim->frame_count == 0) return;

    const pw_frame_coord_t *coord = &s_current_anim->frames[s_current_frame];

    // Free previous frame
    if (s_frame_buf) {
        heap_caps_free(s_frame_buf);
        s_frame_buf = NULL;
    }

    s_frame_buf = pw_sprite_extract_frame_scaled(&s_sprite, coord, PW_SPRITE_SCALE);
    if (!s_frame_buf) return;

    // Update LVGL image descriptor
    s_frame_dsc.header.w = PW_SPRITE_DST_SIZE;
    s_frame_dsc.header.h = PW_SPRITE_DST_SIZE;
    s_frame_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_frame_dsc.data_size = PW_SPRITE_DST_SIZE * PW_SPRITE_DST_SIZE * sizeof(uint16_t);
    s_frame_dsc.data = (const uint8_t *)s_frame_buf;

    if (s_sprite_img) {
        lvgl_port_lock(0);
        lv_img_set_src(s_sprite_img, &s_frame_dsc);
        lvgl_port_unlock();
    }

    // Advance frame
    s_current_frame++;
    if (s_current_frame >= s_current_anim->frame_count) {
        if (s_current_anim->loop) {
            s_current_frame = 0;
        } else {
            s_current_frame = s_current_anim->frame_count - 1;
        }
    }
}

void pw_renderer_init(void)
{
    // Initialize LVGL port (display driver is set up by esp_lvgl_port component)
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&lvgl_cfg);

    // NOTE: The actual display driver (SPI/parallel bus, LCD panel) must be
    // initialized before calling lvgl_port_add_disp(). This code is hardware-specific
    // and depends on the Watcher SDK's display initialization.
    // For now, we assume the display is already initialized by the SDK's HAL layer.
    // TODO: Wire up the Watcher-specific display init from sensecap-watcher component.

    lvgl_port_lock(0);

    // Create main screen
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(MOOD_BG_COLORS[PW_MOOD_SLEEPY]), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    // Apply circular mask (clip to round display)
    // LVGL doesn't have native circular clipping, so we use a rounded rectangle
    // with radius = half the display size
    lv_obj_set_style_radius(s_screen, PW_DISPLAY_WIDTH / 2, 0);
    lv_obj_set_style_clip_corner(s_screen, true, 0);

    // Create sprite image object (centered, lower portion)
    s_sprite_img = lv_img_create(s_screen);
    lv_obj_align(s_sprite_img, LV_ALIGN_CENTER, 0, 40);  // Slightly below center

    // Load the active screen
    lv_scr_load(s_screen);

    lvgl_port_unlock();

    ESP_LOGI(TAG, "Renderer initialized (%dx%d display)", PW_DISPLAY_WIDTH, PW_DISPLAY_HEIGHT);
}

bool pw_renderer_load_pokemon(const char *pokemon_id)
{
    // Free existing sprite
    pw_sprite_free(&s_sprite);

    if (!pw_sprite_load(pokemon_id, &s_sprite)) {
        ESP_LOGE(TAG, "Failed to load sprites for %s", pokemon_id);
        return false;
    }

    // Set initial animation based on current mood
    s_current_anim = pw_sprite_get_mood_anim(&s_sprite, s_current_mood);
    s_current_frame = 0;
    update_frame();

    ESP_LOGI(TAG, "Loaded Pokemon: %s", pokemon_id);
    return true;
}

void pw_renderer_set_mood(pw_mood_t mood)
{
    s_current_mood = mood;

    // Switch animation
    const pw_animation_t *new_anim = pw_sprite_get_mood_anim(&s_sprite, mood);
    if (new_anim && new_anim != s_current_anim) {
        s_current_anim = new_anim;
        s_current_frame = 0;
    }

    // Update background tint
    lvgl_port_lock(0);
    if (s_screen) {
        lv_obj_set_style_bg_color(s_screen, lv_color_hex(MOOD_BG_COLORS[mood]), 0);
    }
    lvgl_port_unlock();

    ESP_LOGI(TAG, "Mood visual set to: %s", pw_mood_to_string(mood));
}

void pw_renderer_play_evolution(const char *new_pokemon_id)
{
    ESP_LOGI(TAG, "Playing evolution animation...");

    // Flash white effect
    lvgl_port_lock(0);
    if (s_screen) {
        lv_obj_set_style_bg_color(s_screen, lv_color_white(), 0);
    }
    lvgl_port_unlock();

    vTaskDelay(pdMS_TO_TICKS(1500));

    // Load new Pokemon
    pw_renderer_load_pokemon(new_pokemon_id);

    // Restore mood background
    lvgl_port_lock(0);
    if (s_screen) {
        lv_obj_set_style_bg_color(s_screen, lv_color_hex(MOOD_BG_COLORS[s_current_mood]), 0);
    }
    lvgl_port_unlock();
}

static void renderer_task(void *arg)
{
    ESP_LOGI(TAG, "Renderer task started");

    TickType_t frame_delay = pdMS_TO_TICKS(1000 / PW_ANIM_FPS);

    while (1) {
        update_frame();
        vTaskDelay(frame_delay);
    }
}

void pw_renderer_task_start(void)
{
    xTaskCreate(renderer_task, "renderer", 8192, NULL, 4, NULL);
}
```

- [ ] **Step 3: Verify build compiles**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher"
idf.py build
```

Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add main/renderer.h main/renderer.c
git commit -m "feat: add LVGL renderer with mood-driven sprite animation and background tinting"
```

---

### Task 8: LLM Personality Engine Task

**Files:**
- Create: `pokewatcher/main/llm_task.h`
- Create: `pokewatcher/main/llm_task.c`

- [ ] **Step 1: Write the LLM task header**

Create `pokewatcher/main/llm_task.h`:
```c
#ifndef POKEWATCHER_LLM_TASK_H
#define POKEWATCHER_LLM_TASK_H

#define PW_LLM_ENDPOINT_LEN  128
#define PW_LLM_API_KEY_LEN   128
#define PW_LLM_MODEL_LEN     64

typedef struct {
    char endpoint[PW_LLM_ENDPOINT_LEN];  // e.g. "https://api.anthropic.com/v1/messages"
    char api_key[PW_LLM_API_KEY_LEN];
    char model[PW_LLM_MODEL_LEN];        // e.g. "claude-haiku-4-5-20251001"
} pw_llm_config_t;

/**
 * Initialize LLM task config. Loads from NVS if available.
 */
void pw_llm_init(void);

/**
 * Update LLM config (from web dashboard). Saves to NVS.
 */
void pw_llm_set_config(const pw_llm_config_t *config);

/**
 * Get current LLM config.
 */
pw_llm_config_t pw_llm_get_config(void);

/**
 * Get the last LLM commentary (for web dashboard).
 */
const char *pw_llm_get_last_commentary(void);

/**
 * Get commentary history (JSON array string, last 10 entries).
 * Caller must free the returned string.
 */
char *pw_llm_get_history_json(void);

/**
 * Start the LLM task. Listens for MOOD_CHANGED events and generates commentary.
 */
void pw_llm_task_start(void);

#endif
```

- [ ] **Step 2: Write the LLM task implementation**

Create `pokewatcher/main/llm_task.c`:
```c
#include "llm_task.h"
#include "event_queue.h"
#include "mood_engine.h"
#include "roster.h"
#include "config.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "pw_llm";

static pw_llm_config_t s_config = {};
static char s_last_commentary[PW_LLM_MAX_RESPONSE_LEN] = "";

#define PW_LLM_HISTORY_SIZE 10
static char s_history[PW_LLM_HISTORY_SIZE][PW_LLM_MAX_RESPONSE_LEN];
static int s_history_index = 0;
static int s_history_count = 0;

// Internal queue for mood change events (LLM task listens to this)
static QueueHandle_t s_llm_queue = NULL;

static void save_config(void)
{
    nvs_handle_t handle;
    if (nvs_open(PW_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_blob(handle, "llm_cfg", &s_config, sizeof(pw_llm_config_t));
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void load_config(void)
{
    nvs_handle_t handle;
    if (nvs_open(PW_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(pw_llm_config_t);
        nvs_get_blob(handle, "llm_cfg", &s_config, &len);
        nvs_close(handle);
    }
}

static void add_to_history(const char *commentary)
{
    strncpy(s_history[s_history_index], commentary, PW_LLM_MAX_RESPONSE_LEN - 1);
    s_history_index = (s_history_index + 1) % PW_LLM_HISTORY_SIZE;
    if (s_history_count < PW_LLM_HISTORY_SIZE) {
        s_history_count++;
    }
}

// HTTP response handler
typedef struct {
    char *buf;
    int len;
    int capacity;
} http_response_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_t *resp = (http_response_t *)evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (resp->len + evt->data_len < resp->capacity) {
            memcpy(resp->buf + resp->len, evt->data, evt->data_len);
            resp->len += evt->data_len;
            resp->buf[resp->len] = '\0';
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

static bool call_llm(const char *prompt, char *response, int response_len)
{
    if (s_config.endpoint[0] == '\0' || s_config.api_key[0] == '\0') {
        ESP_LOGW(TAG, "LLM not configured");
        return false;
    }

    // Build request JSON
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", s_config.model);
    cJSON_AddNumberToObject(root, "max_tokens", 150);

    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    cJSON *system_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(system_msg, "role", "system");
    cJSON_AddStringToObject(system_msg, "content",
        "You are a Pokemon companion living on someone's desk. "
        "Respond in character as the Pokemon species given. "
        "Keep responses to 1-2 short sentences. Be cute and expressive. "
        "React to the mood transition described.");
    cJSON_AddItemToArray(messages, system_msg);

    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", prompt);
    cJSON_AddItemToArray(messages, user_msg);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!body) return false;

    // HTTP request
    http_response_t resp = {
        .buf = heap_caps_malloc(2048, MALLOC_CAP_DEFAULT),
        .len = 0,
        .capacity = 2048,
    };
    if (!resp.buf) {
        free(body);
        return false;
    }
    resp.buf[0] = '\0';

    char auth_header[160];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_config.api_key);

    esp_http_client_config_t http_config = {
        .url = s_config.endpoint,
        .method = HTTP_METHOD_POST,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_http_client_set_post_field(client, body, strlen(body));

    esp_err_t err = esp_http_client_perform(client);
    free(body);

    bool success = false;
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        if (status == 200) {
            // Parse response — handle OpenAI-compatible format
            cJSON *resp_json = cJSON_Parse(resp.buf);
            if (resp_json) {
                cJSON *choices = cJSON_GetObjectItem(resp_json, "choices");
                if (choices && cJSON_GetArraySize(choices) > 0) {
                    cJSON *first = cJSON_GetArrayItem(choices, 0);
                    cJSON *message = cJSON_GetObjectItem(first, "message");
                    cJSON *content = cJSON_GetObjectItem(message, "content");
                    if (content && cJSON_IsString(content)) {
                        strncpy(response, content->valuestring, response_len - 1);
                        success = true;
                    }
                }
                cJSON_Delete(resp_json);
            }
        } else {
            ESP_LOGW(TAG, "LLM API returned status %d", status);
        }
    } else {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    heap_caps_free(resp.buf);
    return success;
}

void pw_llm_init(void)
{
    load_config();
    s_llm_queue = xQueueCreate(4, sizeof(pw_event_t));
    ESP_LOGI(TAG, "LLM engine initialized (endpoint=%s)",
             s_config.endpoint[0] ? s_config.endpoint : "not configured");
}

void pw_llm_set_config(const pw_llm_config_t *config)
{
    s_config = *config;
    save_config();
    ESP_LOGI(TAG, "LLM config updated");
}

pw_llm_config_t pw_llm_get_config(void)
{
    return s_config;
}

const char *pw_llm_get_last_commentary(void)
{
    return s_last_commentary;
}

char *pw_llm_get_history_json(void)
{
    cJSON *arr = cJSON_CreateArray();
    // Iterate from oldest to newest
    int start = (s_history_count < PW_LLM_HISTORY_SIZE) ? 0 : s_history_index;
    for (int i = 0; i < s_history_count; i++) {
        int idx = (start + i) % PW_LLM_HISTORY_SIZE;
        if (s_history[idx][0] != '\0') {
            cJSON_AddItemToArray(arr, cJSON_CreateString(s_history[idx]));
        }
    }
    char *json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}

static void llm_task(void *arg)
{
    ESP_LOGI(TAG, "LLM task started");
    pw_mood_t last_known_mood = PW_MOOD_SLEEPY;

    while (1) {
        // Poll mood state every 2 seconds — check if mood has changed
        vTaskDelay(pdMS_TO_TICKS(2000));

        pw_mood_state_t state = pw_mood_engine_get_state();
        if (state.current_mood == last_known_mood) {
            continue;  // No change
        }

        pw_mood_t old_mood = last_known_mood;
        last_known_mood = state.current_mood;

        const char *active = pw_roster_get_active_id();
        if (!active) continue;

        // Build prompt
        char prompt[256];
        snprintf(prompt, sizeof(prompt),
            "You are %s. Your mood just changed from %s to %s. "
            "You've been the active companion for %lu hours. "
            "Express how you feel about this change.",
            active,
            pw_mood_to_string(old_mood),
            pw_mood_to_string(state.current_mood),
            (unsigned long)(state.evolution_seconds / 3600));

        char response[PW_LLM_MAX_RESPONSE_LEN] = "";
        if (call_llm(prompt, response, sizeof(response))) {
            strncpy(s_last_commentary, response, sizeof(s_last_commentary) - 1);
            add_to_history(response);
            ESP_LOGI(TAG, "LLM commentary: %s", response);
        }
    }
}

void pw_llm_task_start(void)
{
    xTaskCreate(llm_task, "llm", 8192, NULL, 3, NULL);
}
```

- [ ] **Step 3: Verify build compiles**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher"
idf.py build
```

Expected: Build succeeds.

- [ ] **Step 4: Commit**

```bash
git add main/llm_task.h main/llm_task.c
git commit -m "feat: add LLM personality engine with OpenAI-compatible API calls and commentary history"
```

---

### Task 9: Web Dashboard & REST API

**Files:**
- Create: `pokewatcher/main/web_server.h`
- Create: `pokewatcher/main/web_server.c`
- Create: `pokewatcher/main/web/index.html`
- Create: `pokewatcher/main/web/roster.html`
- Create: `pokewatcher/main/web/settings.html`
- Create: `pokewatcher/main/web/style.css`
- Create: `pokewatcher/main/web/app.js`

- [ ] **Step 1: Write the web server header**

Create `pokewatcher/main/web_server.h`:
```c
#ifndef POKEWATCHER_WEB_SERVER_H
#define POKEWATCHER_WEB_SERVER_H

/**
 * Start the web server task (HTTP server + REST API + mDNS).
 */
void pw_web_server_start(void);

/**
 * Stop the web server.
 */
void pw_web_server_stop(void);

#endif
```

- [ ] **Step 2: Write the web server implementation**

Create `pokewatcher/main/web_server.c`:
```c
#include "web_server.h"
#include "roster.h"
#include "mood_engine.h"
#include "llm_task.h"
#include "event_queue.h"
#include "config.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "cJSON.h"
#include <string.h>

static const char *TAG = "pw_web";
static httpd_handle_t s_server = NULL;

// Embedded web files (from EMBED_FILES in CMakeLists.txt)
extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
extern const uint8_t roster_html_start[] asm("_binary_roster_html_start");
extern const uint8_t roster_html_end[]   asm("_binary_roster_html_end");
extern const uint8_t settings_html_start[] asm("_binary_settings_html_start");
extern const uint8_t settings_html_end[]   asm("_binary_settings_html_end");
extern const uint8_t style_css_start[] asm("_binary_style_css_start");
extern const uint8_t style_css_end[]   asm("_binary_style_css_end");
extern const uint8_t app_js_start[] asm("_binary_app_js_start");
extern const uint8_t app_js_end[]   asm("_binary_app_js_end");

// --- Static file handlers ---

static esp_err_t serve_embedded(httpd_req_t *req, const uint8_t *start, const uint8_t *end, const char *content_type)
{
    httpd_resp_set_type(req, content_type);
    httpd_resp_send(req, (const char *)start, end - start);
    return ESP_OK;
}

static esp_err_t handle_index(httpd_req_t *req) { return serve_embedded(req, index_html_start, index_html_end, "text/html"); }
static esp_err_t handle_roster_page(httpd_req_t *req) { return serve_embedded(req, roster_html_start, roster_html_end, "text/html"); }
static esp_err_t handle_settings_page(httpd_req_t *req) { return serve_embedded(req, settings_html_start, settings_html_end, "text/html"); }
static esp_err_t handle_css(httpd_req_t *req) { return serve_embedded(req, style_css_start, style_css_end, "text/css"); }
static esp_err_t handle_js(httpd_req_t *req) { return serve_embedded(req, app_js_start, app_js_end, "application/javascript"); }

// --- REST API handlers ---

static esp_err_t handle_api_status(httpd_req_t *req)
{
    pw_mood_state_t mood = pw_mood_engine_get_state();
    pw_roster_t roster = pw_roster_get();
    const char *active = pw_roster_get_active_id();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "active_pokemon", active ? active : "");
    cJSON_AddStringToObject(root, "mood", pw_mood_to_string(mood.current_mood));
    cJSON_AddBoolToObject(root, "person_present", mood.person_present);
    cJSON_AddNumberToObject(root, "evolution_seconds", mood.evolution_seconds);
    cJSON_AddStringToObject(root, "last_commentary", pw_llm_get_last_commentary());

    // Find evolution threshold for active Pokemon
    if (active) {
        pw_pokemon_def_t def;
        if (pw_pokemon_load_def(active, &def)) {
            cJSON_AddNumberToObject(root, "evolution_threshold_hours", def.evolution_hours);
            cJSON_AddStringToObject(root, "evolves_to", def.evolves_to);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t handle_api_roster_get(httpd_req_t *req)
{
    pw_roster_t roster = pw_roster_get();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "active_id", roster.active_id);

    cJSON *entries = cJSON_AddArrayToObject(root, "entries");
    for (int i = 0; i < roster.count; i++) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "id", roster.entries[i].id);
        cJSON_AddNumberToObject(entry, "evolution_seconds", roster.entries[i].evolution_seconds);

        pw_pokemon_def_t def;
        if (pw_pokemon_load_def(roster.entries[i].id, &def)) {
            cJSON_AddStringToObject(entry, "name", def.name);
            cJSON_AddStringToObject(entry, "evolves_to", def.evolves_to);
            cJSON_AddNumberToObject(entry, "evolution_hours", def.evolution_hours);
        }
        cJSON_AddItemToArray(entries, entry);
    }

    // Available Pokemon on SD (not in roster)
    char available[20][PW_POKEMON_ID_LEN];
    int avail_count = pw_pokemon_scan_available(available, 20);
    cJSON *avail_arr = cJSON_AddArrayToObject(root, "available");
    for (int i = 0; i < avail_count; i++) {
        // Only include if not already in roster
        bool in_roster = false;
        for (int j = 0; j < roster.count; j++) {
            if (strcmp(roster.entries[j].id, available[i]) == 0) {
                in_roster = true;
                break;
            }
        }
        if (!in_roster) {
            cJSON_AddItemToArray(avail_arr, cJSON_CreateString(available[i]));
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t handle_api_roster_add(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    cJSON *id = cJSON_GetObjectItem(root, "id");
    if (!id || !cJSON_IsString(id)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'id'");
        return ESP_FAIL;
    }

    bool ok = pw_roster_add(id->valuestring);
    cJSON_Delete(root);

    if (ok) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to add");
    }
    return ESP_OK;
}

static esp_err_t handle_api_roster_delete(httpd_req_t *req)
{
    // Extract Pokemon ID from URI: /api/roster/pikachu
    const char *uri = req->uri;
    const char *id = strrchr(uri, '/');
    if (!id || strlen(id) < 2) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ID in URI");
        return ESP_FAIL;
    }
    id++; // skip '/'

    bool ok = pw_roster_remove(id);
    if (ok) {
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not in roster");
    }
    return ESP_OK;
}

static esp_err_t handle_api_roster_active(httpd_req_t *req)
{
    char buf[128];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    cJSON *id = cJSON_GetObjectItem(root, "id");
    if (!id || !cJSON_IsString(id)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing 'id'");
        return ESP_FAIL;
    }

    bool ok = pw_roster_set_active(id->valuestring);
    cJSON_Delete(root);

    if (ok) {
        pw_event_t evt = { .type = PW_EVENT_ROSTER_CHANGE };
        strncpy(evt.data.roster.pokemon_id, id->valuestring, sizeof(evt.data.roster.pokemon_id) - 1);
        pw_event_send(&evt);
        httpd_resp_sendstr(req, "{\"ok\":true}");
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Failed to set active");
    }
    return ESP_OK;
}

static esp_err_t handle_api_settings_get(httpd_req_t *req)
{
    pw_mood_config_t mood_cfg = pw_mood_engine_get_config();
    pw_llm_config_t llm_cfg = pw_llm_get_config();

    cJSON *root = cJSON_CreateObject();
    cJSON *mood = cJSON_AddObjectToObject(root, "mood");
    cJSON_AddNumberToObject(mood, "excited_duration_ms", mood_cfg.excited_duration_ms);
    cJSON_AddNumberToObject(mood, "overjoyed_duration_ms", mood_cfg.overjoyed_duration_ms);
    cJSON_AddNumberToObject(mood, "curious_timeout_ms", mood_cfg.curious_timeout_ms);
    cJSON_AddNumberToObject(mood, "lonely_timeout_ms", mood_cfg.lonely_timeout_ms);

    cJSON *llm = cJSON_AddObjectToObject(root, "llm");
    cJSON_AddStringToObject(llm, "endpoint", llm_cfg.endpoint);
    cJSON_AddStringToObject(llm, "model", llm_cfg.model);
    // Don't expose API key in GET — just show if it's set
    cJSON_AddBoolToObject(llm, "api_key_set", llm_cfg.api_key[0] != '\0');

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t handle_api_settings_put(httpd_req_t *req)
{
    char *buf = malloc(1024);
    if (!buf) return ESP_FAIL;
    int ret = httpd_req_recv(req, buf, 1023);
    if (ret <= 0) { free(buf); return ESP_FAIL; }
    buf[ret] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }

    // Update mood config
    cJSON *mood = cJSON_GetObjectItem(root, "mood");
    if (mood) {
        pw_mood_config_t cfg = pw_mood_engine_get_config();
        cJSON *val;
        if ((val = cJSON_GetObjectItem(mood, "curious_timeout_ms"))) cfg.curious_timeout_ms = (uint32_t)val->valuedouble;
        if ((val = cJSON_GetObjectItem(mood, "lonely_timeout_ms"))) cfg.lonely_timeout_ms = (uint32_t)val->valuedouble;
        if ((val = cJSON_GetObjectItem(mood, "excited_duration_ms"))) cfg.excited_duration_ms = (uint32_t)val->valuedouble;
        if ((val = cJSON_GetObjectItem(mood, "overjoyed_duration_ms"))) cfg.overjoyed_duration_ms = (uint32_t)val->valuedouble;
        pw_mood_engine_set_config(&cfg);
    }

    // Update LLM config
    cJSON *llm = cJSON_GetObjectItem(root, "llm");
    if (llm) {
        pw_llm_config_t cfg = pw_llm_get_config();
        cJSON *val;
        if ((val = cJSON_GetObjectItem(llm, "endpoint"))) strncpy(cfg.endpoint, val->valuestring, PW_LLM_ENDPOINT_LEN - 1);
        if ((val = cJSON_GetObjectItem(llm, "api_key"))) strncpy(cfg.api_key, val->valuestring, PW_LLM_API_KEY_LEN - 1);
        if ((val = cJSON_GetObjectItem(llm, "model"))) strncpy(cfg.model, val->valuestring, PW_LLM_MODEL_LEN - 1);
        pw_llm_set_config(&cfg);
    }

    cJSON_Delete(root);
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t handle_api_timeline(httpd_req_t *req)
{
    char *json = pw_llm_get_history_json();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json ? json : "[]");
    free(json);
    return ESP_OK;
}

// --- Server setup ---

static void register_routes(httpd_handle_t server)
{
    // Static pages
    httpd_uri_t routes[] = {
        { .uri = "/",             .method = HTTP_GET,    .handler = handle_index },
        { .uri = "/roster",       .method = HTTP_GET,    .handler = handle_roster_page },
        { .uri = "/settings",     .method = HTTP_GET,    .handler = handle_settings_page },
        { .uri = "/style.css",    .method = HTTP_GET,    .handler = handle_css },
        { .uri = "/app.js",       .method = HTTP_GET,    .handler = handle_js },
        // API
        { .uri = "/api/status",        .method = HTTP_GET,    .handler = handle_api_status },
        { .uri = "/api/roster",        .method = HTTP_GET,    .handler = handle_api_roster_get },
        { .uri = "/api/roster",        .method = HTTP_POST,   .handler = handle_api_roster_add },
        { .uri = "/api/roster/*",      .method = HTTP_DELETE, .handler = handle_api_roster_delete },
        { .uri = "/api/roster/active", .method = HTTP_PUT,    .handler = handle_api_roster_active },
        { .uri = "/api/settings",      .method = HTTP_GET,    .handler = handle_api_settings_get },
        { .uri = "/api/settings",      .method = HTTP_PUT,    .handler = handle_api_settings_put },
        { .uri = "/api/timeline",      .method = HTTP_GET,    .handler = handle_api_timeline },
    };

    for (int i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        httpd_register_uri_handler(server, &routes[i]);
    }
}

static void init_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set("pokewatcher");
    mdns_instance_name_set("PokéWatcher Dashboard");
    mdns_service_add(NULL, "_http", "_tcp", PW_WEB_SERVER_PORT, NULL, 0);
    ESP_LOGI(TAG, "mDNS: pokewatcher.local");
}

void pw_web_server_start(void)
{
    init_mdns();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = PW_WEB_SERVER_PORT;
    config.max_uri_handlers = 16;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start web server: %s", esp_err_to_name(err));
        return;
    }

    register_routes(s_server);
    ESP_LOGI(TAG, "Web server started on port %d", PW_WEB_SERVER_PORT);
}

void pw_web_server_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
```

- [ ] **Step 3: Create the dashboard HTML — index.html**

Create `pokewatcher/main/web/index.html`:
```html
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>PokéWatcher</title>
    <link rel="stylesheet" href="/style.css">
</head>
<body>
    <nav>
        <a href="/" class="active">Status</a>
        <a href="/roster">Roster</a>
        <a href="/settings">Settings</a>
    </nav>
    <main>
        <h1 id="pokemon-name">Loading...</h1>
        <div class="mood-badge" id="mood-badge">--</div>
        <div class="status-card">
            <div class="stat">
                <span class="label">Person</span>
                <span id="person-status">--</span>
            </div>
            <div class="stat">
                <span class="label">Evolution</span>
                <div class="progress-bar">
                    <div class="progress-fill" id="evo-bar"></div>
                </div>
                <span id="evo-text">--</span>
            </div>
        </div>
        <h2>Commentary</h2>
        <div id="timeline"></div>
    </main>
    <script src="/app.js"></script>
    <script>
        async function updateStatus() {
            try {
                const res = await fetch('/api/status');
                const d = await res.json();
                document.getElementById('pokemon-name').textContent = d.active_pokemon || 'No Pokemon';
                document.getElementById('mood-badge').textContent = d.mood;
                document.getElementById('mood-badge').className = 'mood-badge mood-' + d.mood;
                document.getElementById('person-status').textContent = d.person_present ? 'Here' : 'Away';
                const hours = (d.evolution_seconds / 3600).toFixed(1);
                const threshold = d.evolution_threshold_hours || 24;
                const pct = Math.min(100, (d.evolution_seconds / (threshold * 3600)) * 100);
                document.getElementById('evo-bar').style.width = pct + '%';
                document.getElementById('evo-text').textContent = hours + 'h / ' + threshold + 'h';

                const tRes = await fetch('/api/timeline');
                const timeline = await tRes.json();
                const el = document.getElementById('timeline');
                el.innerHTML = timeline.map(t => '<div class="comment">' + escHtml(t) + '</div>').join('');
            } catch(e) { console.error(e); }
        }
        function escHtml(s) { const d = document.createElement('div'); d.textContent = s; return d.innerHTML; }
        updateStatus();
        setInterval(updateStatus, 5000);
    </script>
</body>
</html>
```

- [ ] **Step 4: Create roster.html**

Create `pokewatcher/main/web/roster.html`:
```html
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>PokéWatcher - Roster</title>
    <link rel="stylesheet" href="/style.css">
</head>
<body>
    <nav>
        <a href="/">Status</a>
        <a href="/roster" class="active">Roster</a>
        <a href="/settings">Settings</a>
    </nav>
    <main>
        <h1>Your Pokemon</h1>
        <div id="roster-grid" class="roster-grid"></div>

        <h2>Available on SD Card</h2>
        <div id="available-grid" class="roster-grid"></div>
    </main>
    <script src="/app.js"></script>
    <script>
        async function loadRoster() {
            const res = await fetch('/api/roster');
            const d = await res.json();

            const grid = document.getElementById('roster-grid');
            grid.innerHTML = d.entries.map(e =>
                '<div class="pokemon-card' + (e.id === d.active_id ? ' active' : '') + '">' +
                '<strong>' + escHtml(e.name || e.id) + '</strong>' +
                (e.evolves_to ? '<div class="evolves">→ ' + escHtml(e.evolves_to) + '</div>' : '') +
                '<div class="actions">' +
                (e.id !== d.active_id ? '<button onclick="setActive(\'' + e.id + '\')">Select</button>' : '<span class="badge">Active</span>') +
                '<button class="danger" onclick="removePokemon(\'' + e.id + '\')">Remove</button>' +
                '</div></div>'
            ).join('');

            const avail = document.getElementById('available-grid');
            avail.innerHTML = d.available.map(id =>
                '<div class="pokemon-card available">' +
                '<strong>' + escHtml(id) + '</strong>' +
                '<button onclick="addPokemon(\'' + id + '\')">Add</button>' +
                '</div>'
            ).join('');
        }
        async function setActive(id) { await fetch('/api/roster/active', { method: 'PUT', headers: {'Content-Type':'application/json'}, body: JSON.stringify({id}) }); loadRoster(); }
        async function addPokemon(id) { await fetch('/api/roster', { method: 'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify({id}) }); loadRoster(); }
        async function removePokemon(id) { if(confirm('Remove ' + id + '?')) { await fetch('/api/roster/' + id, { method: 'DELETE' }); loadRoster(); } }
        function escHtml(s) { const d = document.createElement('div'); d.textContent = s; return d.innerHTML; }
        loadRoster();
    </script>
</body>
</html>
```

- [ ] **Step 5: Create settings.html**

Create `pokewatcher/main/web/settings.html`:
```html
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>PokéWatcher - Settings</title>
    <link rel="stylesheet" href="/style.css">
</head>
<body>
    <nav>
        <a href="/">Status</a>
        <a href="/roster">Roster</a>
        <a href="/settings" class="active">Settings</a>
    </nav>
    <main>
        <h1>Settings</h1>

        <section>
            <h2>LLM Configuration</h2>
            <label>API Endpoint<input type="text" id="llm-endpoint" placeholder="https://api.openai.com/v1/chat/completions"></label>
            <label>API Key<input type="password" id="llm-key" placeholder="sk-..."></label>
            <label>Model<input type="text" id="llm-model" placeholder="gpt-4o-mini"></label>
        </section>

        <section>
            <h2>Mood Timers</h2>
            <label>Curious timeout (minutes)<input type="number" id="curious-min" min="1" max="60"></label>
            <label>Lonely timeout (minutes)<input type="number" id="lonely-min" min="1" max="120"></label>
            <label>Excited duration (seconds)<input type="number" id="excited-sec" min="1" max="60"></label>
            <label>Overjoyed duration (seconds)<input type="number" id="overjoyed-sec" min="1" max="60"></label>
        </section>

        <button id="save-btn" onclick="saveSettings()">Save Settings</button>
        <div id="save-status"></div>
    </main>
    <script src="/app.js"></script>
    <script>
        async function loadSettings() {
            const res = await fetch('/api/settings');
            const d = await res.json();
            document.getElementById('llm-endpoint').value = d.llm.endpoint || '';
            document.getElementById('llm-model').value = d.llm.model || '';
            document.getElementById('llm-key').placeholder = d.llm.api_key_set ? '(key set)' : 'sk-...';
            document.getElementById('curious-min').value = Math.round(d.mood.curious_timeout_ms / 60000);
            document.getElementById('lonely-min').value = Math.round(d.mood.lonely_timeout_ms / 60000);
            document.getElementById('excited-sec').value = Math.round(d.mood.excited_duration_ms / 1000);
            document.getElementById('overjoyed-sec').value = Math.round(d.mood.overjoyed_duration_ms / 1000);
        }
        async function saveSettings() {
            const body = {
                llm: {
                    endpoint: document.getElementById('llm-endpoint').value,
                    model: document.getElementById('llm-model').value,
                },
                mood: {
                    curious_timeout_ms: parseInt(document.getElementById('curious-min').value) * 60000,
                    lonely_timeout_ms: parseInt(document.getElementById('lonely-min').value) * 60000,
                    excited_duration_ms: parseInt(document.getElementById('excited-sec').value) * 1000,
                    overjoyed_duration_ms: parseInt(document.getElementById('overjoyed-sec').value) * 1000,
                }
            };
            const key = document.getElementById('llm-key').value;
            if (key) body.llm.api_key = key;
            const res = await fetch('/api/settings', { method: 'PUT', headers: {'Content-Type':'application/json'}, body: JSON.stringify(body) });
            document.getElementById('save-status').textContent = res.ok ? 'Saved!' : 'Error';
            setTimeout(() => document.getElementById('save-status').textContent = '', 2000);
        }
        loadSettings();
    </script>
</body>
</html>
```

- [ ] **Step 6: Create style.css**

Create `pokewatcher/main/web/style.css`:
```css
* { margin: 0; padding: 0; box-sizing: border-box; }
body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', sans-serif; background: #1a1a2e; color: #e0e0e0; max-width: 600px; margin: 0 auto; padding: 16px; }
nav { display: flex; gap: 8px; margin-bottom: 24px; }
nav a { color: #888; text-decoration: none; padding: 8px 16px; border-radius: 8px; }
nav a.active { background: #2a2a4e; color: #fff; }
h1 { font-size: 24px; margin-bottom: 8px; }
h2 { font-size: 18px; margin: 20px 0 12px; color: #aaa; }
.mood-badge { display: inline-block; padding: 4px 12px; border-radius: 12px; font-size: 14px; font-weight: bold; background: #333; margin-bottom: 16px; }
.mood-excited, .mood-overjoyed { background: #ff9f43; color: #1a1a2e; }
.mood-happy { background: #4ecdc4; color: #1a1a2e; }
.mood-curious { background: #a29bfe; color: #1a1a2e; }
.mood-lonely { background: #74b9ff; color: #1a1a2e; }
.mood-sleepy { background: #636e72; }
.status-card { background: #2a2a4e; border-radius: 12px; padding: 16px; }
.stat { display: flex; align-items: center; gap: 12px; padding: 8px 0; }
.label { color: #888; min-width: 80px; font-size: 14px; }
.progress-bar { flex: 1; height: 8px; background: #1a1a2e; border-radius: 4px; overflow: hidden; }
.progress-fill { height: 100%; background: #4ecdc4; border-radius: 4px; transition: width 0.5s; }
.comment { background: #2a2a4e; padding: 12px; border-radius: 8px; margin-bottom: 8px; font-style: italic; }
.roster-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(160px, 1fr)); gap: 12px; }
.pokemon-card { background: #2a2a4e; border-radius: 12px; padding: 16px; text-align: center; }
.pokemon-card.active { border: 2px solid #4ecdc4; }
.pokemon-card .evolves { font-size: 12px; color: #888; margin: 4px 0; }
.pokemon-card .actions { margin-top: 8px; display: flex; gap: 4px; justify-content: center; }
.badge { background: #4ecdc4; color: #1a1a2e; padding: 4px 8px; border-radius: 4px; font-size: 12px; }
button { background: #4ecdc4; color: #1a1a2e; border: none; padding: 8px 16px; border-radius: 8px; cursor: pointer; font-weight: bold; }
button.danger { background: #ff6b6b; color: #fff; }
button:hover { opacity: 0.8; }
section { background: #2a2a4e; border-radius: 12px; padding: 16px; margin-bottom: 16px; }
label { display: block; margin-bottom: 12px; font-size: 14px; color: #aaa; }
input { display: block; width: 100%; margin-top: 4px; padding: 8px; background: #1a1a2e; border: 1px solid #444; border-radius: 6px; color: #e0e0e0; }
#save-btn { width: 100%; padding: 12px; font-size: 16px; }
#save-status { text-align: center; margin-top: 8px; color: #4ecdc4; }
```

- [ ] **Step 7: Create app.js (shared utilities)**

Create `pokewatcher/main/web/app.js`:
```javascript
// Shared utilities for PokéWatcher dashboard
// Currently empty — page-specific JS is inline.
// This file exists as a shared entry point for future common utilities.
```

- [ ] **Step 8: Verify build compiles**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher"
idf.py build
```

Expected: Build succeeds. The embedded files should be linked into the binary.

- [ ] **Step 9: Commit**

```bash
git add main/web_server.h main/web_server.c main/web/
git commit -m "feat: add web dashboard with REST API, roster management, settings, and mDNS"
```

---

### Task 10: Wire Up app_main — Full System Integration

**Files:**
- Modify: `pokewatcher/main/app_main.c`

- [ ] **Step 1: Update app_main.c to initialize and start all subsystems**

Replace `pokewatcher/main/app_main.c` with:
```c
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "config.h"
#include "event_queue.h"
#include "mood_engine.h"
#include "roster.h"
#include "renderer.h"
#include "himax_task.h"
#include "llm_task.h"
#include "web_server.h"

static const char *TAG = "pokewatcher";

// WiFi event handler
static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected, reconnecting...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

static void init_wifi(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    // Load WiFi credentials from NVS
    // For first boot, user configures via serial console or web AP
    // For now, set via menuconfig or hardcode for testing
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = "",       // Set via NVS or web dashboard
            .password = "",   // Set via NVS or web dashboard
        },
    };

    // Try to load from NVS
    nvs_handle_t nvs;
    if (nvs_open(PW_NVS_NAMESPACE, NVS_READONLY, &nvs) == ESP_OK) {
        size_t len = sizeof(wifi_config.sta.ssid);
        nvs_get_str(nvs, "wifi_ssid", (char *)wifi_config.sta.ssid, &len);
        len = sizeof(wifi_config.sta.password);
        nvs_get_str(nvs, "wifi_pass", (char *)wifi_config.sta.password, &len);
        nvs_close(nvs);
    }

    if (wifi_config.sta.ssid[0] != '\0') {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
        ESP_ERROR_CHECK(esp_wifi_start());
        ESP_LOGI(TAG, "WiFi connecting to %s...", wifi_config.sta.ssid);
    } else {
        ESP_LOGW(TAG, "No WiFi credentials configured. Web dashboard will not be available until configured.");
    }
}

static void init_sdcard(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;  // 1-bit mode for compatibility

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t *card;
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(PW_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount SD card: %s", esp_err_to_name(ret));
        ESP_LOGW(TAG, "Continuing without SD card — sprites won't load");
    } else {
        ESP_LOGI(TAG, "SD card mounted at %s", PW_SD_MOUNT_POINT);
        sdmmc_card_print_info(stdout, card);
    }
}

// Mood change callback — called from mood engine task context
static void on_mood_changed(pw_mood_t old_mood, pw_mood_t new_mood)
{
    ESP_LOGI(TAG, "Mood changed: %s → %s", pw_mood_to_string(old_mood), pw_mood_to_string(new_mood));
    pw_renderer_set_mood(new_mood);
    // LLM task will also be notified (it polls mood state on its own timer)
}

// Coordinator task: handles roster changes and evolution (from web server events)
static void coordinator_task(void *arg)
{
    ESP_LOGI(TAG, "Coordinator task started");
    pw_event_t event;

    while (1) {
        // This queue only receives roster/evolution events (not detection events)
        // Detection events go directly to mood engine
        if (pw_event_receive(&event, 1000)) {
            switch (event.type) {
            case PW_EVENT_ROSTER_CHANGE:
                pw_renderer_load_pokemon(event.data.roster.pokemon_id);
                break;

            case PW_EVENT_EVOLUTION_TRIGGERED: {
                const char *active = pw_roster_get_active_id();
                if (active) {
                    pw_pokemon_def_t def;
                    if (pw_pokemon_load_def(active, &def) && def.evolves_to[0] != '\0') {
                        pw_roster_evolve_active();
                        pw_renderer_play_evolution(def.evolves_to);
                        ESP_LOGI(TAG, "Evolution complete: %s → %s", active, def.evolves_to);
                    }
                }
                break;
            }

            default:
                break;
            }
        }
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=== PokéWatcher v1 starting ===");

    // 1. Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "[1/8] NVS initialized");

    // 2. Mount SD card
    init_sdcard();
    ESP_LOGI(TAG, "[2/8] SD card initialized");

    // 3. Initialize event queue
    pw_event_queue_init();
    ESP_LOGI(TAG, "[3/8] Event queue initialized");

    // 4. Initialize roster
    pw_roster_init();
    ESP_LOGI(TAG, "[4/8] Roster initialized");

    // 5. Initialize mood engine
    pw_mood_engine_init();
    ESP_LOGI(TAG, "[5/8] Mood engine initialized");

    // 6. Initialize LLM
    pw_llm_init();
    ESP_LOGI(TAG, "[6/8] LLM engine initialized");

    // 7. Initialize display & renderer
    pw_renderer_init();
    const char *active = pw_roster_get_active_id();
    if (active) {
        pw_renderer_load_pokemon(active);
    }
    ESP_LOGI(TAG, "[7/8] Renderer initialized");

    // 8. Initialize WiFi & start web server
    init_wifi();
    pw_web_server_start();
    ESP_LOGI(TAG, "[8/8] WiFi + web server initialized");

    // Register mood change callback before starting tasks
    pw_mood_engine_set_change_cb(on_mood_changed);

    // Start all tasks
    pw_himax_task_start();      // Pushes detection events to queue
    pw_mood_engine_task_start(); // Consumes detection events, calls mood change callback
    pw_llm_task_start();         // Monitors mood state, generates commentary
    pw_renderer_task_start();    // Animation loop
    xTaskCreate(coordinator_task, "coordinator", 4096, NULL, 5, NULL); // Roster/evolution events

    ESP_LOGI(TAG, "=== PokéWatcher v1 running ===");
    ESP_LOGI(TAG, "Active Pokemon: %s", active ? active : "none");
    ESP_LOGI(TAG, "Dashboard: http://pokewatcher.local");
}
```

- [ ] **Step 2: Verify build compiles**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher"
idf.py build
```

Expected: Build succeeds with all components linked.

- [ ] **Step 3: Commit**

```bash
git add main/app_main.c
git commit -m "feat: wire up full system — init sequence, WiFi, SD card, coordinator task"
```

---

### Task 11: Sprite Conversion Tool (Python Helper)

**Files:**
- Create: `pokewatcher/tools/convert_sprites.py`

This converts downloaded PNG sprite sheets to the raw RGB565 format the firmware expects.

- [ ] **Step 1: Create the conversion tool**

Create `pokewatcher/tools/convert_sprites.py`:
```python
#!/usr/bin/env python3
"""Convert PNG sprite sheets to RGB565 raw format for PokéWatcher.

Usage:
    python convert_sprites.py <input.png> <output.raw>

Output format:
    - First 4 bytes: width (uint16 LE) + height (uint16 LE)
    - Followed by width*height*2 bytes of RGB565 pixel data (LE)

Transparent pixels are converted to magenta (0xF81F) as a color key.
"""

import sys
import struct
from PIL import Image


def rgb888_to_rgb565(r, g, b):
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def convert(input_path, output_path):
    img = Image.open(input_path).convert("RGBA")
    w, h = img.size
    pixels = img.load()

    with open(output_path, "wb") as f:
        # Header: width, height as uint16 LE
        f.write(struct.pack("<HH", w, h))

        # Pixel data
        for y in range(h):
            for x in range(w):
                r, g, b, a = pixels[x, y]
                if a < 128:
                    # Transparent → magenta color key
                    pixel = 0xF81F
                else:
                    pixel = rgb888_to_rgb565(r, g, b)
                f.write(struct.pack("<H", pixel))

    print(f"Converted {input_path} ({w}x{h}) → {output_path} ({4 + w*h*2} bytes)")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <input.png> <output.raw>")
        sys.exit(1)
    convert(sys.argv[1], sys.argv[2])
```

- [ ] **Step 2: Test the conversion tool**

```bash
pip install Pillow  # if not installed
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher/tools"
# Create a test image
python3 -c "from PIL import Image; img = Image.new('RGBA', (128, 128), (255, 200, 0, 255)); img.save('test.png')"
python3 convert_sprites.py test.png test.raw
ls -la test.raw
# Expected: 32772 bytes (4 header + 128*128*2 pixel data)
rm test.png test.raw
```

Expected: Output file is 32772 bytes.

- [ ] **Step 3: Commit**

```bash
git add tools/convert_sprites.py
git commit -m "feat: add Python sprite sheet converter (PNG → RGB565 raw)"
```

---

### Task 12: SD Card Setup Script & Example Pokemon Data

**Files:**
- Create: `pokewatcher/tools/setup_sdcard.sh`
- Modify: `pokewatcher/sdcard/pokemon/pikachu/pokemon.json` (already exists)
- Create: `pokewatcher/sdcard/pokemon/charmander/pokemon.json`
- Create: `pokewatcher/sdcard/pokemon/charmander/frames.json`
- Create: `pokewatcher/sdcard/pokemon/charmeleon/pokemon.json`
- Create: `pokewatcher/sdcard/pokemon/charmeleon/frames.json`

- [ ] **Step 1: Create additional Pokemon JSON definitions**

Create `pokewatcher/sdcard/pokemon/charmander/pokemon.json`:
```json
{
  "id": "charmander",
  "name": "Charmander",
  "sprite_sheet": "overworld.png",
  "frame_manifest": "frames.json",
  "evolves_to": "charmeleon",
  "evolution_hours": 24
}
```

Create `pokewatcher/sdcard/pokemon/charmander/frames.json`:
```json
{
  "frame_width": 32,
  "frame_height": 32,
  "animations": {
    "idle_down":    { "frames": [{ "x": 0,  "y": 0 }, { "x": 32, "y": 0 }, { "x": 64, "y": 0 }], "loop": true },
    "idle_up":      { "frames": [{ "x": 0,  "y": 32 }, { "x": 32, "y": 32 }, { "x": 64, "y": 32 }], "loop": true },
    "idle_left":    { "frames": [{ "x": 0,  "y": 64 }, { "x": 32, "y": 64 }, { "x": 64, "y": 64 }], "loop": true },
    "idle_right":   { "frames": [{ "x": 0,  "y": 96 }, { "x": 32, "y": 96 }, { "x": 64, "y": 96 }], "loop": true },
    "walk_down":    { "frames": [{ "x": 0,  "y": 0 }, { "x": 32, "y": 0 }, { "x": 64, "y": 0 }, { "x": 96, "y": 0 }], "loop": true },
    "walk_up":      { "frames": [{ "x": 0,  "y": 32 }, { "x": 32, "y": 32 }, { "x": 64, "y": 32 }, { "x": 96, "y": 32 }], "loop": true },
    "walk_left":    { "frames": [{ "x": 0,  "y": 64 }, { "x": 32, "y": 64 }, { "x": 64, "y": 64 }, { "x": 96, "y": 64 }], "loop": true },
    "walk_right":   { "frames": [{ "x": 0,  "y": 96 }, { "x": 32, "y": 96 }, { "x": 64, "y": 96 }, { "x": 96, "y": 96 }], "loop": true }
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

Create `pokewatcher/sdcard/pokemon/charmeleon/pokemon.json`:
```json
{
  "id": "charmeleon",
  "name": "Charmeleon",
  "sprite_sheet": "overworld.png",
  "frame_manifest": "frames.json",
  "evolves_to": "charizard",
  "evolution_hours": 48
}
```

Create `pokewatcher/sdcard/pokemon/charmeleon/frames.json`:
```json
{
  "frame_width": 32,
  "frame_height": 32,
  "animations": {
    "idle_down":    { "frames": [{ "x": 0,  "y": 0 }, { "x": 32, "y": 0 }, { "x": 64, "y": 0 }], "loop": true },
    "idle_up":      { "frames": [{ "x": 0,  "y": 32 }, { "x": 32, "y": 32 }, { "x": 64, "y": 32 }], "loop": true },
    "idle_left":    { "frames": [{ "x": 0,  "y": 64 }, { "x": 32, "y": 64 }, { "x": 64, "y": 64 }], "loop": true },
    "idle_right":   { "frames": [{ "x": 0,  "y": 96 }, { "x": 32, "y": 96 }, { "x": 64, "y": 96 }], "loop": true },
    "walk_down":    { "frames": [{ "x": 0,  "y": 0 }, { "x": 32, "y": 0 }, { "x": 64, "y": 0 }, { "x": 96, "y": 0 }], "loop": true },
    "walk_up":      { "frames": [{ "x": 0,  "y": 32 }, { "x": 32, "y": 32 }, { "x": 64, "y": 32 }, { "x": 96, "y": 32 }], "loop": true },
    "walk_left":    { "frames": [{ "x": 0,  "y": 64 }, { "x": 32, "y": 64 }, { "x": 64, "y": 64 }, { "x": 96, "y": 64 }], "loop": true },
    "walk_right":   { "frames": [{ "x": 0,  "y": 96 }, { "x": 32, "y": 96 }, { "x": 64, "y": 96 }, { "x": 96, "y": 96 }], "loop": true }
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

- [ ] **Step 2: Create SD card setup script**

Create `pokewatcher/tools/setup_sdcard.sh`:
```bash
#!/bin/bash
# Setup an SD card with PokéWatcher Pokemon data.
# Usage: ./setup_sdcard.sh <sd_card_mount_path> <sprite_sheets_dir>
#
# sprite_sheets_dir should contain PNG files named by Pokemon ID (e.g. pikachu.png)
# This script converts them to RGB565 and copies the JSON definitions.

set -e

SDCARD="${1:?Usage: $0 <sd_card_path> <sprites_dir>}"
SPRITES="${2:?Usage: $0 <sd_card_path> <sprites_dir>}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"

echo "Setting up SD card at: $SDCARD"
echo "Sprite sheets from: $SPRITES"

# Create directory structure
mkdir -p "$SDCARD/pokemon"
mkdir -p "$SDCARD/background"

# Copy Pokemon definitions and convert sprites
for pokemon_dir in "$PROJECT_DIR/sdcard/pokemon"/*/; do
    pokemon_id=$(basename "$pokemon_dir")
    echo "Processing: $pokemon_id"

    # Create directory on SD
    mkdir -p "$SDCARD/pokemon/$pokemon_id"

    # Copy JSON files
    cp "$pokemon_dir/pokemon.json" "$SDCARD/pokemon/$pokemon_id/"
    cp "$pokemon_dir/frames.json" "$SDCARD/pokemon/$pokemon_id/"

    # Convert sprite sheet if PNG exists
    png_file="$SPRITES/${pokemon_id}.png"
    if [ -f "$png_file" ]; then
        python3 "$SCRIPT_DIR/convert_sprites.py" "$png_file" "$SDCARD/pokemon/$pokemon_id/overworld.raw"
    else
        echo "  WARNING: No sprite sheet found at $png_file"
    fi
done

echo "Done! SD card is ready."
```

- [ ] **Step 3: Make setup script executable**

```bash
chmod +x "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher/tools/setup_sdcard.sh"
```

- [ ] **Step 4: Commit**

```bash
git add sdcard/pokemon/ tools/setup_sdcard.sh
git commit -m "feat: add SD card setup script and example Pokemon data (pikachu, charmander, charmeleon)"
```

---

### Task 13: First Flash & On-Device Smoke Test

**Files:** None created — this is verification.

- [ ] **Step 1: Clone the Watcher SDK (if not already)**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher"
git clone https://github.com/Seeed-Studio/SenseCAP-Watcher-Firmware.git
cd SenseCAP-Watcher-Firmware
git submodule update --init
```

- [ ] **Step 2: Back up nvsfactory partition**

```bash
esptool.py --port /dev/ttyACM0 --baud 2000000 --chip esp32s3 \
  read_flash 0x9000 204800 nvsfactory_backup.bin
```

Expected: `nvsfactory_backup.bin` created (204,800 bytes).

- [ ] **Step 3: Build the firmware**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher"
export IDF_PATH=<your-esp-idf-path>
source $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py build
```

Expected: Build succeeds. If there are SDK API mismatches (especially sscma_client), fix the specific function signatures based on the actual headers.

- [ ] **Step 4: Flash (app partition only — preserves nvsfactory)**

```bash
idf.py --port /dev/ttyACM0 app-flash
```

Expected: Firmware flashes successfully.

- [ ] **Step 5: Monitor serial output**

```bash
idf.py --port /dev/ttyACM0 monitor
```

Expected output includes:
```
PokéWatcher v1 starting...
[1/8] NVS initialized
[2/8] SD card initialized
[3/8] Event queue initialized
[4/8] Roster initialized
[5/8] Mood engine initialized
[6/8] LLM engine initialized
[7/8] Renderer initialized
[8/8] WiFi + web server initialized
PokéWatcher v1 running
```

- [ ] **Step 6: Verify web dashboard**

Connect to the same WiFi network as the Watcher. Open `http://pokewatcher.local` in a browser.

Expected: Dashboard loads showing "No Pokemon" and empty roster.

- [ ] **Step 7: Prepare SD card with test Pokemon**

Download Pikachu HGSS overworld sprite sheet from Spriters Resource, save as `pikachu.png`, then:

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher"
python3 tools/convert_sprites.py ~/Downloads/pikachu.png /Volumes/SDCARD/pokemon/pikachu/overworld.raw
cp sdcard/pokemon/pikachu/*.json /Volumes/SDCARD/pokemon/pikachu/
```

Insert SD card, reboot Watcher. Add Pikachu via the web dashboard Roster page.

Expected: Pikachu appears on the Watcher's round display in idle animation. Mood starts as SLEEPY, transitions to EXCITED → HAPPY when person is detected by Himax.

- [ ] **Step 8: Commit any build fixes**

```bash
git add -A
git commit -m "fix: resolve build issues from first on-device test"
```

---

## Summary

| Task | Component | Est. Steps |
|------|-----------|------------|
| 1 | Project scaffold & build system | 9 |
| 2 | Event queue system | 4 |
| 3 | Mood engine (state machine) | 4 |
| 4 | Roster management & NVS persistence | 4 |
| 5 | Sprite loader | 5 |
| 6 | Himax person detection task | 4 |
| 7 | LVGL renderer | 4 |
| 8 | LLM personality engine | 4 |
| 9 | Web dashboard & REST API | 9 |
| 10 | Full system integration (app_main) | 3 |
| 11 | Sprite conversion tool | 3 |
| 12 | SD card setup & example data | 4 |
| 13 | First flash & on-device smoke test | 8 |
| **Total** | | **65 steps** |
