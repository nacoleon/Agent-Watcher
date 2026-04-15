# Message Queue & Read Confirmations Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a message queue to the MCP server so messages don't get lost, remove firmware auto-dismiss so messages stay until knob press, and notify Zidane when messages are read.

**Architecture:** Firmware gets two tiny changes (remove auto-dismiss, expose dialog_visible in status). MCP server gets a new queue module that intercepts display_message calls, tracks dismiss events via the existing 5s poller, and sends MCP log notifications on read.

**Tech Stack:** C (ESP-IDF firmware), TypeScript (MCP server), `@modelcontextprotocol/sdk`

**Spec:** `docs/superpowers/specs/2026-04-14-message-queue-design.md`

---

## File Map

```
Firmware (pokewatcher/main/):
  dialog.c        — MODIFY: Remove auto-dismiss timer (delete 4 lines)
  web_server.c    — MODIFY: Add dialog_visible to status API (add 1 line)

MCP server (watcher-mcp/src/):
  queue.ts         — CREATE: Message FIFO, dismiss detection, notification dispatch
  watcher-client.ts — MODIFY: Add dialog_visible to WatcherStatus interface
  tools.ts         — MODIFY: display_message/notify go through queue, add get_queue tool
  presence.ts      — MODIFY: Pass dialog_visible to queue on each poll
  index.ts         — MODIFY: Import and wire queue to poller and tools
```

---

### Task 1: Firmware — Remove Auto-Dismiss and Expose Dialog Visibility

**Files:**
- Modify: `pokewatcher/main/dialog.c:278-281`
- Modify: `pokewatcher/main/web_server.c:64`

**IMPORTANT — Build from /tmp:** The project path has a space that breaks the ESP-IDF linker. See `docs/knowledgebase/building-and-flashing-firmware.md` for the full workflow. Summary:
1. rsync `pokewatcher/` to `/tmp/pokewatcher-build/`
2. Build from `/tmp/pokewatcher-build/`
3. Flash with `app-flash` only (preserves NVS)
4. Serial port: `/dev/cu.usbmodem5A8A0533623`

- [ ] **Step 1: Remove the auto-dismiss timer in dialog.c**

In `pokewatcher/main/dialog.c`, delete lines 278-281 in `pw_dialog_tick()`:

```c
// DELETE these 3 lines at the end of pw_dialog_tick():
    int64_t elapsed = now_ms() - s_show_time_ms;
    if (elapsed > PW_DIALOG_DISPLAY_MS) {
        pw_dialog_hide();
    }
```

The closing brace of `pw_dialog_tick()` should now come right after the knob_prev block. The function should end like this:

```c
    if (s_knob_prev) {
        s_knob_prev = false;
        pw_dialog_prev_page();
    }
}
```

- [ ] **Step 2: Add dialog_visible to the status API in web_server.c**

In `pokewatcher/main/web_server.c`, add this line after the `background` line (line 64) in `handle_api_status()`:

```c
    cJSON_AddBoolToObject(root, "dialog_visible", pw_dialog_is_visible());
```

The `#include "dialog.h"` is already present at the top of web_server.c (it's used for `pw_dialog_get_last_text()`).

- [ ] **Step 3: Sync to /tmp and build**

```bash
rsync -a --exclude=build --exclude=.cache --exclude=sdkconfig \
  "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/pokewatcher/" \
  /tmp/pokewatcher-build/

export IDF_PATH="/Users/nacoleon/esp/esp-idf"
. "$IDF_PATH/export.sh"
cd /tmp/pokewatcher-build
idf.py build 2>&1 | tail -10
```

Expected: Build succeeds with no errors.

- [ ] **Step 4: Flash**

```bash
idf.py -p /dev/cu.usbmodem5A8A0533623 app-flash 2>&1 | tail -5
```

Expected: Flash succeeds.

- [ ] **Step 5: Verify dialog_visible appears in status API**

```bash
curl -s http://10.0.0.40/api/status | python3 -m json.tool
```

Expected: JSON includes `"dialog_visible": false` (no message showing).

Then test sending a message:
```bash
curl -s -X POST http://10.0.0.40/api/message -H 'Content-Type: application/json' -d '{"text":"test","level":"info"}'
curl -s http://10.0.0.40/api/status | python3 -m json.tool
```

Expected: `"dialog_visible": true`. Message stays on screen (no auto-dismiss after 10s). Press the physical knob button to dismiss — then status should return `"dialog_visible": false`.

- [ ] **Step 6: Commit**

```bash
git add pokewatcher/main/dialog.c pokewatcher/main/web_server.c
git commit -m "feat(firmware): remove auto-dismiss, expose dialog_visible in status API"
```

---

### Task 2: MCP Server — Add dialog_visible to WatcherStatus

**Files:**
- Modify: `watcher-mcp/src/watcher-client.ts:4-10`

- [ ] **Step 1: Add dialog_visible to the WatcherStatus interface**

In `watcher-mcp/src/watcher-client.ts`, update the interface:

```typescript
export interface WatcherStatus {
  agent_state: string;
  person_present: boolean;
  last_message: string;
  uptime_seconds: number;
  wifi_rssi?: number;
  dialog_visible: boolean;
}
```

- [ ] **Step 2: Verify it compiles**

Run: `cd watcher-mcp && npx tsc --noEmit`
Expected: No errors (dialog_visible is just a new field, all existing code still works)

- [ ] **Step 3: Commit**

```bash
git add watcher-mcp/src/watcher-client.ts
git commit -m "feat(mcp): add dialog_visible to WatcherStatus interface"
```

---

### Task 3: MCP Server — Create Message Queue Module

**Files:**
- Create: `watcher-mcp/src/queue.ts`

- [ ] **Step 1: Create queue.ts**

```typescript
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import * as watcher from "./watcher-client.js";

const MAX_QUEUE_SIZE = 10;

interface QueuedMessage {
  text: string;
  level: string;
}

let queue: QueuedMessage[] = [];
let currentlyShowing = false;
let lastDialogVisible = false;
let server: McpServer | null = null;

export function initQueue(mcpServer: McpServer): void {
  server = mcpServer;
}

export async function enqueue(
  text: string,
  level: string
): Promise<{ sent: boolean; queued: boolean; position?: number; pending: number }> {
  if (queue.length >= MAX_QUEUE_SIZE) {
    throw new Error(`queue full (${MAX_QUEUE_SIZE} messages pending)`);
  }

  if (!currentlyShowing && !lastDialogVisible) {
    await watcher.sendMessage(text, level);
    currentlyShowing = true;
    return { sent: true, queued: false, pending: queue.length };
  }

  queue.push({ text, level });
  return { sent: false, queued: true, position: queue.length, pending: queue.length };
}

export async function onPoll(dialogVisible: boolean): Promise<void> {
  if (lastDialogVisible && !dialogVisible) {
    currentlyShowing = false;

    if (server) {
      await server.sendLoggingMessage({
        level: "info",
        logger: "messages",
        data: "message_read",
      });
    }

    if (queue.length > 0) {
      const next = queue.shift()!;
      try {
        await watcher.sendMessage(next.text, next.level);
        currentlyShowing = true;
      } catch {
        // Watcher offline — put message back at front
        queue.unshift(next);
      }
    } else if (server) {
      await server.sendLoggingMessage({
        level: "info",
        logger: "messages",
        data: "queue_empty",
      });
    }
  }

  lastDialogVisible = dialogVisible;
}

export function getQueueState(): {
  currently_showing: boolean;
  pending: QueuedMessage[];
  count: number;
} {
  return {
    currently_showing: currentlyShowing,
    pending: [...queue],
    count: queue.length,
  };
}
```

- [ ] **Step 2: Verify it compiles**

Run: `cd watcher-mcp && npx tsc --noEmit`
Expected: No errors

- [ ] **Step 3: Commit**

```bash
git add watcher-mcp/src/queue.ts
git commit -m "feat(mcp): message queue with dismiss detection and notifications"
```

---

### Task 4: MCP Server — Wire Queue into Poller

**Files:**
- Modify: `watcher-mcp/src/presence.ts`

- [ ] **Step 1: Update presence.ts to call onPoll**

Replace the entire file content:

```typescript
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { POLL_INTERVAL_MS, DEBOUNCE_COUNT } from "./config.js";
import * as watcher from "./watcher-client.js";
import { updateCachedStatus } from "./resources.js";
import { onPoll } from "./queue.js";

export function startPresencePoller(server: McpServer): void {
  let lastPresent: boolean | null = null;
  let debounceCounter = 0;

  setInterval(async () => {
    try {
      const status = await watcher.getStatus();
      updateCachedStatus(status);

      // Message queue dismiss detection
      await onPoll(status.dialog_visible);

      // Presence change detection
      if (lastPresent === null) {
        lastPresent = status.person_present;
        return;
      }

      if (status.person_present !== lastPresent) {
        debounceCounter++;
        if (debounceCounter >= DEBOUNCE_COUNT) {
          lastPresent = status.person_present;
          debounceCounter = 0;

          await server.sendLoggingMessage({
            level: "info",
            logger: "presence",
            data: status.person_present ? "person_arrived" : "person_left",
          });
        }
      } else {
        debounceCounter = 0;
      }
    } catch (err: any) {
      // Watcher offline — keep retrying silently
    }
  }, POLL_INTERVAL_MS);
}
```

- [ ] **Step 2: Verify it compiles**

Run: `cd watcher-mcp && npx tsc --noEmit`
Expected: No errors

- [ ] **Step 3: Commit**

```bash
git add watcher-mcp/src/presence.ts
git commit -m "feat(mcp): wire message queue dismiss detection into poller"
```

---

### Task 5: MCP Server — Update Tools to Use Queue

**Files:**
- Modify: `watcher-mcp/src/tools.ts`

- [ ] **Step 1: Update tools.ts**

Replace the entire file content. Changes: `display_message` and `notify` go through `enqueue()`, new `get_queue` tool added.

```typescript
import { z } from "zod";
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { VALID_STATES } from "./config.js";
import * as watcher from "./watcher-client.js";
import { enqueue, getQueueState } from "./queue.js";

function error(msg: string) {
  return { content: [{ type: "text" as const, text: msg }], isError: true };
}

function ok(data: any) {
  return { content: [{ type: "text" as const, text: JSON.stringify(data) }] };
}

export function registerTools(server: McpServer): void {
  server.registerTool(
    "display_message",
    {
      title: "Display Message",
      description:
        "Show a message in the FF9 dialog box on the Watcher display. " +
        "Max 1000 chars, paginated at 80 chars/page on device. " +
        "Messages queue if dialog is already showing. " +
        "You'll receive a 'message_read' notification when dismissed.",
      inputSchema: {
        text: z.string().max(1000).describe("Message text to display"),
        level: z
          .enum(["info", "warning", "alert"])
          .default("info")
          .describe("Message urgency level"),
      },
    },
    async ({ text, level }: { text: string; level: string }) => {
      try {
        return ok(await enqueue(text, level));
      } catch (err: any) {
        return error(err.message);
      }
    }
  );

  server.registerTool(
    "set_state",
    {
      title: "Set Agent State",
      description:
        "Change Zidane's visual state on the Watcher display. " +
        "Controls sprite animation and position.",
      inputSchema: {
        state: z
          .enum(VALID_STATES)
          .describe("Agent visual state"),
      },
    },
    async ({ state }: { state: string }) => {
      try {
        return ok(await watcher.setState(state));
      } catch (err: any) {
        return error(`Watcher error: ${err.message}`);
      }
    }
  );

  server.registerTool(
    "get_status",
    {
      title: "Get Watcher Status",
      description:
        "Read current Watcher state: agent_state, person_present, uptime, wifi signal, dialog_visible.",
    },
    async () => {
      try {
        return ok(await watcher.getStatus());
      } catch (err: any) {
        return error(`Watcher error: ${err.message}`);
      }
    }
  );

  server.registerTool(
    "notify",
    {
      title: "Notify",
      description:
        "Convenience: set agent state immediately, then queue a message for display.",
      inputSchema: {
        state: z
          .enum(VALID_STATES)
          .describe("Agent visual state"),
        text: z.string().max(1000).describe("Message text to display"),
        level: z
          .enum(["info", "warning", "alert"])
          .default("info")
          .describe("Message urgency level"),
      },
    },
    async ({ state, text, level }: { state: string; text: string; level: string }) => {
      try {
        await watcher.setState(state);
      } catch (err: any) {
        return error(`Failed to set state: ${err.message}`);
      }
      try {
        return ok(await enqueue(text, level));
      } catch (err: any) {
        return error(`State set to ${state}, but message failed: ${err.message}`);
      }
    }
  );

  server.registerTool(
    "get_queue",
    {
      title: "Get Message Queue",
      description:
        "Check the message queue: whether a message is currently showing, and what's pending.",
    },
    async () => {
      return ok(getQueueState());
    }
  );

  server.registerTool(
    "reboot",
    {
      title: "Reboot Watcher",
      description:
        "Hardware-like reboot of the Watcher device. Power cycles LCD and AI chip before restart.",
    },
    async () => {
      try {
        return ok(await watcher.reboot());
      } catch (err: any) {
        return error(`Watcher error: ${err.message}`);
      }
    }
  );
}
```

- [ ] **Step 2: Verify it compiles**

Run: `cd watcher-mcp && npx tsc --noEmit`
Expected: No errors

- [ ] **Step 3: Commit**

```bash
git add watcher-mcp/src/tools.ts
git commit -m "feat(mcp): display_message and notify use queue, add get_queue tool"
```

---

### Task 6: MCP Server — Wire Queue Init in Entry Point

**Files:**
- Modify: `watcher-mcp/src/index.ts`

- [ ] **Step 1: Update index.ts to initialize the queue**

Replace the entire file content:

```typescript
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { registerTools } from "./tools.js";
import { registerResources } from "./resources.js";
import { startPresencePoller } from "./presence.js";
import { initQueue } from "./queue.js";

const server = new McpServer(
  {
    name: "watcher",
    version: "1.1.0",
  },
  {
    capabilities: {
      logging: {},
    },
  }
);

initQueue(server);
registerTools(server);
registerResources(server);

const transport = new StdioServerTransport();
await server.connect(transport);

startPresencePoller(server);
```

- [ ] **Step 2: Build the project**

Run: `cd watcher-mcp && npm run build`
Expected: `dist/` directory updated, no errors

- [ ] **Step 3: Commit**

```bash
git add watcher-mcp/src/index.ts
git commit -m "feat(mcp): wire queue init into server entry point"
```

---

### Task 7: Test End-to-End

- [ ] **Step 1: Verify MCP server starts**

```bash
cd "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher"
printf '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"test","version":"1.0.0"}}}\n' | node watcher-mcp/dist/index.js 2>/dev/null &
PID=$!; sleep 3; kill $PID 2>/dev/null; wait $PID 2>/dev/null
```

Expected: JSON response with 6 tools (display_message, set_state, get_status, notify, get_queue, reboot).

- [ ] **Step 2: Restart OpenClaw gateway**

```bash
openclaw gateway restart
```

Wait 3 seconds, then verify:
```bash
openclaw gateway status 2>&1 | grep "Runtime"
```

Expected: `Runtime: running`

- [ ] **Step 3: Test via Zidane**

Ask Zidane (via Discord or `openclaw agent`):
> "Send two messages to the watcher: first 'Hello' then 'World'"

Expected: First message appears on Watcher display. Second is queued. After dismissing first with knob, second appears automatically.

- [ ] **Step 4: Commit**

```bash
git add watcher-mcp/
git commit -m "feat(mcp): message queue and read confirmations complete"
```
