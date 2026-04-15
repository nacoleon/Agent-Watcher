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
