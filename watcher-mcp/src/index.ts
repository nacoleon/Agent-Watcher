import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { registerTools } from "./tools.js";
import { registerResources } from "./resources.js";
import { startPresencePoller } from "./presence.js";
import { initQueue } from "./queue.js";
import { initLogger } from "./logger.js";

initLogger();

const server = new McpServer(
  {
    name: "watcher",
    version: "1.3.0",
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

// Exit cleanly when stdio disconnects (prevents zombie processes)
process.stdin.on("end", () => process.exit(0));
process.stdin.on("close", () => process.exit(0));
transport.onclose = () => process.exit(0);
