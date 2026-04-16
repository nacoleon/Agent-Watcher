import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { POLL_INTERVAL_MS, DEBOUNCE_COUNT } from "./config.js";
import * as watcher from "./watcher-client.js";
import { updateCachedStatus } from "./resources.js";
import { onPoll, onDeviceReboot } from "./queue.js";
import { log } from "./logger.js";

export function startPresencePoller(server: McpServer): void {
  let lastPresent: boolean | null = null;
  let lastUptime: number | null = null;
  let debounceCounter = 0;

  setInterval(async () => {
    try {
      const status = await watcher.getStatus();
      updateCachedStatus(status);

      // Reboot detection — uptime dropped means device restarted
      if (lastUptime !== null && status.uptime_seconds < lastUptime) {
        log("reboot", `Watcher rebooted (uptime ${lastUptime}s → ${status.uptime_seconds}s)`);
        await onDeviceReboot();
      }
      lastUptime = status.uptime_seconds;

      // Message queue dismiss detection
      await onPoll(status.dismiss_count);

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

          const event = status.person_present ? "person_arrived" : "person_left";
          log("presence", event);
          await server.sendLoggingMessage({
            level: "info",
            logger: "presence",
            data: event,
          });
        }
      } else {
        debounceCounter = 0;
      }
    } catch (err: any) {
      log("poll", "Watcher unreachable", { error: err.message });
    }
  }, POLL_INTERVAL_MS);
}
