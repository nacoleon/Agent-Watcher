import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { POLL_INTERVAL_MS, DEBOUNCE_COUNT, WHISPER_MODEL } from "./config.js";
import * as watcher from "./watcher-client.js";
import { updateCachedStatus } from "./resources.js";
import { onPoll, onDeviceReboot } from "./queue.js";
import { log } from "./logger.js";
import { writeFileSync, unlinkSync, mkdirSync } from "node:fs";
import { tmpdir } from "node:os";
import { join, dirname } from "node:path";
import { fileURLToPath } from "node:url";
import { execFileSync } from "node:child_process";

// Use whisper.cpp binary directly (whisper-node wrapper has parsing bugs)
const __dirname = dirname(fileURLToPath(import.meta.url));
const WHISPER_CPP_DIR = join(__dirname, "..", "node_modules", "whisper-node", "lib", "whisper.cpp");
const WHISPER_BIN = join(WHISPER_CPP_DIR, "main");
const WHISPER_MODEL_PATH = join(WHISPER_CPP_DIR, "models", `ggml-${WHISPER_MODEL}.bin`);

function transcribeWithWhisperCpp(wavPath: string): string {
  const output = execFileSync(WHISPER_BIN, [
    "-m", WHISPER_MODEL_PATH,
    "-f", wavPath,
    "-nt",
  ], { encoding: "utf-8", timeout: 30000, stdio: ["pipe", "pipe", "pipe"] });
  // Filter out blank audio markers and whisper log lines
  return output
    .split("\n")
    .map((l) => l.trim())
    .filter((l) => l && !l.startsWith("whisper_") && !l.startsWith("ggml_"))
    .map((l) => l.replace(/\[BLANK_AUDIO\]/g, "").trim())
    .filter((l) => l.length > 0)
    .join(" ")
    .trim();
}

export function startPresencePoller(server: McpServer): void {
  let lastPresent: boolean | null = null;
  let lastUptime: number | null = null;
  let debounceCounter = 0;

  setInterval(async () => {
    try {
      const status = await watcher.getStatus();
      updateCachedStatus(status);

      // Reboot detection — uptime dropped means device restarted
      if (lastUptime !== null && status.uptime_seconds < lastUptime) {
        log("reboot", `Watcher rebooted (uptime ${lastUptime}s → ${status.uptime_seconds}s)`);
        await onDeviceReboot();
      }
      lastUptime = status.uptime_seconds;

      // Message queue dismiss detection
      await onPoll(status.dismiss_count);

      // Voice audio pickup
      if (status.audio_ready) {
        try {
          log("audio", "Audio ready — fetching from Watcher");
          const audioBuffer = await watcher.getAudio();
          await watcher.clearAudio();

          if (audioBuffer.length > 44) {
            const tmpDir = join(tmpdir(), "watcher-audio");
            mkdirSync(tmpDir, { recursive: true });
            const tmpFile = join(tmpDir, `voice-${Date.now()}.wav`);

            try {
              writeFileSync(tmpFile, audioBuffer);
              log("audio", `Transcribing ${audioBuffer.length} bytes`);

              const text = transcribeWithWhisperCpp(tmpFile);

              log("audio", `Transcribed: "${text}"`);

              if (text) {
                // Use MCP sampling (createMessage) so OpenClaw treats this
                // as a real user message and responds naturally
                try {
                  await server.server.createMessage({
                    messages: [
                      {
                        role: "user" as const,
                        content: { type: "text" as const, text: `[Voice from Watcher] ${text}` },
                      },
                    ],
                    systemPrompt: "The user spoke to the Watcher device via push-to-talk. Respond naturally and use display_message to show your reply on the Watcher.",
                    maxTokens: 200,
                  });
                  log("audio", "createMessage sent to OpenClaw");
                } catch {
                  // Fallback if client doesn't support sampling
                  log("audio", "createMessage not supported, falling back to logging");
                  await server.sendLoggingMessage({
                    level: "info",
                    logger: "voice",
                    data: `voice_input: ${text}`,
                  });
                }
              }
            } finally {
              try { unlinkSync(tmpFile); } catch {}
            }
          }
        } catch (err: any) {
          log("error", "Audio pickup/transcription failed", { error: err.message });
        }
      }

      // Presence change detection
      if (lastPresent === null) {
        lastPresent = status.person_present;
        return;
      }

      if (status.person_present !== lastPresent) {
        debounceCounter++;
        if (debounceCounter >= DEBOUNCE_COUNT) {
          lastPresent = status.person_present;
          debounceCounter = 0;

          const event = status.person_present ? "person_arrived" : "person_left";
          log("presence", event);
          await server.sendLoggingMessage({
            level: "info",
            logger: "presence",
            data: event,
          });
        }
      } else {
        debounceCounter = 0;
      }
    } catch (err: any) {
      log("poll", "Watcher unreachable", { error: err.message });
    }
  }, POLL_INTERVAL_MS);
}
