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
import os
import re
import subprocess
import sys
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
# How stale a cached response is allowed to be before it's no longer served as a fallback on
# a transient upstream error (below) - found live, 2026-07-17: without a bound, a cache from
# *before* a session reset kept getting re-served as a plain HTTP 200 (indistinguishable from
# a fresh fetch to the device) for as long as Anthropic kept rate-limiting after the bridge
# machine woke from sleep, showing an already-expired resets_at as if current. 15 minutes is
# long enough to ride out a brief 429 storm, short enough that it can't span a real session
# boundary (the shortest limit window, "session", is 5 hours).
MAX_STALE_CACHE_SECONDS = 15 * 60
# Anthropic's `kind` -> pace-marker window length. "weekly_scoped" (per-model) shares the
# same weekly reset as "weekly_all", confirmed live (both reset_at the same Wednesday
# timestamp, just fetched a fraction of a second apart).
KIND_PERIOD_SECONDS = {
    "session": 5 * 3600,
    "weekly_all": 7 * 86400,
    "weekly_scoped": 7 * 86400,
}

_cache = {"body": None, "fetched_at": 0.0}


# Two token sources, 2026-11-04:
#  - "keychain" (default on macOS): the original design - reads the Keychain entry Claude
#    Code itself uses on a Mac that's in everyday use, so that Claude Code keeps it fresh.
#  - "file" (Linux/Docker, e.g. a Raspberry Pi): reads Claude Code's own credentials file
#    (~/.claude/.credentials.json) from a *separate* login made inside the container with
#    `claude auth login`. Nothing else uses that login, so this script has to keep it fresh
#    itself - see refresh_file_credentials(). It must be its own login, never a copy of the
#    Mac's: refresh tokens rotate, so two machines sharing one would log each other out.
#    A `claude setup-token` long-lived token was tried first and does NOT work here - it only
#    carries `user:inference`, and the usage endpoint answers 403 "oauth_scope_insufficient,
#    required user:profile" (confirmed live, 2026-11-04).
TOKEN_SOURCE = os.environ.get(
    "PINECYD_TOKEN_SOURCE", "keychain" if sys.platform == "darwin" else "file"
)
CREDENTIALS_FILE = os.path.expanduser(
    os.environ.get("PINECYD_CREDENTIALS_FILE", "~/.claude/.credentials.json")
)
CLAUDE_BIN = os.environ.get("PINECYD_CLAUDE_BIN", "claude")
# Refresh this long before the access token's own expiresAt, so a fetch never races expiry.
REFRESH_MARGIN_SECONDS = 15 * 60


def _read_keychain_creds() -> dict:
    # Same Keychain entry Claude Code's VSCode extension/CLI itself uses - see the "claude
    # Code-credentials" service name confirmed live via `security find-generic-password`.
    # Raises if Claude Code has never logged in on this machine, or Keychain access is denied
    # (the first run will prompt for Keychain access approval in the macOS UI).
    raw = subprocess.check_output(
        ["security", "find-generic-password", "-s", KEYCHAIN_SERVICE, "-w"],
        stderr=subprocess.DEVNULL,
    )
    return json.loads(raw)["claudeAiOauth"]


def _read_file_creds() -> dict:
    with open(CREDENTIALS_FILE) as f:
        return json.load(f)["claudeAiOauth"]


def _expires_in(creds: dict) -> float:
    return creds.get("expiresAt", 0) / 1000 - time.time()


def refresh_file_credentials() -> None:
    # Lets the Claude Code CLI do the actual OAuth refresh (and write the rotated tokens back
    # to CREDENTIALS_FILE) rather than re-implementing its undocumented token endpoint here.
    # `claude auth status` is tried first since it costs nothing; if that turns out not to
    # refresh (expiresAt unchanged), a one-word `claude -p` prompt on the smallest model is
    # the fallback - that's a real API call, so it reliably refreshes, at the cost of a tiny
    # bit of subscription usage a few times a day. Logs which path worked so it's visible.
    # Success = expiresAt actually moved forward, not just "far enough away" - on a forced
    # refresh after a 401, expiry already looked fine before, so that check alone would
    # report success without anything having changed (caught by a dummy-token test).
    before = _read_file_creds().get("expiresAt", 0)
    for label, cmd in (
        ("auth status", [CLAUDE_BIN, "auth", "status"]),
        ("prompt", [CLAUDE_BIN, "-p", "--model", "haiku", "Reply with just: ok"]),
    ):
        try:
            subprocess.run(cmd, capture_output=True, timeout=120, check=False)
        except Exception as e:
            print(f"[bridge] Token refresh via {label} failed to run: {e}")
            continue
        creds = _read_file_creds()
        if creds.get("expiresAt", 0) > before:
            print(f"[bridge] Token refreshed via {label}, valid {_expires_in(creds) / 3600:.1f}h")
            return
    print("[bridge] Token refresh did not extend expiry - run `claude auth login` again?")


def read_access_token(force_refresh: bool = False) -> str:
    if TOKEN_SOURCE == "keychain":
        return _read_keychain_creds()["accessToken"]
    creds = _read_file_creds()
    if force_refresh or _expires_in(creds) < REFRESH_MARGIN_SECONDS:
        refresh_file_credentials()
        creds = _read_file_creds()
    return creds["accessToken"]


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


def _fetch_with_token(token: str) -> dict:
    req = urllib.request.Request(
        ANTHROPIC_USAGE_URL,
        headers={
            "Authorization": f"Bearer {token}",
            "anthropic-beta": "oauth-2025-04-20",
        },
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        return json.loads(resp.read())


def fetch_from_anthropic() -> dict:
    try:
        return _fetch_with_token(read_access_token())
    except urllib.error.HTTPError as e:
        # A 401 with the file source means the token went bad before its expiresAt said it
        # would (e.g. revoked) - force one refresh and retry once before giving up.
        if e.code != 401 or TOKEN_SOURCE != "file":
            raise
        print("[bridge] Anthropic returned 401 - forcing a token refresh and retrying")
        return _fetch_with_token(read_access_token(force_refresh=True))


def get_usage_json() -> tuple[int, dict]:
    now = time.time()
    if _cache["body"] is not None and (now - _cache["fetched_at"]) < UPSTREAM_CACHE_SECONDS:
        return 200, _cache["body"]

    try:
        sanitized = sanitize(fetch_from_anthropic())
    except urllib.error.HTTPError as e:
        print(f"[bridge] Anthropic returned HTTP {e.code}")
        # A cache still within MAX_STALE_CACHE_SECONDS is useful to serve on a transient
        # upstream error (e.g. a 429) rather than blanking the device's display over one bad
        # poll - but only up to that bound (see its own comment): beyond it, serving stale
        # data as a plain 200 is indistinguishable from fresh to the device, and worse than
        # just reporting the real error.
        if _cache["body"] is not None and (now - _cache["fetched_at"]) < MAX_STALE_CACHE_SECONDS:
            return 200, _cache["body"]
        return e.code, {"error": f"anthropic HTTP {e.code}"}
    except Exception as e:  # Keychain read failure, network error, bad JSON, etc.
        print(f"[bridge] Fetch failed: {e}")
        if _cache["body"] is not None and (now - _cache["fetched_at"]) < MAX_STALE_CACHE_SECONDS:
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
    print(f"[bridge] Listening on 0.0.0.0:{LISTEN_PORT}, serving /usage (token source: {TOKEN_SOURCE})")
    server.serve_forever()


if __name__ == "__main__":
    main()
