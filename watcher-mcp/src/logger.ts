import fs from "node:fs";
import path from "node:path";

const LOG_DIR = path.join(
  process.env.HOME || "/tmp",
  ".openclaw",
  "watcher-mcp-logs"
);
const MAX_LOG_SIZE = 500 * 1024; // 500KB per file, then rotate

let logFile: string;
let stream: fs.WriteStream;

function ensureDir(): void {
  if (!fs.existsSync(LOG_DIR)) {
    fs.mkdirSync(LOG_DIR, { recursive: true });
  }
}

function getLogFile(): string {
  const date = new Date().toISOString().slice(0, 10); // YYYY-MM-DD
  return path.join(LOG_DIR, `watcher-mcp-${date}.log`);
}

function openStream(): void {
  logFile = getLogFile();
  stream = fs.createWriteStream(logFile, { flags: "a" });
}

function rotateIfNeeded(): void {
  try {
    const stats = fs.statSync(logFile);
    if (stats.size > MAX_LOG_SIZE) {
      stream.end();
      const rotated = logFile.replace(".log", `-${Date.now()}.log`);
      fs.renameSync(logFile, rotated);
      openStream();
    }
  } catch {
    // File may not exist yet
  }

  // Also rotate if the date changed
  const expected = getLogFile();
  if (expected !== logFile) {
    stream.end();
    openStream();
  }
}

function cleanOldLogs(): void {
  const maxAge = 3 * 24 * 60 * 60 * 1000; // 3 days
  const now = Date.now();
  try {
    for (const file of fs.readdirSync(LOG_DIR)) {
      if (!file.startsWith("watcher-mcp-")) continue;
      const filePath = path.join(LOG_DIR, file);
      const stats = fs.statSync(filePath);
      if (now - stats.mtimeMs > maxAge) {
        fs.unlinkSync(filePath);
      }
    }
  } catch {
    // ignore cleanup errors
  }
}

export function initLogger(): void {
  ensureDir();
  cleanOldLogs();
  openStream();
  log("system", "MCP server started");
}

export function log(category: string, message: string, data?: any): void {
  rotateIfNeeded();
  const ts = new Date().toISOString();
  const line = data
    ? `${ts} [${category}] ${message} ${JSON.stringify(data)}`
    : `${ts} [${category}] ${message}`;
  stream.write(line + "\n");
}
