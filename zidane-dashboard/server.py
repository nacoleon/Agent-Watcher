#!/usr/bin/env python3
"""Zidane Watcher Dashboard — local preview server.

Shows Zidane sprite from FFRK sprite sheet with agent state behaviors
matching the SenseCap Watcher firmware. Mocks the Watcher API.

Usage: python3 zidane-dashboard/server.py
Then open http://localhost:8091
"""

import json
import os
import time
from http.server import HTTPServer, SimpleHTTPRequestHandler
from urllib.parse import urlparse

DASHBOARD_DIR = os.path.dirname(os.path.abspath(__file__))

# Agent states (matching firmware)
STATES = ["idle", "working", "waiting", "alert", "greeting", "sleeping", "reporting"]
_state_idx = [0]
_manual_state = [False]  # True when state was set manually (stops auto-cycling)

mock_state = {
    "agent_state": "idle",
    "person_present": True,
    "last_message": "",
    "uptime_seconds": 0,
    "wifi_rssi": -42,
    "messages": [
        "Morning! 3 tasks done overnight.",
        "Dagger's research on ETH staking is done.",
        "Deploy to staging complete.",
        "Vivi is running health checks now.",
        "All quiet. Going idle.",
    ],
}

_start_time = time.time()


class ZidaneHandler(SimpleHTTPRequestHandler):
    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path

        if path == '/api/status':
            # Only auto-cycle if state hasn't been manually set
            if not _manual_state[0]:
                _state_idx[0] = (_state_idx[0] + 1) % (len(STATES) * 4)
                mock_state["agent_state"] = STATES[_state_idx[0] // 4]
            mock_state["uptime_seconds"] = int(time.time() - _start_time)
            self._json_response(mock_state)
        elif path == '/api/timeline':
            self._json_response(mock_state["messages"])
        elif path == '/sprites/zidane':
            self._serve_file(os.path.join(DASHBOARD_DIR, 'zidane_spritesheet.png'))
        elif path == '/frames/zidane':
            self._serve_file(os.path.join(DASHBOARD_DIR, 'frames.json'))
        else:
            if path == '/':
                path = '/index.html'
            filepath = os.path.join(DASHBOARD_DIR, path.lstrip('/'))
            if os.path.isfile(filepath):
                self._serve_file(filepath)
            else:
                self.send_error(404)

    def do_PUT(self):
        parsed = urlparse(self.path)
        if parsed.path == '/api/agent-state':
            body = self._read_body()
            data = json.loads(body)
            state = data.get("state", "idle")
            if state in STATES:
                mock_state["agent_state"] = state
                _manual_state[0] = True
            self._json_response({"ok": True, "state": mock_state["agent_state"]})
        else:
            self.send_error(404)

    def do_POST(self):
        parsed = urlparse(self.path)
        if parsed.path == '/api/message':
            body = self._read_body()
            data = json.loads(body)
            text = data.get("text", "")
            if text:
                mock_state["last_message"] = text
                mock_state["messages"].insert(0, text)
                if len(mock_state["messages"]) > 10:
                    mock_state["messages"] = mock_state["messages"][:10]
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
            '.html': 'text/html', '.css': 'text/css',
            '.js': 'application/javascript', '.png': 'image/png',
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
        if '/api/' not in str(args[0]) and '/sprites/' not in str(args[0]) and '/frames/' not in str(args[0]):
            super().log_message(format, *args)


if __name__ == '__main__':
    import sys
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8091
    server = HTTPServer(('localhost', port), ZidaneHandler)
    print(f"Zidane Watcher Dashboard at http://localhost:{port}")
    print(f"Press Ctrl+C to stop\n")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")
