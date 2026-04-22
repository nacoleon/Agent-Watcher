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

function shutdown(reason: string): void {
  log("exit", `pid=${process.pid} reason="${reason}"`);
  setTimeout(() => process.exit(0), 500);
}

transport.onclose = () => shutdown("transport closed");
process.stdin.on("end", () => shutdown("stdin EOF"));
process.stdin.on("close", () => shutdown("stdin closed"));
process.on("SIGTERM", () => shutdown("SIGTERM"));
process.on("SIGINT", () => shutdown("SIGINT"));

// Orphan detection: if parent process dies (gateway crash/restart), we get
// reparented to PID 1 (launchd). Poll ppid to detect this and exit cleanly.
const startupPpid = process.ppid;
setInterval(() => {
  if (process.ppid !== startupPpid) shutdown("parent died (orphaned)");
}, 30_000);
