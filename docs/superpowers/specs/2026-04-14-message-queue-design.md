# Message Queue & Read Confirmations — Design Spec

## Overview

Add a message queue to the MCP server so messages don't get lost when the Watcher dialog is already showing. Remove the firmware's 10-second auto-dismiss timer so messages stay until the user manually dismisses with the knob. The MCP server detects dismiss events via polling and notifies Zidane with read confirmations.

## Architecture

Queue lives in the MCP server. Firmware stays simple — shows one message at a time, stays until knob press dismisses.

```
Zidane → display_message → MCP Queue → (when dialog free) → Firmware → Screen
                                                                ↓
                               Zidane ← "message_read" ← MCP Poller ← status poll
```

## Firmware Changes

### 1. Remove auto-dismiss timer

In `dialog.c`, delete the elapsed time check in `pw_dialog_tick()` (lines 278-281):

```c
// DELETE THIS:
int64_t elapsed = now_ms() - s_show_time_ms;
if (elapsed > PW_DIALOG_DISPLAY_MS) {
    pw_dialog_hide();
}
```

Dialog now stays visible until knob press dismisses it. Page scroll still resets `s_show_time_ms` but it's no longer used for auto-dismiss (can be left or removed).

### 2. Add `dialog_visible` to status API

In `web_server.c`, add to `handle_api_status()`:

```c
cJSON_AddBoolToObject(root, "dialog_visible", pw_dialog_is_visible());
```

`pw_dialog_is_visible()` already exists in `dialog.c` — just needs to be called from the status handler.

### 3. No other firmware changes

Pagination, knob scroll, knob dismiss, page indicator, wakeup animation — all unchanged.

## MCP Server Changes

### New file: `queue.ts`

FIFO message queue with dismiss detection and notification logic.

**Queue state:**
- `messages: Array<{ text: string, level: string }>` — pending messages, max 10
- `currentlyShowing: boolean` — tracks whether firmware is showing a message we sent
- `lastDialogVisible: boolean` — previous poll's `dialog_visible` value, for edge detection

**Core logic:**

```
on display_message(text, level):
  if queue is full (10): return error "queue full"
  if not currentlyShowing and not dialog_visible:
    send to firmware immediately
    currentlyShowing = true
    return { sent: true, pending: queue.length }
  else:
    push to queue
    return { queued: true, position: queue.length, pending: queue.length }

on poll (every 5s):
  read dialog_visible from status
  if lastDialogVisible was true AND now false:
    // message was dismissed
    currentlyShowing = false
    notify Zidane: "message_read"
    if queue has messages:
      shift next message, send to firmware
      currentlyShowing = true
    else:
      notify Zidane: "queue_empty"
  lastDialogVisible = dialog_visible
```

**Sleep behavior:**

Messages sent while display is off: firmware stores in LVGL state, `dialog_visible` returns `true`. MCP server sees this and queues subsequent messages. When display wakes, message is visible. On knob dismiss, `dialog_visible` goes `false`, triggering normal flow.

The `"message_read"` notification only fires on the `true → false` transition of `dialog_visible` — it doesn't matter whether the display was on or off when the message was sent.

### MCP Notifications

Using `server.sendLoggingMessage()`:

| Logger | Data | When |
|--------|------|------|
| `messages` | `"message_read"` | `dialog_visible` transitions `true → false` |
| `messages` | `"queue_empty"` | Last message dismissed, no more pending |
| `presence` | `"person_arrived"` | Existing — unchanged |
| `presence` | `"person_left"` | Existing — unchanged |

### Tool Changes

**`display_message`** — Now queues instead of sending directly. Returns:
```json
{ "sent": true, "pending": 0 }
```
or:
```json
{ "queued": true, "position": 2, "pending": 2 }
```
or error:
```json
{ "error": "queue full (10 messages pending)" }
```

**New tool: `get_queue`** — Returns current queue state:
```json
{
  "currently_showing": true,
  "pending": [
    { "text": "check email", "level": "info" },
    { "text": "meeting in 5", "level": "warning" }
  ],
  "count": 2
}
```

**`notify`** — State change (`set_state`) happens immediately as before. Only the message part goes through the queue.

### Updated `WatcherStatus` interface

Add `dialog_visible` field:

```typescript
export interface WatcherStatus {
  agent_state: string;
  person_present: boolean;
  last_message: string;
  uptime_seconds: number;
  wifi_rssi?: number;
  dialog_visible: boolean;  // NEW
}
```

## File Changes Summary

**Firmware (pokewatcher/main/):**
- `dialog.c` — Remove auto-dismiss timer (delete 4 lines)
- `web_server.c` — Add `dialog_visible` to status response (add 1 line)

**MCP server (watcher-mcp/src/):**
- `queue.ts` — CREATE: Message queue, dismiss detection, notification logic
- `tools.ts` — Modify: `display_message` and `notify` go through queue, add `get_queue` tool
- `presence.ts` — Modify: Pass `dialog_visible` to queue for edge detection
- `watcher-client.ts` — Modify: Add `dialog_visible` to `WatcherStatus` interface

## Edge Cases

- **Queue full:** `display_message` returns error. Zidane can check with `get_queue` and decide to wait.
- **Watcher offline:** Same as before — tools return `isError` responses. Queue drains when Watcher comes back (poller retries).
- **Rapid dismiss:** User presses knob multiple times quickly. Poller runs every 5s so it might miss intermediate states. This is fine — the queue just sends the next message on the next detected dismiss. Worst case: 5-second delay between messages.
- **Message sent while another is showing:** Queued. No message loss.
- **Display sleeps with message showing:** Message persists in LVGL state. Shows after wake. Normal dismiss flow resumes.
