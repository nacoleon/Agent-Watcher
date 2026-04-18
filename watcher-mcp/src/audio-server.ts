import express from "express";
import { writeFileSync, unlinkSync, mkdirSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { whisper } from "whisper-node";
import { AUDIO_PORT, WHISPER_MODEL } from "./config.js";
import { log } from "./logger.js";

export function startAudioServer(
  onTranscription: (text: string) => void
): void {
  const app = express();

  // Accept raw binary body (WAV file from ESP32)
  app.use(
    "/audio",
    express.raw({ type: "application/octet-stream", limit: "1mb" })
  );

  app.post("/audio", async (req, res) => {
    const audioBuffer = req.body as Buffer;
    if (!audioBuffer || audioBuffer.length < 44) {
      res.status(400).json({ error: "No audio data or too short" });
      return;
    }

    log("audio", `Received ${audioBuffer.length} bytes`);

    const tmpDir = join(tmpdir(), "watcher-audio");
    mkdirSync(tmpDir, { recursive: true });
    const tmpFile = join(tmpDir, `voice-${Date.now()}.wav`);

    try {
      writeFileSync(tmpFile, audioBuffer);

      const result = await whisper(tmpFile, {
        modelName: WHISPER_MODEL,
        whisperOptions: { language: "en" },
      });
      const text = result
        .map((r: any) => r.speech)
        .join(" ")
        .trim();

      log("audio", `Transcribed: "${text}"`);

      if (text) {
        onTranscription(text);
      }

      res.json({ ok: true, text });
    } catch (err: any) {
      log("error", "Transcription failed", { error: err.message });
      res.status(500).json({ error: err.message });
    } finally {
      try {
        unlinkSync(tmpFile);
      } catch {}
    }
  });

  app.get("/audio/health", (_req, res) => {
    res.json({ ok: true, model: WHISPER_MODEL });
  });

  app.listen(AUDIO_PORT, () => {
    log("audio", `Audio server listening on :${AUDIO_PORT}`);
  });
}
