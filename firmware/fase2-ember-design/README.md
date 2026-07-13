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

**Measured cost, real hardware:**
- Free heap: ~155KB (BLE+LVGL alone) → **~71-72KB with WiFi connected** (continuous) →
  **~107KB once WiFi is turned off again** after a Pinecil connects (some of WiFi's ~83KB
  cost doesn't fully release even with `WIFI_OFF` - not yet root-caused further).
- Had to switch `board_build.partitions` to `huge_app.csv` (3MB single app partition, no
  OTA/spiffs) - the default ~1.31MB app partition couldn't fit the image at all once WiFi
  was linked in (needed ~1.51MB).
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
