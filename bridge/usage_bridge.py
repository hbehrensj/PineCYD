#!/usr/bin/env python3
"""PineCYD usage bridge - see bridge/README.md.

Runs on a trusted machine that's already logged into Claude Code (so its OAuth session is
kept fresh by Claude Code itself - this script never handles token refresh). Reads the
current access token from the same macOS Keychain entry Claude Code uses, calls Anthropic's
usage endpoint, and re-serves only percentage/reset-time/label fields over plain HTTP on the
LAN - the access token itself is never sent to, or stored on, the ESP32.

Reads from the response's `limits` array (2026-07-16), not the older flat `five_hour`/
`seven_day`/... top-level fields - confirmed live that only `limits` actually contains
per-model scoped usage (e.g. a "Fable" entry), which the flat fields never did. `limits`
matches what Anthropic's own web usage dashboard shows 1:1 (Current session / All models /
per-model weekly), so it's treated as the current, authoritative shape rather than the older
one - the flat fields are no longer read here at all.
"""

import json
import re
import subprocess
import time
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

KEYCHAIN_SERVICE = "Claude Code-credentials"
ANTHROPIC_USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
LISTEN_PORT = 8787
# Anthropic rate-limited a 10s device poll interval during PineCYD's own on-device testing
# (see firmware/fase2-ember-design/README.md) - this cache decouples however often the
# device polls this bridge from how often we actually hit Anthropic's API. The device's own
# poll interval was set to match this exactly (see USAGE_FETCH_INTERVAL_MS in main.cpp) -
# keep the two in sync if either changes.
UPSTREAM_CACHE_SECONDS = 60
# Anthropic's `kind` -> pace-marker window length. "weekly_scoped" (per-model) shares the
# same weekly reset as "weekly_all", confirmed live (both reset_at the same Wednesday
# timestamp, just fetched a fraction of a second apart).
KIND_PERIOD_SECONDS = {
    "session": 5 * 3600,
    "weekly_all": 7 * 86400,
    "weekly_scoped": 7 * 86400,
}

_cache = {"body": None, "fetched_at": 0.0}


def read_access_token() -> str:
    # Same Keychain entry Claude Code's VSCode extension/CLI itself uses - see the "claude
    # Code-credentials" service name confirmed live via `security find-generic-password`.
    # Raises if Claude Code has never logged in on this machine, or Keychain access is denied
    # (the first run will prompt for Keychain access approval in the macOS UI).
    raw = subprocess.check_output(
        ["security", "find-generic-password", "-s", KEYCHAIN_SERVICE, "-w"],
        stderr=subprocess.DEVNULL,
    )
    creds = json.loads(raw)
    return creds["claudeAiOauth"]["accessToken"]


def _slug(text: str) -> str:
    return re.sub(r"[^a-z0-9]+", "_", text.lower()).strip("_")


def sanitize(anthropic_json: dict) -> dict:
    # Iterates the `limits` array structurally (by kind/scope shape), not a fixed name list -
    # same "pick up whatever Anthropic actually sends" approach as the bucket-name handling
    # this replaced, now applied to entries instead of top-level keys. A "weekly_scoped" entry
    # is keyed by its scoped model's slugified display name (stable across fetches, and there
    # could be more than one - e.g. a second scoped model added later) so each stays pinned to
    # its own slot device-side; "session"/"weekly_all" are unique already.
    out = {}
    for entry in anthropic_json.get("limits") or []:
        if not isinstance(entry, dict):
            continue
        kind = entry.get("kind")
        percent = entry.get("percent")
        resets_at = entry.get("resets_at")
        if kind not in KIND_PERIOD_SECONDS or percent is None or not resets_at:
            continue

        scope = entry.get("scope") or {}
        model_name = (scope.get("model") or {}).get("display_name")
        if kind == "weekly_scoped" and model_name:
            key = f"weekly_scoped_{_slug(model_name)}"
            label = model_name  # e.g. "Fable" - more readable than the bare kind here
        else:
            key = kind
            label = kind

        out[key] = {
            "label": label,
            "utilization": percent,
            "resets_at": resets_at,
            "period_seconds": KIND_PERIOD_SECONDS[kind],
        }
    return out


def fetch_from_anthropic() -> dict:
    token = read_access_token()
    req = urllib.request.Request(
        ANTHROPIC_USAGE_URL,
        headers={
            "Authorization": f"Bearer {token}",
            "anthropic-beta": "oauth-2025-04-20",
        },
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        return json.loads(resp.read())


def get_usage_json() -> tuple[int, dict]:
    now = time.time()
    if _cache["body"] is not None and (now - _cache["fetched_at"]) < UPSTREAM_CACHE_SECONDS:
        return 200, _cache["body"]

    try:
        sanitized = sanitize(fetch_from_anthropic())
    except urllib.error.HTTPError as e:
        print(f"[bridge] Anthropic returned HTTP {e.code}")
        # A stale cache is still useful to serve on a transient upstream error (e.g. a 429)
        # rather than blanking the device's display over one bad poll.
        if _cache["body"] is not None:
            return 200, _cache["body"]
        return e.code, {"error": f"anthropic HTTP {e.code}"}
    except Exception as e:  # Keychain read failure, network error, bad JSON, etc.
        print(f"[bridge] Fetch failed: {e}")
        if _cache["body"] is not None:
            return 200, _cache["body"]
        return 502, {"error": str(e)}

    _cache["body"] = sanitized
    _cache["fetched_at"] = now
    return 200, sanitized


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/usage":
            self.send_response(404)
            self.end_headers()
            return
        status, body = get_usage_json()
        payload = json.dumps(body).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt, *args):
        print(f"[bridge] {self.address_string()} - {fmt % args}")


def main():
    server = ThreadingHTTPServer(("0.0.0.0", LISTEN_PORT), Handler)
    print(f"[bridge] Listening on 0.0.0.0:{LISTEN_PORT}, serving /usage")
    server.serve_forever()


if __name__ == "__main__":
    main()
