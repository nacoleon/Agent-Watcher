import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import * as watcher from "./watcher-client.js";

export function registerResources(server: McpServer): void {
  server.registerResource(
    "status",
    "watcher://status",
    {
      title: "Watcher Status",
      description:
        "Live Watcher state: agent_state, person_present, uptime, wifi_rssi. " +
        "Fetched on demand from the device.",
      mimeType: "application/json",
    },
    async () => {
      const status = await watcher.getStatus();
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
