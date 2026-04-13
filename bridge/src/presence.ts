import { WatcherClient, WatcherStatus } from "./watcher-client";
import { EventWriter } from "./events";

export class PresencePoller {
  private lastPresent: boolean | null = null;
  private debounceCounter = 0;
  private interval: NodeJS.Timeout | null = null;
  private startTime = Date.now();

  constructor(
    private client: WatcherClient,
    private events: EventWriter,
    private pollIntervalMs: number,
    private debounceCount: number
  ) {}

  start(): void {
    this.interval = setInterval(() => this.poll(), this.pollIntervalMs);
    console.log(`[presence] Polling every ${this.pollIntervalMs}ms`);
  }

  stop(): void {
    if (this.interval) clearInterval(this.interval);
  }

  private async poll(): Promise<void> {
    try {
      const status: WatcherStatus = await this.client.getStatus();

      this.events.writeContext({
        person_present: status.person_present,
        agent_state: status.agent_state,
        last_message: status.last_message,
        uptime_seconds: status.uptime_seconds,
        wifi_rssi: status.wifi_rssi,
        bridge_uptime_seconds: Math.floor((Date.now() - this.startTime) / 1000),
      });

      if (this.lastPresent === null) {
        this.lastPresent = status.person_present;
        return;
      }

      if (status.person_present !== this.lastPresent) {
        this.debounceCounter++;
        if (this.debounceCounter >= this.debounceCount) {
          this.lastPresent = status.person_present;
          this.debounceCounter = 0;
          this.events.writeEvent("presence_changed", { present: status.person_present });
          console.log(`[presence] Changed: person_present=${status.person_present}`);
        }
      } else {
        this.debounceCounter = 0;
      }
    } catch (err: any) {
      console.error(`[presence] Poll failed: ${err.message}`);
    }
  }
}
