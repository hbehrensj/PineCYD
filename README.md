# PineCYD

Direct BLE dashboard for a Pinecil V2 soldering iron on an ESP32 CYD ("Cheap Yellow
Display") - bypassing the ~5s update latency of a Home Assistant / IronOS integration
chain used by a separate, private home-dashboard project. Full motivation and risk
assessment: [`docs/pinecil-direct-ble-handoff.md`](docs/pinecil-direct-ble-handoff.md).

**Standalone project**, not an extension of that other project's HA-driven, multi-page
context-switching design - see the handoff doc for why.

The firmware lives in [`firmware/fase2-ember-design/`](firmware/fase2-ember-design/) - see
its README for the full build instructions, design sources, and history of what's been
found and fixed on real hardware.

## Three ways to use it

The Pinecil dashboard and the Claude Code usage zone are independent features, gated on two
unrelated conditions (a live BLE connection to a Pinecil; a configured, reachable usage
bridge - see below) - so each of these gets the experience that's actually relevant to it,
with nothing broken or half-shown for lacking the other piece:

1. **Both a Pinecil V2 and Claude Pro/Max.** The full experience: the soldering dashboard
   while the iron is connected, a clock with the Claude usage zone while it isn't, and
   `http://pinecyd.local/` switches automatically between the Pinecil settings page (while
   connected) and the plain WiFi/timezone/bridge settings form (while not) - see the
   firmware README.
2. **Only a Pinecil V2.** The soldering dashboard works exactly the same. Leave the usage
   bridge address blank in settings - the clock (shown whenever the iron isn't connected)
   just displays the time/date with no usage zone, not an error or an empty placeholder.
3. **Only Claude Pro/Max, no Pinecil.** The device never finds a Pinecil to connect to, so
   it simply always shows the clock + usage zone (once a bridge address is configured - see
   [`bridge/README.md`](bridge/README.md)) - the dashboard code path just never runs, no
   Pinecil-specific setup required anywhere.

## Architecture at a glance

- **Firmware** (`firmware/fase2-ember-design/`): ESP32 + LVGL, BLE-polls the Pinecil
  directly (no Home Assistant), and serves a small always-on config website over WiFi.
- **Usage bridge** (`bridge/usage_bridge.py`, optional - only needed for the Claude usage
  zone): a script for a trusted machine already logged into Claude Code, so its OAuth
  session stays fresh for free. Re-serves sanitized usage percentages to the device over
  plain LAN HTTP - **the device itself never stores or sees an access token.**

## Key findings (see the firmware README for full detail)

- **Pinecil V2 only** - BLE hardware doesn't exist on V1.
- **IronOS's BLE characteristics are read-only, no notify/indicate** - "direct BLE" is
  still a polling architecture, not push-based. The win is a faster poll interval
  (~90-100ms/characteristic measured), not event-driven updates.
- All GATT UUIDs and enum values used here are taken from IronOS's actual source
  (`Ralim/IronOS`), not assumed or taken from third-party summaries without checking - one
  early third-party summary's claimed unit for the DC input characteristic was wrong (said
  mV, is actually decivolts) and was caught by checking real hardware readings.
- Display hardware (Sunton ESP32-2432S028R) pinout was carried over from an existing,
  already-verified config for the same physical board, but **color order and rotation had
  to be independently re-verified** for this project's own TFT_eSPI-based rendering path -
  they don't automatically transfer just because it's the same physical board.
- **Found and fixed a real ~6KB/hour heap leak**: NimBLE's default scan config
  permanently caches every unique BLE advertiser it ever sees (up to 255), which this
  project never needed since it only reacts to the live scan callback. One-line fix
  (`setMaxResults(0)`).
- **The BOOST-mode dispute is resolved, confirmed live:** engaging boost pushed tip temp
  to 426°C against a 350°C setpoint while `operating_mode` stayed at `Soldering` (1) the
  whole time - never the value a third-party Python library claims. The firmware
  control-flow trace was right.
- **Found and fixed a real crash**: a null-pointer panic if the Pinecil disconnects
  mid-poll-cycle (the disconnect callback nulls characteristic pointers on a different
  task than the polling loop reads them from). Decoded via the panic backtrace.
- **Several more real bugs surfaced only once eyes were actually on the screen**, none
  visible from serial logs alone: LVGL's own label-text formatting silently mangles `%f`
  (no float support by default); `handle_temp` needed the same x10-scaling correction as
  other characteristics despite IronOS's current source suggesting otherwise; a separator
  character was the wrong Unicode glyph for the built-in font (bullet vs. middle dot); and
  a `style_transform_zoom()` attempt to enlarge a label made it invisible (fixed with a
  real custom-generated font via `lv_font_conv` instead).
- **Adding WiFi (for a clock shown while no Pinecil is connected) costs ~83KB of heap**,
  confirmed by direct before/after measurement, and noticeably slowed BLE polling via
  radio coexistence contention - originally mitigated by turning WiFi off automatically
  whenever a Pinecil was connected. **That toggle is gone as of the Pinecil config page
  below** - WiFi and BLE now run simultaneously all the time, a real, measured latency cost
  (`cycle_ms` ~49-137ms vs. ~54-70ms) accepted deliberately so `pinecyd.local` stays
  reachable while soldering, not overlooked.
- **The same radio coexistence contention runs the other way too**: the WiFi setup
  portal's access point couldn't be joined at all (confirmed on both iOS and macOS) while
  this project's continuous BLE scan was left running - fixed by pausing the scan for as
  long as the portal is open. See the firmware README for the full root-cause writeup.
- **A partition subtype name isn't the same as working OTA support.** `huge_app.csv`
  labels its single 3MB app partition `ota_0`, but with no second slot to write into,
  there's no safe way to flash a new image without overwriting the code that's currently
  executing. Real OTA needed a genuine two-slot partition table - see the firmware README.
- **A bare `platform = espressif32` in `platformio.ini` isn't reproducible across
  machines.** Every real-hardware test in this project quietly ran on a different fork
  (pioarduino) than a fresh CI environment resolved by default - now pinned explicitly.
- **The clock's timezone was hardcoded** (Copenhagen) until a user in another timezone
  would have seen the wrong time - now configurable via the same WiFi setup portal.
- **The clock screen now shows Claude Code usage** (session/weekly/per-model-weekly,
  ported from a sibling project's design doc). First built with an explicitly user-chosen
  security tradeoff (an OAuth token entered and stored on-device) that was documented rather
  than silently decided - then **reversed a day later** once that token turned out to expire
  in hours and need constant manual re-entry: the device now polls a small bridge script on
  a trusted, already-logged-in-to-Claude-Code machine instead (`bridge/`) and never sees a
  token at all, a strictly safer design than the one it replaced. Getting the on-device fetch
  reliable in the meantime took real diagnosis: TLS handshakes were failing from heap
  fragmentation (not total free memory - see the firmware README's "Claude usage zone"
  section for the full writeup), fixed by measuring and shrinking LVGL's over-provisioned
  memory pool, pausing BLE scanning during each fetch, and failing fast when a fetch is
  clearly doomed instead of blocking on it - all still relevant to the bridge fetch today.
  **Verified working end-to-end on real hardware**, real data on screen.
- **A web page for configuring the Pinecil's own settings** (`http://pinecyd.local/`,
  swapping in automatically while a Pinecil is connected) - confirmed directly from
  `Ralim/IronOS` source that every one of the Pinecil's 56 settings has its own BLE
  characteristic (uint16 read/write, RAM-only until a separate save characteristic is
  written), not just the live dashboard values this project already read. Exposes 40 of the
  41 that are actually reachable over BLE at all (the enum lists 56, but 15 have no
  registered characteristic in the firmware regardless) - the BLE-enable setting itself is
  deliberately left out, since disabling it from the very page that depends on it being
  writable would cut its own access off. See the firmware README's "Pinecil config page"
  section.

## Building

```sh
cd firmware/fase2-ember-design
pio run
pio run -t upload
```

No compile-time WiFi credentials needed - on first boot the CYD opens its own
`PineCYD-Setup` WiFi network with a captive config portal (see the firmware's own README
for the full setup flow, cadence, and the BOOT-button reset).

**After that first USB flash, updates can go over WiFi** - no cable needed:

```sh
pio run -e ota -t upload
```

Pushes a build to the board at `pinecyd.local` via `ArduinoOTA`. See the firmware README's
"OTA updates over WiFi" section for how this works and why it needed a partition-table
change first.

## Flashing a release

Prebuilt binaries are attached to each [GitHub
Release](https://github.com/hbehrensj/PineCYD/releases) - no PlatformIO or compiling
needed:

- **`pinecyd-firmware-factory.bin`** - the one most people want. A single merged image
  (bootloader + partition table + app) for a brand-new or blank board. Flash at offset
  `0x0`.
- **`pinecyd-firmware.bin`** - app only, for updating a board that's already running
  PineCYD. Flash at offset `0x10000`.

```sh
pip install esptool
esptool.py --chip esp32 --port /dev/ttyUSB0 write_flash 0x0 pinecyd-firmware-factory.bin
```

(Use your board's actual serial port - e.g. `/dev/cu.usbserial-XXXX` on macOS, `COM3` on
Windows.) First boot opens the `PineCYD-Setup` WiFi network to configure your own network -
see "No compile-time WiFi credentials needed" above. This is the only USB flash needed -
every release binary already includes OTA support, so future updates can go over WiFi (see
"Building" above).

## Layout

```
docs/                                   handoff docs and design/decision records
  pinecil-direct-ble-handoff.md         original project motivation/risk assessment
  claude-usage-zone/                    handoff for the usage-zone widget
  pinecil-config-page/                  handoff + mockup for the Pinecil config page
firmware/
  fase2-ember-design/                   the firmware (PlatformIO project)
bridge/                                 optional usage bridge (only for the Claude usage zone)
  usage_bridge.py
  README.md                            setup, security model, launchd service
```
