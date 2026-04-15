import { z } from "zod";
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { VALID_STATES } from "./config.js";
import * as watcher from "./watcher-client.js";

function error(msg: string) {
  return { content: [{ type: "text" as const, text: msg }], isError: true };
}

function ok(data: any) {
  return { content: [{ type: "text" as const, text: JSON.stringify(data) }] };
}

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
      try {
        return ok(await watcher.sendMessage(text, level));
      } catch (err: any) {
        return error(`Watcher error: ${err.message}`);
      }
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
      try {
        return ok(await watcher.setState(state));
      } catch (err: any) {
        return error(`Watcher error: ${err.message}`);
      }
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
      try {
        return ok(await watcher.getStatus());
      } catch (err: any) {
        return error(`Watcher error: ${err.message}`);
      }
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
      try {
        await watcher.setState(state);
      } catch (err: any) {
        return error(`Failed to set state: ${err.message}`);
      }
      try {
        return ok(await watcher.sendMessage(text, level));
      } catch (err: any) {
        return error(`State set to ${state}, but message failed: ${err.message}`);
      }
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
      try {
        return ok(await watcher.reboot());
      } catch (err: any) {
        return error(`Watcher error: ${err.message}`);
      }
    }
  );
}
