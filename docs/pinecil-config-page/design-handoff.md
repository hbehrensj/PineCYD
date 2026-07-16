# Handoff: Pinecil Configuration Page

## Overview
A web page, served at `pinecyd.local`, that lets a user read and write a connected Pinecil V2's own settings over BLE while it is connected to PineCYD. This is a new route on the existing ESP32 `WebServer` (`settingsServer` in `main.cpp`) — not an LVGL screen. It reuses the existing pattern (single inline HTML string, POST to a `/save`-style route, explicit `Connection: close` header) from the current `/` route (`handleSettingsRoot()`).

## About the Design Files
The bundled file (`pinecil-config-page.dc.html`) is a **design reference built in HTML** — it demonstrates layout, states, and interaction flow with realistic (locally-simulated) data. It is not production firmware code. The task is to recreate this design as a real page served by the ESP32 `WebServer`: a single embedded HTML string (built at compile time or served as a static asset, per the codebase's existing convention) with vanilla JS driving the actual BLE-read/BLE-write calls — no framework, matching the flash constraints described below.

## Fidelity
**High-fidelity.** Colors, typography, spacing, and interaction states in the design should be recreated closely. The specific settings list, ranges, and BLE protocol notes below are load-bearing — verify them against `Ralim/IronOS` source (`ble_characteristics.h`, `ble_handlers.cpp`, `ble_peripheral.c`) during implementation, not just this doc.

## Screens / Views

### 1. Main page (connected state)
**Purpose:** View and edit the 40 BLE-writable Pinecil settings (all 41 available minus the BluetoothLE setting itself, which is out of scope — see below), grouped into 5 tabs.

**Layout:**
- Root: full-height flex column, background `#0D0D0D`, text `#F2E7D8`, system sans-serif font stack.
- Sticky header (top: 0, z-index 10): background `#241C14`, bottom border `1px solid rgba(242,231,216,0.1)`, padding `16px 24px`, flex row `justify-content: space-between; align-items: center`.
  - Left: title "Pinecil Settings" (18px, weight 600) + subtitle "pinecyd.local/pinecil" (12px, `#8A7A6A`, 2px margin-top).
  - Right: 8px circle dot (`#FF6A2B`) + "Connected" label (13px, `#8A7A6A`), gap 8px.
- Tabs row: horizontal flex, gap 8px, padding `12px 24px`, `overflow-x: auto` (scrolls on narrow viewports), bottom border same as header. Each tab is a pill button (`padding: 8px 16px; border-radius: 999px; border: none; font-size: 13px; font-weight: 600`). Active tab: background `#FF6A2B`, text `#0D0D0D`. Inactive: transparent background, text `#8A7A6A`.
- Content area: `flex: 1`, `max-width: 760px`, centered (`margin: 0 auto`), padding `8px 24px 32px`.
  - Section heading: 12px, uppercase, `letter-spacing: 0.08em`, `#8A7A6A`, weight 600, margin `24px 0 4px`.
  - Field row: flex row, `flex-wrap: wrap`, `gap: 16px`, `justify-content: space-between`, padding `14px 0`, bottom border `1px solid rgba(242,231,216,0.08)`.
    - Left block (label + note): `flex: 1 1 220px; min-width: 200px`. Label 14px/500 in `#F2E7D8`. Note (when present) 12px, `#8A7A6A`, `line-height: 1.4`, `max-width: 420px`, `margin-top: 4px`.
    - Right block (control): flex row, `align-items: center`, `gap: 8px`.
- Sticky footer (bottom: 0, z-index 10): background `#241C14`, top border matching header, padding `14px 24px`, flex row `justify-content: space-between`, `flex-wrap: wrap`, `gap: 12px`.
  - Left: status text, 13px, `#8A7A6A` — "N changes not saved" or "All changes saved".
  - Right: "Discard" (secondary, outlined) + "Save changes" (primary) buttons, gap 10px.
- Toast (on save): fixed, `bottom: 80px`, horizontally centered, background `#FF6A2B`, text `#0D0D0D`, `padding: 10px 18px`, `border-radius: 20px`, 13px/600, drop shadow. Auto-dismisses after ~2.2s.

### 2. Disconnected state
**Purpose:** Clearly communicate the page is unusable without an active BLE connection, instead of showing a broken/empty form.
**Layout:** Full-screen fixed overlay (`inset: 0`, z-index 50) over background `#0D0D0D`, centered column, `gap: 10px`, `text-align: center`, `padding: 24px`.
- 12px circle dot, `#FF3B2F`.
- Heading "No Pinecil connected" (18px/600).
- Body "Connect a Pinecil V2 via BLE to PineCYD to read and change its settings." (14px, `#8A7A6A`, `max-width: 320px`).
- Countdown "Retrying in Ns…" (13px, `#8A7A6A`, monospace).
- Behavior: auto-retry countdown from 5s, looping, intended to poll/reload until BLE connection is detected.

### 3. Advanced tab — calibration gate
**Purpose:** Prevent accidental edits to hardware calibration values.
**Layout:** When the Advanced tab is opened and calibration fields haven't been revealed yet, show a warning banner instead of the fields: border `1px solid rgba(255,59,47,0.4)`, background `rgba(255,59,47,0.08)`, `border-radius: 8px`, padding 16px, margin `16px 0`.
- Heading "Calibration fields" (`#FF6A2B`, 14px/600).
- Body copy (13px, `#F2E7D8`, `line-height: 1.5`) warning that incorrect values affect temperature/voltage accuracy.
- Outlined button "Show calibration fields" (`border: 1px solid #FF6A2B; color: #FF6A2B`) that reveals the gated fields for the rest of the session.
- The "Diagnostics (read-only)" section (2 counters) is **not** gated — always visible on the Advanced tab.

## Interactions & Behavior

### Control types (per field)
- **Number**: `<input type="number">`, fixed width 90px, with a unit suffix label to its right (e.g. "°C", "W", "×0.1V"). Background `#0D0D0D`, border `1px solid rgba(242,231,216,0.2)`, `border-radius: 6px`, monospace font.
- **Slider**: `<input type="range">` (140px wide) + live numeric readout (monospace, right-aligned) to its right. Track/thumb accent color `#FF6A2B`.
- **Select (dropdown)**: native `<select>`, min-width 180px, same dark styling as number inputs, for enum-like settings (see Design Tokens/data below for label text).
- **Toggle**: custom switch, not a native checkbox. Track: 44×24px, `border-radius: 12px`, background `#FF6A2B` when on / `#3a3128` when off, `transition: background 0.15s`. Knob: 18×18px circle, `#F2E7D8`, positioned `left: 23px` (on) or `left: 3px` (off), `transition: left 0.15s`.
- **Read-only** (diagnostic counters): plain monospace text, `#8A7A6A`, no control.
- **Disabled/fixed** (tip type on Pinecil V2 hardware): a disabled text input showing the single valid value, muted background `#1a1512`, muted text `#8A7A6A`.

### Save flow
- All edits are held in local state only; nothing is written to the Pinecil until "Save changes" is clicked (matches the firmware's RAM-until-SAVE-characteristic behavior).
- Footer status text and Save/Discard button enabled-state reflect a live dirty-field count (compares current values to last-saved snapshot).
- Save button: primary (`#FF6A2B` bg / `#0D0D0D` text) when dirty and not saving; muted (`#3a3128` bg / `#8A7A6A` text, non-interactive) otherwise. Label switches to "Saving…" mid-save.
- Discard button: outlined, enabled only when dirty; resets all fields to the last-saved snapshot.
- On successful save, shows the toast "Saved N change(s) to the Pinecil" (or "No changes to save" if nothing was dirty), auto-dismissing after ~2.2s.
- **Not modeled in this design, required in production:** a save error state — if the BLE write is rejected (silently, at the protocol level, when the Pinecil's own BluetoothLE setting is read-only), the page needs a visible error explaining that saving failed because BLE write access is off on the device, distinct from the generic success toast.

### Fahrenheit toggle
- The "Show in Fahrenheit" toggle (Soldering tab) is purely a display convenience in this design: all temperature-unit fields (Soldering temp, Boost temp, Sleep temp) convert their **displayed** value and unit suffix live (°C ⇄ °F, standard conversion, rounded to the nearest integer) without altering the underlying stored value until an edit is made in that field.
- **Open question carried from the source doc:** confirm whether the real Pinecil TemperatureInF setting changes the *raw* value's unit (not just its display) — if so, the production implementation needs to convert on write, not just on display.

### Tabs
- Clicking a tab swaps the visible field set; state (dirty edits, revealed-advanced) persists across tab switches within a session.

## State Management
- `values`: current (possibly unsaved) value per setting key.
- `savedValues`: snapshot of last-saved values; diffed against `values` to compute dirty count and dirty state.
- `activeTab`: one of `soldering | sleep | power | display | advanced`.
- `advancedRevealed`: boolean, gates the 4 calibration fields on the Advanced tab (session-only, resets on reload).
- `saving`: boolean, drives the Save button's disabled/label state during the (simulated, ~700ms) save round-trip.
- `toast`: current toast message or null, auto-cleared after ~2.2s.
- `reconnectSeconds`: countdown shown in the disconnected overlay, loops 5→1.
- **Production data requirements not modeled here:** each field's actual value must be read from its own BLE characteristic (`f6d70000 + index`, raw uint16) when the page loads — there is no bulk-read characteristic. Save must write each dirty field's characteristic individually, then write `1` to the SAVE characteristic (`f6d7FFFF`) to persist RAM → flash.

## Design Tokens
| Token | Hex | Used for |
|---|---|---|
| `ember-bg` | `#0D0D0D` | Page background |
| `ember-surface` | `#241C14` | Header/footer/tab-row background |
| `ember-core` | `#FF6A2B` | Primary accent — active tab, toggle-on, primary button, connected dot |
| `ember-alert` | `#FF3B2F` | Disconnected-state dot, calibration warning border/bg |
| `ember-ash` | `#8A7A6A` | Secondary text, muted controls, inactive tab |
| `ember-text` | `#F2E7D8` | Primary text |

Typography: system sans-serif stack (`-apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif`); `ui-monospace, monospace` for numeric values/inputs. No custom webfonts (flash-constrained target).

Spacing scale in use: 4 / 6 / 8 / 10 / 12 / 14 / 16 / 24 / 32px. Border radius: 6px (inputs/buttons), 8px (banner), 12/20/999px (toggle track / toast / tab pills).

## Assets
None — no images or icons used. Status indicators are plain CSS circles, not SVGs.

## Settings data (all 40 fields, grouped by tab)

**Soldering tab** — Setpoint: Soldering temperature (#0, 10–450°C, default 320), Boost temperature (#22, 250–450°C, default 420), Show in Fahrenheit (#15, toggle). Buttons: Short-press step (#27, 1–50°), Long-press step (#26, 5–90°), Swap +/- buttons (#25, toggle). Startup & lock: Auto-start (#10, select: None/Soldering/Sleep temp/Zero power), Button lock (#17, select: Off/Boost only/Full), Power limit (#24, 0–120W step 5, default 0 = no limit).

**Sleep tab** — Sleep: Sleep temperature (#1, 10–450°C, default 150), Time to sleep (#2, 0–15 raw, default 5 — **unit unconfirmed, verify on hardware**), Time to shutdown (#11, 0–60min, default 10), Blink temp while cooling (#12, toggle, below 50°C). Sensors: Motion sensitivity (#7, slider 0–9), Hall-effect sensitivity (#28, slider 0–9), Time to sleep via magnet (#53, 0–12 ×5s).

**Power tab** — Power source: Power source type (#3, select: DC fixed 9V / 3S / 4S / 5S / 6S battery), Min. voltage per cell (#4, 24–38 ×0.1V), QC ideal voltage (#5, 90–220 step 2, ×0.1V), PD negotiation timeout (#32, 0–50 ×100ms), USB PD mode (#38, select: Fixed PDO only / PPS+EPR+extra power / PPS+EPR safe). Keep-awake pulse: Pulse power (#18, 0–100 ×0.1W), Wait between pulses (#19, 1–9 ×2.5s), Pulse duration (#20, 1–9 ×250ms).

**Display tab** — Display: Orientation (#6, select: Right/Left/Auto), Brightness (#34, slider 1–101 step 25), Invert colors (#33, toggle), Boot logo duration (#35, 0–6s), Fast description scroll (#16, toggle). Screens: Detailed idle screen (#13, toggle), Detailed soldering screen (#14, toggle), Loop animation (#8, toggle), Animation speed (#9, select: Off/Slow/Medium/Fast), Language code raw (#31, number — no human-readable lookup table built yet).

**Advanced tab** — Calibration (gated behind warning): Voltage calibration (#21, 360–900), Tip ADC offset (#23, 100–2500 µV), Trigger CJC calibration on next boot (#36, toggle, one-time action), Tip type (#54, disabled/fixed — auto tip detection active on this hardware, nothing to select). Diagnostics (read-only, ungated): Missing-accelerometer warning count (#29), Missing-PD-interface warning count (#30).

## Explicitly out of scope (do not build)
- **BluetoothLE setting (#37) itself** — changing it to read-only from this page would cut off the page's own write access on the next call. Only editable from the Pinecil's own physical menu.
- **Factory reset** — a separate RESET characteristic (`f6d7FFFE`) that wipes all 56 settings at once. Left out of v1 per product decision.
- **Profile-mode settings (#39–52) and button-swap (#55)** — not compiled into Pinecil V2 firmware, not available over BLE at all.

## Files
- `pinecil-config-page.dc.html` — the full design reference (self-contained; open directly in a browser). Includes a `connected` toggle (top of file, editable via the host's Tweaks panel) to preview the disconnected overlay.
