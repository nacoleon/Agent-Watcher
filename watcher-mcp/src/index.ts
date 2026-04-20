import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { registerTools } from "./tools.js";
import { registerResources } from "./resources.js";
import { initLogger, log } from "./logger.js";

initLogger();
log("system", `MCP server started pid=${process.pid}`);

const server = new McpServer(
  {
    name: "watcher",
    version: "1.4.0",
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

// --- Lifecycle: clean exit when gateway closes the pipe ---
// The MCP server is now stateless (no poller, no queue, no timers).
// Zombie processes are harmless — they just sit idle using ~5MB RAM.

function shutdown(reason: string): void {
  log("exit", `pid=${process.pid} reason="${reason}"`);
  setTimeout(() => process.exit(0), 500);
}

transport.onclose = () => shutdown("transport closed");
process.stdin.on("end", () => shutdown("stdin EOF"));
process.stdin.on("close", () => shutdown("stdin closed"));
process.on("SIGTERM", () => shutdown("SIGTERM"));
process.on("SIGINT", () => shutdown("SIGINT"));
