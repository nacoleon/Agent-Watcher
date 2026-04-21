export const WATCHER_URL = process.env.WATCHER_URL || "http://10.0.0.40";
export const POLL_INTERVAL_MS = 1000;
export const DEBOUNCE_COUNT = 2;
export const REQUEST_TIMEOUT_MS = 5000;

export const VALID_STATES = [
  "idle", "working", "waiting", "alert",
  "greeting", "sleeping", "reporting", "down",
] as const;

export type AgentState = typeof VALID_STATES[number];

export const WHISPER_MODEL = "base.en";
