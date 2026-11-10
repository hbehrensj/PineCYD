#!/usr/bin/env python3
"""Mock status server for PineCYD's alert mode - see docs/alert-mode/handoff.md.

Serves the alert contract at /api/alert and lets you switch what it answers, so every
display state can be tried without a real monitoring server:

    python3 tools/alert_mock.py [port]          # default port 8790
    curl http://localhost:8790/mode/crit        # switch mode (list: /mode)

Then set PineCYD's Alert URL (settings page) to http://<this machine>:8790/api/alert.

Modes:
    off      {"alert": false}
    crit     a normal alert
    crit2    a different alert (new id/text) - replaces the first one immediately
    long     40-character text with æøåÆØÅ (glyph + wrapping check)
    warn     level "warn" - must be ignored (treated as no alert)
    invalid  a 200 with a body that isn't JSON
    http500  HTTP 500
    down     closes the connection without answering (server unreachable)

For "server down since boot": stop this script, reboot PineCYD, and wait >10 min - the
screen must stay normal. For "down after answering": run any mode first, then switch to
"down" (or stop the script) and wait >10 min - "Statusserver svarer ikke" must appear.
"""

import json
import sys
from datetime import datetime, timedelta, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

LONG_TEXT = "Ældre backup på NAS: Øv, ø og Å i år. æ!"  # exactly 40, all of æøåÆØÅ
assert len(LONG_TEXT) == 40, len(LONG_TEXT)


def _since(minutes_ago: int) -> str:
    t = datetime.now(timezone.utc) - timedelta(minutes=minutes_ago)
    return t.strftime("%Y-%m-%dT%H:%M:%SZ")


def _alert(alert_id: str, text: str, level: str = "crit", minutes_ago: int = 12) -> dict:
    return {"alert": True, "id": alert_id, "level": level, "text": text, "since": _since(minutes_ago)}


MODES = {
    "off": lambda: {"alert": False},
    "crit": lambda: _alert("a-1", "Backup er 2 døgn gammel"),
    "crit2": lambda: _alert("a-2", "Disk på server er 95% fuld", minutes_ago=1),
    "long": lambda: _alert("a-3", LONG_TEXT),
    "warn": lambda: _alert("a-4", "Kun en advarsel", level="warn"),
    "invalid": None,
    "http500": None,
    "down": None,
}

state = {"mode": "off"}


class Handler(BaseHTTPRequestHandler):
    def _send(self, code: int, body: bytes, ctype: str = "application/json; charset=utf-8"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/mode"):
            name = self.path[len("/mode/"):] if self.path.startswith("/mode/") else ""
            if name in MODES:
                state["mode"] = name
                print(f"[mock] mode -> {name}")
            body = f"mode: {state['mode']}\navailable: {', '.join(MODES)}\n".encode()
            return self._send(200, body, "text/plain; charset=utf-8")

        if self.path != "/api/alert":
            return self._send(404, b'{"error": "not found"}')

        mode = state["mode"]
        if mode == "down":
            self.close_connection = True
            self.connection.close()
            return
        if mode == "http500":
            return self._send(500, b'{"error": "mock 500"}')
        if mode == "invalid":
            return self._send(200, b"this is {not json")
        self._send(200, json.dumps(MODES[mode](), ensure_ascii=False).encode("utf-8"))

    def log_message(self, fmt, *args):
        print(f"[mock] {self.address_string()} [{state['mode']}] {fmt % args}")


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8790
    print(f"[mock] Alert mock on 0.0.0.0:{port} - GET /api/alert, switch with GET /mode/<name>")
    ThreadingHTTPServer(("0.0.0.0", port), Handler).serve_forever()


if __name__ == "__main__":
    main()
