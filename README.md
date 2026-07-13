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

## Building

```sh
cd firmware/fase2-ember-design
pio run
pio run -t upload
```

Needs WiFi credentials: copy `firmware/fase2-ember-design/include/secrets.h.example` to
`secrets.h` in the same directory and fill in your own `WIFI_SSID`/`WIFI_PASSWORD`
(gitignored, never commit real credentials).

## Layout

```
docs/                                   handoff doc and any future design/decision docs
firmware/
  fase2-ember-design/                   the firmware (PlatformIO project)
```
