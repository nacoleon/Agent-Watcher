import { z } from "zod";
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { VALID_STATES } from "./config.js";
import * as watcher from "./watcher-client.js";
import { enqueue, getQueueState } from "./queue.js";
import { log } from "./logger.js";

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
        "Show a message on the Watcher display and set Zidane's visual state. " +
        "State controls sprite animation (e.g., alert=combat pose, greeting=wave). " +
        "Max 1000 chars, paginated at ~95 chars/page. " +
        "Messages queue if dialog is already showing — state applies when that message reaches the screen. " +
        "You'll receive a 'message_read' notification when dismissed. " +
        "Defaults to 'reporting' state if not specified.",
      inputSchema: {
        text: z.string().max(1000).describe("Message text to display"),
        state: z
          .enum(VALID_STATES)
          .default("reporting")
          .describe("Agent visual state (default: reporting)"),
        level: z
          .enum(["info", "warning", "alert"])
          .default("info")
          .describe("Message urgency level"),
      },
    },
    async ({ text, state, level }: { text: string; state: string; level: string }) => {
      log("tool", "display_message", { state, level, text: text.slice(0, 80) });
      try {
        const result = await enqueue(text, level, state);
        log("tool", "display_message result", result);
        return ok(result);
      } catch (err: any) {
        log("error", "display_message failed", { error: err.message });
        return error(err.message);
      }
    }
  );

  server.registerTool(
    "set_state",
    {
      title: "Set Agent State",
      description:
        "Change Zidane's visual state without showing a message. " +
        "Use for silent state changes like going to sleep or idle.",
      inputSchema: {
        state: z
          .enum(VALID_STATES)
          .describe("Agent visual state"),
      },
    },
    async ({ state }: { state: string }) => {
      log("tool", "set_state", { state });
      try {
        const result = await watcher.setState(state);
        return ok(result);
      } catch (err: any) {
        log("error", "set_state failed", { state, error: err.message });
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

  server.registerTool(
    "heartbeat",
    {
      title: "Heartbeat",
      description:
        "Send a heartbeat to the Watcher to indicate OpenClaw is alive. " +
        "Call this every hour. If the Watcher doesn't receive a heartbeat " +
        "for 1.5 hours, it switches to 'down' state. Sending a heartbeat " +
        "while in 'down' state auto-recovers to idle.",
    },
    async () => {
      log("heartbeat", "sending");
      try {
        const result = await watcher.heartbeat();
        log("heartbeat", "ok", result);
        return ok(result);
      } catch (err: any) {
        log("heartbeat", "FAILED — watcher unreachable", { error: err.message });
        return error(`Watcher error: ${err.message}`);
      }
    }
  );
}
