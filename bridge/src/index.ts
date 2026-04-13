import express from "express";
import { loadConfig } from "./config";
import { WatcherClient } from "./watcher-client";
import { EventWriter } from "./events";
import { PresencePoller } from "./presence";

const config = loadConfig();
const client = new WatcherClient(config.watcher_url);
const events = new EventWriter(config.events_file, config.context_file);
const poller = new PresencePoller(client, events, config.poll_interval_ms, config.debounce_count);

const app = express();
app.use(express.json());

app.post("/tools/display_message", async (req, res) => {
  try {
    const { text, level } = req.body;
    if (!text) return res.status(400).json({ error: "Missing 'text'" });
    const result = await client.sendMessage(text, level || "info");
    res.json(result);
  } catch (err: any) {
    res.status(502).json({ error: err.message });
  }
});

app.post("/tools/set_state", async (req, res) => {
  try {
    const { state } = req.body;
    if (!state) return res.status(400).json({ error: "Missing 'state'" });
    const result = await client.setState(state);
    res.json(result);
  } catch (err: any) {
    res.status(502).json({ error: err.message });
  }
});

app.get("/tools/get_status", async (_req, res) => {
  try {
    const status = await client.getStatus();
    res.json(status);
  } catch (err: any) {
    res.status(502).json({ error: err.message });
  }
});

app.post("/tools/notify", async (req, res) => {
  try {
    const { text, level, state } = req.body;
    if (!text) return res.status(400).json({ error: "Missing 'text'" });
    if (state) await client.setState(state);
    const result = await client.sendMessage(text, level || "info");
    res.json(result);
  } catch (err: any) {
    res.status(502).json({ error: err.message });
  }
});

app.get("/health", (_req, res) => res.json({ ok: true }));

app.listen(config.bridge_port, () => {
  console.log(`[bridge] Watcher Bridge running on port ${config.bridge_port}`);
  console.log(`[bridge] Watcher URL: ${config.watcher_url}`);
  poller.start();
});
