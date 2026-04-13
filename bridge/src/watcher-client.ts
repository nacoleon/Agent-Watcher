import http from "http";

export interface WatcherStatus {
  agent_state: string;
  person_present: boolean;
  last_message: string;
  uptime_seconds: number;
  wifi_rssi?: number;
}

export class WatcherClient {
  constructor(private baseUrl: string) {}

  private request(method: string, path: string, body?: object): Promise<any> {
    return new Promise((resolve, reject) => {
      const url = new URL(path, this.baseUrl);
      const data = body ? JSON.stringify(body) : undefined;

      const req = http.request(
        {
          hostname: url.hostname,
          port: url.port || 80,
          path: url.pathname,
          method,
          headers: data
            ? { "Content-Type": "application/json", "Content-Length": Buffer.byteLength(data) }
            : {},
          timeout: 5000,
        },
        (res) => {
          let chunks: Buffer[] = [];
          res.on("data", (chunk) => chunks.push(chunk));
          res.on("end", () => {
            const text = Buffer.concat(chunks).toString();
            try {
              resolve(JSON.parse(text));
            } catch {
              resolve(text);
            }
          });
        }
      );
      req.on("error", reject);
      req.on("timeout", () => { req.destroy(); reject(new Error("timeout")); });
      if (data) req.write(data);
      req.end();
    });
  }

  async getStatus(): Promise<WatcherStatus> {
    return this.request("GET", "/api/status");
  }

  async setState(state: string): Promise<any> {
    return this.request("PUT", "/api/agent-state", { state });
  }

  async sendMessage(text: string, level: string = "info"): Promise<any> {
    return this.request("POST", "/api/message", { text, level });
  }
}
