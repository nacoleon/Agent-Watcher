import { appendFileSync, writeFileSync, mkdirSync } from "fs";
import { dirname } from "path";

export class EventWriter {
  constructor(
    private eventsFile: string,
    private contextFile: string
  ) {
    mkdirSync(dirname(eventsFile), { recursive: true });
    mkdirSync(dirname(contextFile), { recursive: true });
  }

  writeEvent(type: string, data: Record<string, any>): void {
    const event = { type, ...data, timestamp: new Date().toISOString() };
    appendFileSync(this.eventsFile, JSON.stringify(event) + "\n");
  }

  writeContext(context: Record<string, any>): void {
    const data = { ...context, updated_at: new Date().toISOString() };
    writeFileSync(this.contextFile, JSON.stringify(data, null, 2));
  }
}
