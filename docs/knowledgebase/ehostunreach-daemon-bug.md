# EHOSTUNREACH Daemon Hang — macOS Local Network Permission (TCC)

## Status: ROOT CAUSE CONFIRMED — TCC Local Network permission denied for launchd-spawned node (2026-04-29)

## The Bug

The watcher-daemon Node.js process gets stuck reporting `EHOSTUNREACH 10.0.0.40:80` on every poll, even though:

- `ping 10.0.0.40` works from the same Mac
- `curl http://10.0.0.40/api/status` works from the same Mac
- Other node processes launched from the shell reach the Watcher fine

Mac reboot does NOT fix it. Daemon restart sometimes fixes it transiently, sometimes doesn't.

Voice input is broken while this is active (audio queues on the device but the daemon can't fetch it).

## Root Cause: macOS Sequoia Local Network permission

macOS 13+ requires apps to be explicitly granted Local Network access (System Settings → Privacy & Security → Local Network). The permission is **per-TCC-identity**, not per-binary.

The `node` binary at `/opt/homebrew/bin/node` is **ad-hoc signed** (no Apple Developer ID). For ad-hoc-signed binaries, macOS creates a TCC identity tied to the launch context. As a result, **the same node binary spawned by Terminal vs. spawned by launchd ends up as TWO separate entries in the Local Network permission list**.

Verified on 2026-04-29: System Settings → Privacy & Security → Local Network showed two `node` entries. One was ON (Terminal-spawned), one was OFF (launchd-spawned daemon). Toggling the OFF one to ON immediately fixed the daemon — even with ExpressVPN active and connected.

Why ExpressVPN looked like the cause:
- ExpressVPN's NKE enforces Local Network permission stricter than the kernel does on its own
- With VPN OFF, the kernel allowed the LAN traffic despite the missing permission
- With VPN ON, the NKE checked TCC and denied → returned EHOSTUNREACH
- We mistakenly thought the VPN was filtering by process lineage; really it was just enforcing the OS's permission decision

## The Fix

1. Open **System Settings → Privacy & Security → Local Network**
2. Find the OFF `node` entry (there will be two — toggle the off one ON)
3. Restart the daemon:
   ```
   launchctl kickstart -k gui/$(id -u)/ai.openclaw.watcher-daemon
   ```
4. Verify with `tail -f ~/.openclaw/watcher-daemon-logs/daemon-*.log` — should see `[poll] alive` lines, no EHOSTUNREACH

That's it. No code changes needed. Survives Mac reboots, VPN reconnects, etc.

## Why daemon restart sometimes "worked" before

When VPN was disconnecting/reconnecting, its NKE was briefly bypassing TCC checks. A daemon restart that happened to land in that window would succeed. Once VPN stabilized again, the NKE would resume enforcing TCC — but existing connections weren't always re-evaluated. So the daemon could appear healthy for hours after a "lucky" restart, then suddenly fail again.

Without VPN, the kernel alone is more lenient about Local Network permission (it doesn't always block, especially for IP-literal connections to RFC1918 ranges). That's why the bug never showed up when VPN was off.

## Diagnosis Checklist (if this happens again)

```bash
# 1. Is the device actually reachable?
curl -s --max-time 3 http://10.0.0.40/api/status
# If this fails, it's a real network issue.

# 2. Routing not hijacked
route get 10.0.0.40
# Should show "interface: en1"

# 3. Confirm shell-spawned node works
/opt/homebrew/bin/node -e "require('http').get('http://10.0.0.40/api/status', r => console.log('OK', r.statusCode)).on('error', e => console.log('ERR', e.code))"
# If this works but the daemon fails, it's TCC.

# 4. Open System Settings → Privacy & Security → Local Network
# Two `node` entries? Toggle the OFF one to ON. Done.
```

## Why two `node` entries

`node` is ad-hoc signed. macOS TCC tracks ad-hoc-signed binaries by a hash-based identifier that includes the launch context (responsible process). When launchd executes node, TCC assigns one identity. When Terminal executes node, TCC assigns a different identity. Both reference the same on-disk binary, but TCC treats them as separate apps.

Verified via system log:
```
identifier=node-55554944a5c9bf5685733a0e9128c5a5e702b44d
binary_path=/opt/homebrew/Cellar/node@22/22.22.1_3/bin/node
```

## Not the Cause

Confirmed NOT caused by:
- Reboot detection threshold fix in daemon.ts (just a comparison change)
- MCP server orphan detection change (doesn't affect daemon networking)
- Firmware changes (UI/state only; device HTTP server unchanged)
- Stuck Node.js sockets (fresh daemon process hits the same TCC denial)
- Mac network stack (curl/ping/shell-node all work)
- ExpressVPN's split tunneling or "Allow LAN access" setting (both already correct)
- Routing table hijack
- macOS Network Lock / kill switch

## Symptoms in Logs

```
[heartbeat] FAILED — watcher unreachable {"error":"connect EHOSTUNREACH 10.0.0.40:80 - Local (10.0.0.17:57079)"}
[poll] Watcher unreachable {"error":"connect EHOSTUNREACH 10.0.0.40:80 - Local (10.0.0.17:63751)"}
```

Note `Local (10.0.0.17:...)` — the daemon is correctly binding to the LAN source IP. The block is at the OS permission layer, not the routing/socket layer.

Downstream effect: if the MCP server's `heartbeat` tool call fails with EHOSTUNREACH for 1.5 hours, the firmware transitions to `PW_STATE_DOWN` and the Watcher displays the DOWN animation. With our 2026-04-24 firmware fix, the device automatically recovers to IDLE on the next successful heartbeat after the daemon comes back.

## Related Files

- `watcher-mcp/src/daemon.ts` — affected process
- `watcher-mcp/src/tools.ts` — contains the heartbeat tool that fails when this bug is active
- `~/Library/LaunchAgents/ai.openclaw.watcher-daemon.plist` — LaunchAgent config (uses `/opt/homebrew/bin/node`)
- `~/.openclaw/watcher-daemon-logs/` — EHOSTUNREACH errors logged here
- `~/.openclaw/watcher-mcp-logs/` — heartbeat FAILED entries logged here

## Occurrences

- 2026-04-22 — First seen, misdiagnosed as VPN reconnect race
- 2026-04-24 — Confirmed VPN-correlated but blamed VPN process filter
- 2026-04-27 — Daemon restart didn't fix; voice input also broken
- 2026-04-29 — Root cause identified as TCC; one-toggle fix
