#!/usr/bin/env python3
"""Local preview server for the PokéWatcher web dashboard.

Serves the static web files and mocks all REST API endpoints with
realistic sample data so you can preview the UI without hardware.

Usage: python3 tools/preview_server.py
Then open http://localhost:8080
"""

import json
import os
import time
from http.server import HTTPServer, SimpleHTTPRequestHandler
from urllib.parse import urlparse

WEB_DIR = os.path.join(os.path.dirname(__file__), '..', 'main', 'web')
SDCARD_DIR = os.path.join(os.path.dirname(__file__), '..', 'sdcard', 'pokemon')

# --- Mock state ---
mock_state = {
    "active_id": "pikachu",
    "mood": "happy",
    "person_present": True,
    "evolution_seconds": 14400,  # 4 hours
    "roster": [
        {"id": "pikachu", "evolution_seconds": 14400},
        {"id": "charmander", "evolution_seconds": 7200},
        {"id": "eevee", "evolution_seconds": 0},
    ],
    "timeline": [
        "Pika pika! Someone's here! I was getting so lonely...",
        "Hmm, they left again. I'll just look around for a bit.",
        "Zzz... maybe they'll come back soon... zzz...",
        "*bounces excitedly* PIKA! You're back!!",
        "Feeling happy just being here with you. Pika~",
    ],
    "settings": {
        "mood": {
            "excited_duration_ms": 10000,
            "overjoyed_duration_ms": 15000,
            "curious_timeout_ms": 300000,
            "lonely_timeout_ms": 900000,
        },
        "llm": {
            "endpoint": "https://api.openai.com/v1/chat/completions",
            "model": "gpt-4o-mini",
            "api_key_set": True,
        }
    }
}

# Cycle mood for demo effect
MOODS = ["excited", "happy", "curious", "lonely", "sleepy", "overjoyed"]
_mood_idx = [1]  # mutable in closure


def load_pokemon_defs():
    """Load all pokemon.json files from sdcard dir."""
    defs = {}
    if not os.path.isdir(SDCARD_DIR):
        return defs
    for name in os.listdir(SDCARD_DIR):
        pj = os.path.join(SDCARD_DIR, name, 'pokemon.json')
        if os.path.isfile(pj):
            with open(pj) as f:
                defs[name] = json.load(f)
    return defs


POKEMON_DEFS = load_pokemon_defs()


def get_roster_response():
    roster_ids = {e["id"] for e in mock_state["roster"]}
    entries = []
    for e in mock_state["roster"]:
        d = POKEMON_DEFS.get(e["id"], {})
        entries.append({
            "id": e["id"],
            "name": d.get("name", e["id"]),
            "evolves_to": d.get("evolves_to", ""),
            "evolution_hours": d.get("evolution_hours", 24),
            "evolution_seconds": e["evolution_seconds"],
        })
    available = [pid for pid in POKEMON_DEFS if pid not in roster_ids]
    return {
        "active_id": mock_state["active_id"],
        "entries": entries,
        "available": sorted(available),
    }


def get_status_response():
    active = mock_state["active_id"]
    d = POKEMON_DEFS.get(active, {})
    return {
        "active_pokemon": active,
        "mood": mock_state["mood"],
        "person_present": mock_state["person_present"],
        "evolution_seconds": mock_state["evolution_seconds"],
        "evolution_threshold_hours": d.get("evolution_hours", 24),
        "evolves_to": d.get("evolves_to", ""),
        "last_commentary": mock_state["timeline"][-1] if mock_state["timeline"] else "",
    }


class MockHandler(SimpleHTTPRequestHandler):
    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path

        # API routes
        if path == '/api/status':
            # Cycle mood every few refreshes for demo
            _mood_idx[0] = (_mood_idx[0] + 1) % (len(MOODS) * 3)
            mock_state["mood"] = MOODS[_mood_idx[0] // 3]
            mock_state["evolution_seconds"] += 180
            self._json_response(get_status_response())
        elif path == '/api/roster':
            self._json_response(get_roster_response())
        elif path == '/api/settings':
            self._json_response(mock_state["settings"])
        elif path == '/api/timeline':
            self._json_response(mock_state["timeline"])
        elif path.startswith('/sprites/'):
            # Serve sprite PNGs from sdcard data
            pokemon_id = path.split('/')[2] if len(path.split('/')) >= 3 else ''
            sprite_path = os.path.join(SDCARD_DIR, pokemon_id, 'overworld.png')
            if os.path.isfile(sprite_path):
                self._serve_file(sprite_path)
            else:
                self.send_error(404)
        elif path.startswith('/frames/'):
            # Serve frames.json for animation data
            pokemon_id = path.split('/')[2] if len(path.split('/')) >= 3 else ''
            frames_path = os.path.join(SDCARD_DIR, pokemon_id, 'frames.json')
            if os.path.isfile(frames_path):
                self._serve_file(frames_path)
            else:
                self.send_error(404)
        else:
            # Serve static files — map clean URLs to HTML files
            if path == '/':
                path = '/index.html'
            elif path in ('/roster', '/settings'):
                path = path + '.html'
            filepath = os.path.join(WEB_DIR, path.lstrip('/'))
            if os.path.isfile(filepath):
                self._serve_file(filepath)
            else:
                self.send_error(404)

    def do_POST(self):
        parsed = urlparse(self.path)
        if parsed.path == '/api/roster':
            body = self._read_body()
            data = json.loads(body)
            pid = data.get("id", "")
            if pid and pid in POKEMON_DEFS:
                if not any(e["id"] == pid for e in mock_state["roster"]):
                    mock_state["roster"].append({"id": pid, "evolution_seconds": 0})
                    if len(mock_state["roster"]) == 1:
                        mock_state["active_id"] = pid
            self._json_response({"ok": True})
        else:
            self.send_error(404)

    def do_PUT(self):
        parsed = urlparse(self.path)
        if parsed.path == '/api/roster/active':
            body = self._read_body()
            data = json.loads(body)
            pid = data.get("id", "")
            if any(e["id"] == pid for e in mock_state["roster"]):
                mock_state["active_id"] = pid
                mock_state["evolution_seconds"] = next(
                    (e["evolution_seconds"] for e in mock_state["roster"] if e["id"] == pid), 0
                )
            self._json_response({"ok": True})
        elif parsed.path == '/api/settings':
            body = self._read_body()
            data = json.loads(body)
            if "mood" in data:
                mock_state["settings"]["mood"].update(data["mood"])
            if "llm" in data:
                for k in ("endpoint", "model"):
                    if k in data["llm"]:
                        mock_state["settings"]["llm"][k] = data["llm"][k]
                if "api_key" in data["llm"]:
                    mock_state["settings"]["llm"]["api_key_set"] = True
            self._json_response({"ok": True})
        else:
            self.send_error(404)

    def do_DELETE(self):
        parsed = urlparse(self.path)
        if parsed.path.startswith('/api/roster/'):
            pid = parsed.path.split('/')[-1]
            mock_state["roster"] = [e for e in mock_state["roster"] if e["id"] != pid]
            if mock_state["active_id"] == pid:
                mock_state["active_id"] = mock_state["roster"][0]["id"] if mock_state["roster"] else ""
            self._json_response({"ok": True})
        else:
            self.send_error(404)

    def _read_body(self):
        length = int(self.headers.get('Content-Length', 0))
        return self.rfile.read(length).decode('utf-8')

    def _json_response(self, data):
        body = json.dumps(data).encode('utf-8')
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', len(body))
        self.end_headers()
        self.wfile.write(body)

    def _serve_file(self, filepath):
        ext = os.path.splitext(filepath)[1]
        content_types = {
            '.html': 'text/html',
            '.css': 'text/css',
            '.js': 'application/javascript',
            '.png': 'image/png',
            '.json': 'application/json',
        }
        with open(filepath, 'rb') as f:
            data = f.read()
        self.send_response(200)
        self.send_header('Content-Type', content_types.get(ext, 'application/octet-stream'))
        self.send_header('Content-Length', len(data))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, format, *args):
        # Quieter logging
        if '/api/' not in str(args[0]):
            super().log_message(format, *args)


if __name__ == '__main__':
    import sys
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8090
    server = HTTPServer(('localhost', port), MockHandler)
    print(f"PokéWatcher preview server running at http://localhost:{port}")
    print(f"Serving web files from: {os.path.abspath(WEB_DIR)}")
    print(f"Loaded {len(POKEMON_DEFS)} Pokemon from SD card data")
    print(f"Press Ctrl+C to stop\n")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
