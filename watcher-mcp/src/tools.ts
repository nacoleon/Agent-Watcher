import { z } from "zod";
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { VALID_STATES } from "./config.js";
import * as watcher from "./watcher-client.js";

export function registerTools(server: McpServer): void {
  server.registerTool(
    "display_message",
    {
      title: "Display Message",
      description:
        "Show a message in the FF9 dialog box on the Watcher display. " +
        "Max 1000 chars, paginated at 80 chars/page on device.",
      inputSchema: {
        text: z.string().max(1000).describe("Message text to display"),
        level: z
          .enum(["info", "warning", "alert"])
          .default("info")
          .describe("Message urgency level"),
      },
    },
    async ({ text, level }: { text: string; level: string }) => {
      const result = await watcher.sendMessage(text, level);
      return { content: [{ type: "text" as const, text: JSON.stringify(result) }] };
    }
  );

  server.registerTool(
    "set_state",
    {
      title: "Set Agent State",
      description:
        "Change Zidane's visual state on the Watcher display. " +
        "Controls sprite animation and position.",
      inputSchema: {
        state: z
          .enum(VALID_STATES)
          .describe("Agent visual state"),
      },
    },
    async ({ state }: { state: string }) => {
      const result = await watcher.setState(state);
      return { content: [{ type: "text" as const, text: JSON.stringify(result) }] };
    }
  );

  server.registerTool(
    "get_status",
    {
      title: "Get Watcher Status",
      description:
        "Read current Watcher state: agent_state, person_present, uptime, wifi signal.",
    },
    async () => {
      const status = await watcher.getStatus();
      return { content: [{ type: "text" as const, text: JSON.stringify(status) }] };
    }
  );

  server.registerTool(
    "notify",
    {
      title: "Notify",
      description:
        "Convenience: set agent state and show a message in one call.",
      inputSchema: {
        state: z
          .enum(VALID_STATES)
          .describe("Agent visual state"),
        text: z.string().max(1000).describe("Message text to display"),
        level: z
          .enum(["info", "warning", "alert"])
          .default("info")
          .describe("Message urgency level"),
      },
    },
    async ({ state, text, level }: { state: string; text: string; level: string }) => {
      await watcher.setState(state);
      const result = await watcher.sendMessage(text, level);
      return { content: [{ type: "text" as const, text: JSON.stringify(result) }] };
    }
  );

  server.registerTool(
    "reboot",
    {
      title: "Reboot Watcher",
      description:
        "Hardware-like reboot of the Watcher device. Power cycles LCD and AI chip before restart.",
    },
    async () => {
      const result = await watcher.reboot();
      return { content: [{ type: "text" as const, text: JSON.stringify(result) }] };
    }
  );
}
