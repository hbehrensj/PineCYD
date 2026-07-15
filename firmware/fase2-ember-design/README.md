# Fase 2 - "Ember" dashboard (the active firmware)

Raw-LVGL Pinecil dashboard for an ESP32 CYD, connecting directly over BLE to a Pinecil V2
soldering iron (no Home Assistant / IronOS integration in the loop). Started as a
three-phase experiment (BLE-only proof of concept → minimal screen → full dashboard); the
earlier two phases were folded into this one once the design converged - this is now the
only firmware in the repo. History of what those phases found is kept below since the
lessons (and the fixes) are still live in this code.

Design (screen `P2`, "Ember" theme) was originally created for a separate, private
home-dashboard project and ported here - implementation rewritten from ESPHome/LVGL-YAML to
raw LVGL C++, design itself kept faithful except where flagged below.

## Hardware

Board: Sunton ESP32-2432S028R (the "CYD" - Cheap Yellow Display), ILI9341 320x240, no
PSRAM, NimBLE for BLE.

**Confirmed on real hardware** (pinout in `platformio.ini`):
- Color order `TFT_RGB` (not ILI9341's usual default `TFT_BGR`).
- Panel is **landscape-native at rotation 0**, not portrait+rotate90 like a "typical"
  TFT_eSPI ILI9341 setup: `TFT_WIDTH=320`/`TFT_HEIGHT=240` with `tft.setRotation(2)`.
  Declaring it portrait-native and rotating 90 in software produced a scrambled image; the
  working combination was found by trying rotation values on the real device.
- Backlight: GPIO21, active-high, must be driven explicitly (`pinMode`/`digitalWrite`) or
  the screen stays black even with a working display init.

## Pinecil BLE - GATT UUIDs (confirmed from source, not assumed)

Pulled directly from IronOS's own source, not from third-party docs (which disagreed with
each other on some digits):
[`ble_characteristics.h`](https://github.com/Ralim/IronOS/blob/main/source/Core/BSP/Pinecilv2/ble_characteristics.h),
[`ble_peripheral.c`](https://github.com/Ralim/IronOS/blob/main/source/Core/BSP/Pinecilv2/ble_peripheral.c).
**Pinecil V2 only** - this BLE service doesn't exist on Pinecil V1 hardware.

| Service | UUID |
|---|---|
| Live Data | `d85ef000-168e-4a71-aa55-33e27f9bc533` |
| Bulk Data (used here only as a scan filter) | `9eae1000-9d0d-48c5-aa55-33e27f9bc533` |

| Characteristic | UUID | Type | Properties |
|---|---|---|---|
| Live temp | `d85ef001-...` | uint32 LE, °C | Read |
| Setpoint temp | `d85ef002-...` | uint32 LE, °C | Read |
| DC input | `d85ef003-...` | uint32 LE, decivolts (202 = 20.2V) | Read |
| Handle temp | `d85ef004-...` | uint32 LE, decivolts-style x10 °C (see below) | Read |
| Operating mode | `d85ef00D-...` | uint32 LE, `OperatingMode` enum | Read |
| Estimated watts | `d85ef00E-...` | uint32 LE, x10-scaled watts | Read |

**Every one of these is read-only - no `NOTIFY`/`INDICATE`, no CCC descriptor in
`ble_peripheral.c`.** IronOS does not push updates; it only answers reads. So "direct BLE"
is a polling architecture, same shape as a Home Assistant integration would use - the win
here is a *faster poll interval* (~90-100ms/characteristic measured, vs. a typical 5s HA
poll), not event-driven push.

## Design sources

- An existing design spec and its actual built ESPHome/LVGL-YAML widget tree (from a
  separate private project) - colors, type ramp, layout rationale, exact device-pixel
  positions, font sizes, and color tokens were taken directly from there, not re-eyeballed
  from prose.
- `Ralim/IronOS` source (`OperatingModes.h`, `ble_handlers.cpp`, `Soldering.cpp`,
  `GUIThread.cpp`) - confirms `dc_input` and `est_watts` are both **x10-scaled**
  (`getInputVoltageX10`, `x10WattHistory.average()`), and gives the real `OperatingMode`
  enum values (and control flow) used for mode-based styling.
- `tr4nt0r/pynecil` (`pynecil/types.py`) - the Python BLE client library Home Assistant's
  own IronOS integration uses; its `OperatingMode` enum disagreed with IronOS firmware on
  one value (BOOST - see below; resolved).

## What's a faithful port vs. a flagged simplification

**Faithful (same colors, fonts, positions as the original design):**
- Hero tip-temp value, unit, "TIP TEMP" label (since enlarged - see Layout changes below).
- Right rail: mode label, target/setpoint value, power label, power bar (`warm_track`
  track / `amber` fill), handle-temp label.
- Bottom strip: live temp-history line (`ep_orange`, 3px, rounded) and setpoint reference
  line - **live**, fed from this firmware's own rolling BLE-poll history via an LVGL
  `chart`, unlike the original's static demo polyline (its ESPHome LVGL binding had no
  native `chart` widget).
- Ember palette hex values, Nunito-ramp font *sizes* (12/20/24/36/48, plus a custom 72px -
  see Layout changes).

**Simplified, and why (see the file header comment in `src/main.cpp` for the same list):**
- **Font:** LVGL built-in Montserrat, not the actual Nunito SemiBold - embedding the real
  typeface would need `lv_font_conv` and per-size `.c` asset generation for the whole
  ramp; out of proportion, except where a custom size was worth generating anyway (the
  72px hero value - see below). Visually similar (both humanist sans, semibold-ish).
- **No MDI icon glyphs** (soldering iron / fire / sleep icons) - same reason. The colored
  mode-label text carries the same signal the icon was reinforcing, not solely carrying.
- **No BOOST state - resolved via a live hardware test, not left as a guess.** The other
  project's HA wiring (via HA's IronOS integration → the `pynecil` Python library) treats
  a "boost" operating mode as real, and `pynecil`'s `OperatingMode` enum defines
  `BOOST = 2`. IronOS's own C++ `OperatingMode` enum has no name for value 2 at all - it
  jumps from 1 to 3. **Confirmed live, 2026-07-13:** engaging boost on the real iron pushed
  tip temp to 426°C against a 350°C setpoint while `operating_mode` stayed at `Soldering`
  (1) throughout - never 2. The firmware trace was right; `pynecil`'s `BOOST=2` is stale
  for this hardware/firmware combination. States mapped: `HomeScreen`→IDLE (not in the
  original spec - added since this project has no multi-page context-switch to hide the
  screen during idle), `Soldering`/`SolderingProfile`→SOLDER, `Sleeping`/`Hibernating`→SLEEP.
- **No setpoint write control.** The user controls setpoint on the iron itself - see
  Layout changes below (the on-screen +/- control was removed entirely).
- **Power bar fill %** is estimated watts against an assumed nominal ceiling
  (`ASSUMED_MAX_WATTS = 100.0f` in `main.cpp`), not an iron-reported max. The bar's *color
  ramp* (amber → deep_orange) is better-grounded: adopted the other project's own
  documented judgment call directly - a fixed 65W threshold (a common USB-PD charger tier
  boundary), independent of the fill-percentage assumption above.
- **No PD/QC power-source decoding** - the `power_source` characteristic exists and is
  readable, but decoding its enum meaningfully needs more IronOS source-diving than seemed
  proportionate. Label just shows watts and voltage.

## Layout changes from the original design (2026-07-13, live user feedback)

The initially design-faithful layout was revised once the user actually used it:
- **Setpoint +/- buttons removed entirely** - the user controls setpoint on the iron
  itself and never wanted a touch control here (there's no touchscreen driver in this
  project anyway). The target/setpoint value moved to the right rail (read-only) instead
  of living between two now-nonexistent buttons.
- **Tip-temp hero enlarged ~50%** using a **custom-generated 72px font**
  (`src/fonts/lv_font_montserrat_72_digits.c`: digits + hyphen + colon only, made with
  `lv_font_conv` from the same `Montserrat-Medium.ttf` LVGL's built-in fonts use - see that
  file's header comment for the exact command). Built-in Montserrat tops out at 48px.
  **A `style_transform_zoom()` attempt to fake a bigger size without a real font asset made
  the value invisible on real hardware** - LVGL 8's zoom transform likely doesn't expand
  the invalidated/flush region to match, so the zoomed content never reached the
  partial-buffer `flush_cb`. Reverted in favor of the real font.
- **Chart strip shrunk and its header label removed** (redundant next to the hero value),
  freeing the vertical space the bigger tip-temp font uses.
- **Setpoint reference line in the chart is real scrolling history**, not a flat line at
  the current value (so you can see how the target changed over time, e.g. across a
  sleep/wake cycle), colored `warm_track` (the same dark color the removed buttons used)
  so it reads as a quiet backdrop rather than competing with the orange tip-temp line.
- **The separate "BLE: connected/disconnected" label was removed.** The mode label (top
  of the right rail) doubles as connection status: real mode (SOLDER/IDLE/SLEEP) once
  connected, or "SCANNING..."/"DISCONNECTED (xN)" when not - redundant otherwise, since a
  real tip-temp value already implies a connection.

## WiFi + NTP clock

Shows a clock (separate LVGL screen, `scr_clock`) whenever no Pinecil is connected - added
at the user's explicit request, after being offered a lower-risk "WiFi only briefly at
boot for an NTP sync, then off" alternative and choosing continuous WiFi instead. This is a
materially different risk than plain BLE+LVGL: this board has no PSRAM, and layering a
third concurrent radio-sharing subsystem (WiFi, via ESP32's WiFi/BT coexistence layer) on
top of BLE+LVGL was always going to cost something measurable - it did (see below).

**Auto on/off (2026-07-13, at the user's suggestion):** WiFi now only runs while the clock
is showing (no Pinecil connected). The moment a Pinecil connects, WiFi turns off
(`WiFi.disconnect(true); WiFi.mode(WIFI_OFF)`) - this recovers a meaningful chunk of the
heap cost below and removes the WiFi/BT coexistence contention that was measurably slowing
BLE polling, right when low latency matters most (an iron actively in use). WiFi comes
back on the moment the Pinecil disconnects again, and NTP is reconfigured automatically on
each reconnect (detected via a connected-state edge, not a blocking wait, so `loop()` never
stalls waiting for WiFi).

### WiFi setup: captive portal, no compile-time credentials

Originally required a gitignored `include/secrets.h` with a hardcoded SSID/password -
replaced (2026-07-13) with [`tzapu/WiFiManager`](https://github.com/tzapu/WiFiManager) so a
single prebuilt release binary works for anyone, no rebuild needed. First boot (or any boot
with no reachable saved network): the CYD opens its own AP (`PineCYD-Setup`) with a captive
portal; join it, browse to `http://192.168.4.1`, and pick your network. Credentials are
saved to NVS and the clock screen shows join/URL instructions while the portal is open.

**Cadence, per explicit spec:** try the saved network for 30s (`WIFI_CONNECT_TIMEOUT_S`); if
that fails, auto-open the config portal; if the portal itself isn't configured either (also
120s, `WIFI_RETRY_INTERVAL_MS`), give up for now and retry the whole thing again every 2
minutes - never block forever in either state. **Hold the BOOT button (GPIO0) through
power-on** to force a fresh portal (`wm.resetSettings()` + reconnect) even if a stored
network would otherwise still work - useful for switching networks without re-flashing.

**Non-blocking by design** (`wm.setConfigPortalBlocking(false)` + `wm.process()` in
`loop()`), specifically because an auto-opened portal must not stall BLE scanning/LVGL for
the minutes it might sit open and unconfigured - this project cares about BLE latency more
than almost anything else (see the tip-temp investigation below), so a library default that
blocks for the portal's lifetime was not acceptable here.

**Bug found and fixed the same day: the portal's AP couldn't be joined at all.** The
firmware's own log showed the AP starting normally (`StartAP`, `AP IP address:
192.168.4.1`), but both a phone and a Mac reported "could not be joined" when trying to
connect to it. **Root cause:** this project's continuous BLE scan
(`setInterval(100)`==`setWindow(100)`, effectively 100% duty cycle - see below) shares
ESP32's single 2.4GHz radio with WiFi via the same coexistence layer already documented
above as slowing BLE polling when WiFi is on - this is that same contention mirrored the
other direction, starving the softAP of the airtime it needs to complete a client's
association handshake. iOS/macOS are strict about that timing and fail outright rather than
retry. A first fix attempt - a single `NimBLEScan::stop()` call in the portal-open callback
- wasn't enough: the log showed `E NimBLEScan: Failed to cancel scan; rc=30` (the BLE host
task rejected the cancel, likely mid-scan-window right as WiFi was switching into `AP_STA`
mode), so the scan silently kept running the whole time. **Fix:** retry the stop every
`loop()` tick for as long as the portal is open (`if (pScan->isScanning()) pScan->stop();`),
resuming the scan only once the portal actually closes. **User-confirmed working** after
this fix - the portal could be joined from both a phone and a Mac.

**Credentials are cached in RAM after the first successful connect, and
`WiFi.persistent(false)` is set from then on** - a lesson borrowed from a sibling project
(TDAI-2170): `WiFi.begin(ssid, pass)` with persistent storage left at its default writes NVS
flash on every call, fine for an occasional outage-driven reconnect but not for this
project's on/off toggle, which reconnects on every single Pinecil disconnect - potentially
many times per session.

**Known tradeoff, flagged rather than silently decided:** auto-opening the portal on a
plain connect failure is a deliberate choice specific to this device. TDAI-2170's own
`net_config.cpp` explicitly avoids this ("a temporary router outage must not turn the
device into an unsolicited hotspot that needs manual reconfiguration") and instead only
retries known credentials, forever, never auto-opening a portal - appropriate for an
unattended device. PineCYD sits on a desk where the user would see the portal appear, so
the tradeoff was judged acceptable here - but it's a real divergence from the other
project's hard-won lesson, not an oversight.

**Timezone is configurable via the same portal (2026-07-13), not hardcoded.** The clock
NTP config was originally a hardcoded Copenhagen POSIX TZ string (`CET-1CEST,M3.5.0,
M10.5.0/3`) - silently wrong for anyone outside CET/CEST, a real gap once this project
started shipping prebuilt binaries for anyone to flash (see "Flashing a release" in the
top-level README). Fixed by adding a `WiFiManagerParameter` text field ("Timezone (POSIX TZ
string)") to the portal, pre-filled with the current value and persisted via `Preferences`
(NVS) on save - same pattern TDAI-2170 uses for its own portal-collected settings (MQTT
host/user/pass). Defaults to the Copenhagen string for a first-time setup; enter your own
POSIX TZ string (the same format as the Linux `TZ` environment variable) if you're
elsewhere. **Not yet verified live** - the device went offline (USB/power disconnected)
right after this shipped, before a real-hardware check of a non-default timezone.

### OTA updates over WiFi (2026-07-13)

Added so ongoing firmware updates don't need a USB cable: `pio run -e ota -t upload` (see
`platformio.ini`'s `[env:ota]`) pushes a build to `pinecyd.local` via `ArduinoOTA` instead of
a serial port. **Needed a real partition-table change first** - `huge_app.csv` (previously
used) labels its single 3MB app partition `ota_0`, which looks OTA-capable but isn't: with
no second slot, `esp_ota_get_next_update_partition()` has nothing to return except the
partition the code is currently executing from, and you cannot safely overwrite that while
it's running (code execution reads through the flash cache/MMU - erasing the region backing
it out from under itself would corrupt the running program, not update it safely). Switched
to a custom `partitions_ota.csv` with two real ~1.875MiB `ota_0`/`ota_1` slots instead (see
that file's own header comment for the exact layout) - the current image (~1.7MB, 86% of a
slot) fits with real but tighter headroom than `huge_app.csv`'s old 53%-of-3MB.

**This partition-table change itself needed one more USB flash** to take effect (the
bootloader and partition table can't be rewritten by an app-only OTA push) - full merged
`firmware.factory.bin` flashed once at offset `0x0`, confirmed the board still booted and
ran identically afterward (BLE dashboard, heap, WiFi toggle all unaffected). From that point
on, `pio run -e ota -t upload` works with no USB involved - **confirmed end-to-end on real
hardware**: pushed a build over WiFi, watched it authenticate, transfer, and the board
reboot automatically into the new firmware, dashboard fully functional afterward.

**A Pinecil connecting mid-OTA-transfer is guarded against**: the existing WiFi on/off
toggle (WiFi off once a Pinecil connects, to save heap/reduce radio contention - see above)
would otherwise yank WiFi out from under an in-progress flash. `ArduinoOTA`'s
`onStart`/`onError` callbacks set/clear an `otaInProgress` flag that defers the "turn WiFi
off" transition (not the reverse "turn WiFi back on" direction, which never touches WiFi.OFF
and needs no guard) until the transfer finishes. `otaStarted` also resets on every WiFi-off
transition, since `ArduinoOTA`'s listener doesn't survive a WiFi power-cycle - it's
re-initialized fresh on each reconnect (alongside mDNS, which is restarted the same way).

**OTA password is a hardcoded, overridable default** (`pinecyd-ota`, override at build time
with `-D OTA_PASSWORD=\"...\"`), not plumbed through the portal/NVS like WiFi credentials or
the timezone - matches TDAI-2170's own established judgment that this class of same-LAN-only
credential doesn't need that treatment.

**Measured cost, real hardware:**
- Free heap: ~155KB (BLE+LVGL alone) → **~71-72KB with WiFi connected** (continuous) →
  **~107KB once WiFi is turned off again** after a Pinecil connects (some of WiFi's ~83KB
  cost doesn't fully release even with `WIFI_OFF` - not yet root-caused further). Adding
  ArduinoOTA/ESPmDNS/Preferences on top of WiFiManager cost a further few KB of both flash
  and heap - modest, but real; see the partition-table note above for the flash-side impact.
- ~~Had to switch `board_build.partitions` to `huge_app.csv` (3MB single app partition, no
  OTA/spiffs)~~ - superseded by `partitions_ota.csv` above, once real OTA needed a genuine
  second slot.
- BLE poll `cycle_ms` got noticeably slower and more variable with WiFi continuously on
  (557-1111ms vs. a steady ~554-663ms without it) - a direct, measurable sign of ESP32
  WiFi/BT radio coexistence contention. **The ~554-663ms itself is not a WiFi cost** -
  it's the intrinsic round-trip time for six sequential BLE characteristic reads on this
  hardware, present with or without WiFi; only the *excess* above that floor (up to
  1111ms) came from coexistence contention. The on/off toggle removes that excess while a
  Pinecil is connected (confirmed once: cycle_ms back in the ~554-663ms range immediately
  after WiFi turned off), but don't expect poll cycles faster than ~554ms even with the
  toggle - that's the real floor, not a WiFi symptom.

**Tip-temp-only fast poll (2026-07-13, at the user's suggestion):** since six sequential
reads is what makes the full cycle slow, and the tip-temp number is the one value that
actually needs to feel responsive while soldering (the whole reason this project exists -
see the handoff doc), it's now polled on its own, independent of the 6-characteristic full
cycle (`TIP_POLL_INTERVAL_MS` in `main.cpp`). **Measured real hardware: a single-
characteristic read takes only ~20-45ms** (much faster than the ~90-100ms/read implied by
dividing the 6-read cycle time by 6 - reading one characteristic in isolation avoids
whatever per-characteristic overhead accumulates across six sequential attribute-handle
switches). This gives roughly 15-20Hz visible tip-temp updates - a large jump from the full
cycle's ~1.5-2Hz - while the chart, target, power, mode, and handle-temp still update on
the original slower full-cycle cadence (deliberately: pushing tip temp into the chart at
the fast rate would desync it from the setpoint-reference series, which only advances on
the full cycle - see the code comment in `loop()`).

**Update, same day: the fast poll wasn't visibly faster on screen at first** - the user
correctly pushed back after testing while the iron was actively heating. Root cause,
found by comparing tip-only `cycle_ms` over a longer run: it stayed fast (~20-45ms) for
only the first few seconds of each connection, then jumped to ~85-103ms/read and stayed
there - not related to rendering (a chart-redraw-cost theory was tested and ruled out
along the way, see the reduced `HISTORY_POINTS`/decoupled chart-push-rate change in the
code, which helped a little but wasn't the main cause). **The actual cause:**
`NimBLEClientCallbacks::onConnParamsUpdateRequest()` defaults to auto-accepting *any*
connection-parameter change the Pinecil proposes. A few seconds into every connection the
Pinecil renegotiates to a slower interval, after which *every* GATT read costs a full
connection interval no matter how simple - not a rendering or overhead difference, a
genuinely slower radio schedule. **Fix:** override that callback to reject the peer's
request, keeping this project's own `setConnectionParams(12, 12, ...)` (15ms) in effect
for the life of the connection. Confirmed on real hardware over a continuous 33-second
capture while the iron heated from 39°C to 349°C: tip-only reads held steady at 20-55ms
throughout, no degradation, with the previously-intended ~5-6 fast updates landing between
each full cycle.

**Update, same day again: still visible as a periodic stutter, even with the interval
fix** - the user reported the display updated quickly now but "came in jerks." With the
connection interval no longer degrading, the remaining cause was structural: a fast
tip-only poll (~50ms) running alongside a periodic 6-read "full cycle" (~500ms) still
meant every ~500ms, five extra reads landed back-to-back, briefly monopolizing the
connection and interrupting the otherwise-smooth tip-temp cadence - visible as a stutter
even though the *average* rate was fast. **Fix, at the user's suggestion:** replaced both
polls with a single round-robin cycle - every cycle reads tip_temp plus exactly one of the
other five characteristics (setpoint, dc_input, handle_temp, op_mode, est_watts) in
rotation, so every cycle costs roughly the same instead of alternating between small and
large bursts. Confirmed on real hardware: cycles landed at a uniform ~55ms (occasionally
~70ms), each secondary value refreshing roughly every 275ms (5 cycles) - plenty for
values that don't need tip-temp's responsiveness. **User-confirmed smooth on screen**,
no more jerks, once this shipped.

- **Backlight flicker** observed with WiFi on, powered via a laptop USB port - resolved by
  switching to a dedicated power source. Consistent with WiFi TX current spikes (routinely
  300-500mA bursts, well above BLE alone) exceeding what a laptop USB port can supply
  cleanly; no `BROWNOUT` reset was logged (hadn't crossed that threshold), just visible
  voltage sag. Don't power a WiFi-enabled build from a laptop/hub USB port for real testing.

### Claude usage zone on the clock screen (2026-07-15)

Ported from a handoff doc originally written for a sibling project's ESPHome/LVGL-YAML +
Home Assistant + MQTT stack (`docs/claude-usage-zone/`) - PineCYD
has none of that infrastructure, so this is the direct-fetch equivalent: the device itself
polls `https://api.anthropic.com/api/oauth/usage` (an undocumented endpoint used internally
by Claude Code's own HUD) every 60s while the clock screen is showing, and renders three
bars (5-hour, 7-day, 7-day-Sonnet utilization) beneath the clock - visual design (colors,
geometry, the pace-marker/stale-state logic) ported 1:1 from that doc; the data plumbing is
new, built for this project's own raw-LVGL/WiFi architecture instead of MQTT.

**Security tradeoff, explicitly chosen by the user, not silently decided:** the safer
design - a small bridge script on a trusted machine holding the OAuth token, publishing
only sanitized percentages/reset-times for the device to poll - was offered and declined.
The user's explicit choice: enter the token directly via the same WiFi captive portal
(masked input, `Preferences`/NVS storage, never pre-filled back into the portal's HTML so
the stored value is never re-exposed via view-source), matching how WiFi credentials and
timezone are already configured. Real, accepted risks worth restating plainly:
- This is a live OAuth access token for the user's own Anthropic account - full API access,
  not scoped to just this usage endpoint.
- **The token isn't refreshed.** The device stores a snapshot of whatever's pasted in; if
  Anthropic's access tokens expire on the usual OAuth timescale (hours, not months), the
  usage zone will start reporting "FORSINKET" (stale) until the token is manually
  re-entered via the portal. Not yet measured how long a given token actually lasts in
  practice - flag for whoever verifies this next.
- Undocumented endpoint, not part of Anthropic's public API reference - could change or
  disappear without notice.
- HTTPS uses `WiFiClientSecure::setInsecure()` (no certificate pinning), matching this
  project's OTA self-update precedent - acceptable for a hobby device, but worth knowing.

**Verified working end-to-end on real hardware, 2026-07-15**, including a real token and
real data rendering on screen. Getting there took a real diagnostic journey:

**The TLS handshake to `api.anthropic.com` initially failed with a memory allocation
error** (`X509 - Allocation of memory failed` / `BIGNUM - Memory allocation failed`), and
`ESP.getMinFreeHeap()` was observed dropping as low as **5.7-5.9KB**. Not "too little total
free memory" - the board typically reported 50KB+ free at the same moment - **heap
fragmentation**: a TLS handshake needs a large *contiguous* block (mbedTLS's
`MBEDTLS_SSL_IN_CONTENT_LEN`/`MBEDTLS_SSL_OUT_CONTENT_LEN` default to 16KB each, 32KB total,
compiled into the prebuilt framework library - `NetworkClientSecure` on this core has no
runtime `setBufferSizes()`, unlike older ESP32 Arduino cores, and `MBEDTLS_SSL_VARIABLE_
BUFFER_LENGTH` - the flag that would let the buffer shrink to match a negotiated smaller TLS
fragment size - was confirmed absent from the actual compiled `sdkconfig.h`, so the 32KB
figure is a hard, fixed property of this framework build, not reducible from application
code without rebuilding ESP-IDF from source).

Two hypotheses were tested and **disproved** (kept as a record so they're not re-tried):
- *Reduced BLE scan duty cycle* (thinking it was the same WiFi-starved-by-BLE contention
  that made the captive portal unjoinable, see above) - no effect on the failure rate.
- *Keeping WiFi on continuously instead of toggling it off while a Pinecil is connected*
  (thinking repeated init/teardown of mDNS/OTA/the settings server was fragmenting the
  heap) - **also no effect**: `min_heap` still collapsed to ~5.7KB with WiFi never once
  power-cycled. This also cost real BLE latency (`cycle_ms` ~49-137ms vs. the normal
  ~54-70ms) for zero benefit, so the WiFi on/off toggle was reverted to its original
  behavior.

**What actually fixed it - not shrinking the 32KB requirement (confirmed not possible from
application code), but making a large enough contiguous block reliably available:**
- **`LV_MEM_SIZE` cut from 40KB to 20KB**, based on measurement, not another guess:
  `lv_mem_monitor()` on real hardware (all three screens built) showed only ~13KB actually
  used. The original 40KB was never measured, just a round-number guess. Freed ~20KB back
  to the general heap with ~7KB of real headroom still spare.
- **The BLE scan is now paused for the duration of each fetch attempt** (same trick already
  proven for the captive portal), freeing whatever headroom its own buffers were holding
  right when it matters, and resumed immediately after.
- **A fast-fail check** via `heap_caps_get_largest_free_block()` before even attempting the
  connection - if there isn't a ~36KB contiguous block available, skip the attempt entirely
  (logging why) instead of blocking on a doomed multi-second connect timeout.
- The earlier eager-fetch-on-connect removal and the 60s->5min interval reduction (see
  git history) were real, kept improvements too, just not sufficient alone.
- **A second, unrelated bug surfaced once the memory issue cleared**: `deserializeJson()`
  reading directly from `http.getStream()` failed with `InvalidInput` even after a
  successful `HTTP 200` - cause not fully root-caused, worked around by buffering the
  response into a `String` via `http.getString()` first and parsing that instead (the
  response is only a few hundred bytes, buffering it costs nothing meaningful).

Confirmed via repeated live testing: 4 consecutive fetch attempts all completed the TLS
handshake successfully (`largest free contiguous block` stable around ~59-61KB, not
degrading across repeated attempts), and a real token produced `[Usage] Fetched OK` with
data rendering correctly on the physical screen.

**Structural limits worth knowing, not eliminated, just made reliable enough:** the 32KB
requirement itself is still real and fixed. If future changes to this firmware add more
concurrent memory pressure, the failure could return - the fast-fail check means it'll fail
*visibly and cheaply* (a log line, next retry in 5 min) rather than *silently and slowly*
(a multi-second doomed connect attempt) if that happens. The originally-offered bridge-
script alternative (a script on a trusted machine holds the token, device only polls plain-
HTTP sanitized percentages, never does TLS at all) remains available as a fallback if this
ever regresses. **Confirmed via a real rate-limit hit**: the fetch interval was briefly
dropped to 10s purely to iterate faster while testing - within a few minutes Anthropic's API
started returning `HTTP 429`, good real-world confirmation that 5 minutes is the right
production cadence, not just a heap-safety guess.

**Two more real bugs found and fixed once the fetch itself was reliable:**
- **Percentages displayed 100x too large** ("4300%" instead of "43%"), and the bar fill
  correspondingly clamped to always-100%-full. The original example payload the user
  supplied showed `utilization` as a 0-1 fraction (`0.42`); the real live API returns it
  already as a percentage (`43.0`). Fixed by removing the `* 100` conversion - trusted the
  live response over the earlier illustrative example, same "verify, don't assume"
  correction this project has made several times before.
- **Visible color banding/stripes in the bar fill gradient**, confirmed live (not a photo/
  camera moiré artifact - checked explicitly) and scaling with fill width. Root cause:
  `LV_DITHER_GRADIENT` was `0` in `lv_conf.h` - naive per-pixel linear interpolation on a
  16-bit RGB565 display without dithering is a well-known source of exactly this kind of
  banding, and LVGL has a purpose-built option for it. Fixed by enabling ordered dithering
  (not error-diffusion, cheaper and sufficient here) - costs ~800 bytes extra per gradient
  draw for these ~200px-wide bars.

**Flash headroom is now genuinely tight**: ~94% of the 1.875MiB OTA slot (`partitions_ota.csv`)
after adding ArduinoJson + HTTPClient + WiFiClientSecure on top of everything else - only
~120KB of slot headroom left for any future addition. If that becomes a real constraint,
the next lever is enlarging the OTA slots themselves (fewer than two would defeat OTA's own
purpose) - which means another partition-table change and another mandatory USB reflash,
same as the one `partitions_ota.csv` itself required.

**Planned next (not yet started), per the user, 2026-07-15:**
1. Revisit WiFi+BLE running simultaneously (the on/off toggle was tested and reverted
   tonight for cost with no benefit - but that test was specifically about the usage
   fetch's memory problem, which is now fixed a different way; the *reason* to revisit is
   item 2 below, a different motivation than tonight's failed experiment).
2. A web page for configuring the Pinecil itself, reachable at `pinecyd.local` while a
   Pinecil is connected (fast tip-temp reads aren't important on this page) - needs #1,
   since WiFi is currently off exactly when a Pinecil is connected.
3. Drop the numeric percentage text next to each bar - the bar fill alone is enough.
4. Make the number of usage-zone rows genuinely dynamic from the JSON response (currently
   hardcoded to exactly 3 pre-created rows matching today's known API shape - see
   `g_usageLines[3]` in `main.cpp`), and show nothing at all (not empty placeholder bars)
   when there's no token/no data - matches the original handoff doc's actual 2-4-row design
   intent, which was simplified away when this was first built.

### Always-on settings page (2026-07-15)

`http://pinecyd.local/` (plain HTTP, reachable while the clock screen is showing) lets you
update the Claude usage token or timezone without the BOOT-button/captive-portal dance -
added because the token isn't refreshed (see above) and was expected to need re-entry every
few hours, making the portal-only flow too much friction. Two real bugs found and fixed
along the way:
- **Saving used to hang the browser** - `handleSettingsSave()` called `fetchClaudeUsage()`
  (an HTTPS call with an up-to-16s timeout budget) *synchronously*, before sending the HTTP
  response. Fixed by setting a `g_forceUsageFetch` flag instead and letting `loop()` do the
  actual fetch on its next tick, so the browser gets an instant response.
- **Reloading the page was unstable** - the ESP32's basic `WebServer` handles HTTP
  keep-alive poorly; a browser reload firing more than one request close together (page +
  favicon.ico) could leave it stuck. Fixed with an explicit `Connection: close` header on
  every response and a real `onNotFound()` handler (mainly to answer the favicon request
  quickly rather than leave it unhandled).

## Bugs found and fixed along the way

All found via real hardware, not caught by review or serial logs alone in most cases:

- **Idle-scan heap leak (~6KB/hour).** An unattended overnight run (Pinecil powered off,
  firmware continuously scanning without connecting) showed heap declining steadily with
  no reset and no error. Root cause, traced into NimBLE's `NimBLEScan.cpp`: it retains
  every unique BLE advertiser it ever sees in an internal cache (`m_scanResults`, capped
  at 255 entries, never evicted) by default, for `NimBLEScan::getResults()` to use later -
  a feature never used here, since this firmware only reacts to the live `onResult()`
  callback. In a BLE-noisy environment (phones/wearables rotate their random MAC address
  roughly every 15 minutes for privacy), even a handful of physical nearby devices produce
  a steady stream of "never seen before" addresses, each permanently retained. **Fix:**
  `pScan->setMaxResults(0)` before `pScan->start(...)` - frees each device's memory right
  after its callback instead of caching it forever.
- **Crash on disconnect mid-poll-cycle.** A `Guru Meditation Error: Core 1 panic'ed
  (LoadProhibited)` (null-pointer read) crashed and rebooted the board during live testing.
  The panic backtrace (decoded with `xtensa-esp32-elf-addr2line` against the build's
  `.elf`) pointed at `readU32LE()` calling `chr->readValue()` with `chr == nullptr`.
  **Root cause:** `onDisconnect()` runs on NimBLE's own host task, not the Arduino `loop()`
  task, and nulls all characteristic pointers when it fires. `loop()` reads six
  characteristics back-to-back in one poll cycle using the raw global pointers, checked
  non-null only once at the *start* of the cycle - if the Pinecil disconnects between two
  reads in the same cycle, a later read dereferences an already-nulled pointer. Confirmed
  trigger in the log: `E NimBLERemoteValueAttribute: << Read complete; Not connected`
  immediately preceding the panic. **Fix:** `readU32LE()` now null-checks `chr` before
  dereferencing, failing that one read gracefully instead of crashing.
- **Power/voltage label showed "fW [box] fV" instead of real numbers.** Root cause:
  `lv_label_set_text_fmt()` goes through LVGL's own lightweight `snprintf`, which has
  `LV_SPRINTF_USE_FLOAT=0` by default (confirmed in `include/lv_conf.h`) - unlike
  `Serial.printf()` (real newlib printf, unaffected), LVGL's label text silently mangles
  `%f`. **Fix:** format watts/volts as separate integer whole/tenths parts instead of
  floats - also more natural given the underlying BLE data is already fixed-point.
- **Handle temp displayed higher than tip temp** (e.g. 406-411 while tip was ~350-360).
  IronOS's current source (`BSP.cpp`'s `NTCHandleLookup` table comment) claims
  `getHandleTemperature()` returns plain °C, but a handle at 406-411°C is physically
  impossible (the plastic would melt). Trusted the live reading's physical plausibility
  over the source comment and divided by 10 (giving 40.6-41.1°C, a very plausible handle
  temp after extended soldering) - conflicts with what the source comment claims; possible
  firmware-version drift. Flagged, not resolved to full source-level certainty.
- **A separator character showed as a missing-glyph box** in the power label. Root cause:
  `lv_font_montserrat_12` was built with `-r 0x20-0x7F,0xB0,0x2022` (see the font file's
  own header comment) - that's U+2022 (**bullet**, •), not U+00B7 (**middle dot**, ·),
  which the code had used by mistake (the two glyphs look nearly identical but are
  different Unicode code points, and only one is in this font). **Fix:** switched to the
  bullet's UTF-8 bytes.

## Status

Visually confirmed correct on real hardware while connected and heating, including a real
boost event, with flat heap throughout that session. All bugs above found and fixed, then
re-confirmed visually on a second round of hardware testing. The WiFi+BLE+LVGL combination
(with the new on/off toggle) is still accumulating soak-test time as of this writing - no
crash or disconnect observed yet, but this is the primary open stability question left.

## Building

```sh
cd firmware/fase2-ember-design
pio run
pio run -t upload
pio device monitor -b 115200
```

No compile-time WiFi credentials needed - connect the CYD to `PineCYD-Setup` after first
boot to configure WiFi (see the captive-portal section above).
