import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import * as watcher from "./watcher-client.js";

const MAX_QUEUE_SIZE = 10;

interface QueuedMessage {
  text: string;
  level: string;
  state?: string;
}

let queue: QueuedMessage[] = [];
let currentlyShowing = false;
let lastDialogVisible = false;
let server: McpServer | null = null;

export function initQueue(mcpServer: McpServer): void {
  server = mcpServer;
}

export async function enqueue(
  text: string,
  level: string,
  state?: string
): Promise<{ sent: boolean; queued: boolean; position?: number; pending: number }> {
  if (queue.length >= MAX_QUEUE_SIZE) {
    throw new Error(`queue full (${MAX_QUEUE_SIZE} messages pending)`);
  }

  if (!currentlyShowing && !lastDialogVisible) {
    if (state) await watcher.setState(state);
    await watcher.sendMessage(text, level);
    currentlyShowing = true;
    return { sent: true, queued: false, pending: queue.length };
  }

  queue.push({ text, level, state });
  return { sent: false, queued: true, position: queue.length, pending: queue.length };
}

export async function onPoll(dialogVisible: boolean): Promise<void> {
  if (lastDialogVisible && !dialogVisible) {
    currentlyShowing = false;

    if (server) {
      await server.sendLoggingMessage({
        level: "info",
        logger: "messages",
        data: "message_read",
      });
    }

    if (queue.length > 0) {
      const next = queue.shift()!;
      try {
        if (next.state) await watcher.setState(next.state);
        await watcher.sendMessage(next.text, next.level);
        currentlyShowing = true;
      } catch {
        queue.unshift(next);
      }
    } else if (server) {
      await server.sendLoggingMessage({
        level: "info",
        logger: "messages",
        data: "queue_empty",
      });
    }
  }

  lastDialogVisible = dialogVisible;
}

export function getQueueState(): {
  currently_showing: boolean;
  pending: QueuedMessage[];
  count: number;
} {
  return {
    currently_showing: currentlyShowing,
    pending: [...queue],
    count: queue.length,
  };
}
