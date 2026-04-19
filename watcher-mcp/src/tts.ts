import { spawn } from "node:child_process";
import { existsSync } from "node:fs";
import { execSync } from "node:child_process";
import { join } from "node:path";
import { log } from "./logger.js";

const PIPER_VOICES_DIR = "/tmp/piper-voices";
const DEFAULT_LENGTH_SCALE = 0.7;

/**
 * Run Piper TTS and return 16kHz 16-bit mono PCM buffer.
 * Piper outputs 22050 Hz — we resample to 16000 Hz in-process.
 */
export async function textToSpeech(
  text: string,
  voice: string
): Promise<Buffer> {
  const modelPath = await ensureModel(voice);
  const raw22k = await runPiper(text, modelPath);
  return resample(raw22k, 22050, 16000);
}

/**
 * Ensure the voice model .onnx file exists. Download from Hugging Face if missing.
 */
async function ensureModel(voice: string): Promise<string> {
  const modelPath = join(PIPER_VOICES_DIR, `${voice}.onnx`);
  const configPath = join(PIPER_VOICES_DIR, `${voice}.onnx.json`);

  if (existsSync(modelPath) && existsSync(configPath)) {
    return modelPath;
  }

  // Parse voice name: en_US-amy-medium → en/en_US/amy/medium/en_US-amy-medium
  const parts = voice.split("-");
  const lang = parts[0]; // en_US
  const langShort = lang.split("_")[0]; // en
  const name = parts[1]; // amy
  const quality = parts[2]; // medium
  const hfBase = `https://huggingface.co/rhasspy/piper-voices/resolve/main/${langShort}/${lang}/${name}/${quality}`;

  log("tts", `Downloading voice model: ${voice}`);

  try {
    execSync(`mkdir -p "${PIPER_VOICES_DIR}"`, { stdio: "pipe" });
    execSync(
      `curl -L -o "${modelPath}" "${hfBase}/${voice}.onnx?download=true"`,
      { stdio: "pipe", timeout: 120000 }
    );
    execSync(
      `curl -L -o "${configPath}" "${hfBase}/${voice}.onnx.json?download=true"`,
      { stdio: "pipe", timeout: 30000 }
    );
    log("tts", `Downloaded voice model: ${voice}`);
  } catch (err: any) {
    log("error", `Failed to download voice model: ${voice}`, { error: err.message });
    throw new Error(`Failed to download voice model: ${voice}`);
  }

  return modelPath;
}

function runPiper(text: string, modelPath: string): Promise<Buffer> {
  return new Promise((resolve, reject) => {
    const proc = spawn("piper", ["--model", modelPath, "--output-raw", "--length-scale", String(DEFAULT_LENGTH_SCALE)], {
      stdio: ["pipe", "pipe", "pipe"],
    });

    const chunks: Buffer[] = [];

    proc.stdout.on("data", (chunk: Buffer) => chunks.push(chunk));

    proc.stderr.on("data", (data: Buffer) => {
      // Piper logs to stderr — only log actual errors
      const msg = data.toString().trim();
      if (msg && !msg.startsWith("[") && !msg.includes("Real-time factor")) {
        log("tts", `piper stderr: ${msg}`);
      }
    });

    proc.on("close", (code) => {
      if (code !== 0) {
        reject(new Error(`piper exited with code ${code}`));
        return;
      }
      resolve(Buffer.concat(chunks));
    });

    proc.on("error", (err) => {
      reject(
        new Error(
          `piper not found — install with: pip install piper-tts (${err.message})`
        )
      );
    });

    proc.stdin.write(text);
    proc.stdin.end();
  });
}

/**
 * Resample 16-bit mono PCM from srcRate to dstRate using linear interpolation.
 */
function resample(input: Buffer, srcRate: number, dstRate: number): Buffer {
  const srcSamples = input.length / 2;
  const dstSamples = Math.floor((srcSamples * dstRate) / srcRate);
  const output = Buffer.alloc(dstSamples * 2);

  for (let i = 0; i < dstSamples; i++) {
    const srcPos = (i * srcRate) / dstRate;
    const srcIdx = Math.floor(srcPos);
    const frac = srcPos - srcIdx;

    const s0 = input.readInt16LE(srcIdx * 2);
    const s1 =
      srcIdx + 1 < srcSamples ? input.readInt16LE((srcIdx + 1) * 2) : s0;

    const sample = Math.round(s0 + frac * (s1 - s0));
    output.writeInt16LE(Math.max(-32768, Math.min(32767, sample)), i * 2);
  }

  return output;
}
