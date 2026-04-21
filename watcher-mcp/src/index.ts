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

// --- Idle timeout: exit if no stdin activity for 5 minutes ---
// OpenClaw gateway doesn't always close the stdio pipe, leaving orphaned
// processes. Tool calls complete in seconds, so 5 min of silence = abandoned.
const IDLE_TIMEOUT_MS = 5 * 60 * 1000;
let idleTimer = setTimeout(() => shutdown("idle timeout (5m)"), IDLE_TIMEOUT_MS);
process.stdin.on("data", () => {
  clearTimeout(idleTimer);
  idleTimer = setTimeout(() => shutdown("idle timeout (5m)"), IDLE_TIMEOUT_MS);
});
