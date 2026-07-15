# Handoff: Claude Usage-visualisering til PineCYD (DeskDash ur-skærm)

## Overview
Udvidelse af DeskDash's ur-skærm (idle/default) på en ESP32 "Cheap Yellow Display" (device `pinecyd`, ESPHome + LVGL 8.x, styret fra Home Assistant). Uret forbliver primær funktion; under det tilføjes en usage-zone der visualiserer Claude-abonnementets forbrug via en Python MQTT-bridge (topic `deskdash/claude/usage`).

## About the design files
Filen `DeskDash Ember Usage Mockup.dc.html` i denne mappe er en **HTML design-reference** — en pixel-nøjagtig 320×240 mockup af skærmen i 7 tilstande, ikke kode der skal kopieres. Opgaven er at **genskabe layoutet i ESPHome YAML / LVGL-widgets** (`lvgl:` config), ikke at bruge HTML/CSS/JS i firmwaren. Åbn filen i en browser for at se alle 7 states side om side.

## Fidelity
**High-fidelity.** Alle mål (x, y, bredde, højde, radius), farver (hex) og fontstørrelser er annoteret og skal oversættes 1:1. Layout er tegnet på et 8px-grid.

## Hardware constraints (must respect)
- Skærm: 320×240 px, ILI9341, landscape, RGB565 (16-bit) — ingen effekter der bander i gradienter med for få trin.
- LVGL 8.x tilladt: rektangler, rounded corners, lineære gradienter (horisontal/vertikal), linjer, tekst, ikon-glyffer.
- **Ikke tilladt:** blur, drop shadow, glow, transparency layering, komplekse SVG-paths. "Glød" opnås udelukkende med farve/gradient.
- Max 2 typefaces, max 3–4 font-størrelser total (hver koster flash som bitmap font).
- Ingen blink-effekter for alert-state.

## Screen composition

```
+--------------------------------------------------------------+
| y=14  clock "21:47"            font 60px bold, ember-text     |
| y=92  date "ons 15. jul"        font 14px medium, ember-ash     |
|                                                                |
| usage-zone: x=8 y=~120 w=304 h=112 (bottom third, 8px grid)    |
|   row (3 lines): h=32px, gap=8px between rows                 |
|   row (4 lines): h=22px, gap=6px between rows                 |
|   [label] [bar track + fill + pace marker] [pct%]              |
|   [resets_in, right-aligned, small, below track]               |
+--------------------------------------------------------------+
```

### Clock (unchanged from existing design — reuse as-is)
- Text: `HH:MM`, font-size 60px, weight 700, color `ember-text` (#F2E7D8), centered, box x=0 y=14 w=320 h=70.
- Date: `ddd D. mmm` (Danish, e.g. "ons 15. jul"), font-size 14px, weight 500, color `ember-ash` (#8A7A6A), centered, box x=0 y=92 w=320 h=16.

### Usage zone — the new element
Container: x=8, y computed to vertically center the row stack in a 112px-tall band starting at y=120 (`zoneTop = 120 + round((112 - totalRowsHeight) / 2)`), width=304.

**Row geometry — 3 lines (default):**
- Row height: 32px. Gap between rows: 8px.
- Label column: x=0 (relative to row), width=28px, font-size 12px weight 600, color `ember-ash`, vertically centered.
- Bar track: x=32, width=200px, height=16px, border-radius=8px, background `ember-surface` (#241C14).
- Bar fill: same x/y/height/radius as track, width = `track_width * pct/100`, horizontal linear gradient left→right:
  - Normal (fill behind pace marker, pct < 90): `ember-core` (#FF6A2B) → `ember-hot` (#FFB03A).
  - Over-pace or critical (pct ≥ period_pct, or pct ≥ 90): `ember-core` → `ember-alert` (#FF3B2F).
  - Stale data: `ember-surface` → `ember-ash` (desaturated, no core/hot/alert hues).
- Pace marker: vertical tick, width=2px, height = bar height + 4px (overshoots track top/bottom by 2px each), x = `track_width * period_pct/100 - 1` (relative to track x), color `ember-ash`, radius=1px. Always neutral — never a data color.
- Pct text: right-aligned in a 40px-wide column at the row's right edge, font-size 13px weight 700, color `ember-text` normally, `ember-alert` if pct ≥ 90 or depleted (pct=100).
- Reset countdown (`resets_in`): right-aligned, spans track+pct width, font-size 10px weight 500, color `ember-ash` — EXCEPT in the "Opbrugt" (100%) state where it becomes the important info: weight 700, color `ember-text`. Shown diskret in every state (per stakeholder decision — not just critical/depleted).

**Row geometry — 4 lines:** row height=22px, gap=6px, bar height=14px, radius=7px. Same column widths/colors as above.

**Row geometry — 2 lines:** same as 3-line spec (row h=32, bar h=16); rows are simply vertically centered in the 112px band (more surrounding whitespace).

### Stale-data indicator
When `stale: true` (or payload > 15 min old): all bars desaturate to `ember-surface`→`ember-ash` fill (no orange/red hues), plus a small label "FORSINKET" — font-size 9px weight 700, letter-spacing 0.08em, color `ember-ash`, top-right corner, x offset 8px from right edge, y=8. Clock and date are NOT affected.

## States to implement (7, see mockup)
1. **Normal** — all lines pct < period_pct → core→hot fill.
2. **Over pace** — one or more lines pct > period_pct (but < 90) → core→alert fill on that line.
3. **Kritisk** — a line ≥ 90% → alert fill + pct text turns `ember-alert`.
4. **Opbrugt** — a line at 100% → full track, resets_in becomes the emphasized text.
5. **Stale** — desaturated bars + "FORSINKET" tag; clock unaffected.
6. **2 linjer** — usage zone with 2 rows, vertically centered.
7. **4 linjer** — usage zone with 4 rows, compressed row height (22px/14px bar).

## Data source / state logic
MQTT topic `deskdash/claude/usage`, retained JSON:
```json
{
  "ts": "2026-07-15T14:32:00Z",
  "stale": false,
  "lines": [
    { "id": "five_hour", "label": "5T", "pct": 42, "period_pct": 63, "resets_in": "1t 51m" }
  ]
}
```
- `lines` length is dynamic (2–4); the usage-zone layout must scale row height/gap per count as specified above (see `buildAnnotation`/`makeFrame` in the mockup's JS for the exact formula: `rowH = 4 lines ? 22 : 32`, `gap = 4 lines ? 6 : 8`, `zoneTop = 120 + round((112 - totalRowsHeight)/2)`).
- Per line: `overPace = pct > period_pct`, `critical = pct >= 90`, `depleted = pct >= 100`. These three booleans drive fill gradient end-color and pct-text color per the rules above.
- Treat `stale === true` OR `now - ts > 15min` identically (dim + tag).
- No touch affordance on the usage zone in v1 (pure display, per stakeholder decision). No dedicated night-dim variant — handled globally by backlight/Home Assistant.

## Design tokens (Ember theme — reference palette, confirm against DeskDash's canonical ESPHome theme file if one exists)

| Token | Hex | Use |
|---|---|---|
| `ember-bg` | `#14100C` | Screen background |
| `ember-surface` | `#241C14` | Bar track (empty) |
| `ember-core` | `#FF6A2B` | Bar fill, primary |
| `ember-hot` | `#FFB03A` | Fill gradient leading edge (normal) |
| `ember-alert` | `#FF3B2F` | Fill leading edge over-pace/critical/depleted pct text |
| `ember-ash` | `#8A7A6A` | Secondary text, pace marker, stale tag |
| `ember-text` | `#F2E7D8` | Clock, primary numbers |

## Typography
Single typeface (system default in mockup — swap for the project's existing custom TTF/bitmap font). Sizes used (3 total, within the 3–4 budget):
- 60px / weight 700 — clock only.
- 14px / weight 500 — date, and 12–13px/600–700 for bar labels & pct (round to whatever the existing bitmap font ladder already defines, e.g. 12/14).
- 9–11px / weight 500–700 — reset countdown, stale tag.

## LVGL implementation notes
- Bar track + fill → two stacked `lv_obj` (or a single `lv_bar` for track+fill, but the two-gradient-color requirement per pct/period_pct means a plain `lv_bar` with a gradient style works only for the "normal vs alert" binary; recompute the fill object's `bg_grad_color`/`bg_color` in the MQTT-update lambda whenever `overPace`/`critical` changes).
- Pace marker → a thin `lv_obj` (line) positioned via `x` computed from `period_pct` in the same update lambda; re-position on every payload.
- Row/zone repositioning for 2/3/4-line counts → either pre-declare 4 row containers and hide unused ones (simplest, fixed flash cost), or generate rows dynamically — pre-declared + `obj.add_flag(LV_OBJ_FLAG_HIDDEN)` is more ESPHome-idiomatic.
- All geometry above is in absolute px against the 320×240 canvas — map directly to LVGL `x`/`y`/`w`/`h` on each widget.

## Assets
None — no icons/images, glyph-only.

## Files
- `DeskDash Ember Usage Mockup.dc.html` — the 7-state HTML mockup (open in any browser).
