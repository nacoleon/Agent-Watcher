import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { registerTools } from "./tools.js";
import { registerResources } from "./resources.js";
import { startPresencePoller } from "./presence.js";
import { initQueue } from "./queue.js";
import { initLogger } from "./logger.js";
import { startAudioServer } from "./audio-server.js";

initLogger();

const server = new McpServer(
  {
    name: "watcher",
    version: "1.2.0",
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

// Start audio receiver — transcriptions go to OpenClaw as logging messages
startAudioServer(async (text: string) => {
  await server.sendLoggingMessage({
    level: "info",
    logger: "voice",
    data: `voice_input: ${text}`,
  });
});
