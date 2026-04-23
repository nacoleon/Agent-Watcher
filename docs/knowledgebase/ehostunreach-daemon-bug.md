# EHOSTUNREACH Daemon Hang — Likely VPN-Related

## Status: Workaround known (2026-04-22). Root cause unconfirmed.

## The Bug

The watcher-daemon Node.js process gets stuck reporting `EHOSTUNREACH 10.0.0.40:80` on every poll, even though:

- `ping 10.0.0.40` works from the same Mac
- `curl http://10.0.0.40/api/status` works from the same Mac
- Other processes on the Mac can reach the Watcher fine

Mac reboot does NOT fix it. Manually restarting only the daemon does.

## Symptoms in Logs

```
2026-04-22T16:14:02.468Z [openclaw-ws] WebSocket closed
2026-04-22T16:14:03.464Z [poll] Watcher unreachable {"error":"connect EHOSTUNREACH 10.0.0.40:80 - Local (10.0.0.17:64827)"}
2026-04-22T16:14:12.469Z [poll] Watcher unreachable {"error":"connect EHOSTUNREACH 10.0.0.40:80 - Local (10.0.0.17:64867)"}
...
```

Note the `Local (10.0.0.17:...)` — the daemon IS binding to the correct LAN source IP on `en1`. So at the socket level, it's using the right interface. Yet the kernel returns EHOSTUNREACH.

## Workaround

```bash
launchctl kickstart -k gui/$(id -u)/ai.openclaw.watcher-daemon
```

Daemon recovers immediately after restart. No Mac reboot needed.

## Prime Suspect: ExpressVPN

The Mac runs ExpressVPN, which installs a Network Kernel Extension and multiple `utun` interfaces (5 active at time of investigation: utun0-utun4). VPN clients cause exactly this symptom via several mechanisms:

1. **Routing table hijack** — VPN pushes default route that blackholes LAN traffic if 10.0.0.0/24 is not explicitly excluded. *Partially ruled out:* the daemon logs show `Local (10.0.0.17:...)` which means the socket was sourced from the LAN IP, not a utun IP.

2. **Kill-switch / firewall at NKE layer** — ExpressVPN's Network Kernel Extension can block packets at L2/L3 even when routing is correct, returning EHOSTUNREACH. This fits the evidence: routing was correct but the kernel refused to deliver the packet.

3. **Per-process routing inconsistency** — macOS VPNs can route different processes through different paths. `curl` (short-lived, fresh socket) may be treated differently than the long-lived daemon process.

4. **Reconnect window** — when the VPN drops and reconnects, long-lived Node.js processes can get stuck in a bad socket state. Short-lived processes avoid it by starting fresh after reconnect completes.

## Why Mac Reboot Doesn't Help

ExpressVPN auto-starts at boot and re-applies its routing/firewall rules. The daemon also auto-starts via LaunchAgent and immediately hits the same VPN-induced bad state. Manual daemon restart works because:
- It happens after the VPN has fully stabilized
- The daemon gets fresh sockets that happen to be treated correctly

## How to Confirm When It Next Happens

Before restarting the daemon:

```bash
# Check routing for the Watcher — should show "interface: en1"
route get 10.0.0.40

# If it shows "interface: utun*" → VPN is hijacking the route
# If it shows "interface: en1" but daemon still fails → kill-switch/NKE layer

# Check if ExpressVPN is in a reconnecting state
ifconfig | grep -E "^utun|^ppp"

# Try curl from command line to confirm network is fine
curl -s --max-time 3 http://10.0.0.40/api/status
```

## Settings to Check in ExpressVPN

- **Allow access to devices on the local network** — must be ON (Preferences → General)
- **Split tunneling** — if enabled, ensure `node` (or the whole daemon) is in the "don't use VPN" list

## Not the Cause

Confirmed NOT caused by:
- Our reboot detection threshold fix (just a comparison change, doesn't touch networking)
- Our MCP server orphan detection change (doesn't affect daemon at all)
- Firmware changes (UI/state only; HTTP server unchanged; EHOSTUNREACH is a Mac kernel error anyway)

## Potential Fix (Intentionally Not Implemented Yet)

Options considered:
- **In-process recovery:** destroy the HTTP agent (`http.globalAgent.destroy()`) and recreate internal state after N consecutive network errors.
- **Process-level recovery:** exit on N consecutive failures; LaunchAgent auto-restarts (`KeepAlive=true` in the plist). Simpler than in-process recovery.

### Why we're not adding it yet (decision 2026-04-22)

1. **It has happened once.** Adding defensive code for a single incident is premature.
2. **It may mask the root cause.** If ExpressVPN's kill-switch/NKE is blocking the node process, a restart-loop might not even work — and we'd never see the real problem clearly.
3. **Project is in maintenance mode** (no new features, critical bug fixes only). This isn't critical until it recurs.
4. **Config fix might be sufficient.** If ExpressVPN's "Allow LAN access" was off, or `node` wasn't in split-tunneling exclusions, a settings change is the right fix — not code.

### Decision sequence when it recurs

1. Before restarting anything, run `route get 10.0.0.40` — capture whether the interface is `en1` or `utun*`.
2. Check ExpressVPN settings (LAN access, split tunneling, kill-switch state).
3. Only if the issue continues after VPN config is verified correct, add process-level self-healing (exit-and-restart via LaunchAgent is preferred over in-process recovery — simpler, more reliable).

## Related Files

- `watcher-mcp/src/daemon.ts` — the affected process
- `~/Library/LaunchAgents/ai.openclaw.watcher-daemon.plist` — LaunchAgent config
- `~/.openclaw/watcher-daemon-logs/` — where EHOSTUNREACH errors get logged
