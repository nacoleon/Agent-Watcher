# Watcher MCP Server Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Express HTTP bridge with a stdio MCP server so OpenClaw's Zidane agent auto-discovers Watcher tools and receives presence notifications via MCP protocol.

**Architecture:** Node.js stdio MCP server spawned by OpenClaw as a child process. Reuses the HTTP client pattern from the existing bridge to talk to Watcher firmware at 10.0.0.40. Presence poller runs inside the server process and pushes MCP log notifications on change.

**Tech Stack:** TypeScript, `@modelcontextprotocol/sdk` (stdio transport), `zod` (tool schemas), Node.js native `http` module (Watcher HTTP calls)

**Spec:** `docs/superpowers/specs/2026-04-14-watcher-mcp-server-design.md`

---

## File Map

```
watcher-mcp/
  package.json          — CREATE: name watcher-mcp, deps: @modelcontextprotocol/sdk, zod
  tsconfig.json         — CREATE: ES2022 target, ESM output (type: module)
  src/
    index.ts            — CREATE: MCP server setup, stdio transport, starts presence poller
    tools.ts            — CREATE: 5 tool registrations (display_message, set_state, get_status, notify, reboot)
    resources.ts        — CREATE: watcher://status resource with subscription
    presence.ts         — CREATE: 5s poller, 2-poll debounce, sends MCP log notifications
    watcher-client.ts   — CREATE: HTTP client for Watcher firmware API (adapted from bridge/src/watcher-client.ts)
    config.ts           — CREATE: Watcher IP, poll interval, debounce count constants
```

---

### Task 1: Project Scaffold

**Files:**
- Create: `watcher-mcp/package.json`
- Create: `watcher-mcp/tsconfig.json`

- [ ] **Step 1: Create package.json**

```json
{
  "name": "watcher-mcp",
  "version": "1.0.0",
  "description": "MCP server for SenseCap Watcher — tools and presence for OpenClaw agents",
  "type": "module",
  "main": "dist/index.js",
  "scripts": {
    "build": "tsc",
    "start": "node dist/index.js"
  },
  "dependencies": {
    "@modelcontextprotocol/sdk": "^1.29.0",
    "zod": "^3.24.0"
  },
  "devDependencies": {
    "@types/node": "^20.0.0",
    "typescript": "^5.0.0"
  }
}
```

- [ ] **Step 2: Create tsconfig.json**

```json
{
  "compilerOptions": {
    "target": "ES2022",
    "module": "Node16",
    "moduleResolution": "Node16",
    "outDir": "dist",
    "rootDir": "src",
    "strict": true,
    "esModuleInterop": true,
    "declaration": true
  },
  "include": ["src/**/*"]
}
```

- [ ] **Step 3: Install dependencies**

Run: `cd watcher-mcp && npm install`
Expected: `node_modules/` created, `package-lock.json` generated, no errors

- [ ] **Step 4: Commit**

```bash
git add watcher-mcp/package.json watcher-mcp/tsconfig.json watcher-mcp/package-lock.json
git commit -m "feat(mcp): scaffold watcher-mcp project"
```

---

### Task 2: Config and Watcher HTTP Client

**Files:**
- Create: `watcher-mcp/src/config.ts`
- Create: `watcher-mcp/src/watcher-client.ts`

- [ ] **Step 1: Create config.ts**

```typescript
export const WATCHER_URL = "http://10.0.0.40";
export const POLL_INTERVAL_MS = 5000;
export const DEBOUNCE_COUNT = 2;
export const REQUEST_TIMEOUT_MS = 5000;

export const VALID_STATES = [
  "idle", "working", "waiting", "alert",
  "greeting", "sleeping", "reporting", "down",
] as const;

export type AgentState = typeof VALID_STATES[number];
```

- [ ] **Step 2: Create watcher-client.ts**

Adapted from `bridge/src/watcher-client.ts` — same HTTP client logic, uses native `http` module with 5s timeout.

```typescript
import http from "node:http";
import { WATCHER_URL, REQUEST_TIMEOUT_MS } from "./config.js";

export interface WatcherStatus {
  agent_state: string;
  person_present: boolean;
  last_message: string;
  uptime_seconds: number;
  wifi_rssi?: number;
}

function request(method: string, path: string, body?: object): Promise<any> {
  return new Promise((resolve, reject) => {
    const url = new URL(path, WATCHER_URL);
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
        timeout: REQUEST_TIMEOUT_MS,
      },
      (res) => {
        const chunks: Buffer[] = [];
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
    req.on("timeout", () => {
      req.destroy();
      reject(new Error("timeout"));
    });
    if (data) req.write(data);
    req.end();
  });
}

export async function getStatus(): Promise<WatcherStatus> {
  return request("GET", "/api/status");
}

export async function setState(state: string): Promise<any> {
  return request("PUT", "/api/agent-state", { state });
}

export async function sendMessage(text: string, level: string = "info"): Promise<any> {
  return request("POST", "/api/message", { text, level });
}

export async function reboot(): Promise<any> {
  return request("POST", "/api/reboot");
}
```

- [ ] **Step 3: Verify it compiles**

Run: `cd watcher-mcp && npx tsc --noEmit`
Expected: No errors

- [ ] **Step 4: Commit**

```bash
git add watcher-mcp/src/config.ts watcher-mcp/src/watcher-client.ts
git commit -m "feat(mcp): add config and watcher HTTP client"
```

---

### Task 3: Tool Registrations

**Files:**
- Create: `watcher-mcp/src/tools.ts`

- [ ] **Step 1: Create tools.ts**

Registers all 5 tools on the McpServer instance. Each tool validates input via zod schemas and calls the watcher-client functions.

```typescript
import { z } from "zod";
import { McpServer } from "@modelcontextprotocol/sdk/server/index.js";
import { VALID_STATES } from "./config.js";
import * as watcher from "./watcher-client.js";

export function registerTools(server: McpServer): void {
  server.registerTool(
    "display_message",
    {
      title: "Display Message",
      description:
        "Show a message in the FF9 dialog box on the Watcher display. " +
        "Max 1000 chars, paginated at 80 chars/page on device.",
      inputSchema: {
        text: z.string().max(1000).describe("Message text to display"),
        level: z
          .enum(["info", "warning", "alert"])
          .default("info")
          .describe("Message urgency level"),
      },
    },
    async ({ text, level }) => {
      const result = await watcher.sendMessage(text, level);
      return { content: [{ type: "text" as const, text: JSON.stringify(result) }] };
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
    async ({ state }) => {
      const result = await watcher.setState(state);
      return { content: [{ type: "text" as const, text: JSON.stringify(result) }] };
    }
  );

  server.registerTool(
    "get_status",
    {
      title: "Get Watcher Status",
      description:
        "Read current Watcher state: agent_state, person_present, uptime, wifi signal.",
    },
    async () => {
      const status = await watcher.getStatus();
      return { content: [{ type: "text" as const, text: JSON.stringify(status) }] };
    }
  );

  server.registerTool(
    "notify",
    {
      title: "Notify",
      description:
        "Convenience: set agent state and show a message in one call.",
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
    async ({ state, text, level }) => {
      await watcher.setState(state);
      const result = await watcher.sendMessage(text, level);
      return { content: [{ type: "text" as const, text: JSON.stringify(result) }] };
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
      const result = await watcher.reboot();
      return { content: [{ type: "text" as const, text: JSON.stringify(result) }] };
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
git commit -m "feat(mcp): register 5 watcher tools"
```

---

### Task 4: Status Resource

**Files:**
- Create: `watcher-mcp/src/resources.ts`

- [ ] **Step 1: Create resources.ts**

Registers the `watcher://status` resource. Returns the latest cached status from the presence poller (or fetches fresh if no cache).

```typescript
import { McpServer } from "@modelcontextprotocol/sdk/server/index.js";
import * as watcher from "./watcher-client.js";
import type { WatcherStatus } from "./watcher-client.js";

let cachedStatus: WatcherStatus | null = null;

export function updateCachedStatus(status: WatcherStatus): void {
  cachedStatus = status;
}

export function registerResources(server: McpServer): void {
  server.registerResource(
    "status",
    "watcher://status",
    {
      title: "Watcher Status",
      description:
        "Live Watcher state: agent_state, person_present, uptime, wifi_rssi. " +
        "Updated every 5 seconds by presence poller.",
      mimeType: "application/json",
    },
    async () => {
      const status = cachedStatus ?? (await watcher.getStatus());
      return {
        contents: [
          {
            uri: "watcher://status",
            text: JSON.stringify(status),
            mimeType: "application/json",
          },
        ],
      };
    }
  );
}
```

- [ ] **Step 2: Verify it compiles**

Run: `cd watcher-mcp && npx tsc --noEmit`
Expected: No errors

- [ ] **Step 3: Commit**

```bash
git add watcher-mcp/src/resources.ts
git commit -m "feat(mcp): add watcher://status resource"
```

---

### Task 5: Presence Poller with MCP Notifications

**Files:**
- Create: `watcher-mcp/src/presence.ts`

- [ ] **Step 1: Create presence.ts**

Polls Watcher status every 5s, debounces presence changes (2 consecutive polls), sends MCP log notifications and updates the resource cache.

```typescript
import { McpServer } from "@modelcontextprotocol/sdk/server/index.js";
import { POLL_INTERVAL_MS, DEBOUNCE_COUNT } from "./config.js";
import * as watcher from "./watcher-client.js";
import { updateCachedStatus } from "./resources.js";

export function startPresencePoller(server: McpServer): void {
  let lastPresent: boolean | null = null;
  let debounceCounter = 0;

  setInterval(async () => {
    try {
      const status = await watcher.getStatus();
      updateCachedStatus(status);

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
git commit -m "feat(mcp): presence poller with MCP log notifications"
```

---

### Task 6: Server Entry Point

**Files:**
- Create: `watcher-mcp/src/index.ts`

- [ ] **Step 1: Create index.ts**

Wires everything together: creates McpServer, registers tools and resources, connects stdio transport, starts presence poller.

```typescript
import { McpServer } from "@modelcontextprotocol/sdk/server/index.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/index.js";
import { registerTools } from "./tools.js";
import { registerResources } from "./resources.js";
import { startPresencePoller } from "./presence.js";

const server = new McpServer(
  {
    name: "watcher",
    version: "1.0.0",
  },
  {
    capabilities: {
      logging: {},
    },
  }
);

registerTools(server);
registerResources(server);

const transport = new StdioServerTransport();
await server.connect(transport);

startPresencePoller(server);
```

- [ ] **Step 2: Build the project**

Run: `cd watcher-mcp && npm run build`
Expected: `dist/` directory created with compiled .js files, no errors

- [ ] **Step 3: Commit**

```bash
git add watcher-mcp/src/index.ts
git commit -m "feat(mcp): server entry point with stdio transport"
```

---

### Task 7: Build, Test with MCP Inspector, Register in OpenClaw

**Files:**
- Modify: `~/.openclaw/openclaw.json` (add `mcp.servers.watcher` entry)

- [ ] **Step 1: Verify the server starts and exits cleanly**

Run: `echo '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26","capabilities":{},"clientInfo":{"name":"test","version":"1.0.0"}}}' | node watcher-mcp/dist/index.js 2>/dev/null | head -1`

Expected: JSON response containing `"result"` with server info and capabilities (tools, resources, logging). The server should not hang — it reads one message from stdin, responds, then waits for more.

- [ ] **Step 2: Test with MCP inspector (if available)**

Run: `npx @modelcontextprotocol/inspector node watcher-mcp/dist/index.js`

This opens a browser UI where you can:
- See the 5 registered tools
- See the `watcher://status` resource
- Call tools interactively (will fail if Watcher device is offline, but tool registration is verified)

- [ ] **Step 3: Register in OpenClaw**

Add to `~/.openclaw/openclaw.json` under `mcp.servers`:

```json
{
  "watcher": {
    "command": "node",
    "args": ["dist/index.js"],
    "cwd": "/Users/nacoleon/HQ/02-Projects/SenseCap Watcher/watcher-mcp"
  }
}
```

Run: `openclaw mcp list`
Expected: `watcher` appears in the list of configured MCP servers

- [ ] **Step 4: Commit**

```bash
git add watcher-mcp/
git commit -m "feat(mcp): watcher MCP server complete and registered"
```

---

### Task 8: Clean Up Old Bridge

Only do this after verifying the MCP server works end-to-end with OpenClaw.

**Files:**
- Modify: `~/clawd/TOOLS.md` (remove watcher.* tool definitions, lines ~83-108)
- Modify: `~/clawd/HEARTBEAT.md` (remove Desk Context section, lines ~69-95)

- [ ] **Step 1: Unload the old bridge LaunchAgent**

Run: `launchctl unload ~/Library/LaunchAgents/ai.openclaw.watcher-bridge.plist`
Expected: No output (service stops)

Verify: `curl -s http://localhost:3847/health` should fail (connection refused)

- [ ] **Step 2: Remove watcher.* tool definitions from TOOLS.md**

Open `~/clawd/TOOLS.md` and remove the `watcher.display_message`, `watcher.set_state`, `watcher.get_status`, `watcher.notify` definitions (lines ~83-108). These are now auto-discovered via MCP.

- [ ] **Step 3: Remove Desk Context section from HEARTBEAT.md**

Open `~/clawd/HEARTBEAT.md` and remove the Desk Context section (lines ~69-95) that reads `watcher-context.json` and `watcher-events.json`. Presence now comes via MCP notifications.

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "chore: remove old bridge config, tools auto-discovered via MCP"
```
