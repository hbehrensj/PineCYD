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
  radio coexistence contention - now mitigated by turning WiFi off automatically whenever
  a Pinecil is connected, back on only while the clock is showing.
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
- **The clock screen now shows Claude Code usage** (5-hour/7-day/7-day-Sonnet, ported from
  a sibling project's design doc) - built with an explicitly user-chosen security tradeoff
  (an OAuth token entered and stored on-device, not kept off-device via a safer bridge
  script) that's documented rather than silently decided. See the firmware README's "Claude
  usage zone" section - **not yet verified on real hardware.**

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
docs/                                   handoff doc and any future design/decision docs
firmware/
  fase2-ember-design/                   the firmware (PlatformIO project)
```
