import { z } from "zod";
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { VALID_STATES } from "./config.js";
import * as watcher from "./watcher-client.js";
import { enqueue, getQueueState } from "./queue.js";

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
        "Max 1000 chars, paginated at 80 chars/page on device. " +
        "Messages queue if dialog is already showing. " +
        "You'll receive a 'message_read' notification when dismissed.",
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
        return ok(await enqueue(text, level));
      } catch (err: any) {
        return error(err.message);
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
        "Read current Watcher state: agent_state, person_present, uptime, wifi signal, dialog_visible.",
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
        "Convenience: set agent state immediately, then queue a message for display.",
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
        return ok(await enqueue(text, level, state));
      } catch (err: any) {
        return error(err.message);
      }
    }
  );

  server.registerTool(
    "get_queue",
    {
      title: "Get Message Queue",
      description:
        "Check the message queue: whether a message is currently showing, and what's pending.",
    },
    async () => {
      return ok(getQueueState());
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
