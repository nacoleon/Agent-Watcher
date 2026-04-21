/**
 * OpenClaw gateway client — sends messages via `openclaw gateway call`.
 *
 * Uses the gateway's RPC method directly instead of `openclaw agent` CLI.
 * The `gateway call` command handles device identity auth automatically
 * and is much faster than the full `agent` command (~5s vs ~40s).
 *
 * Does NOT wait for the full agent response — returns as soon as the
 * gateway accepts the message.
 */

import { spawn } from "node:child_process";
import { randomUUID } from "node:crypto";

const OPENCLAW_BIN = "/opt/homebrew/bin/openclaw";
const TIMEOUT_MS = 30000;

export class OpenClawClient {
  private log: (category: string, message: string, data?: any) => void;

  constructor(logFn?: (category: string, message: string, data?: any) => void) {
    this.log = logFn || ((_c, msg) => console.log(`[openclaw] ${msg}`));
  }

  connect(): void {
    // No-op — gateway call is stateless, no persistent connection needed.
    // Kept for API compatibility with daemon.ts.
    this.log("openclaw", "Client ready (using gateway call)");
  }

  sendToAgent(message: string, agentId: string = "main"): Promise<void> {
    const params = JSON.stringify({
      message,
      agentId,
      deliver: true,
      idempotencyKey: randomUUID(),
    });

    return new Promise<void>((resolve, reject) => {
      const args = [
        "gateway", "call", "agent",
        "--json",
        "--timeout", String(TIMEOUT_MS),
        "--params", params,
      ];

      const child = spawn(OPENCLAW_BIN, args, {
        stdio: ["pipe", "pipe", "pipe"],
        timeout: TIMEOUT_MS + 5000,
      });

      let stdout = "";
      let stderr = "";
      child.stdout.on("data", (d: Buffer) => { stdout += d.toString(); });
      child.stderr.on("data", (d: Buffer) => { stderr += d.toString(); });

      child.on("close", (code) => {
        if (code === 0) {
          resolve();
        } else {
          reject(new Error(stderr.slice(0, 200) || stdout.slice(0, 200) || `exit code ${code}`));
        }
      });

      child.on("error", (err: any) => {
        reject(new Error(`spawn failed: ${err.message}`));
      });
    });
  }
}
