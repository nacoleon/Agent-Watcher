import http from "node:http";
import { z } from "zod";
import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { VALID_STATES } from "./config.js";
import * as watcher from "./watcher-client.js";
import { log } from "./logger.js";
import { textToSpeech } from "./tts.js";

// --- Daemon API client (message queue lives in the daemon) ---
const DAEMON_API_PORT = 8378;

function daemonRequest(method: string, path: string, body?: object): Promise<any> {
  return new Promise((resolve, reject) => {
    const data = body ? JSON.stringify(body) : undefined;
    const req = http.request(
      {
        hostname: "127.0.0.1",
        port: DAEMON_API_PORT,
        path,
        method,
        headers: data
          ? { "Content-Type": "application/json", "Content-Length": Buffer.byteLength(data) }
          : {},
        timeout: 5000,
      },
      (res) => {
        const chunks: Buffer[] = [];
        res.on("data", (c) => chunks.push(c));
        res.on("end", () => {
          const text = Buffer.concat(chunks).toString();
          try {
            const json = JSON.parse(text);
            if (res.statusCode && res.statusCode >= 400) {
              reject(new Error(json.error || `daemon returned ${res.statusCode}`));
            } else {
              resolve(json);
            }
          } catch {
            reject(new Error(`daemon returned non-JSON: ${text.slice(0, 100)}`));
          }
        });
      }
    );
    req.on("error", (err) => reject(new Error(`Daemon unreachable: ${err.message}`)));
    req.on("timeout", () => { req.destroy(); reject(new Error("Daemon timeout")); });
    if (data) req.write(data);
    req.end();
  });
}

function error(msg: string) {
  return { content: [{ type: "text" as const, text: msg }], isError: true };
}

function ok(data: any) {
  return { content: [{ type: "text" as const, text: JSON.stringify(data) }] };
}

// --- Voice context + response mode helpers ---

async function getVoiceContext(): Promise<boolean> {
  try {
    const result = await daemonRequest("GET", "/voice-context");
    return result?.active === true;
  } catch {
    return false;
  }
}

async function clearVoiceContext(): Promise<void> {
  try {
    await daemonRequest("DELETE", "/voice-context");
  } catch {}
}

async function getResponseMode(): Promise<string> {
  try {
    const config = await watcher.getVoiceConfig();
    return config.response_mode || "both";
  } catch {
    return "both";
  }
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
        const isVoiceReply = await getVoiceContext();
        const responseMode = isVoiceReply ? await getResponseMode() : "text_only";

        // Display on screen unless response_mode is voice_only
        let result: any = {};
        if (responseMode !== "voice_only") {
          result = await daemonRequest("POST", "/queue", { text, level, state });
          log("tool", "display_message result", result);
        }

        // Auto-pair: also speak if response_mode requires it
        if (isVoiceReply && (responseMode === "both" || responseMode === "voice_only")) {
          log("tool", "display_message auto-pairing with speak", { responseMode });
          try {
            let voice: string | undefined;
            const config = await watcher.getVoiceConfig();
            voice = config.voice;

            const pcm = await textToSpeech(text, voice);
            const playResult = await watcher.playAudio(pcm);

            if (playResult?.error === "speaker busy") {
              await new Promise((r) => setTimeout(r, 1000));
              await watcher.playAudio(pcm);
            }
          } catch (err: any) {
            log("error", "auto-pair speak failed", { error: err.message });
          }
        }

        if (isVoiceReply) await clearVoiceContext();

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
      try {
        return ok(await daemonRequest("GET", "/queue"));
      } catch (err: any) {
        return error(`Daemon error: ${err.message}`);
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

  server.registerTool(
    "speak",
    {
      title: "Speak (TTS)",
      description:
        "Speak text aloud through the Watcher's speaker using text-to-speech. " +
        "Use this for responding to voice input from the Watcher. " +
        "Keep responses short and conversational (1-2 sentences). " +
        "The voice parameter is optional — defaults to the voice configured on the device.",
      inputSchema: {
        text: z.string().describe("Text to speak aloud"),
        voice: z
          .string()
          .optional()
          .describe(
            "Piper voice model (e.g. en_US-amy-medium). Omit to use device default."
          ),
      },
    },
    async ({ text, voice }: { text: string; voice?: string }) => {
      if (!text.trim()) {
        return error("No text to speak");
      }
      log("tool", "speak", { voice, text: text.slice(0, 80) });
      try {
        const isVoiceReply = await getVoiceContext();
        const responseMode = isVoiceReply ? await getResponseMode() : "voice_only";

        if (!voice) {
          const config = await watcher.getVoiceConfig();
          voice = config.voice;
        }

        let durationS = 0;

        // Auto-pair: display text BEFORE playing audio so reporting state is visible while speaking
        if (isVoiceReply && (responseMode === "both" || responseMode === "text_only")) {
          log("tool", "speak auto-pairing with display_message", { responseMode });
          try {
            await daemonRequest("POST", "/queue", {
              text,
              level: "info",
              state: "reporting",
            });
          } catch (err: any) {
            log("error", "auto-pair display failed", { error: err.message });
          }
        }

        // Do TTS unless response_mode is text_only
        if (responseMode !== "text_only") {
          const pcm = await textToSpeech(text, voice!);
          durationS = +(pcm.length / 32000).toFixed(1);
          log("tts", `Generated ${pcm.length} bytes of PCM (${durationS}s)`);

          const result = await watcher.playAudio(pcm);

          if (result?.error === "speaker busy") {
            log("tts", "Speaker busy, retrying in 1s");
            await new Promise((r) => setTimeout(r, 1000));
            await watcher.playAudio(pcm);
          }
        }

        if (isVoiceReply) await clearVoiceContext();

        return ok({
          spoke: text,
          voice,
          response_mode: responseMode,
          auto_paired: isVoiceReply && responseMode !== "voice_only",
          duration_s: durationS,
        });
      } catch (err: any) {
        log("error", "speak failed", { error: err.message });
        return error(`Speak failed: ${err.message}`);
      }
    }
  );
}
