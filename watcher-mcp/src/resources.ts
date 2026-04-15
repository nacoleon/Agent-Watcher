import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
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
