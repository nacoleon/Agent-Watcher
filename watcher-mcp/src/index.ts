import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { registerTools } from "./tools.js";
import { registerResources } from "./resources.js";
import { startPresencePoller } from "./presence.js";

const server = new McpServer(
  {
    name: "watcher",
    version: "1.0.0",
  },
  {
    capabilities: {
      logging: {},
    },
  }
);

registerTools(server);
registerResources(server);

const transport = new StdioServerTransport();
await server.connect(transport);

startPresencePoller(server);
