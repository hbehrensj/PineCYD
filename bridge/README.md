# PineCYD usage bridge

A small script (`usage_bridge.py`) that runs on a trusted machine already logged into
Claude Code, and re-serves sanitized Claude usage percentages/reset-times to PineCYD over
plain HTTP on the LAN. The ESP32 never sees an Anthropic access token - this script is the
only thing that does, and it never handles refreshing it either (see "Why this exists"
below).

## Why this exists

The original design (2026-07-15) had the device itself hold a live OAuth access token,
entered via the WiFi captive portal, and call `api.anthropic.com` directly over HTTPS. This
bridge design was offered at the time and explicitly declined in favor of that simpler
approach - see `firmware/fase2-ember-design/README.md`'s "Claude usage zone" section for the
full history of what was accepted and why.

It didn't hold up in practice: **the token isn't refreshed, and access tokens expire in
hours, not months** - re-entering it via the portal every few hours was real, ongoing
friction. Moved to this bridge design instead, 2026-07-16, specifically to solve that: this
script runs on a machine (e.g. a Mac) that's *already* logged into Claude Code for normal,
everyday use - so its own OAuth session gets refreshed for free, as a side effect of that
normal use, with no separate refresh logic needed here at all. The device polls this script
instead of Anthropic directly.

## What it does

1. Reads the current access token from the same macOS Keychain entry Claude Code's own
   CLI/VSCode extension uses (`security find-generic-password -s "Claude Code-credentials"`).
2. Calls Anthropic's usage endpoint (`api.anthropic.com/api/oauth/usage`) with that token.
3. Strips the response down to just `label`/`utilization`/`resets_at`/`period_seconds` per
   bucket (session, weekly, and any per-model weekly-scoped entries) - no token, no account
   details, nothing else from the response ever leaves this script.
4. Serves that sanitized JSON at `http://<this-machine>:8787/usage`, cached 60s upstream
   (matches the device's own `USAGE_FETCH_INTERVAL_MS`) so the device's poll cadence never
   drives extra load on Anthropic's API - keep the two in sync if either changes (see the
   comment at the top of `usage_bridge.py`).

## Running it

**Requirements:** macOS (for the Keychain read - see `read_access_token()`), Python 3, and
Claude Code already logged in at least once on this machine.

Manually, for a quick test:

```sh
python3 bridge/usage_bridge.py
```

**As a persistent background service** (so it survives reboots and restarts itself if it
crashes) - a `launchd` agent, `~/Library/LaunchAgents/com.pinecyd.usage-bridge.plist`:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.pinecyd.usage-bridge</string>
    <key>ProgramArguments</key>
    <array>
        <string>/opt/homebrew/bin/python3</string>
        <string>-u</string>
        <string>/Users/YOU/PineCYD/bridge/usage_bridge.py</string>
    </array>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>/Users/YOU/Library/Logs/pinecyd-usage-bridge.log</string>
    <key>StandardErrorPath</key>
    <string>/Users/YOU/Library/Logs/pinecyd-usage-bridge.log</string>
</dict>
</plist>
```

Adjust the two `/Users/YOU/...` paths, then:

```sh
launchctl load ~/Library/LaunchAgents/com.pinecyd.usage-bridge.plist
```

The first run prompts for Keychain access approval in the macOS UI (one-time) - approve it,
or the token read will fail every time.

## Connecting PineCYD to it

Set this machine's LAN address as `host:port` (e.g. `192.168.1.50:8787`) via either the WiFi
captive portal (first-boot setup) or the always-on settings page at `http://pinecyd.local/`
(shown there whenever no Pinecil is connected - see the firmware README's "Always-on settings
page" section). Leave it blank if you don't want the Claude usage zone at all - the clock
screen just won't show it (see the firmware README's three-user-types note).

## Security model

- The Anthropic access token lives only in this machine's Keychain and this script's memory
  - never written to disk by this script, never sent to the ESP32.
- What the ESP32 receives (`/usage`'s response) is just percentages, reset timestamps, and
  labels - nothing that could be replayed against Anthropic's API.
- Runs on plain HTTP, LAN-only by design (`0.0.0.0:8787`, no auth) - don't expose this port
  beyond your own LAN/firewall.
