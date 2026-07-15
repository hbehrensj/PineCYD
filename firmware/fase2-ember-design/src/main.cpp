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
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ArduinoJson.h>
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

// --- Ember Usage palette (2026-07-15) - Claude usage clock-screen zone. Distinct exact hex
// values from a handoff doc originally written for a sibling project (DeskDash), close to
// but not identical to the dashboard palette above (e.g. ember-core #FF6A2B vs. this file's
// own ep_orange #F37320) - kept as separate named constants for 1:1 fidelity to that doc
// rather than reusing the nearest existing token. Screen background reuses COLOR_BLACK
// (#0D0D0D) instead of the doc's ember-bg (#14100C) - close enough to not be worth a second
// near-duplicate black, and keeps the clock screen consistent with the dashboard screen.
static const lv_color_t COLOR_USAGE_SURFACE = LV_COLOR_MAKE(0x24, 0x1C, 0x14);
static const lv_color_t COLOR_USAGE_CORE    = LV_COLOR_MAKE(0xFF, 0x6A, 0x2B);
static const lv_color_t COLOR_USAGE_HOT     = LV_COLOR_MAKE(0xFF, 0xB0, 0x3A);
static const lv_color_t COLOR_USAGE_ALERT   = LV_COLOR_MAKE(0xFF, 0x3B, 0x2F);
static const lv_color_t COLOR_USAGE_ASH     = LV_COLOR_MAKE(0x8A, 0x7A, 0x6A);
static const lv_color_t COLOR_USAGE_TEXT    = LV_COLOR_MAKE(0xF2, 0xE7, 0xD8);

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
static lv_obj_t* label_clock_date;
static bool      clockScreenActive = false; // set explicitly once at boot, see setup()

// Claude usage zone (2026-07-15) - see buildClockUi()/updateUsageZoneDisplay(). Fixed at 3
// rows (five_hour/seven_day/seven_day_sonnet - matches the real API shape) rather than the
// handoff doc's general 2-4-row system.
static lv_obj_t* usageZone;
static lv_obj_t* usageRowLabel[3];
static lv_obj_t* usageTrack[3];
static lv_obj_t* usageFill[3];
static lv_obj_t* usageMarker[3];
static lv_obj_t* usagePct[3];
static lv_obj_t* usageReset[3];
static lv_obj_t* usageStaleTag;

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

// Timezone (2026-07-13) - was hardcoded to Copenhagen's POSIX TZ string, silently wrong for
// anyone flashing this outside CET/CEST (a real gap, since this project now ships prebuilt
// binaries for anyone - see README.md). Configurable via the same WiFi captive portal,
// persisted in NVS via Preferences - same pattern a sibling project (TDAI-2170) uses for its
// own portal-collected settings (MQTT host/user/pass).
static const char*      DEFAULT_TZ = "CET-1CEST,M3.5.0,M10.5.0/3"; // Copenhagen - this project's own default
static String            g_timezone = DEFAULT_TZ;
static Preferences        prefs;
static WiFiManagerParameter* pTzParam = nullptr; // registered in setup(), read in onWifiConfigSaved()

// Claude usage clock-screen widget (2026-07-15) - user's own explicit choice, after being
// offered a safer alternative (a local bridge script holding the token, device only ever
// sees derived percentages), to instead enter the OAuth token directly on-device via this
// same portal, matching how WiFi credentials/timezone already work. Real risk accepted
// knowingly: this is a live OAuth access token for the user's own Anthropic account,
// against an undocumented endpoint the device can't refresh on its own - see README.md for
// the full tradeoff writeup and the expiry caveat.
static String                g_claudeToken;
static WiFiManagerParameter* pTokenParam = nullptr; // registered in setup(), read below

// Fires when the portal's form is submitted (WiFiManager's own save-config hook) -
// independent of whether the WiFi connect attempt that follows succeeds, so this also
// captures these fields on a portal session that's only being used to switch networks.
// Blank input means "leave unchanged" for both fields (matches TDAI's own optional-field
// pattern) - the timezone field is pre-filled with its current value so blank only happens
// if deliberately cleared; the token field is deliberately left BLANK by default (not
// pre-filled with the stored token) so the live secret is never re-emitted into a served
// HTML page's source just to show the user what they'd already saved.
static void onWifiConfigSaved() {
  if (pTzParam && strlen(pTzParam->getValue()) > 0) {
    g_timezone = pTzParam->getValue();
    prefs.begin("pinecyd", false);
    prefs.putString("tz", g_timezone);
    prefs.end();
    Serial.printf("[WiFi] Timezone saved: %s\n", g_timezone.c_str());
  }
  if (pTokenParam && strlen(pTokenParam->getValue()) > 0) {
    g_claudeToken = pTokenParam->getValue();
    prefs.begin("pinecyd", false);
    prefs.putString("claude_tok", g_claudeToken);
    prefs.end();
    Serial.println("[WiFi] Claude usage token saved");
  }
}

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

// --- OTA updates over WiFi (2026-07-13) ---
// Added so a firmware update doesn't require USB - `pio run -e ota -t upload` (see
// platformio.ini) pushes a build to pinecyd.local over the network instead. Needs the
// two-slot partition table (partitions_ota.csv, see platformio.ini) - huge_app.csv's single
// "ota_0"-labeled slot could not have safely supported this (see that file's own comment).
static const char* OTA_HOSTNAME = "pinecyd";
// Same-LAN-only credential, not a secret worth a NVS/portal flow over - matches a sibling
// project's (TDAI-2170) own established pattern of a hardcoded, overridable default rather
// than plumbing this through the WiFi captive portal too. Override per-build with
// `-D OTA_PASSWORD=\"...\"` if you want your own.
#ifndef OTA_PASSWORD
#define OTA_PASSWORD "pinecyd-ota"
#endif

static bool otaStarted    = false;
static bool otaInProgress = false; // gates the WiFi on/off toggle in loop() - see there

static void otaBegin() {
  if (otaStarted) return;
  if (WiFi.status() != WL_CONNECTED) return;

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);

  ArduinoOTA.onStart([]() {
    otaInProgress = true;
    Serial.println("[OTA] Update starting");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("[OTA] Update complete, rebooting");
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    Serial.printf("[OTA] %u%%\r", total ? (done * 100 / total) : 0);
  });
  ArduinoOTA.onError([](ota_error_t err) {
    otaInProgress = false; // upload aborted - let the WiFi on/off toggle resume normally
    Serial.printf("[OTA] Error %u\n", err);
  });

  ArduinoOTA.begin();
  otaStarted = true;
  Serial.printf("[OTA] Ready: %s.local (espota), hostname=%s\n", OTA_HOSTNAME, OTA_HOSTNAME);
}

// --- Always-on settings page (2026-07-15) ---
// The WiFiManager portal only exists for a few seconds at a time (auto-triggered on
// connect failure, or forced by holding BOOT) - fine for one-time WiFi setup, but the
// Claude usage token is expected to need periodic re-entry (it isn't refreshed - see
// README.md), and going through the BOOT-hold/portal dance every time is real friction for
// something you might do every few hours. This is a small always-on page at
// http://pinecyd.local/, reachable during normal WiFi-connected operation (same on/off
// lifecycle as ArduinoOTA/mDNS above - started on WiFi connect, torn down on WiFi off) -
// no separate portal/AP needed to update the token or timezone.
static WebServer settingsServer(80);
static bool       settingsServerStarted = false;
static void        fetchClaudeUsage(); // defined below; forward-declared for handleSettingsSave()
// Set by handleSettingsSave() to trigger an early fetch from loop() instead of calling
// fetchClaudeUsage() synchronously from the HTTP handler itself - that blocked the
// browser's Save request for the fetch's own up-to-16s HTTPS timeout budget, confirmed
// live, 2026-07-15 (the page just hung on "Waiting for pinecyd.local...").
static bool g_forceUsageFetch = false;

// Explicit "Connection: close" on every response below - the ESP32 WebServer library is
// synchronous/single-connection and handles HTTP keep-alive poorly; a browser reload often
// fires more than one request close together (the page plus a favicon.ico fetch, at least)
// and without this, the server can end up stuck waiting on a connection the browser thinks
// it can reuse - confirmed live, 2026-07-15, as "the page hangs on reload."
static void handleSettingsRoot() {
  String html =
      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width, initial-scale=1'>"
      "<title>PineCYD settings</title></head><body style='font-family:sans-serif; max-width:420px; margin:2em auto;'>"
      "<h3>PineCYD settings</h3>"
      "<form method='POST' action='/save'>"
      "<label>Claude usage token (blank = keep existing):</label><br>"
      "<input type='password' name='token' style='width:100%; box-sizing:border-box;'><br><br>"
      "<label>Timezone (POSIX TZ string):</label><br>"
      "<input type='text' name='tz' value='" +
      g_timezone +
      "' style='width:100%; box-sizing:border-box;'><br><br>"
      "<input type='submit' value='Save'>"
      "</form></body></html>";
  settingsServer.sendHeader("Connection", "close");
  settingsServer.send(200, "text/html", html);
}

static void handleSettingsNotFound() {
  settingsServer.sendHeader("Connection", "close");
  settingsServer.send(404, "text/plain", "Not found");
}

// Same blank-means-unchanged pattern as the portal's own onWifiConfigSaved() - see that
// function's comment for why the token field is never pre-filled with its current value.
static void handleSettingsSave() {
  if (settingsServer.hasArg("token") && settingsServer.arg("token").length() > 0) {
    g_claudeToken = settingsServer.arg("token");
    prefs.begin("pinecyd", false);
    prefs.putString("claude_tok", g_claudeToken);
    prefs.end();
    Serial.println("[Settings] Claude usage token updated via web page");
    g_forceUsageFetch = true; // loop() fetches on its next tick - see that flag's comment
  }
  if (settingsServer.hasArg("tz") && settingsServer.arg("tz").length() > 0) {
    g_timezone = settingsServer.arg("tz");
    prefs.begin("pinecyd", false);
    prefs.putString("tz", g_timezone);
    prefs.end();
    configTzTime(g_timezone.c_str(), "pool.ntp.org", "time.google.com");
    Serial.println("[Settings] Timezone updated via web page");
  }
  settingsServer.sendHeader("Location", "/");
  settingsServer.sendHeader("Connection", "close");
  settingsServer.send(303);
}

static void settingsServerBegin() {
  if (settingsServerStarted) return;
  if (WiFi.status() != WL_CONNECTED) return;
  settingsServer.on("/", HTTP_GET, handleSettingsRoot);
  settingsServer.on("/save", HTTP_POST, handleSettingsSave);
  settingsServer.onNotFound(handleSettingsNotFound); // e.g. the browser's automatic favicon.ico request
  settingsServer.begin();
  settingsServerStarted = true;
  Serial.printf("[Settings] Web settings page ready: http://%s.local/\n", OTA_HOSTNAME);
}

// --- Claude usage clock-screen widget (2026-07-15) ---
// Ported from a handoff doc originally written for a sibling project's ESPHome/MQTT stack
// (see README.md's "Claude usage zone" section) - this is the direct-fetch equivalent: the
// device itself polls api.anthropic.com's undocumented usage endpoint using a user-supplied
// OAuth token (see the WiFiManagerParameter above), rather than a local bridge script. The
// endpoint shape, the "5T"/"UGE"/"SON" labels, and the pace-marker/stale-state logic are
// taken directly from that doc.
struct UsageLine {
  const char* label;
  uint32_t    periodSeconds; // known window length - the API gives resets_at but not the
                              // window's start, so period_pct (pace marker position) is
                              // derived from this + resets_at + now, not read from the API.
  float       pct;           // last-fetched utilization, 0-100
  time_t      resetsAt;      // last-fetched reset time, epoch seconds (UTC)
  bool        haveData;      // false until the first successful fetch
};

// 60s -> 5min, 2026-07-15: each fetch's TLS handshake needs ~32KB of heap (see
// fetchClaudeUsage()'s comment) that can't be reduced on this core - the only lever left is
// how often it's attempted at all. 5-minute-stale usage data is a fine tradeoff; a
// heap-exhaustion crash is not.
static const uint32_t USAGE_FETCH_INTERVAL_MS = 5 * 60 * 1000;
static const uint32_t USAGE_STALE_AFTER_MS    = 15 * 60 * 1000; // 15 min, per the handoff doc's stale rule

static UsageLine g_usageLines[3] = {
  {"5T",  5 * 3600UL,  0.0f, 0, false},
  {"UGE", 7 * 86400UL, 0.0f, 0, false},
  {"SON", 7 * 86400UL, 0.0f, 0, false},
};
static uint32_t g_lastUsageFetchOkMs = 0; // 0 = never succeeded yet

// UTC civil-date -> epoch-seconds, no libc dependency: this toolchain doesn't provide
// timegm() (confirmed by a real build failure, 2026-07-15), and mktime() is the wrong tool
// anyway (applies local-timezone rules; these timestamps are always Z-suffixed UTC).
// Howard Hinnant's "days_from_civil" algorithm - portable, correct across the whole
// proleptic Gregorian calendar including leap years.
static time_t utcTmToEpoch(int year, int mon /* 1-12 */, int day, int hour, int min, int sec) {
  int      y   = year - (mon <= 2 ? 1 : 0);
  long     era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);                                   // [0, 399]
  unsigned doy = (153 * (mon + (mon > 2 ? -3 : 9)) + 2) / 5 + day - 1;         // [0, 365]
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                       // [0, 146096]
  long     days = era * 146097L + (long)doe - 719468L;                       // since 1970-01-01
  return (time_t)days * 86400L + hour * 3600L + min * 60L + sec;
}

// Minimal ISO8601 UTC parser ("2026-02-28T17:00:00Z") - the API only ever returns this exact
// shape (per the sample payload), so a full RFC3339 parser is out of scope.
static time_t parseIso8601Utc(const char* s) {
  int y, mo, d, h, mi, se;
  if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
  return utcTmToEpoch(y, mo, d, h, mi, se);
}

// Fetches usage and updates g_usageLines in place. Runs on its own schedule (see loop())
// while WiFi is on; a failure just leaves the last-known values in place - staleness is
// tracked separately via g_lastUsageFetchOkMs (last *successful* fetch), not fetch attempts,
// matching the handoff doc's ">15min since last update" stale rule.
static void fetchClaudeUsage() {
  if (g_claudeToken.isEmpty() || WiFi.status() != WL_CONNECTED) return;

  // Confirmed live, 2026-07-15: this handshake's default mbedTLS RX/TX buffers (16KB each,
  // baked into the prebuilt framework library - not runtime-configurable on this core;
  // NetworkClientSecure has no setBufferSizes() here, unlike older ESP32 Arduino cores) are
  // a real problem on this no-PSRAM board once WiFiManager+OTA+mDNS+the settings webserver
  // are also resident: min_heap dropped to 5780 bytes and the handshake itself started
  // failing ("X509 - Allocation of memory failed", HTTP -1/"connection refused"). Can't
  // shrink the buffers without rebuilding ESP-IDF itself (out of scope tonight) - the lever
  // actually available is frequency: USAGE_FETCH_INTERVAL_MS below controls how often this
  // ~32KB-hungry handshake runs at all, which is what got tuned instead.
  WiFiClientSecure client;
  client.setInsecure(); // no cert pinning - matches this project's OTA self-update precedent
                        // (TDAI-2170's selfupdate.cpp does the same for its GitHub HTTPS calls)
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(8000);
  if (!http.begin(client, "https://api.anthropic.com/api/oauth/usage")) {
    Serial.println("[Usage] http.begin() failed");
    return;
  }
  http.addHeader("Authorization", String("Bearer ") + g_claudeToken);
  http.addHeader("anthropic-beta", "oauth-2025-04-20");

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[Usage] HTTP %d\n", code);
    http.end();
    return;
  }

  JsonDocument          doc;
  DeserializationError  err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    Serial.printf("[Usage] JSON parse error: %s\n", err.c_str());
    return;
  }

  static const char* KEYS[3] = {"five_hour", "seven_day", "seven_day_sonnet"};
  bool                anyOk  = false;
  for (int i = 0; i < 3; i++) {
    JsonVariantConst bucket = doc[KEYS[i]];
    if (bucket.isNull()) continue;
    float       util         = bucket["utilization"] | -1.0f;
    const char* resetsAtStr  = bucket["resets_at"] | "";
    if (util < 0 || !resetsAtStr[0]) continue;
    time_t resetsAt = parseIso8601Utc(resetsAtStr);
    if (resetsAt == 0) continue;
    g_usageLines[i].pct      = util * 100.0f;
    g_usageLines[i].resetsAt = resetsAt;
    g_usageLines[i].haveData = true;
    anyOk                    = true;
  }

  if (anyOk) {
    g_lastUsageFetchOkMs = millis();
    Serial.println("[Usage] Fetched OK");
  } else {
    Serial.println("[Usage] Response had no usable buckets");
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
//
// Layout below (clock top-anchored, date/usage-zone cascaded via lv_obj_align_to() rather
// than fixed y-coordinates) is a deliberate departure from this project's usual absolute-
// position style: the handoff doc this was ported from (see README.md) assumed a 60px
// clock font at a known y; this project's existing clock reuses its own already-verified
// 72px custom digit font (permitted by the doc's own Typography section - "round to
// whatever the existing bitmap font ladder already defines"), whose rendered height isn't
// precisely known without measuring on real hardware. Cascading alignment sidesteps that
// uncertainty entirely - each element sizes itself against its actual neighbor, not a
// guessed pixel offset. **Not yet visually confirmed on real hardware** - the device went
// offline before this could be flashed; row spacing/overall fit may need tuning once seen.
static void buildClockUi() {
  scr_clock = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(scr_clock, COLOR_BLACK, 0);
  lv_obj_set_style_bg_opa(scr_clock, LV_OPA_COVER, 0);

  // Fixed width (320, full screen) + centered text, on BOTH labels below - not just cosmetic.
  // Confirmed live, 2026-07-15: an auto-sized label's lv_obj_align() only centers its
  // box *once*, using whatever text is showing at that call - "--:--" (placeholder) and
  // "--" (empty date) at build time. Later lv_label_set_text() calls change the label's
  // content (and therefore its natural width) without ever re-running alignment, so the
  // box's *position* stays anchored to the old, now-wrong width - visible on the real
  // device as the date sitting off-center. Fixing width to the full screen and centering
  // text *within* that fixed box sidesteps the whole problem: the box never resizes, so a
  // one-time align is enough regardless of what text it ends up showing.
  label_clock_time = lv_label_create(scr_clock);
  lv_label_set_text(label_clock_time, "--:--");
  lv_obj_set_style_text_font(label_clock_time, &lv_font_montserrat_72_digits, 0);
  lv_obj_set_style_text_color(label_clock_time, COLOR_WHITE, 0);
  lv_obj_set_width(label_clock_time, 320);
  lv_obj_set_style_text_align(label_clock_time, LV_TEXT_ALIGN_CENTER, 0);
  // Pushed down from an initial y=2, 2026-07-15: real hardware showed the digits'
  // top edge overlapping the "SCANNING..."/"FORSINKET" corner labels at y=6-8.
  // 24 -> 44, 2026-07-15: real-hardware feedback was "the whole clock/date/bars group sits
  // too high, with unused space at the bottom" - the relative gaps between clock/date/first
  // bar were fine as-is, so only this top margin (which the align_to() cascade below
  // inherits) needed to move, not the individual gaps.
  lv_obj_align(label_clock_time, LV_ALIGN_TOP_MID, 0, 44);

  label_clock_date = lv_label_create(scr_clock);
  lv_label_set_text(label_clock_date, "");
  lv_obj_set_style_text_font(label_clock_date, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(label_clock_date, COLOR_USAGE_ASH, 0);
  lv_obj_set_width(label_clock_date, 320);
  lv_obj_set_style_text_align(label_clock_date, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align_to(label_clock_date, label_clock_time, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);

  lv_obj_t* subLabel = makeLabel(scr_clock, 4, 6, &lv_font_montserrat_12, COLOR_GRAY, "SCANNING...");
  lv_obj_set_style_text_letter_space(subLabel, 1, 0);

  // --- Claude usage zone (2026-07-15, see README.md) ---
  // Geometry (track/label/pct column widths, marker/fill logic) is a 1:1 translation of the
  // handoff doc's buildLine()/makeFrame() JS - see that doc for the full derivation. Row gap
  // (8px in the doc) tightened twice after real-hardware feedback (8->5->3) - the user
  // asked for the three bars to sit closer together than the doc's original spacing. Fixed
  // at 3 rows (5T/UGE/SON, matching g_usageLines) rather than the doc's general 2-4-row
  // system - the real API always returns exactly these three buckets.
  usageZone = lv_obj_create(scr_clock);
  lv_obj_set_style_bg_opa(usageZone, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(usageZone, 0, 0);
  lv_obj_set_style_pad_all(usageZone, 0, 0);
  lv_obj_clear_flag(usageZone, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(usageZone, 304, 102); // 3 rows * 32px + 2 gaps * 3px
  // Gap from the date (16) confirmed fine as-is on real hardware - only the group's overall
  // vertical position (label_clock_time's own top margin, above) needed to move, not this.
  lv_obj_align_to(usageZone, label_clock_date, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);

  static const int trackW = 200, labelW = 32, pctW = 40, rowH = 32, barH = 16, gap = 3;
  for (int i = 0; i < 3; i++) {
    int rowY = i * (rowH + gap);

    usageRowLabel[i] = makeLabel(usageZone, 0, rowY + 8, &lv_font_montserrat_12, COLOR_USAGE_ASH, g_usageLines[i].label);

    usageTrack[i] = lv_obj_create(usageZone);
    lv_obj_set_pos(usageTrack[i], labelW, rowY + 3);
    lv_obj_set_size(usageTrack[i], trackW, barH);
    lv_obj_set_style_radius(usageTrack[i], barH / 2, 0);
    lv_obj_set_style_bg_color(usageTrack[i], COLOR_USAGE_SURFACE, 0);
    lv_obj_set_style_bg_opa(usageTrack[i], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(usageTrack[i], 0, 0);
    lv_obj_set_style_pad_all(usageTrack[i], 0, 0);
    lv_obj_clear_flag(usageTrack[i], LV_OBJ_FLAG_SCROLLABLE);

    // Fill is a separate stacked lv_obj, not a plain lv_bar - per-row gradient end-color
    // needs to change (hot/alert/desaturated) depending on state, recomputed every refresh
    // in updateUsageZoneDisplay(), which a single shared lv_bar style can't do per-instance
    // as cheaply.
    usageFill[i] = lv_obj_create(usageZone);
    lv_obj_set_pos(usageFill[i], labelW, rowY + 3);
    lv_obj_set_size(usageFill[i], 2, barH);
    lv_obj_set_style_radius(usageFill[i], barH / 2, 0);
    lv_obj_set_style_bg_color(usageFill[i], COLOR_USAGE_CORE, 0);
    lv_obj_set_style_bg_grad_color(usageFill[i], COLOR_USAGE_HOT, 0);
    lv_obj_set_style_bg_grad_dir(usageFill[i], LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_bg_opa(usageFill[i], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(usageFill[i], 0, 0);
    lv_obj_set_style_pad_all(usageFill[i], 0, 0);
    lv_obj_clear_flag(usageFill[i], LV_OBJ_FLAG_SCROLLABLE);

    usageMarker[i] = lv_obj_create(usageZone);
    lv_obj_set_pos(usageMarker[i], labelW, rowY + 3 - 2);
    lv_obj_set_size(usageMarker[i], 2, barH + 4);
    lv_obj_set_style_radius(usageMarker[i], 1, 0);
    lv_obj_set_style_bg_color(usageMarker[i], COLOR_USAGE_ASH, 0);
    lv_obj_set_style_bg_opa(usageMarker[i], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(usageMarker[i], 0, 0);
    lv_obj_set_style_pad_all(usageMarker[i], 0, 0);
    lv_obj_clear_flag(usageMarker[i], LV_OBJ_FLAG_SCROLLABLE);

    usagePct[i] = makeLabel(usageZone, labelW + trackW + 4, rowY + 6, &lv_font_montserrat_14, COLOR_USAGE_TEXT, "--%");
    lv_obj_set_width(usagePct[i], pctW - 4);
    lv_obj_set_style_text_align(usagePct[i], LV_TEXT_ALIGN_RIGHT, 0);

    usageReset[i] = makeLabel(usageZone, labelW, rowY + 3 + barH + 2, &lv_font_montserrat_12, COLOR_USAGE_ASH, "");
    lv_obj_set_width(usageReset[i], trackW + pctW);
    lv_obj_set_style_text_align(usageReset[i], LV_TEXT_ALIGN_RIGHT, 0);
  }

  usageStaleTag = makeLabel(scr_clock, 0, 0, &lv_font_montserrat_12, COLOR_USAGE_ASH, "FORSINKET");
  lv_obj_set_style_text_letter_space(usageStaleTag, 1, 0);
  lv_obj_align(usageStaleTag, LV_ALIGN_TOP_RIGHT, -8, 8);
  lv_obj_add_flag(usageStaleTag, LV_OBJ_FLAG_HIDDEN);
}

// Recomputes and applies every usage-zone row's fill width/color, pace-marker position,
// pct text/color, and resets-in text - called once a second alongside the clock tick (see
// loop()) so the pace marker and countdown stay live between fetches, not just on a new
// fetch. period_pct (pace-marker position) is derived here from resetsAt + the known period
// length + now, not read from the API - see fetchClaudeUsage()'s UsageLine comment.
static void updateUsageZoneDisplay() {
  time_t now   = time(nullptr);
  bool   stale = (g_lastUsageFetchOkMs == 0) || (millis() - g_lastUsageFetchOkMs > USAGE_STALE_AFTER_MS);

  if (stale) {
    lv_obj_clear_flag(usageStaleTag, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(usageStaleTag, LV_OBJ_FLAG_HIDDEN);
  }

  static const int trackW = 200;
  for (int i = 0; i < 3; i++) {
    UsageLine& l = g_usageLines[i];
    if (!l.haveData) {
      lv_label_set_text(usagePct[i], "--%");
      lv_label_set_text(usageReset[i], "");
      lv_obj_set_width(usageFill[i], 2);
      lv_obj_set_style_bg_color(usageFill[i], COLOR_USAGE_SURFACE, 0);
      lv_obj_set_style_bg_grad_color(usageFill[i], COLOR_USAGE_SURFACE, 0);
      continue;
    }

    long secsLeft = (long)(l.resetsAt - now);
    if (secsLeft < 0) secsLeft = 0;
    float periodPct = 100.0f * (1.0f - (float)secsLeft / (float)l.periodSeconds);
    if (periodPct < 0.0f) periodPct = 0.0f;
    if (periodPct > 100.0f) periodPct = 100.0f;

    bool critical = l.pct >= 90.0f;
    bool depleted = l.pct >= 100.0f;
    bool overPace = !stale && l.pct > periodPct;

    int fillW = (int)(trackW * l.pct / 100.0f);
    if (fillW < 2) fillW = 2;
    if (fillW > trackW) fillW = trackW;
    lv_obj_set_width(usageFill[i], fillW);

    lv_color_t fillStart = stale ? COLOR_USAGE_SURFACE : COLOR_USAGE_CORE;
    lv_color_t fillEnd;
    if (stale) {
      fillEnd = COLOR_USAGE_ASH;
    } else if (critical || overPace) {
      fillEnd = COLOR_USAGE_ALERT;
    } else {
      fillEnd = COLOR_USAGE_HOT;
    }
    lv_obj_set_style_bg_color(usageFill[i], fillStart, 0);
    lv_obj_set_style_bg_grad_color(usageFill[i], fillEnd, 0);

    int markerX = (int)(trackW * periodPct / 100.0f) - 1;
    if (markerX < 0) markerX = 0;
    lv_obj_set_x(usageMarker[i], 32 + markerX); // 32 = labelW, see buildClockUi()

    lv_label_set_text_fmt(usagePct[i], "%d%%", (int)(l.pct + 0.5f));
    lv_obj_set_style_text_color(usagePct[i], (critical || depleted) ? COLOR_USAGE_ALERT : COLOR_USAGE_TEXT, 0);

    // Danish "Xd Yt" (days/hours) or "Xt Ym" (hours/minutes), matching the handoff doc's
    // "1t 51m" / "4d 9t" examples.
    char resetBuf[24];
    if (secsLeft >= 86400) {
      snprintf(resetBuf, sizeof(resetBuf), "%ldd %ldt", secsLeft / 86400, (secsLeft % 86400) / 3600);
    } else {
      snprintf(resetBuf, sizeof(resetBuf), "%ldt %ldm", secsLeft / 3600, (secsLeft % 3600) / 60);
    }
    lv_label_set_text(usageReset[i], resetBuf);
    bool depletedEmphasis = depleted && !stale;
    lv_obj_set_style_text_color(usageReset[i], depletedEmphasis ? COLOR_USAGE_TEXT : COLOR_USAGE_ASH, 0);
  }
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
  wm.setSaveConfigCallback(onWifiConfigSaved);

  prefs.begin("pinecyd", true);
  g_timezone    = prefs.getString("tz", DEFAULT_TZ);
  g_claudeToken = prefs.getString("claude_tok", "");
  prefs.end();
  static WiFiManagerParameter tzParam("tz", "Timezone (POSIX TZ string)", g_timezone.c_str(), 63);
  pTzParam = &tzParam;
  wm.addParameter(&tzParam);
  // Deliberately NOT pre-filled with g_claudeToken (see onWifiConfigSaved()'s comment) -
  // type="password" masks entry in the UI; blank on save means "keep the existing token".
  static WiFiManagerParameter tokenParam("claude_tok", "Claude usage token (blank = keep existing)", "", 400,
                                          "type=\"password\"");
  pTokenParam = &tokenParam;
  wm.addParameter(&tokenParam);

  if (bootButtonHeld) {
    // startConfigPortal() (not resetSettings()+autoConnect()) - forces the portal open
    // without erasing the stored WiFi network first. Originally used resetSettings(), but
    // that meant editing just the token or timezone field required re-entering WiFi
    // credentials too in the same portal visit - unnecessary friction once this portal
    // started collecting more than just WiFi. Still lets you pick a different network from
    // here if you want to; it just doesn't force you to.
    Serial.println("[BOOT] BOOT button held at boot - forcing the WiFi/token/timezone portal (network kept as-is)");
    wm.startConfigPortal(WIFI_AP_NAME);
  } else {
    connectWifiNow(); // returns almost immediately either way; loop() drives the rest
  }
  Serial.println("[BOOT] WiFi connect/portal kicked off (non-blocking) - see loop() for progress");
  Serial.printf("[BOOT] Free heap after WiFi init: %u bytes\n", ESP.getFreeHeap());

  NimBLEDevice::init("PineCYD-Fase2");
  NimBLEDevice::setPower(3);

  NimBLEScan* pScan = NimBLEDevice::getScan();
  pScan->setScanCallbacks(&scanCallbacks, false);
  pScan->setInterval(100);
  pScan->setWindow(100);
  // A 40% duty cycle was tried here, 2026-07-15, as a hypothesis for the settings page's
  // slowness (matching the already-confirmed BLE-scan-starves-WiFi contention that made the
  // captive portal unjoinable). Reverted - real-hardware testing with USB serial access
  // showed the actual cause was heap exhaustion during TLS handshakes (see
  // fetchClaudeUsage()'s setBufferSizes() comment), not radio contention. No reason to pay
  // slower Pinecil discovery for a fix that measurably didn't help.
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
  //
  // Tried removing this toggle entirely (WiFi always on), 2026-07-15, testing whether it
  // was contributing to the heap fragmentation behind the Claude usage fetch's TLS
  // allocation failures (see fetchClaudeUsage()'s comment) - real-hardware testing
  // disproved this: min_heap still collapsed to ~5.7KB with WiFi continuously on, no
  // different from before. It also cost real BLE latency (cycle_ms ~49-137ms, averaging
  // notably worse than the ~54-70ms this toggle normally achieves) for zero benefit.
  // Reverted. The fragmentation appears structural to running BLE+LVGL+WiFiManager+OTA+
  // mDNS+WebServer+periodic TLS together on this no-PSRAM board, not an artifact of this
  // toggle's on/off cycling - see README.md's "Claude usage zone" section for the fuller
  // writeup and what's actually left to try (the bridge-script architecture).
  static uint32_t lastWifiAttempt = 0; // shared with the retry block below
  bool            shouldShowClock = !connected;
  // If a Pinecil connects while an OTA transfer is actively in progress, defer turning WiFi
  // off (and the screen switch) until the transfer finishes - yanking WiFi mid-flash would
  // corrupt the update. The "turn WiFi back on" direction never needs this guard since it
  // doesn't touch WiFi.mode(WIFI_OFF).
  bool wifiOffWouldInterruptOta = (!shouldShowClock && otaInProgress);
  if (shouldShowClock != clockScreenActive && !wifiOffWouldInterruptOta) {
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
      MDNS.end();
      otaStarted            = false; // ArduinoOTA's listener doesn't survive a WiFi power-cycle -
                                     // otaBegin()/settingsServerBegin() re-run fresh next
      settingsServerStarted = false; // time WiFi reconnects (see below)
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
    }
  }

  // Services the non-blocking captive portal (near-zero cost when none is open - same
  // shape as lv_timer_handler() above), and drives the on-screen switch back to the clock
  // once the portal closes (saved successfully, or its own timeout - see setup()).
  if (clockScreenActive) {
    wm.process();
    if (otaStarted) ArduinoOTA.handle();
    if (settingsServerStarted) settingsServer.handleClient();

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
    configTzTime(g_timezone.c_str(), "pool.ntp.org", "time.google.com");
    if (MDNS.begin(OTA_HOSTNAME)) {
      Serial.printf("[WiFi] mDNS ready: http://%s.local/\n", OTA_HOSTNAME);
    }
    otaBegin();
    settingsServerBegin();
    // Deliberately NOT calling fetchClaudeUsage() eagerly here anymore, 2026-07-15: this
    // used to fire immediately on every WiFi (re)connect, right in the middle of
    // otaBegin()/settingsServerBegin()/mDNS all initializing at once - confirmed live as
    // the actual trigger for the TLS handshake's memory-allocation failures (worst possible
    // moment for a ~32KB allocation attempt). The periodic check below now handles the
    // first fetch too, naturally landing after things have settled.
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
      static const char* DA_WEEKDAYS[7] = {"s\xC3\xB8n", "man", "tir", "ons", "tor", "fre", "l\xC3\xB8r"}; // tm_wday: 0=Sunday
      static const char* DA_MONTHS[12]  = {"jan", "feb", "mar", "apr", "maj", "jun",
                                            "jul", "aug", "sep", "okt", "nov", "dec"};
      lv_label_set_text_fmt(label_clock_date, "%s %d. %s", DA_WEEKDAYS[timeinfo.tm_wday], timeinfo.tm_mday,
                             DA_MONTHS[timeinfo.tm_mon]);
    }
    // Recomputed every tick (not just on fetch) so the pace marker and countdown stay live
    // between the 60s fetch cycles below - see updateUsageZoneDisplay()'s own comment.
    updateUsageZoneDisplay();
  }

  // Claude usage fetch cadence (2026-07-15): only while the clock is showing (WiFi on) and
  // a token has been configured. 60s, not tied to the clock tick above - usage doesn't need
  // per-second freshness, and this keeps the two concerns (display refresh vs. network
  // fetch) independently tunable.
  static uint32_t lastUsageFetch = 0;
  if (clockScreenActive && !g_claudeToken.isEmpty() && wifiNowConnected &&
      (g_forceUsageFetch || millis() - lastUsageFetch >= USAGE_FETCH_INTERVAL_MS)) {
    lastUsageFetch     = millis();
    g_forceUsageFetch  = false;
    fetchClaudeUsage();
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
