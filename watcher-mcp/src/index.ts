import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { registerTools } from "./tools.js";
import { registerResources } from "./resources.js";
import { startPresencePoller } from "./presence.js";
import { initQueue } from "./queue.js";

const server = new McpServer(
  {
    name: "watcher",
    version: "1.1.0",
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
