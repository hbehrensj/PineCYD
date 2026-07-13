// PineCYD - Fase 2: port of an "Ember" Pinecil screen design (screen P2, originally made
// for a separate private project) to raw LVGL.
//
// Design source: that other project's own design-handoff doc and the actual built widget
// tree in its ESPHome config (colors, fonts, and device-pixel positions below are taken
// from there, not re-derived). Only the
// *implementation* is rewritten (ESPHome/LVGL-YAML -> raw LVGL C++, per the handoff doc's
// Fase 2 instructions) - the design itself is a straight port, with these known
// simplifications (see README.md for the full list):
//   - No MDI icon glyphs (would need a custom font-conversion pipeline) - mode is
//     communicated by the colored text label alone.
//   - No "BOOST" state - confirmed via a live hardware test, 2026-07-13: engaging boost
//     pushed tip temp to 426C against a 350C setpoint while op_mode stayed at 1
//     (Soldering) throughout - never the value 2 that pynecil (the Python library HA's own
//     IronOS integration uses) defines as BOOST. See README.md for the full trace.
//   - No setpoint +/- control at all (removed at the user's request, 2026-07-13 - they
//     control setpoint on the iron itself). The setpoint/"TARGET" display in the right
//     rail and the chart's reference line are both still read-only telemetry.
//   - Power bar % is estimated watts against an assumed nominal max, not an iron-reported
//     ceiling - flagged as approximate per the design doc's own "tune on-device" note.
//   - Setpoint reference line in the chart is a real scrolling history (like the tip-temp
//     line), not a flat line at the current value - matches the design doc's simpler
//     documented alternative to a dashed 50%-opacity line, and directly answers the user's
//     2026-07-13 request to see how the target changed over time, not just its current
//     value. Same solid-line style as the tip-temp series (LVGL 8 charts share line style
//     across all series in one chart) - distinguished by color instead.
//
// Logs heap/reset-reason continuously (see setup()/loop()) - the cheapest signal for the
// watchdog-reset and heap-leak failure modes this board is prone to (see README.md).

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
// TFT_eSPI.h (included above) defines FS_NO_GLOBALS before its own #include <FS.h>, to get
// SPIFFS support for smooth fonts without polluting the global namespace - this permanently
// suppresses FS.h's "using fs::FS;" for the rest of this translation unit (macros aren't
// scoped, and FS.h's include guard means a later plain #include <FS.h> is a no-op anyway).
// WebServer.h (pulled in by WiFiManager.h) needs bare `FS` at global scope, so restore the
// alias explicitly - confirmed by a real build failure, 2026-07-13, not a hypothetical
// include-order nicety.
using fs::FS;
using fs::File;
#include <WiFiManager.h>
#include <esp_system.h>
#include <lvgl.h>
#include <time.h>

// WiFi credentials are no longer compile-time (see the removed secrets.h) - WiFiManager
// (2026-07-13) puts the CYD into its own AP + captive portal on first boot (or whenever no
// stored network is reachable), so a single prebuilt release binary works for anyone
// without rebuilding with their own credentials baked in. See README.md for the setup flow
// and setup()'s WiFiManager block below.

// Experimental, 2026-07-13: WiFi + NTP, running continuously alongside BLE + LVGL, at the
// user's explicit choice after being told the tradeoff - this is a materially different
// risk than anything tested so far. The other project's own docs flag BLE+LVGL on this
// no-PSRAM board as already fragile (watchdog resets during BT init); this adds a third
// concurrent subsystem (WiFi) sharing the same single radio via ESP32's WiFi/BT
// coexistence layer, on top of that. Needs its own soak test, same rigor as BLE+LVGL
// alone got - don't assume this combination is fine just because BLE+LVGL alone was.

// Custom 72px font (digits + hyphen only) for the tip-temp hero value, generated via
// lv_font_conv from the same Montserrat-Medium.ttf LVGL's own built-in fonts use - see
// src/fonts/lv_font_montserrat_72_digits.c for the exact command. Built-in Montserrat only
// goes up to 48px; a style_transform_zoom() attempt to go bigger without a real font asset
// made the value invisible on real hardware (see buildUi() for why) - this is the reliable
// fix, at the user's request for a ~50% bigger hero number, 2026-07-13.
LV_FONT_DECLARE(lv_font_montserrat_72_digits);

// --- Confirmed IronOS BLE UUIDs (Pinecil V2 only) - Live Data service ---
static const char* SERVICE_LIVE_DATA  = "d85ef000-168e-4a71-aa55-33e27f9bc533";
static const char* CHAR_LIVE_TEMP     = "d85ef001-168e-4a71-aa55-33e27f9bc533"; // uint32 LE, deg C
static const char* CHAR_SETPOINT_TEMP = "d85ef002-168e-4a71-aa55-33e27f9bc533"; // uint32 LE, deg C
static const char* CHAR_DC_INPUT      = "d85ef003-168e-4a71-aa55-33e27f9bc533"; // uint32 LE, decivolts
static const char* CHAR_HANDLE_TEMP   = "d85ef004-168e-4a71-aa55-33e27f9bc533"; // uint32 LE, deg C
static const char* CHAR_OP_MODE       = "d85ef00D-168e-4a71-aa55-33e27f9bc533"; // uint32 LE, OperatingMode enum
static const char* CHAR_EST_WATTS     = "d85ef00E-168e-4a71-aa55-33e27f9bc533"; // uint32 LE, decwatts (IronOS: x10WattHistory)
static const char* SERVICE_BULK_DATA  = "9eae1000-9d0d-48c5-aa55-33e27f9bc533"; // scan filter only

// Every cycle reads tip_temp plus one of the other five characteristics in round-robin
// (see loop()) - not a fixed 6-read burst. 50ms is below the real read latency floor
// (~20-45ms/read once the connection-interval fix - see README.md - keeps the fast
// interval from being renegotiated away), so the achieved cycle rate is bounded by that
// floor, not by this constant.
static const uint32_t TIP_POLL_INTERVAL_MS = 50;

// IronOS OperatingMode enum values that matter here (confirmed from
// Ralim/IronOS source/Core/Threads/UI/logic/OperatingModes.h, 2026-07-13; note the enum
// skips value 2 entirely - see the BOOST discussion in this file's header comment for why
// that's deliberate, not a typo here).
enum IronOSOperatingMode {
  MODE_HOME_SCREEN = 0,
  MODE_SOLDERING   = 1,
  MODE_SLEEPING    = 3,
  MODE_SOLDERING_PROFILE = 6,
  MODE_HIBERNATING = 14,
};

enum UiState { UI_IDLE, UI_SOLDERING, UI_SLEEP };

// Estimated watts is reported against an assumed nominal ceiling for the *bar fill %* -
// approximate per the design doc's own "tune on-device" fidelity note. The bar's *color
// ramp* threshold below is separate and better-grounded: borrowed directly from the other
// project's own documented judgment call - 65W as a common USB-PD charger tier boundary,
// not a fraction of this assumed ceiling.
static const float ASSUMED_MAX_WATTS       = 100.0f;
static const float HIGH_DRAW_THRESHOLD_W   = 65.0f;

// --- Ember theme palette (hex values from the original design's color/theme tokens) ---
static const lv_color_t COLOR_BLACK      = LV_COLOR_MAKE(0x0D, 0x0D, 0x0D);
static const lv_color_t COLOR_WARM_TRACK = LV_COLOR_MAKE(0x2E, 0x21, 0x14);
static const lv_color_t COLOR_WARM_LINE  = LV_COLOR_MAKE(0x24, 0x1A, 0x10);
static const lv_color_t COLOR_WHITE      = LV_COLOR_MAKE(0xF2, 0xF0, 0xEB);
static const lv_color_t COLOR_LIGHT_GRAY = LV_COLOR_MAKE(0x99, 0x99, 0x99);
static const lv_color_t COLOR_GRAY       = LV_COLOR_MAKE(0x66, 0x66, 0x66);
static const lv_color_t COLOR_EP_ORANGE  = LV_COLOR_MAKE(0xF3, 0x73, 0x20);
static const lv_color_t COLOR_AMBER      = LV_COLOR_MAKE(0xF4, 0xA9, 0x00);
static const lv_color_t COLOR_DEEP_ORANGE = LV_COLOR_MAKE(0xFF, 0x66, 0x00);
static const lv_color_t COLOR_STEEL_BLUE = LV_COLOR_MAKE(0x60, 0x66, 0x82);
static const lv_color_t COLOR_MISTY_BLUE = LV_COLOR_MAKE(0x9B, 0xA2, 0xBC);

static const NimBLEAdvertisedDevice* advDevice    = nullptr;
static NimBLEClient*                 pClient      = nullptr;
static NimBLERemoteCharacteristic*   pChrLiveTemp   = nullptr;
static NimBLERemoteCharacteristic*   pChrSetpoint   = nullptr;
static NimBLERemoteCharacteristic*   pChrDcInput    = nullptr;
static NimBLERemoteCharacteristic*   pChrHandleTemp = nullptr;
static NimBLERemoteCharacteristic*   pChrOpMode     = nullptr;
static NimBLERemoteCharacteristic*   pChrEstWatts   = nullptr;

static volatile bool doConnect        = false;
static volatile bool connected        = false;
static uint32_t       disconnectCount = 0;

// --- Display / LVGL ---
static const uint16_t SCREEN_W = 320;
static const uint16_t SCREEN_H = 240;
// 2 min at 2000ms per chart point (see CHART_PUSH_INTERVAL_MS) - reduced from 240 points
// at 500ms, 2026-07-13: redrawing a 240-point chart line turned out to cost ~450ms of
// lv_timer_handler() time on every full-poll cycle (found while diagnosing why the fast
// tip-only poll wasn't visibly faster on screen - it was being starved by this redraw,
// not by BLE latency). At this display's ~310px chart width, 60 points is still smooth.
static const int      HISTORY_POINTS = 60;
static const uint32_t CHART_PUSH_INTERVAL_MS = 2000; // independent of the label/BLE poll rate

TFT_eSPI                   tft = TFT_eSPI();
static lv_disp_draw_buf_t  draw_buf;
static lv_color_t          buf1[SCREEN_W * 20];

static lv_obj_t*        label_tip_value;
static lv_obj_t*        label_setpoint_value;
static lv_obj_t*        label_mode;
static lv_obj_t*        label_power;
static lv_obj_t*         label_handle;
static lv_obj_t*        bar_power;
static lv_obj_t*        chart_history;
static lv_chart_series_t* series_temp;
static lv_chart_series_t* series_setpoint_ref;

// Clock screen (shown while no Pinecil is connected) vs. the dashboard screen built above.
static lv_obj_t* scr_dashboard;
static lv_obj_t* scr_clock;
static lv_obj_t* label_clock_time;
static bool      clockScreenActive = false; // set explicitly once at boot, see setup()

// WiFi captive-portal config screen (shown only while the WiFiManager AP+portal is open).
static lv_obj_t* scr_wifi_config;

// --- WiFi captive portal (2026-07-13) ---
// Replaces compile-time secrets.h credentials so a single prebuilt release binary works for
// anyone. Cadence is per the user's explicit spec: try the saved network for 30s; if that
// fails, automatically open the config portal; if the portal itself isn't configured either,
// give up for now and retry the whole thing again every 2 minutes - never block forever in
// either state. WiFiManager's non-blocking mode (setConfigPortalBlocking(false) below) is
// what makes this safe to drive from this project's own loop() without stalling BLE/LVGL for
// the duration of a connect attempt or an open portal - process() below only ever does a
// small chunk of work per call, same as lv_timer_handler().
static WiFiManager    wm;
static bool           wifiConfigActive      = false; // mirrors wm.getConfigPortalActive(), see loop()
static const char*    WIFI_AP_NAME          = "PineCYD-Setup";
static const uint32_t WIFI_CONNECT_TIMEOUT_S = 30;     // give up on the saved network after this long
static const uint32_t WIFI_RETRY_INTERVAL_MS = 120000; // 2 min - also used as the portal's own timeout, see setup()

// Credentials cached in RAM after the first successful connect this session, and used for
// every reconnect from then on instead of a no-arg WiFi.begin(). Borrowed from a lesson in a
// sibling project (TDAI-2170): WiFi.begin(ssid, pass) with WiFi.persistent() left at its
// default (true) writes NVS flash on every call, which is fine for an occasional
// outage-driven reconnect but not for this project's on/off toggle, which reconnects on
// every single Pinecil disconnect - potentially many times per session. Set
// WiFi.persistent(false) once these are cached (see loop()'s NTP-edge block) so that cost
// disappears entirely; only the initial WiFiManager-driven connect/portal save still writes
// flash, same as any other WiFiManager use.
static String g_wifiSsid;
static String g_wifiPsk;

// GPIO0 is this board's BOOT button (also used for entering flash mode, but freely readable
// at runtime afterwards - confirmed not to collide with any TFT pin in platformio.ini).
// Holding it down through boot forces a fresh WiFi config portal even if a stored network
// would otherwise still connect - useful for switching networks without re-flashing.
static const int WIFI_CONFIG_BUTTON_PIN = 0;

// Fires when WiFiManager's AP+portal actually opens (saved network unreachable within
// WIFI_CONNECT_TIMEOUT_S, or the BOOT-button reset below forced it). Gives the user an
// on-screen instruction instead of a silently-frozen clock - this project's screen is
// otherwise not being driven by anything else while the portal is up.
static void onConfigPortalStart(WiFiManager* wmPtr) {
  Serial.printf("[WiFi] Config portal open - join \"%s\" and visit http://192.168.4.1\n", WIFI_AP_NAME);
  wifiConfigActive = true;
  // The continuous BLE scan (setInterval==setWindow, ~100% duty cycle - see setup()) shares
  // ESP32's single 2.4GHz radio with WiFi via its WiFi/BT coexistence layer, same root cause
  // as the documented BLE-poll slowdown when WiFi is on (see README.md) - just mirrored here.
  // Confirmed live, 2026-07-13: the AP started fine per this project's own log
  // (StartAP/AP IP address both printed), but a phone and a Mac both got "could not join" -
  // the softAP's association handshake needs timely airtime iOS/macOS won't tolerate
  // stalling on. Pausing the scan for the portal's duration (resumed in loop() once it
  // closes) is a low-cost tradeoff: the Pinecil isn't expected to be the focus while the
  // user is actively configuring WiFi anyway.
  NimBLEDevice::getScan()->stop();
  lv_scr_load(scr_wifi_config);
  lv_timer_handler(); // flush this one frame now; wm.process() drives everything from here
}

// Kicks off a (re)connect attempt, used from setup(), the WiFi on/off toggle, and the
// periodic retry in loop() alike. Uses the cached credentials directly once we have them
// (fast path, no flash write - see g_wifiSsid/g_wifiPsk above); otherwise falls back to
// WiFiManager's autoConnect(), which tries any NVS-stored network and opens the config
// portal if that fails. Both paths are non-blocking (setConfigPortalBlocking(false) is set
// once in setup()) - safe to call from loop() without stalling BLE/LVGL.
static void connectWifiNow() {
  WiFi.mode(WIFI_STA);
  if (g_wifiSsid.length()) {
    WiFi.begin(g_wifiSsid.c_str(), g_wifiPsk.c_str());
  } else {
    wm.autoConnect(WIFI_AP_NAME);
  }
}

static const char* resetReasonToStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXTERNAL_PIN";
    case ESP_RST_SW:        return "SOFTWARE";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INTERRUPT_WATCHDOG";
    case ESP_RST_TASK_WDT:  return "TASK_WATCHDOG";
    case ESP_RST_WDT:       return "OTHER_WATCHDOG";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP_WAKE";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

static uint32_t readU32LE(NimBLERemoteCharacteristic* chr, bool& ok) {
  // chr can go null between the top of a poll cycle and this call: onDisconnect() runs on
  // NimBLE's own host task and can null out all six pointers mid-cycle, since our loop()
  // does six sequential reads without re-checking. Confirmed as a real crash on real
  // hardware, 2026-07-13: LoadProhibited panic inside NimBLE's readValue() with EXCVADDR=0,
  // backtrace through this function, right after the Pinecil disconnected mid-poll.
  if (chr == nullptr) {
    ok = false;
    return 0;
  }
  NimBLEAttValue v = chr->readValue();
  if (v.size() < 4) {
    ok = false;
    return 0;
  }
  const uint8_t* d = v.data();
  ok               = true;
  return (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
}

static void lvglDispFlush(lv_disp_drv_t* disp, const lv_area_t* area, lv_color_t* color_p) {
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.pushColors((uint16_t*)&color_p->full, w * h, true);
  tft.endWrite();

  lv_disp_flush_ready(disp);
}

static lv_obj_t* makeLabel(lv_obj_t* parent, int16_t x, int16_t y, const lv_font_t* font, lv_color_t color, const char* text) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_obj_set_pos(l, x, y);
  return l;
}

static void buildUi() {
  lv_obj_t* scr = lv_scr_act();
  scr_dashboard = scr; // kept as its own screen so we can lv_scr_load() back to it - see buildClockUi()
  lv_obj_set_style_bg_color(scr, COLOR_BLACK, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

  // ---- hero: tip temperature ----
  // Enlarged and given the whole left side, 2026-07-13: the setpoint +/- buttons are gone
  // (user controls setpoint on the iron itself, not via touch - there's no touchscreen
  // driver in this project anyway) and setpoint moved to the right rail below, freeing up
  // this space. Uses the custom 72px digits-only font (see the LV_FONT_DECLARE above) -
  // built-in Montserrat only goes up to 48px, and a style_transform_zoom() attempt to go
  // bigger without a real font asset made the value invisible on real hardware (LVGL 8's
  // zoom transform likely doesn't expand the invalidated/flush area to match, so the
  // zoomed content never reached our partial-buffer flush callback).
  lv_obj_t* tipLabel = makeLabel(scr, 12, 18, &lv_font_montserrat_12, COLOR_LIGHT_GRAY, "TIP TEMP");
  lv_obj_set_style_text_letter_space(tipLabel, 1, 0);

  label_tip_value = makeLabel(scr, 10, 40, &lv_font_montserrat_72_digits, COLOR_EP_ORANGE, "--");

  makeLabel(scr, 150, 74, &lv_font_montserrat_20, COLOR_LIGHT_GRAY, "\xC2\xB0" "C"); // UTF-8 degree sign, baseline-ish beside the value

  // ---- right rail: target / mode / power / handle temp ----
  // No MDI icon glyph (see file header) - mode label starts where the icon+label row did,
  // reclaiming the icon's space rather than leaving a gap. Doubles as the BLE connection
  // status when not connected (removed the separate "BLE: ..." label at the user's
  // request, 2026-07-13 - redundant once connected, since a real tip temp already implies
  // that; this slot is otherwise unused while disconnected anyway).
  label_mode = makeLabel(scr, 196, 20, &lv_font_montserrat_12, COLOR_GRAY, "SCANNING...");
  lv_obj_set_style_text_letter_space(label_mode, 1, 0);

  // Read-only display now (moved here from the removed +/- buttons, 2026-07-13) - the
  // iron's own setpoint value, not adjustable from this screen.
  lv_obj_t* targetLabel = makeLabel(scr, 196, 40, &lv_font_montserrat_12, COLOR_LIGHT_GRAY, "TARGET");
  lv_obj_set_style_text_letter_space(targetLabel, 1, 0);
  label_setpoint_value = makeLabel(scr, 196, 52, &lv_font_montserrat_24, COLOR_WHITE, "--\xC2\xB0");

  label_power = makeLabel(scr, 196, 88, &lv_font_montserrat_12, COLOR_LIGHT_GRAY, "-- W \xE2\x80\xA2 --.-V");

  bar_power = lv_bar_create(scr);
  lv_obj_set_pos(bar_power, 196, 106);
  lv_obj_set_size(bar_power, 112, 8);
  lv_obj_set_style_radius(bar_power, 4, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar_power, COLOR_WARM_TRACK, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bar_power, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(bar_power, 4, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(bar_power, COLOR_AMBER, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(bar_power, LV_OPA_COVER, LV_PART_INDICATOR);
  lv_bar_set_range(bar_power, 0, 100);
  lv_bar_set_value(bar_power, 0, LV_ANIM_OFF);

  label_handle = makeLabel(scr, 196, 122, &lv_font_montserrat_12, COLOR_LIGHT_GRAY, "HANDLE --\xC2\xB0" "C");

  // ---- bottom strip: temp history ----
  // Shrunk from the original 98px (y=142) at the user's request, 2026-07-13, once the
  // header label below was removed as redundant - frees the extra height for a bigger tip
  // temp above instead.
  lv_obj_t* strip = lv_obj_create(scr);
  lv_obj_set_pos(strip, 0, 170);
  lv_obj_set_size(strip, 320, 70);
  lv_obj_set_style_bg_color(strip, COLOR_BLACK, 0);
  lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(strip, 1, 0);
  lv_obj_set_style_border_color(strip, COLOR_WARM_LINE, 0);
  lv_obj_set_style_border_side(strip, LV_BORDER_SIDE_TOP, 0);
  lv_obj_set_style_radius(strip, 0, 0);
  lv_obj_set_style_pad_all(strip, 0, 0);
  lv_obj_clear_flag(strip, LV_OBJ_FLAG_SCROLLABLE);

  // "TIP - LAST 2 MIN" header removed at the user's request, 2026-07-13 - redundant next
  // to the hero tip-temp value above. Chart now uses nearly the whole (shrunk) strip.
  chart_history = lv_chart_create(strip);
  lv_obj_set_pos(chart_history, 5, 4);
  lv_obj_set_size(chart_history, 310, 62);
  lv_obj_set_style_bg_opa(chart_history, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(chart_history, 0, 0);
  lv_obj_set_style_pad_all(chart_history, 0, 0);
  lv_obj_set_style_size(chart_history, 0, LV_PART_INDICATOR); // hide point markers
  lv_obj_set_style_line_width(chart_history, 3, LV_PART_ITEMS);
  // Rounded joints cost more to render per segment - not worth it for a fast-scrolling
  // trend line at this size (found while cutting chart redraw cost, 2026-07-13).
  lv_obj_set_style_line_rounded(chart_history, false, LV_PART_ITEMS);
  lv_chart_set_type(chart_history, LV_CHART_TYPE_LINE);
  lv_chart_set_div_line_count(chart_history, 0, 0);
  lv_chart_set_point_count(chart_history, HISTORY_POINTS);
  lv_chart_set_range(chart_history, LV_CHART_AXIS_PRIMARY_Y, 0, 450);
  lv_chart_set_update_mode(chart_history, LV_CHART_UPDATE_MODE_SHIFT);

  // Setpoint reference line first (drawn under), temp history on top - both share this
  // chart's LV_PART_ITEMS line style (width/rounded above); distinguished by color, not a
  // dashed/thinner line as the design calls for (LVGL 8 charts style all series alike).
  // Darkened to the same warm_track color the removed +/- buttons used, at the user's
  // request, 2026-07-13 - a quiet backdrop line rather than competing with the tip line.
  series_setpoint_ref = lv_chart_add_series(chart_history, COLOR_WARM_TRACK, LV_CHART_AXIS_PRIMARY_Y);
  series_temp         = lv_chart_add_series(chart_history, COLOR_EP_ORANGE, LV_CHART_AXIS_PRIMARY_Y);
  // lv_chart_add_series() leaves each series' point buffer uninitialized (lv_mem_alloc,
  // not zeroed) - without this, both lines would plot garbage until the first BLE read
  // arrives (or indefinitely, for setpoint_ref, if the Pinecil is never seen at all).
  lv_chart_set_all_value(chart_history, series_temp, LV_CHART_POINT_NONE);
  lv_chart_set_all_value(chart_history, series_setpoint_ref, LV_CHART_POINT_NONE);
}

// Shown whenever no Pinecil is connected (see loop()) - a separate LVGL screen, switched
// to/from via lv_scr_load(), rather than hiding/showing individual dashboard widgets.
static void buildClockUi() {
  scr_clock = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_clock, COLOR_BLACK, 0);
  lv_obj_set_style_bg_opa(scr_clock, LV_OPA_COVER, 0);

  label_clock_time = lv_label_create(scr_clock);
  lv_label_set_text(label_clock_time, "--:--");
  lv_obj_set_style_text_font(label_clock_time, &lv_font_montserrat_72_digits, 0);
  lv_obj_set_style_text_color(label_clock_time, COLOR_WHITE, 0);
  lv_obj_center(label_clock_time);

  lv_obj_t* subLabel = makeLabel(scr_clock, 0, 0, &lv_font_montserrat_12, COLOR_GRAY, "SCANNING FOR PINECIL...");
  lv_obj_set_style_text_letter_space(subLabel, 1, 0);
  lv_obj_align(subLabel, LV_ALIGN_BOTTOM_MID, 0, -12);
}

// Shown only while the WiFiManager captive portal is open (see onConfigPortalStart()) -
// static instructions, since nothing else is happening on this screen while it's up.
static void buildWifiConfigUi() {
  scr_wifi_config = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_wifi_config, COLOR_BLACK, 0);
  lv_obj_set_style_bg_opa(scr_wifi_config, LV_OPA_COVER, 0);

  lv_obj_t* title = makeLabel(scr_wifi_config, 0, 0, &lv_font_montserrat_20, COLOR_WHITE, "WIFI SETUP");
  lv_obj_align(title, LV_ALIGN_CENTER, 0, -40);

  lv_obj_t* joinLabel = makeLabel(scr_wifi_config, 0, 0, &lv_font_montserrat_14, COLOR_AMBER, "");
  lv_label_set_text_fmt(joinLabel, "Join WiFi: %s", WIFI_AP_NAME);
  lv_obj_align(joinLabel, LV_ALIGN_CENTER, 0, -5);

  lv_obj_t* urlLabel = makeLabel(scr_wifi_config, 0, 0, &lv_font_montserrat_12, COLOR_LIGHT_GRAY,
                                  "Then open http://192.168.4.1");
  lv_obj_align(urlLabel, LV_ALIGN_CENTER, 0, 25);
}

static UiState modeToUiState(uint32_t rawMode) {
  switch (rawMode) {
    case MODE_SOLDERING:
    case MODE_SOLDERING_PROFILE:
      return UI_SOLDERING;
    case MODE_SLEEPING:
    case MODE_HIBERNATING:
      return UI_SLEEP;
    case MODE_HOME_SCREEN:
      return UI_IDLE;
    default:
      // Unmapped OperatingMode value - log once so it can be identified later rather than
      // silently guessing what it should look like. Value 2 was the disputed "is it
      // BOOST?" case (see this file's header comment) - CONFIRMED via a live hardware test
      // on 2026-07-13 that it does NOT happen: engaging boost pushed live_temp to 426C
      // against a 350C setpoint while op_mode stayed at 1 (Soldering) the entire time. The
      // firmware control-flow trace was right; pynecil's BOOST=2 is stale for this
      // hardware/firmware. Kept as a named case below in case a future firmware version
      // changes this, but it's now a confirmed non-event, not an open question.
      static uint32_t lastLoggedUnknown = 0xFFFFFFFF;
      if (rawMode != lastLoggedUnknown) {
        if (rawMode == 2) {
          Serial.println("[UI] OperatingMode=2 seen live - this is the disputed BOOST value (pynecil says BOOST, firmware "
                          "control-flow trace suggested it's unreachable) - now confirmed reachable on real hardware. "
                          "Falling back to IDLE styling for now; worth wiring up a real BOOST UI state.");
        } else {
          Serial.printf("[UI] Unmapped OperatingMode value: %lu - falling back to IDLE styling\n", rawMode);
        }
        lastLoggedUnknown = rawMode;
      }
      return UI_IDLE;
  }
}

static void applyUiState(UiState state, float watts) {
  lv_color_t tipColor, modeColor, barColor;
  const char* modeText;

  switch (state) {
    case UI_SOLDERING:
      tipColor  = COLOR_EP_ORANGE;
      modeColor = COLOR_AMBER;
      modeText  = "SOLDER";
      barColor  = (watts >= HIGH_DRAW_THRESHOLD_W) ? COLOR_DEEP_ORANGE : COLOR_AMBER;
      break;
    case UI_SLEEP:
      tipColor  = COLOR_MISTY_BLUE;
      modeColor = COLOR_MISTY_BLUE;
      modeText  = "SLEEP";
      barColor  = COLOR_STEEL_BLUE;
      break;
    case UI_IDLE:
    default:
      tipColor  = COLOR_LIGHT_GRAY;
      modeColor = COLOR_LIGHT_GRAY;
      modeText  = "IDLE";
      barColor  = COLOR_AMBER;
      break;
  }

  lv_obj_set_style_text_color(label_tip_value, tipColor, 0);
  lv_obj_set_style_text_color(label_mode, modeColor, 0);
  lv_label_set_text(label_mode, modeText);
  lv_obj_set_style_bg_color(bar_power, barColor, LV_PART_INDICATOR);
}

class ClientCallbacks : public NimBLEClientCallbacks {
  void onDisconnect(NimBLEClient* c, int reason) override {
    connected      = false;
    pChrLiveTemp = pChrSetpoint = pChrDcInput = pChrHandleTemp = pChrOpMode = pChrEstWatts = nullptr;
    disconnectCount++;
    Serial.printf("[BLE] Disconnected, reason=%d, disconnect_count=%u, heap=%u - resuming scan\n", reason, disconnectCount,
                  ESP.getFreeHeap());
    lv_label_set_text_fmt(label_mode, "DISCONNECTED (x%u)", disconnectCount);
    lv_obj_set_style_text_color(label_mode, COLOR_GRAY, 0);
    NimBLEDevice::getScan()->start(0, false, true);
  }

  // Root-caused, 2026-07-13: NimBLEClientCallbacks' default here returns true (auto-accept
  // any peer-proposed connection parameter update). The Pinecil renegotiates to a much
  // slower interval a few seconds into every connection - every subsequent GATT read then
  // costs a full connection interval regardless of how simple the read is (measured:
  // ~20-30ms/read before this renegotiation, ~85-103ms/read after - not a rendering or
  // BLE-stack overhead difference, just a slower interval). Rejecting keeps our own
  // setConnectionParams(12, 12, ...) (15ms) in effect for the rest of the connection.
  bool onConnParamsUpdateRequest(NimBLEClient* pClient, const ble_gap_upd_params* params) override {
    Serial.printf("[BLE] Rejecting peer connection-param update request (itvl %u-%u x1.25ms) - keeping our fast interval\n",
                  params->itvl_min, params->itvl_max);
    return false;
  }
} clientCallbacks;

class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (dev->isAdvertisingService(NimBLEUUID(SERVICE_BULK_DATA))) {
      Serial.printf("[BLE] Found Pinecil: %s\n", dev->toString().c_str());
      NimBLEDevice::getScan()->stop();
      advDevice = dev;
      doConnect = true;
    }
  }

  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    if (!doConnect) {
      NimBLEDevice::getScan()->start(0, false, true);
    }
  }
} scanCallbacks;

static bool connectToPinecil() {
  if (!pClient) {
    pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(&clientCallbacks, false);
    pClient->setConnectionParams(12, 12, 0, 150);
    pClient->setConnectTimeout(5000);
  }

  if (!pClient->connect(advDevice)) {
    Serial.println("[BLE] connect() failed");
    return false;
  }

  Serial.printf("[BLE] Connected to %s, RSSI=%d\n", pClient->getPeerAddress().toString().c_str(), pClient->getRssi());

  NimBLERemoteService* pSvc = pClient->getService(NimBLEUUID(SERVICE_LIVE_DATA));
  if (!pSvc) {
    Serial.println("[BLE] Live Data service not found - disconnecting");
    pClient->disconnect();
    return false;
  }

  pChrLiveTemp   = pSvc->getCharacteristic(NimBLEUUID(CHAR_LIVE_TEMP));
  pChrSetpoint   = pSvc->getCharacteristic(NimBLEUUID(CHAR_SETPOINT_TEMP));
  pChrDcInput    = pSvc->getCharacteristic(NimBLEUUID(CHAR_DC_INPUT));
  pChrHandleTemp = pSvc->getCharacteristic(NimBLEUUID(CHAR_HANDLE_TEMP));
  pChrOpMode     = pSvc->getCharacteristic(NimBLEUUID(CHAR_OP_MODE));
  pChrEstWatts   = pSvc->getCharacteristic(NimBLEUUID(CHAR_EST_WATTS));

  if (!pChrLiveTemp || !pChrSetpoint || !pChrDcInput || !pChrHandleTemp || !pChrOpMode || !pChrEstWatts) {
    Serial.println("[BLE] One or more Live Data characteristics not found - disconnecting");
    pClient->disconnect();
    return false;
  }

  // label_mode is left as-is here ("SCANNING..." or a stale mode/temp from before a prior
  // disconnect) - the mode label only reflects the real mode once its own turn comes up in
  // the round-robin cycle (see loop()), and a real tip temp appearing is itself the
  // connected signal now (see file header) - no need to also flip this label immediately here.
  connected = true;
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  esp_reset_reason_t reason = esp_reset_reason();
  Serial.printf("\n\n[BOOT] PineCYD Fase 2 - Ember design port\n");
  Serial.printf("[BOOT] Reset reason: %s\n", resetReasonToStr(reason));
  Serial.printf("[BOOT] Free heap at boot: %u bytes\n", ESP.getFreeHeap());

  tft.init();
  tft.setRotation(2); // confirmed correct for this unit on real hardware - see README.md
  tft.fillScreen(TFT_BLACK);
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  lv_init();
  lv_disp_draw_buf_init(&draw_buf, buf1, nullptr, SCREEN_W * 20);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res  = SCREEN_W;
  disp_drv.ver_res  = SCREEN_H;
  disp_drv.flush_cb = lvglDispFlush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  buildUi();
  buildClockUi();
  buildWifiConfigUi();
  lv_scr_load(scr_clock); // not connected to a Pinecil yet - start on the clock
  clockScreenActive = true;
  lv_timer_handler();

  Serial.printf("[BOOT] Free heap after display+LVGL init: %u bytes\n", ESP.getFreeHeap());

  // WiFi + NTP (experimental, see file header) - kicked off once at boot, then left running
  // continuously alongside BLE, toggled off only while a Pinecil is connected (see loop()).
  // Credentials are no longer compile-time (see the removed secrets.h) - see the WiFi
  // captive-portal block above (wm, connectWifiNow()) for how this connects.
  pinMode(WIFI_CONFIG_BUTTON_PIN, INPUT_PULLUP);
  bool bootButtonHeld = (digitalRead(WIFI_CONFIG_BUTTON_PIN) == LOW);

  wm.setConfigPortalBlocking(false); // critical: keeps BLE scanning/LVGL alive during setup too
  wm.setConnectTimeout(WIFI_CONNECT_TIMEOUT_S);
  wm.setConfigPortalTimeout(WIFI_RETRY_INTERVAL_MS / 1000);
  wm.setAPCallback(onConfigPortalStart);

  if (bootButtonHeld) {
    Serial.println("[BOOT] BOOT button held at boot - resetting stored WiFi config and forcing the portal");
    wm.resetSettings();
  }

  connectWifiNow(); // returns almost immediately either way; loop() drives the rest
  Serial.println("[BOOT] WiFi connect/portal kicked off (non-blocking) - see loop() for progress");
  Serial.printf("[BOOT] Free heap after WiFi init: %u bytes\n", ESP.getFreeHeap());

  NimBLEDevice::init("PineCYD-Fase2");
  NimBLEDevice::setPower(3);

  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setScanCallbacks(&scanCallbacks, false);
  pScan->setInterval(100);
  pScan->setWindow(100);
  pScan->setActiveScan(true);
  // Prevents NimBLE from permanently caching every unique advertiser it ever sees (up to
  // 255) - we only use the live onResult() callback, never getResults(). An unattended
  // overnight run without this found a ~6KB/hour heap decline while idle-scanning - see
  // README.md for the full root-cause writeup.
  pScan->setMaxResults(0);
  pScan->start(0, false);

  Serial.println("[BOOT] Scanning for Pinecil...");
}

void loop() {
  lv_timer_handler();

  if (doConnect) {
    doConnect = false;
    if (!connectToPinecil()) {
      Serial.println("[BLE] Connect attempt failed, resuming scan");
      NimBLEDevice::getScan()->start(0, false, true);
    }
  }

  // Switch screens on the connected/disconnected transition (see buildClockUi()), and
  // toggle WiFi with it: off while a Pinecil is connected (recovers the ~83KB heap cost
  // and removes the WiFi/BT coexistence contention that was measurably slowing down BLE
  // polling - see README.md), back on only while showing the clock, which is the only
  // thing that needs it. At the user's suggestion, 2026-07-13.
  static uint32_t lastWifiAttempt = 0; // shared with the retry block below
  bool            shouldShowClock = !connected;
  if (shouldShowClock != clockScreenActive) {
    clockScreenActive = shouldShowClock;
    lv_scr_load(clockScreenActive ? scr_clock : scr_dashboard);
    if (clockScreenActive) {
      Serial.println("[WiFi] Pinecil disconnected - turning WiFi back on for the clock");
      connectWifiNow();
      lastWifiAttempt = millis();
    } else {
      Serial.println("[WiFi] Pinecil connected - turning WiFi off to free heap/radio time");
      // Don't leave the AP+webserver running while intentionally powering the radio off.
      if (wm.getConfigPortalActive()) wm.stopConfigPortal();
      wifiConfigActive = false;
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
    }
  }

  // Services the non-blocking captive portal (near-zero cost when none is open - same
  // shape as lv_timer_handler() above), and drives the on-screen switch back to the clock
  // once the portal closes (saved successfully, or its own timeout - see setup()).
  if (clockScreenActive) {
    wm.process();

    // onConfigPortalStart()'s stop() call is a single synchronous attempt made right as
    // WiFi switches into AP_STA mode - confirmed live, 2026-07-13, that NimBLE's host task
    // can be mid-scan-window right then and reject it ("Failed to cancel scan; rc=30"),
    // silently leaving the scan running for the portal's whole lifetime (still 100% duty
    // cycle - see setup()) even though the code intended to pause it. Retrying here every
    // loop() tick while the portal is active is far more robust than a one-shot call.
    if (wifiConfigActive) {
      NimBLEScan* pScan = NimBLEDevice::getScan();
      if (pScan->isScanning()) pScan->stop();
    }

    if (wifiConfigActive && !wm.getConfigPortalActive()) {
      wifiConfigActive = false;
      lv_scr_load(scr_clock);
      // Resume the Pinecil scan paused above - safe unconditionally here: clockScreenActive
      // can only be true if no Pinecil is connected yet (a connect would have already
      // flipped it false above), so scanning was never needed to stay off.
      Serial.println("[WiFi] Config portal closed - resuming Pinecil scan");
      NimBLEDevice::getScan()->start(0, false, true);
    }
  }

  // NTP only needs (re)configuring once per WiFi (re)connection, detected here rather than
  // blocking loop() while (re)connecting the way setup()'s initial connect does. Also where
  // credentials get cached in RAM for connectWifiNow()'s fast path (see g_wifiSsid/g_wifiPsk).
  static bool wifiWasConnected = false;
  bool        wifiNowConnected = (WiFi.status() == WL_CONNECTED);
  if (wifiNowConnected && !wifiWasConnected) {
    Serial.printf("[WiFi] Connected, IP=%s - configuring NTP\n", WiFi.localIP().toString().c_str());
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.google.com");
    if (g_wifiSsid.isEmpty()) {
      g_wifiSsid = WiFi.SSID();
      g_wifiPsk  = WiFi.psk();
      WiFi.persistent(false); // see g_wifiSsid/g_wifiPsk comment - avoids a flash write on
                              // every one of this project's frequent on/off-toggle reconnects
      Serial.println("[WiFi] Cached credentials in RAM for future reconnects");
    }
  }
  wifiWasConnected = wifiNowConnected;

  // Retry cadence per the user's spec, 2026-07-13: if we're not connected and no portal is
  // open (i.e. a prior attempt's 30s connect timeout and, if triggered, its 120s portal
  // window both ran out unconfigured), try the whole thing again every 2 minutes rather
  // than giving up for the rest of the session.
  if (clockScreenActive && !wifiNowConnected && !wm.getConfigPortalActive() &&
      millis() - lastWifiAttempt >= WIFI_RETRY_INTERVAL_MS) {
    lastWifiAttempt = millis();
    Serial.println("[WiFi] Not connected, no portal open - retrying");
    connectWifiNow();
  }

  static uint32_t lastClockUpdate = 0;
  if (clockScreenActive && millis() - lastClockUpdate >= 1000) {
    lastClockUpdate = millis();
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 100)) {
      lv_label_set_text_fmt(label_clock_time, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
    }
  }

  // Poll cycle: tip_temp every time, plus exactly one of the other five characteristics
  // in round-robin, instead of a fast tip-only poll alongside a periodic 6-read "full"
  // burst. At the user's suggestion, 2026-07-13: the full-cycle burst (even after
  // connection-interval and chart-cost fixes - see README.md) was still visible as a
  // periodic stutter in an otherwise-smooth tip-temp update rate, since it briefly
  // monopolized the connection for 5 extra reads every cycle. Spreading those 5 reads one
  // at a time across five consecutive cycles keeps every cycle roughly the same size (two
  // reads), so there's no more periodic burst to stutter on. The five secondary values are
  // individually a bit slower to refresh now (~5 cycles apart instead of every cycle), but
  // none of them need tip-temp's responsiveness.
  enum SecondaryChar { SEC_SETPOINT, SEC_DC_INPUT, SEC_HANDLE_TEMP, SEC_OP_MODE, SEC_EST_WATTS, SEC_COUNT };
  static uint32_t lastCycle    = 0;
  static int      secIdx       = 0;
  static uint32_t g_setpoint   = 0;
  static uint32_t g_dcInputDv  = 0;
  static uint32_t g_handleTemp = 0;
  static uint32_t g_opMode     = 0;
  static uint32_t g_estWattsDw = 0;
  bool haveAllChars = pChrLiveTemp && pChrSetpoint && pChrDcInput && pChrHandleTemp && pChrOpMode && pChrEstWatts;

  if (connected && haveAllChars && millis() - lastCycle >= TIP_POLL_INTERVAL_MS) {
    lastCycle = millis();

    uint32_t t0 = micros();
    bool     okTip;
    uint32_t liveTemp = readU32LE(pChrLiveTemp, okTip);

    NimBLERemoteCharacteristic* secChr = nullptr;
    switch (secIdx) {
      case SEC_SETPOINT:    secChr = pChrSetpoint; break;
      case SEC_DC_INPUT:    secChr = pChrDcInput; break;
      case SEC_HANDLE_TEMP: secChr = pChrHandleTemp; break;
      case SEC_OP_MODE:     secChr = pChrOpMode; break;
      case SEC_EST_WATTS:   secChr = pChrEstWatts; break;
    }
    bool     okSec;
    uint32_t secVal  = readU32LE(secChr, okSec);
    uint32_t cycleUs = micros() - t0;

    if (okTip) {
      lv_label_set_text_fmt(label_tip_value, "%lu", liveTemp);
    }
    if (okSec) {
      switch (secIdx) {
        case SEC_SETPOINT:    g_setpoint   = secVal; break;
        case SEC_DC_INPUT:    g_dcInputDv  = secVal; break;
        case SEC_HANDLE_TEMP: g_handleTemp = secVal; break;
        case SEC_OP_MODE:     g_opMode     = secVal; break;
        case SEC_EST_WATTS:   g_estWattsDw = secVal; break;
      }
    }
    Serial.printf("%lu,live_temp=%lu,secondary_idx=%d,secondary_val=%lu,cycle_ms=%.1f,heap=%u\n", millis(), liveTemp, secIdx,
                  secVal, cycleUs / 1000.0, ESP.getFreeHeap());
    secIdx = (secIdx + 1) % SEC_COUNT;

    // Re-render the derived labels every cycle from the cached values above (cheap - no
    // BLE cost, just formatting), even though only one of them was freshly read this
    // cycle - keeps the dashboard internally consistent regardless of which value is
    // "newest" at any given moment.
    float watts    = g_estWattsDw / 10.0f;
    float wattsPct = (watts / ASSUMED_MAX_WATTS) * 100.0f;
    if (wattsPct > 100.0f) wattsPct = 100.0f;
    if (wattsPct < 0.0f) wattsPct = 0.0f;

    // lv_label_set_text_fmt() goes through LVGL's own lightweight snprintf, which has
    // LV_SPRINTF_USE_FLOAT=0 by default (confirmed in lv_conf.h) - %f silently produces
    // garbage ("fW", a missing-glyph box, "fV" - seen live on real hardware, 2026-07-13).
    // Serial.printf() above is unaffected (that's the real newlib printf) - only LVGL
    // label text is. Fixed by formatting watts/volts as separate integer parts instead.
    uint32_t wattsWhole  = (g_estWattsDw + 5) / 10; // decwatts -> whole watts, rounded
    uint32_t voltsWhole  = g_dcInputDv / 10;
    uint32_t voltsTenths = g_dcInputDv % 10;
    // Also empirically corrected, 2026-07-13: IronOS's current source
    // (BSP.cpp's NTCHandleLookup table) claims getHandleTemperature() returns plain
    // degC, but real hardware reported handle_temp=406-411 while actively soldering -
    // physically impossible for a handle (would mean the plastic is melting), and
    // exactly plausible as 40.6-41.1 C if this value is actually x10-scaled on this
    // iron's actual firmware version (which may differ from IronOS's current source).
    // Trusting the live reading's physical plausibility over the source comment.
    uint32_t handleTempC = g_handleTemp / 10;

    lv_label_set_text_fmt(label_setpoint_value, "%lu\xC2\xB0", g_setpoint);
    lv_label_set_text_fmt(label_power, "%luW \xE2\x80\xA2 %lu.%luV", wattsWhole, voltsWhole, voltsTenths);
    lv_label_set_text_fmt(label_handle, "HANDLE %lu\xC2\xB0" "C", handleTempC);
    lv_bar_set_value(bar_power, (int32_t)wattsPct, LV_ANIM_OFF);

    applyUiState(modeToUiState(g_opMode), watts);

    // Decoupled from the per-cycle reads above, 2026-07-13: the chart redraw is the
    // expensive part of any cycle that touches it - pushing to it much less often than
    // the labels update keeps that cost off most cycles.
    static uint32_t lastChartPush = 0;
    if (okTip && millis() - lastChartPush >= CHART_PUSH_INTERVAL_MS) {
      lastChartPush = millis();
      lv_chart_set_next_value(chart_history, series_temp, (lv_coord_t)liveTemp);
      // Real scrolling history, not a flat line at the current value - see file header.
      lv_chart_set_next_value(chart_history, series_setpoint_ref, (lv_coord_t)g_setpoint);
    }
  }

  static uint32_t lastHeartbeat = 0;
  if (millis() - lastHeartbeat > 10000) {
    lastHeartbeat = millis();
    Serial.printf("[HEARTBEAT] uptime_ms=%lu connected=%d disconnects=%u heap=%u min_heap=%u wifi=%d rssi=%d\n", millis(),
                  connected, disconnectCount, ESP.getFreeHeap(), ESP.getMinFreeHeap(), WiFi.status() == WL_CONNECTED,
                  WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  }

  delay(5);
}
