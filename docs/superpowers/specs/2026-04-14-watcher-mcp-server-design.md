# Watcher MCP Server — Design Spec

## Overview

Replace the Express-based HTTP bridge with a stdio MCP server that OpenClaw spawns as a child process. Zidane auto-discovers Watcher tools via MCP protocol instead of manual TOOLS.md definitions. Presence changes push directly to Zidane via MCP notifications instead of file-polling.

## Architecture

```
OpenClaw (Zidane) ←—stdio—→ Watcher MCP Server ←—HTTP—→ Watcher Firmware (10.0.0.40)
                              (Node.js process)
```

- OpenClaw spawns the MCP server via stdio transport
- MCP server makes HTTP requests to the Watcher firmware API
- Presence poller runs as a background interval inside the server process
- OpenClaw manages the process lifecycle (start/stop/restart)

## Tools

### `display_message`

Show a message in the FF9 dialog box on the Watcher display.

| Param | Type | Required | Description |
|-------|------|----------|-------------|
| `text` | string | yes | Message text, max 1000 chars. Paginated at 80 chars/page on device. |
| `level` | string | no | `info` (default), `warning`, or `alert` |

Calls `POST http://10.0.0.40/api/message` with `{ text, level }`.

### `set_state`

Change the agent's visual state on the Watcher display.

| Param | Type | Required | Description |
|-------|------|----------|-------------|
| `state` | string | yes | One of: `idle`, `working`, `waiting`, `alert`, `greeting`, `sleeping`, `reporting`, `down` |

`wakeup` is excluded — it's firmware-triggered only (plays when display wakes from off).

Calls `PUT http://10.0.0.40/api/agent-state` with `{ state }`.

### `get_status`

Read the current Watcher state. No parameters.

Returns:
```json
{
  "agent_state": "idle",
  "person_present": false,
  "uptime_seconds": 758,
  "wifi_rssi": -42
}
```

Calls `GET http://10.0.0.40/api/status`.

### `notify`

Convenience tool — sets state and shows a message in one call.

| Param | Type | Required | Description |
|-------|------|----------|-------------|
| `state` | string | yes | Same values as `set_state` |
| `text` | string | yes | Message text, max 1000 chars |
| `level` | string | no | `info` (default), `warning`, or `alert` |

Calls `set_state` then `display_message` sequentially.

### `reboot`

Reboot the Watcher device. No parameters. Hardware-like reboot (power cycles LCD/AI chip before restart).

Calls `POST http://10.0.0.40/api/reboot`.

## Resources

### `watcher://status`

Live Watcher state, polled from device every 5 seconds.

Returns the same payload as `get_status`: agent_state, person_present, uptime_seconds, wifi_rssi.

Subscribable — clients can subscribe to updates. The server emits `notifications/resources/updated` when the polled status changes.

## Presence Notifications

The server runs a 5-second poller against `GET http://10.0.0.40/api/status` (reusing the debounce logic from the existing bridge's `presence.ts` — 2-poll debounce to avoid flapping).

On presence change, the server sends an MCP log message via `server.sendLoggingMessage()`:

```json
{
  "method": "notifications/message",
  "params": {
    "level": "info",
    "logger": "presence",
    "data": "person_arrived"
  }
}
```

Values for `data`: `"person_arrived"` or `"person_left"`. The `logger` field is always `"presence"` so Zidane can filter for it.

Zidane receives this and decides the response (greeting, sleeping, etc.). The MCP server does not auto-set any state — notify only.

## Project Structure

```
watcher-mcp/
  package.json          — name: watcher-mcp, type: module
  tsconfig.json         — ES2022 target, ESM output
  src/
    index.ts            — MCP server setup, stdio transport, starts presence poller
    tools.ts            — 5 tool handlers (display_message, set_state, get_status, notify, reboot)
    resources.ts        — watcher://status resource + subscription
    presence.ts         — 5s poller, 2-poll debounce, sends MCP notifications
    watcher-client.ts   — HTTP client for Watcher firmware API
    config.ts           — Watcher IP (10.0.0.40), poll interval (5s), debounce count (2)
```

New directory at project root alongside `bridge/`. The old bridge code stays as reference but is no longer used.

## OpenClaw Configuration

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

Tools will be auto-discovered as `watcher__display_message`, `watcher__set_state`, `watcher__get_status`, `watcher__notify`, `watcher__reboot`.

## What Gets Removed

After the MCP server is verified working:

- **LaunchAgent** `ai.openclaw.watcher-bridge` — unload and remove plist
- **TOOLS.md** `watcher.*` tool definitions (lines ~83-108) — auto-discovered via MCP now
- **HEARTBEAT.md** Desk Context section (lines ~69-95) — presence comes via MCP notifications
- **Context/event files** `~/.openclaw/watcher-context.json` and `watcher-events.json` — no longer needed

## Dependencies

- `@modelcontextprotocol/sdk` — MCP server SDK (stdio transport, tool/resource registration)
- Node.js native `fetch` (Node 18+) — HTTP calls to Watcher firmware

## Error Handling

- **Watcher offline:** Tools return error responses with descriptive messages. Presence poller logs failures but keeps retrying. No crash.
- **MCP transport error:** Handled by the MCP SDK. Stdio is reliable (no network involved).
- **Timeout:** HTTP requests to Watcher timeout after 5 seconds (device is on local network).

## Testing

- **Unit:** Mock `watcher-client.ts` responses, verify tool handlers return correct MCP responses
- **Integration:** Run MCP server with stdio, send tool calls via MCP inspector, verify Watcher receives correct HTTP requests
- **End-to-end:** Register in OpenClaw, have Zidane call `watcher__set_state`, verify display changes on device
