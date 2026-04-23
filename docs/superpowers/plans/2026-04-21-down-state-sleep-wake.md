# DOWN State Sleep/Wake Behavior Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make DOWN state persist through display sleep/wake cycles, suppress LED while display is off, skip wakeup animation when waking into DOWN, and recover to IDLE only when OpenClaw sends an API call.

**Architecture:** Four targeted changes across two firmware files. The renderer loop gets three fixes (sleep guard, wake flow, LED guard) and the web server gets a helper function called from all write-oriented OpenClaw endpoints. Person/gesture detection wake the display but no longer override DOWN state.

**Tech Stack:** ESP-IDF C firmware (renderer.c, web_server.c, agent_state.c)

---

## File Map

| Action | File | Responsibility |
|--------|------|----------------|
| Modify | `pokewatcher/main/renderer.c:996-1002` | Guard pre-sleep transition to skip SLEEPING when in DOWN |
| Modify | `pokewatcher/main/renderer.c:857-878` | Skip wakeup animation when waking from DOWN |
| Modify | `pokewatcher/main/renderer.c:1154-1177` | Suppress LED blink when display is sleeping |
| Modify | `pokewatcher/main/web_server.c:300-331` | Extract DOWN recovery into helper |
| Modify | `pokewatcher/main/web_server.c:170-250` | Call recovery helper from agent-state, message handlers |
| Modify | `pokewatcher/main/web_server.c:252-376` | Call recovery helper from background, model handlers |
| Modify | `pokewatcher/main/web_server.c:419-455` | Call recovery helper from audio-play, voice-put handlers |
| Modify | `pokewatcher/main/agent_state.c:167-189` | Stop person/gesture detection from overriding DOWN → IDLE |

---

### Task 1: Guard pre-sleep transition to preserve DOWN state

**Files:**
- Modify: `pokewatcher/main/renderer.c:996-1002`

The pre-sleep check currently transitions any non-SLEEPING state to `PW_STATE_SLEEPING`. When the device is in DOWN, this overwrites the DOWN state. We need to skip the SLEEPING state transition but still allow `display_sleep()` to fire on the normal idle timeout.

- [ ] **Step 1: Add DOWN guard to pre-sleep state transition**

In `renderer.c`, find the pre-sleep block (line 996-1002):

```c
if (!s_display_sleeping && !s_pre_sleep_triggered &&
    idle_ms >= (PW_DISPLAY_SLEEP_TIMEOUT_MS - PW_SLEEP_STATE_BEFORE_MS) &&
    s_current_state != PW_STATE_SLEEPING) {
    s_pre_sleep_triggered = true;
    s_pending_state = PW_STATE_SLEEPING;
    s_state_changed = true;
    ESP_LOGI(TAG, "Pre-sleep: switching to sleeping state");
}
```

Replace with:

```c
if (!s_display_sleeping && !s_pre_sleep_triggered &&
    idle_ms >= (PW_DISPLAY_SLEEP_TIMEOUT_MS - PW_SLEEP_STATE_BEFORE_MS) &&
    s_current_state != PW_STATE_SLEEPING &&
    s_current_state != PW_STATE_DOWN) {
    s_pre_sleep_triggered = true;
    s_pending_state = PW_STATE_SLEEPING;
    s_state_changed = true;
    ESP_LOGI(TAG, "Pre-sleep: switching to sleeping state");
}
```

This skips the ZZZ sleeping animation when in DOWN, but the `display_sleep()` call at line 1018 still fires based on `auto_sleep` (pure idle timeout), so the display still turns off.

- [ ] **Step 2: Verify display still sleeps when in DOWN**

The `display_sleep()` call at line 1018 triggers on `auto_sleep || state_sleep`. With DOWN, `state_sleep` won't fire (because `s_sleeping_state_started_ms` stays 0 since we never enter SLEEPING). But `auto_sleep` (`idle_ms >= PW_DISPLAY_SLEEP_TIMEOUT_MS`) will still fire on the normal idle timeout. Confirm this by reading the code path — no change needed, just verification.

- [ ] **Step 3: Commit**

```bash
git add pokewatcher/main/renderer.c
git commit -m "fix(renderer): preserve DOWN state during display sleep

Skip PW_STATE_SLEEPING transition when already in DOWN so the agent
state is not overwritten. Display still turns off via auto_sleep timeout."
```

---

### Task 2: Skip wakeup animation when waking from DOWN

**Files:**
- Modify: `pokewatcher/main/renderer.c:857-878`

Currently all display-off wakes play `PW_STATE_WAKEUP` animation. When the stored state before sleep was DOWN, we should skip the wakeup animation and go straight to the DOWN animation and position.

- [ ] **Step 1: Track the pre-sleep state for DOWN wake**

We need to know what state the device was in before the display went to sleep. The `s_current_state` gets set to WAKEUP during the wake flow, so we need to check it before that happens. The simplest approach: check if the state was DOWN at wake time.

In the wake block at line 857-878, find:

```c
// If display was actually off, play wakeup animation before anything else
if (woke_from_display_off) {
    // Queue whatever state change is pending — we'll apply it after wakeup
    if (s_state_changed) {
        s_wakeup_queued_state = s_pending_state;
        s_wakeup_has_queued_state = true;
        s_state_changed = false;  // consume — will re-apply after wakeup
    }

    // Start wakeup animation at current position (sleeping/down pos)
    s_wakeup_playing = true;
    s_current_state = PW_STATE_WAKEUP;
    const pw_animation_t *wake_anim = pw_sprite_get_state_anim(&s_sprite, PW_STATE_WAKEUP);
    if (wake_anim) {
        s_current_anim = wake_anim;
        s_current_frame = 0;
        s_frame_tick = 0;
    }
    s_behav_state = BEHAV_IDLE;
    s_state_visuals_dirty = true;
    ESP_LOGI(TAG, "Display woke from off — playing wakeup animation");
}
```

Replace with:

```c
if (woke_from_display_off) {
    if (s_current_state == PW_STATE_DOWN) {
        // DOWN state: skip wakeup animation, stay in DOWN
        s_state_visuals_dirty = true;
        s_behav_state = BEHAV_IDLE;
        ESP_LOGI(TAG, "Display woke from off — staying in DOWN (no wakeup anim)");
    } else {
        // Queue whatever state change is pending — we'll apply it after wakeup
        if (s_state_changed) {
            s_wakeup_queued_state = s_pending_state;
            s_wakeup_has_queued_state = true;
            s_state_changed = false;
        }

        // Start wakeup animation at current position (sleeping/down pos)
        s_wakeup_playing = true;
        s_current_state = PW_STATE_WAKEUP;
        const pw_animation_t *wake_anim = pw_sprite_get_state_anim(&s_sprite, PW_STATE_WAKEUP);
        if (wake_anim) {
            s_current_anim = wake_anim;
            s_current_frame = 0;
            s_frame_tick = 0;
        }
        s_behav_state = BEHAV_IDLE;
        s_state_visuals_dirty = true;
        ESP_LOGI(TAG, "Display woke from off — playing wakeup animation");
    }
}
```

When waking from DOWN: display turns on, the DOWN animation and dark gray background are already showing (visuals dirty triggers re-render), red LED starts blinking. No wakeup animation plays.

- [ ] **Step 2: Commit**

```bash
git add pokewatcher/main/renderer.c
git commit -m "fix(renderer): skip wakeup animation when waking from DOWN state

When display wakes from off while in DOWN, go straight to DOWN animation
instead of playing the wakeup sequence. The DOWN visual feedback (dark
background + red LED) shows immediately."
```

---

### Task 3: Suppress LED while display is sleeping

**Files:**
- Modify: `pokewatcher/main/renderer.c:1154-1177`

The red LED blink for DOWN should only fire when the display is awake. When the display is off, no LED — person/gesture detection wakes the display, and then the LED becomes visible.

- [ ] **Step 1: Wrap LED blink block with display-awake guard**

Find the LED blink block at line 1154-1177:

```c
// RGB LED blink: flash state color for 3 frames every 100 frames (~10s at 10 FPS)
{
    int blink_phase = s_frame_counter % 100;
    if (blink_phase == 0) {
        // Turn on: state-specific color
        switch (s_current_state) {
            case PW_STATE_ALERT:     bsp_rgb_set(26, 0, 0);      break;  // red (10%)
            case PW_STATE_WAITING:   bsp_rgb_set(26, 14, 0);     break;  // orange (10%)
            case PW_STATE_GREETING:  bsp_rgb_set(26, 5, 10);     break;  // pink (10%)
            case PW_STATE_REPORTING: bsp_rgb_set(0, 26, 0);      break;  // green (10%)
            case PW_STATE_DOWN:      bsp_rgb_set(26, 0, 0);      break;  // red (10%)
            default: break;  // no blink for other states
        }
    } else if (blink_phase == 3) {
        // Turn off after 3 frames (~300ms)
        if (s_current_state == PW_STATE_ALERT ||
            s_current_state == PW_STATE_WAITING ||
            s_current_state == PW_STATE_GREETING ||
            s_current_state == PW_STATE_REPORTING ||
            s_current_state == PW_STATE_DOWN) {
            bsp_rgb_set(0, 0, 0);
        }
    }
}
```

Replace with:

```c
// RGB LED blink: flash state color for 3 frames every 100 frames (~10s at 10 FPS)
// Suppressed while display is sleeping — LED activates when display wakes
if (!s_display_sleeping) {
    int blink_phase = s_frame_counter % 100;
    if (blink_phase == 0) {
        switch (s_current_state) {
            case PW_STATE_ALERT:     bsp_rgb_set(26, 0, 0);      break;
            case PW_STATE_WAITING:   bsp_rgb_set(26, 14, 0);     break;
            case PW_STATE_GREETING:  bsp_rgb_set(26, 5, 10);     break;
            case PW_STATE_REPORTING: bsp_rgb_set(0, 26, 0);      break;
            case PW_STATE_DOWN:      bsp_rgb_set(26, 0, 0);      break;
            default: break;
        }
    } else if (blink_phase == 3) {
        if (s_current_state == PW_STATE_ALERT ||
            s_current_state == PW_STATE_WAITING ||
            s_current_state == PW_STATE_GREETING ||
            s_current_state == PW_STATE_REPORTING ||
            s_current_state == PW_STATE_DOWN) {
            bsp_rgb_set(0, 0, 0);
        }
    }
}
```

Note: when the display is sleeping, the frame rate drops to 1 FPS (line 1187: `vTaskDelay(s_display_sleeping ? pdMS_TO_TICKS(1000) : frame_delay)`), so this block barely executes anyway. But the guard ensures the LED stays fully dark.

- [ ] **Step 2: Commit**

```bash
git add pokewatcher/main/renderer.c
git commit -m "fix(renderer): suppress LED blink while display is sleeping

LED only flashes when display is awake. Person/gesture detection wakes
the display first, then the LED starts blinking."
```

---

### Task 4: Recover from DOWN on any OpenClaw API call

**Files:**
- Modify: `pokewatcher/main/web_server.c:~165` (add helper near top)
- Modify: `pokewatcher/main/web_server.c:170-512` (call helper from 6 handlers)

Currently only `/api/heartbeat` recovers from DOWN → IDLE. Any write endpoint called by OpenClaw should also recover, since it proves the agent is alive. The read-only endpoints (`/api/status`, `/api/audio` GET, `/api/voice` GET) are used by the daemon poll loop and should NOT trigger recovery.

**Endpoints that should recover:**
- `handle_api_agent_state` — PUT `/api/agent-state`
- `handle_api_message` — POST `/api/message`
- `handle_api_background` — PUT `/api/background`
- `handle_api_heartbeat` — POST `/api/heartbeat` (already does)
- `handle_api_model` — PUT `/api/model`
- `handle_api_audio_play` — POST `/api/audio/play`
- `handle_api_voice_put` — PUT `/api/voice`

- [ ] **Step 1: Add a static helper function for DOWN recovery**

Add this before `handle_api_agent_state` (around line 168):

```c
static void recover_from_down(void)
{
    pw_agent_state_data_t state = pw_agent_state_get();
    if (state.current_state == PW_STATE_DOWN) {
        ESP_LOGI(TAG, "OpenClaw API call: recovering from DOWN -> IDLE");
        pw_agent_state_set(PW_STATE_IDLE);
        pw_renderer_set_state(PW_STATE_IDLE);
    }
}
```

Using `pw_renderer_set_state(PW_STATE_IDLE)` instead of `pw_renderer_wake_display()` because `set_state` both queues the IDLE state AND sets `s_wake_requested = true` (line 786 of renderer.c). This ensures the display wakes and the renderer transitions to IDLE in one call.

- [ ] **Step 2: Call recover_from_down() at the top of each OpenClaw handler**

Add `recover_from_down();` as the first line of each handler function body:

In `handle_api_agent_state` (line ~170):
```c
static esp_err_t handle_api_agent_state(httpd_req_t *req)
{
    recover_from_down();
    char body[128];
    // ... rest unchanged
```

In `handle_api_message` (line ~207):
```c
static esp_err_t handle_api_message(httpd_req_t *req)
{
    recover_from_down();
    char body[1280];
    // ... rest unchanged
```

In `handle_api_background` (line ~252):
```c
static esp_err_t handle_api_background(httpd_req_t *req)
{
    recover_from_down();
    char body[128];
    // ... rest unchanged
```

In `handle_api_heartbeat` (line ~300) — replace the inline recovery with the helper:
```c
static esp_err_t handle_api_heartbeat(httpd_req_t *req)
{
    int64_t now_ms = esp_timer_get_time() / 1000;
    s_last_heartbeat_ms = now_ms;

    // Shift log and add new entry
    for (int i = HEARTBEAT_LOG_SIZE - 1; i > 0; i--) {
        s_heartbeat_log[i] = s_heartbeat_log[i - 1];
    }
    s_heartbeat_log[0] = now_ms;
    if (s_heartbeat_log_count < HEARTBEAT_LOG_SIZE) s_heartbeat_log_count++;

    ESP_LOGI(TAG, "Heartbeat received at %lld ms (log count=%d)", now_ms, s_heartbeat_log_count);

    recover_from_down();

    cJSON *resp = cJSON_CreateObject();
    // ... rest unchanged
```

In `handle_api_model` (line ~333):
```c
static esp_err_t handle_api_model(httpd_req_t *req)
{
    recover_from_down();
    char body[64];
    // ... rest unchanged
```

In `handle_api_audio_play` (line ~419):
```c
static esp_err_t handle_api_audio_play(httpd_req_t *req)
{
    recover_from_down();
    if (s_playing) {
    // ... rest unchanged
```

In `handle_api_voice_put` (line ~471):
```c
static esp_err_t handle_api_voice_put(httpd_req_t *req)
{
    recover_from_down();
    char body[256];
    // ... rest unchanged
```

- [ ] **Step 3: Commit**

```bash
git add pokewatcher/main/web_server.c
git commit -m "feat(web_server): recover from DOWN state on any OpenClaw API call

Add recover_from_down() helper called from all write endpoints. Any
intentional API call from the agent proves it is alive and transitions
the device from DOWN back to IDLE. Read-only endpoints used by the
daemon poll loop do not trigger recovery."
```

---

### Task 5: Stop person/gesture detection from overriding DOWN state

**Files:**
- Modify: `pokewatcher/main/agent_state.c:167-189`

Currently, `PW_EVENT_PERSON_DETECTED` and `PW_EVENT_GESTURE_DETECTED` both transition from DOWN → IDLE. Per requirements, only OpenClaw API calls should recover from DOWN. Person/gesture should wake the display but keep DOWN state.

- [ ] **Step 1: Remove DOWN from person detection recovery**

In `agent_state.c`, find the person detection handler (line 167-175):

```c
case PW_EVENT_PERSON_DETECTED:
    pw_agent_state_set_person_present(true);
    log_presence_event(true);
    pw_renderer_wake_display();
    if (s_state.current_state == PW_STATE_SLEEPING ||
        s_state.current_state == PW_STATE_DOWN) {
        pw_agent_state_set(PW_STATE_IDLE);
    }
    break;
```

Replace with:

```c
case PW_EVENT_PERSON_DETECTED:
    pw_agent_state_set_person_present(true);
    log_presence_event(true);
    pw_renderer_wake_display();
    if (s_state.current_state == PW_STATE_SLEEPING) {
        pw_agent_state_set(PW_STATE_IDLE);
    }
    break;
```

Person detection still wakes the display (line `pw_renderer_wake_display()`), so when in DOWN the screen turns on and shows the DOWN animation + red LED. But it no longer forces IDLE.

- [ ] **Step 2: Remove DOWN from gesture detection recovery**

Find the gesture detection handler (line 184-189):

```c
case PW_EVENT_GESTURE_DETECTED:
    log_gesture_event(event.data.gesture.gesture, event.data.gesture.score, event.data.gesture.box_w, event.data.gesture.box_h);
    ESP_LOGI(TAG, "Gesture: %s (score=%d)", event.data.gesture.gesture, event.data.gesture.score);
    if (s_state.current_state == PW_STATE_SLEEPING ||
        s_state.current_state == PW_STATE_DOWN) {
        pw_agent_state_set(PW_STATE_IDLE);
```

Replace the condition:

```c
case PW_EVENT_GESTURE_DETECTED:
    log_gesture_event(event.data.gesture.gesture, event.data.gesture.score, event.data.gesture.box_w, event.data.gesture.box_h);
    ESP_LOGI(TAG, "Gesture: %s (score=%d)", event.data.gesture.gesture, event.data.gesture.score);
    if (s_state.current_state == PW_STATE_SLEEPING) {
        pw_agent_state_set(PW_STATE_IDLE);
```

Same logic: gesture still wakes the display (the gesture handler further down calls `pw_renderer_wake_display()`), but DOWN is preserved.

- [ ] **Step 3: Verify person-left doesn't break DOWN**

Check the person-left handler (line 176-182):

```c
case PW_EVENT_PERSON_LEFT:
    pw_agent_state_set_person_present(false);
    log_presence_event(false);
    if (s_state.current_state == PW_STATE_IDLE &&
        !pw_dialog_is_visible()) {
        pw_agent_state_set(PW_STATE_SLEEPING);
    }
    break;
```

This only transitions IDLE → SLEEPING when person leaves. It won't touch DOWN state since the guard checks for `PW_STATE_IDLE`. No change needed.

- [ ] **Step 4: Commit**

```bash
git add pokewatcher/main/agent_state.c
git commit -m "fix(agent_state): person/gesture detection no longer overrides DOWN

Person and gesture events wake the display but keep DOWN state intact.
Only OpenClaw API calls can recover from DOWN to IDLE."
```

---

## Summary of behavior after all changes

| Scenario | Before | After |
|----------|--------|-------|
| DOWN + idle timeout | Overwrites to SLEEPING, then display off | Display off, stays DOWN |
| Wake from display-off while DOWN | Plays wakeup anim, returns to SLEEPING | Straight to DOWN anim, no wakeup |
| LED while display sleeping | Blinks red | No LED |
| Person detected while DOWN | Recovers to IDLE | Wakes display, stays DOWN |
| Gesture detected while DOWN | Recovers to IDLE | Wakes display, stays DOWN |
| OpenClaw sends message while DOWN | No recovery | Recovers to IDLE |
| OpenClaw sends heartbeat while DOWN | Recovers to IDLE | Recovers to IDLE (unchanged) |
| OpenClaw sets state while DOWN | No recovery | Recovers to IDLE |
