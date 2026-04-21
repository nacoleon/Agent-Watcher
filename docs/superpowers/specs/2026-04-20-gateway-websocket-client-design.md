# Direct WebSocket Gateway Client Design

**Date**: 2026-04-20
**Status**: Approved

## Summary

Replace the `openclaw agent` CLI spawn in the daemon with a persistent WebSocket connection to the OpenClaw gateway. This eliminates ~40s of CLI startup overhead per voice message, bringing total voice-to-reply latency from ~55s down to ~15-20s.

## Problem

The daemon's `sendToZidane()` spawns `openclaw agent --deliver` via `spawn("sh", ...)` for each voice message. This CLI command takes ~40s just for Node.js startup + WebSocket handshake before the message even reaches the gateway. The actual LLM inference is only ~15s.

## Decisions

| Decision | Choice | Rationale |
|---|---|---|
| Connection model | Persistent WebSocket | Daemon runs 24/7, zero overhead per message |
| Response wait strategy | Wait for "accepted" only | Don't block on full agent response; MCP tools handle reply delivery independently |
| Auth token source | Read from openclaw config at boot | `execSync("openclaw config get gateway.auth.token")` once at startup, avoids LaunchAgent plist changes |
| Reconnection | Exponential backoff (1s → 30s max) | Handles gateway restarts gracefully |

## Architecture

### New File: `watcher-mcp/src/openclaw-client.ts`

Lightweight WebSocket client (~80 lines) that maintains a persistent connection to `ws://127.0.0.1:18789`.

**Lifecycle:**
1. Created at daemon startup
2. Connects to gateway WebSocket
3. Handles `connect.challenge` event → responds with auth token
4. Stays connected, auto-reconnects on failure
5. Provides `sendToAgent(message, agentId?)` method

**Public API:**
```typescript
class OpenClawClient {
  connect(): void;                                          // Start connection (auto-reconnects)
  sendToAgent(message: string, agentId?: string): Promise<void>;  // Send message, wait for "accepted"
}
```

**Gateway Protocol (version 3):**

Connection handshake:
1. Client opens WebSocket to `ws://127.0.0.1:18789`
2. Server sends: `{ "event": "connect.challenge", "payload": { "nonce": "..." } }`
3. Client sends request:
   ```json
   {
     "type": "req", "id": "<uuid>", "method": "connect",
     "params": {
       "minProtocol": 3, "maxProtocol": 3,
       "client": { "id": "watcher-daemon", "version": "1.0", "mode": "backend" },
       "auth": { "token": "<gateway-token>" },
       "role": "operator",
       "scopes": ["operator.write"],
       "caps": []
     }
   }
   ```
4. Server responds with `{ "ok": true, "id": "...", "payload": { ... } }`

Sending a message:
```json
{
  "type": "req", "id": "<uuid>", "method": "agent",
  "params": {
    "message": "the voice transcription",
    "agentId": "main",
    "deliver": true,
    "idempotencyKey": "<uuid>"
  }
}
```

Response flow:
1. First response: `{ "ok": true, "payload": { "status": "accepted" } }` — message received
2. Later: `{ "ok": true, "payload": { "status": "ok", "result": { ... } } }` — agent finished

We only wait for the first response ("accepted"). The agent's actual reply comes back through MCP tool calls (speak/display_message), which we handle separately.

**Reconnection:**
- On WebSocket close/error, schedule reconnect with exponential backoff
- Backoff: 1s → 2s → 4s → 8s → 16s → 30s (max)
- Reset backoff on successful connection
- Requests made while disconnected fail immediately with an error

**Auth token:**
```typescript
const token = execSync("openclaw config get gateway.auth.token", { encoding: "utf-8" }).trim();
```
Read once at module load time. If the command fails, log error and exit (gateway token is required).

### Changes to `daemon.ts`

**Import and initialize:**
```typescript
import { OpenClawClient } from "./openclaw-client.js";
const openclawClient = new OpenClawClient();
openclawClient.connect();
```

**Replace `sendToZidane`:**

Before (spawn-based, ~25 lines):
```typescript
function sendToZidane(message: string): void {
  const cmd = `openclaw agent --agent main -m ${JSON.stringify(message)} --deliver --timeout 60`;
  const child = spawn("sh", ["-c", cmd], { ... });
  // ... stdout/stderr handlers, close handler, error handler
}
```

After (~5 lines):
```typescript
async function sendToZidane(message: string): Promise<void> {
  try {
    await openclawClient.sendToAgent(message, "main");
    log("openclaw", `Sent to Zidane: "${message}"`);
  } catch (err: any) {
    log("error", "Failed to send to OpenClaw", { error: err.message });
  }
}
```

The `poll()` function calls `sendToZidane` in the voice audio pickup block and the presence change block. Both are inside `try/catch` already. Since `sendToZidane` is now async, the call sites need `await`. The `poll()` function is already `async`.

**Remove unused imports:** `spawn` and `execSync` (for the openclaw command) can be removed from daemon.ts. `execFileSync` stays (used by `transcribe()`).

### Dependencies

Check if `ws` is already available in `node_modules` (other deps may pull it in). If not:
```bash
cd watcher-mcp && npm install ws && npm install -D @types/ws
```

## Files Changed

| File | Action | Change |
|---|---|---|
| `watcher-mcp/src/openclaw-client.ts` | Create | WebSocket client class (~80 lines) |
| `watcher-mcp/src/daemon.ts` | Modify | Replace spawn-based sendToZidane with client call, init client at startup, clean up imports |
| `watcher-mcp/package.json` | Modify | Add `ws` dependency (if not already present) |

## Expected Performance

| Metric | Before (CLI spawn) | After (WebSocket) |
|---|---|---|
| Message delivery to gateway | ~40s | ~150ms |
| Total voice-to-reply | ~55s | ~15-20s |
| CPU per message | 12s user (Node.js startup) | ~0s (no process spawn) |

## Edge Cases

- **Gateway not running at daemon start**: Connection attempt fails, backoff retry kicks in. Once gateway starts, daemon auto-connects.
- **Gateway restarts while daemon is running**: WebSocket closes, reconnect with backoff. Pending requests fail with error, next voice message retries normally.
- **Auth token invalid/expired**: Connect handshake fails, logged as error. Daemon continues running but can't send messages until restart with valid token.
- **Concurrent voice messages**: Each `sendToAgent` call gets its own request ID. The pending Map tracks them independently. No conflicts.
- **Gateway token command fails at boot**: Log error and continue without OpenClaw connection. Voice recording still works, messages just won't be delivered.
