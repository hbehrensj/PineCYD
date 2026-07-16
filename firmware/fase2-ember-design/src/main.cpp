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
#include <WebServer.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
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

// --- Confirmed IronOS BLE UUIDs - Settings service (2026-07-16, see docs/pinecil-config-page/) ---
// Every setting has its own characteristic, f6d7XXXX-5a10-4eba-aa55-33e27f9bc533 where XXXX
// is the setting's 4-hex-digit index (raw uint16, read+write) - confirmed directly from
// Ralim/IronOS source (ble_characteristics.h/ble_handlers.cpp/ble_peripheral.c), not assumed.
// There is no bulk read/write characteristic. RAM-only until SETTINGS_CHAR_SAVE is written
// with 1 - see the handoff doc's "protocol mechanics" section.
static const char* SERVICE_SETTINGS_DATA = "f6d80000-5a10-4eba-aa55-33e27f9bc533";
static const char* SETTINGS_CHAR_SAVE    = "f6d7ffff-5a10-4eba-aa55-33e27f9bc533";

// The 40 settings the Pinecil config page exposes - all 41 indices that actually have a
// registered BLE characteristic (confirmed in ble_peripheral.c), minus #37 (BluetoothLE -
// deliberately excluded, see the handoff doc: setting it read-only from this page would cut
// off the page's own write access on its very next call). The 14 profile-mode settings
// (#39-52) and #55 (button swap) aren't here because Pinecil V2's firmware never registers a
// BLE characteristic for them at all - not a choice made in this file.
static const uint8_t SETTINGS_INDICES[] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13,
                                            14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
                                            28, 29, 30, 31, 32, 33, 34, 35, 36, 38, 53, 54};

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
// ember-hot (#FFB03A) dropped, 2026-07-15 - was only used as a gradient's leading-edge
// color; the fill is a flat per-state color now (see updateUsageZoneDisplay()), no gradient.
static const lv_color_t COLOR_USAGE_SURFACE = LV_COLOR_MAKE(0x24, 0x1C, 0x14);
static const lv_color_t COLOR_USAGE_CORE    = LV_COLOR_MAKE(0xFF, 0x6A, 0x2B);
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

// Claude usage zone (2026-07-15, made row-count-dynamic 2026-07-15) - see
// buildClockUi()/updateUsageZoneDisplay(). Up to 3 slots (five_hour/seven_day/
// seven_day_sonnet - matches the real API shape) rather than the handoff doc's general
// 2-4-row system, but which slots are actually shown - and where - is decided at runtime
// from which buckets the last successful fetch actually contained, not hardcoded: all 3 are
// pre-created (fixed flash cost, per the handoff doc's own suggested LVGL approach) but start
// hidden, and updateUsageZoneDisplay() shows/hides/repositions them to compact out any gaps.
static lv_obj_t* usageZone;
static lv_obj_t* usageRowLabel[3];
static lv_obj_t* usageTrack[3];
static lv_obj_t* usageFill[3];
static lv_obj_t* usageMarker[3];
static lv_obj_t* usageReset[3];

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

// Claude usage clock-screen widget (2026-07-15, moved to a bridge architecture 2026-07-16) -
// the device no longer stores an OAuth token or talks to Anthropic directly. It was tried
// that way first (user's own explicit choice, after being offered this same bridge design
// and initially declining it) but a live access token expires in hours and isn't refreshable
// on-device, requiring frequent manual re-entry - see README.md for the full history. Now a
// small script on a trusted machine (bridge/usage_bridge.py) holds the token, keeps itself
// fresh for free via Claude Code's own normal usage on that machine, and re-serves only
// sanitized percentages/reset-times over plain HTTP on the LAN - this device never sees the
// real token at all. g_bridgeAddr is that machine's "host:port" (e.g. "192.168.1.50:8787").
static String                g_bridgeAddr;
static WiFiManagerParameter* pBridgeParam = nullptr; // registered in setup(), read below

// Fires when the portal's form is submitted (WiFiManager's own save-config hook) -
// independent of whether the WiFi connect attempt that follows succeeds, so this also
// captures these fields on a portal session that's only being used to switch networks.
// Blank input means "leave unchanged" for both fields (matches TDAI's own optional-field
// pattern) - both are pre-filled with their current value, since neither is a secret worth
// hiding from a served HTML page's source (unlike the OAuth token this field replaced).
static void onWifiConfigSaved() {
  if (pTzParam && strlen(pTzParam->getValue()) > 0) {
    g_timezone = pTzParam->getValue();
    prefs.begin("pinecyd", false);
    prefs.putString("tz", g_timezone);
    prefs.end();
    Serial.printf("[WiFi] Timezone saved: %s\n", g_timezone.c_str());
  }
  if (pBridgeParam && strlen(pBridgeParam->getValue()) > 0) {
    g_bridgeAddr = pBridgeParam->getValue();
    prefs.begin("pinecyd", false);
    prefs.putString("bridge_addr", g_bridgeAddr);
    prefs.end();
    Serial.printf("[WiFi] Usage bridge address saved: %s\n", g_bridgeAddr.c_str());
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

// Kicks off a (re)connect attempt, used from setup() and the periodic retry in loop() alike
// (the connected-Pinecil WiFi on/off toggle that used to also call this is gone, 2026-07-16 -
// see loop()). Uses the cached credentials directly once we have them
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

static bool     otaStarted    = false;
static bool     otaInProgress = false; // gates the OTA-window auto-teardown below, so an active transfer is never cut off mid-upload
static uint32_t otaStartedAtMs = 0;
// Bounded listening window, 2026-07-15: freeing ArduinoOTA's listener (and whatever
// contiguous headroom it's holding) for the long tail of uptime - one more lever in the
// same effort as fetchClaudeUsage()'s heap-fragmentation fight. A reboot re-opens it
// (power-cycle or a fresh WiFi reconnect); no on-demand reopen UI built for this yet -
// this project already has USB/console access as a fallback if OTA is needed outside the
// window.
static const uint32_t OTA_WINDOW_MS = 20 * 60 * 1000;

static void otaBegin() {
  if (otaStarted) return;
  if (WiFi.status() != WL_CONNECTED) return;

  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.setMdnsEnabled(false); // we manage mDNS ourselves (MDNS.begin()/end() tied to
                                    // the WiFi connect/disconnect transition, see loop()) -
                                    // ArduinoOTA.end() would otherwise also tear down mDNS,
                                    // breaking the settings page's pinecyd.local resolution
                                    // whenever the OTA window closes

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
    otaInProgress = false; // upload aborted - let the OTA-window auto-teardown resume normally
    Serial.printf("[OTA] Error %u\n", err);
  });

  ArduinoOTA.begin();
  otaStarted    = true;
  otaStartedAtMs = millis();
  Serial.printf("[OTA] Ready: %s.local (espota), hostname=%s - open for %lu min\n", OTA_HOSTNAME, OTA_HOSTNAME,
                OTA_WINDOW_MS / 60000UL);
}

// --- Always-on settings page (2026-07-15) ---
// The WiFiManager portal only exists for a few seconds at a time (auto-triggered on
// connect failure, or forced by holding BOOT) - fine for one-time WiFi setup, but the
// Claude usage token is expected to need periodic re-entry (it isn't refreshed - see
// README.md), and going through the BOOT-hold/portal dance every time is real friction for
// something you might do every few hours. This is a small always-on page at
// http://pinecyd.local/, reachable during normal WiFi-connected operation (same lifecycle as
// ArduinoOTA/mDNS above - started on WiFi connect, torn down only on a real WiFi disconnect,
// see loop() - no longer tied to whether a Pinecil is connected, 2026-07-16) - no separate
// portal/AP needed to update the token or timezone. The Pinecil config page
// (docs/pinecil-config-page/, /pinecil below) is a second route on this same server.
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
// handleSettingsRoot() (bound to GET "/") is defined further down, right after
// PINECIL_PAGE_HTML - it dispatches between that page and the plain settings form below
// depending on whether a Pinecil is connected, see that section's own comment.

static void handleSettingsNotFound() {
  settingsServer.sendHeader("Connection", "close");
  settingsServer.send(404, "text/plain", "Not found");
}

// Same blank-means-unchanged pattern as the portal's own onWifiConfigSaved() - neither field
// is a secret (the bridge address replaced the old OAuth token field, 2026-07-16), so both
// are pre-filled above, unlike the token field this used to be.
static void handleSettingsSave() {
  if (settingsServer.hasArg("bridge") && settingsServer.arg("bridge").length() > 0) {
    g_bridgeAddr = settingsServer.arg("bridge");
    prefs.begin("pinecyd", false);
    prefs.putString("bridge_addr", g_bridgeAddr);
    prefs.end();
    Serial.println("[Settings] Usage bridge address updated via web page");
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

// --- Pinecil config page (2026-07-16, see docs/pinecil-config-page/handoff.md) ---
// Shares the settingsServer above with the plain bridge/timezone settings form. Originally
// its own route (/pinecil); moved to replace the root route while a Pinecil is BLE-connected
// instead, per the user's explicit request (see handleSettingsRoot(), further down) - needs
// task 1 above (WiFi no longer turns off for that) to ever be reachable at all. Every field's
// value is fetched fresh over BLE on page load/reconnect (GET /pinecil/data) - no fast/live
// requirement here (see handoff doc), unlike the dashboard's tip-temp polling - and only
// dirty fields are written back on Save (POST /pinecil/save), matching the browser UI's own
// dirty-tracking so we never rewrite settings that were never touched.
static bool isExposedSettingIndex(uint8_t idx) {
  for (uint8_t v : SETTINGS_INDICES) {
    if (v == idx) return true;
  }
  return false;
}

static String settingCharUuid(uint8_t idx) {
  char buf[5];
  snprintf(buf, sizeof(buf), "%04x", idx);
  return String("f6d7") + buf + "-5a10-4eba-aa55-33e27f9bc533";
}

static NimBLERemoteCharacteristic* getSettingChar(uint8_t idx) {
  if (!connected || !pClient) return nullptr;
  NimBLERemoteService* pSvc = pClient->getService(NimBLEUUID(SERVICE_SETTINGS_DATA));
  if (!pSvc) return nullptr;
  return pSvc->getCharacteristic(NimBLEUUID(settingCharUuid(idx).c_str()));
}

static bool readSettingU16(uint8_t idx, uint16_t& outVal) {
  NimBLERemoteCharacteristic* chr = getSettingChar(idx);
  if (!chr) return false;
  NimBLEAttValue v = chr->readValue();
  if (v.size() < 2) return false;
  const uint8_t* d = v.data();
  outVal            = (uint16_t)d[0] | ((uint16_t)d[1] << 8);
  return true;
}

static bool writeSettingU16(uint8_t idx, uint16_t val) {
  NimBLERemoteCharacteristic* chr = getSettingChar(idx);
  if (!chr) return false;
  uint8_t buf[2] = {(uint8_t)(val & 0xFF), (uint8_t)(val >> 8)};
  // write-with-response (true): a read-only rejection (Pinecil's own BluetoothLE setting set
  // to 2, see handoff doc) then actually surfaces here as a failed call instead of silently
  // "succeeding" against a peripheral that never applied it.
  return chr->writeValue(buf, sizeof(buf), true);
}

// Persists RAM -> flash on the Pinecil itself (writes 1 to the dedicated SAVE characteristic).
// Without this, every writeSettingU16() above only changes RAM and is lost on the Pinecil's
// next reboot - see the handoff doc's "protocol mechanics" section.
static bool saveSettingsToFlash() {
  if (!connected || !pClient) return false;
  NimBLERemoteService* pSvc = pClient->getService(NimBLEUUID(SERVICE_SETTINGS_DATA));
  if (!pSvc) return false;
  NimBLERemoteCharacteristic* chr = pSvc->getCharacteristic(NimBLEUUID(SETTINGS_CHAR_SAVE));
  if (!chr) return false;
  uint8_t buf[2] = {1, 0};
  return chr->writeValue(buf, sizeof(buf), true);
}

// Static page: every field's metadata (label/range/tab/options) lives in the JS below, not
// server-side - the server only ever speaks raw {index: uint16} JSON (see the two handlers
// below), so this HTML never needs per-request templating and can be one PROGMEM constant.
// Design reference: docs/pinecil-config-page/mockup.dc.html + design-handoff.md. Simplified
// from that reference, deliberately, to fit this device's flash/RAM budget: no live countdown
// timer on the disconnected overlay (plain 5s retry poll instead), no toast fade animation,
// vanilla DOM calls instead of a component framework.
static const char PINECIL_PAGE_HTML[] PROGMEM = R"PINECILPAGE(<!DOCTYPE html><html><head><meta charset='utf-8'>
<meta name='viewport' content='width=device-width, initial-scale=1'><title>Pinecil Settings</title>
<style>
*{box-sizing:border-box}
body{margin:0;background:#0D0D0D;color:#F2E7D8;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;display:flex;flex-direction:column;min-height:100vh}
header{position:sticky;top:0;background:#241C14;border-bottom:1px solid rgba(242,231,216,.1);padding:16px 24px;display:flex;justify-content:space-between;align-items:center;z-index:10}
.title{font-size:18px;font-weight:600}
.subtitle{font-size:12px;color:#8A7A6A;margin-top:2px}
.conn{display:flex;align-items:center;gap:8px}
.dot{width:8px;height:8px;border-radius:50%}
#tabs{display:flex;gap:8px;padding:12px 24px;overflow-x:auto;border-bottom:1px solid rgba(242,231,216,.08)}
.tab{padding:8px 16px;border-radius:999px;border:none;font-size:13px;font-weight:600;cursor:pointer;white-space:nowrap;background:transparent;color:#8A7A6A}
.tab.active{background:#FF6A2B;color:#0D0D0D}
#content{flex:1;max-width:760px;width:100%;margin:0 auto;padding:8px 24px 32px}
.section{font-size:12px;text-transform:uppercase;letter-spacing:.08em;color:#8A7A6A;font-weight:600;margin:24px 0 4px}
.row{display:flex;flex-wrap:wrap;gap:16px;justify-content:space-between;align-items:flex-start;padding:14px 0;border-bottom:1px solid rgba(242,231,216,.08)}
.row-left{flex:1 1 220px;min-width:200px}
.label{font-size:14px;font-weight:500}
.note{font-size:12px;color:#8A7A6A;margin-top:4px;line-height:1.4;max-width:420px}
.row-right{flex:0 0 auto;min-width:120px;display:flex;align-items:center;gap:8px}
.input-number,.input-select,.input-disabled{background:#0D0D0D;border:1px solid rgba(242,231,216,.2);color:#F2E7D8;padding:8px 10px;border-radius:6px;font-size:14px}
.input-number{width:90px;font-family:ui-monospace,monospace}
.input-select{min-width:180px}
.input-disabled{width:90px;background:#1a1512;border-color:rgba(242,231,216,.1);color:#8A7A6A;font-family:ui-monospace,monospace}
.unit{font-size:13px;color:#8A7A6A;min-width:36px}
.input-slider{width:140px;accent-color:#FF6A2B}
.slider-val{font-size:13px;min-width:24px;font-family:ui-monospace,monospace;text-align:right}
.readonly{font-size:14px;color:#8A7A6A;font-family:ui-monospace,monospace}
.toggle{width:44px;height:24px;border-radius:12px;cursor:pointer;position:relative;background:#3a3128}
.toggle.on{background:#FF6A2B}
.toggle .knob{width:18px;height:18px;border-radius:50%;background:#F2E7D8;position:absolute;top:3px;left:3px;transition:left .15s}
.toggle.on .knob{left:23px}
.banner{border:1px solid rgba(255,59,47,.4);background:rgba(255,59,47,.08);border-radius:8px;padding:16px;margin:16px 0}
.banner-title{color:#FF6A2B;font-weight:600;font-size:14px;margin-bottom:6px}
.banner-body{font-size:13px;line-height:1.5;margin-bottom:12px}
footer{position:sticky;bottom:0;background:#241C14;border-top:1px solid rgba(242,231,216,.1);padding:14px 24px;display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:12px;z-index:10}
#footerStatus{font-size:13px;color:#8A7A6A}
.btn-primary{padding:10px 20px;border-radius:6px;border:none;font-size:14px;font-weight:600;cursor:pointer;background:#FF6A2B;color:#0D0D0D}
.btn-primary.muted{background:#3a3128;color:#8A7A6A;cursor:default}
.btn-outline{padding:10px 18px;border-radius:6px;font-size:14px;font-weight:500;background:transparent;border:1px solid rgba(242,231,216,.2);color:#F2E7D8;cursor:pointer}
.btn-outline.muted{color:#8A7A6A;cursor:default}
#toast{display:none;position:fixed;bottom:80px;left:50%;transform:translateX(-50%);background:#FF6A2B;color:#0D0D0D;padding:10px 18px;border-radius:20px;font-size:13px;font-weight:600;box-shadow:0 4px 16px rgba(0,0,0,.4);z-index:20}
#disconnected{display:none;position:fixed;inset:0;background:#0D0D0D;flex-direction:column;align-items:center;justify-content:center;gap:10px;z-index:50;text-align:center;padding:24px}
</style></head><body>
<header><div><div class='title'>Pinecil Settings</div><div class='subtitle'>pinecyd.local</div></div>
<div class='conn'><div class='dot' id='connDot'></div><span id='connLabel' style='font-size:13px;color:#8A7A6A'></span></div></header>
<div id='tabs'></div>
<div id='content'></div>
<footer><div id='footerStatus'></div><div style='display:flex;gap:10px'>
<button class='btn-outline' id='discardBtn'>Discard</button><button class='btn-primary' id='saveBtn'>Save changes</button>
</div></footer>
<div id='toast'></div>
<div id='disconnected'><div class='dot' style='width:12px;height:12px;background:#FF3B2F'></div>
<div style='font-size:18px;font-weight:600'>No Pinecil connected</div>
<div style='font-size:14px;color:#8A7A6A;max-width:320px'>Connect a Pinecil V2 via BLE to PineCYD to read and change its settings.</div>
<div style='font-size:13px;color:#8A7A6A'>Retrying every 5s&hellip;</div></div>
<script>
const FIELDS=[
{idx:0,tab:'soldering',sec:'Setpoint',label:'Soldering temperature',type:'number',min:10,max:450,step:1,unit:'C',def:320,note:'The setpoint - shown read-only on the main dashboard'},
{idx:22,tab:'soldering',sec:'Setpoint',label:'Boost temperature',type:'number',min:250,max:450,step:1,unit:'C',def:420,note:'Target in boost mode'},
{idx:15,tab:'soldering',sec:'Setpoint',label:'Show in Fahrenheit',type:'toggle',def:0,note:'Converts all temperature fields in this UI'},
{idx:27,tab:'soldering',sec:'Buttons',label:'Short-press step',type:'number',min:1,max:50,step:1,unit:'deg',def:1},
{idx:26,tab:'soldering',sec:'Buttons',label:'Long-press step',type:'number',min:5,max:90,step:1,unit:'deg',def:10},
{idx:25,tab:'soldering',sec:'Buttons',label:'Swap +/- buttons',type:'toggle',def:0},
{idx:10,tab:'soldering',sec:'Startup & lock',label:'Auto-start',type:'select',def:0,opts:[[0,'None'],[1,'Soldering'],[2,'Sleep temp'],[3,'Zero power']]},
{idx:17,tab:'soldering',sec:'Startup & lock',label:'Button lock',type:'select',def:0,opts:[[0,'Off'],[1,'Boost only'],[2,'Full']]},
{idx:24,tab:'soldering',sec:'Startup & lock',label:'Power limit',type:'number',min:0,max:120,step:5,unit:'W',def:0,note:'0 = no limit'},
{idx:1,tab:'sleep',sec:'Sleep',label:'Sleep temperature',type:'number',min:10,max:450,step:1,unit:'C',def:150},
{idx:2,tab:'sleep',sec:'Sleep',label:'Time to sleep',type:'number',min:0,max:15,step:1,unit:'raw',def:5,note:'Unit unconfirmed - sources disagree on minutes vs. raw x10 seconds. Verify on hardware before trusting this label.'},
{idx:11,tab:'sleep',sec:'Sleep',label:'Time to shutdown',type:'number',min:0,max:60,step:1,unit:'min',def:10},
{idx:12,tab:'sleep',sec:'Sleep',label:'Blink temp while cooling',type:'toggle',def:1,note:'Below 50C'},
{idx:7,tab:'sleep',sec:'Sensors',label:'Motion sensitivity',type:'slider',min:0,max:9,step:1,def:7},
{idx:28,tab:'sleep',sec:'Sensors',label:'Hall-effect sensitivity',type:'slider',min:0,max:9,step:1,def:7},
{idx:53,tab:'sleep',sec:'Sensors',label:'Time to sleep (magnet)',type:'number',min:0,max:12,step:1,unit:'x5s',def:0},
{idx:3,tab:'power',sec:'Power source',label:'Power source type',type:'select',def:0,opts:[[0,'DC (fixed 9V floor)'],[1,'3S battery'],[2,'4S battery'],[3,'5S battery'],[4,'6S battery']]},
{idx:4,tab:'power',sec:'Power source',label:'Min. voltage per cell',type:'number',min:24,max:38,step:1,unit:'x0.1V',def:33,note:'Combines with power source type into an effective cutoff voltage'},
{idx:5,tab:'power',sec:'Power source',label:'QC ideal voltage',type:'number',min:90,max:220,step:2,unit:'x0.1V',def:90},
{idx:32,tab:'power',sec:'Power source',label:'PD negotiation timeout',type:'number',min:0,max:50,step:1,unit:'x100ms',def:20},
{idx:38,tab:'power',sec:'Power source',label:'USB PD mode',type:'select',def:0,opts:[[0,'Fixed PDO only'],[1,'PPS+EPR+extra power'],[2,'PPS+EPR safe']]},
{idx:18,tab:'power',sec:'Keep-awake pulse',label:'Pulse power',type:'number',min:0,max:100,step:1,unit:'x0.1W',def:0},
{idx:19,tab:'power',sec:'Keep-awake pulse',label:'Wait between pulses',type:'number',min:1,max:9,step:1,unit:'x2.5s',def:4},
{idx:20,tab:'power',sec:'Keep-awake pulse',label:'Pulse duration',type:'number',min:1,max:9,step:1,unit:'x250ms',def:1},
{idx:6,tab:'display',sec:'Display',label:'Orientation',type:'select',def:2,opts:[[0,'Right'],[1,'Left'],[2,'Auto']]},
{idx:34,tab:'display',sec:'Display',label:'Brightness',type:'slider',min:1,max:101,step:25,def:26},
{idx:33,tab:'display',sec:'Display',label:'Invert colors',type:'toggle',def:0},
{idx:35,tab:'display',sec:'Display',label:'Boot logo duration',type:'number',min:0,max:6,step:1,unit:'s',def:1},
{idx:16,tab:'display',sec:'Display',label:'Fast description scroll',type:'toggle',def:0},
{idx:13,tab:'display',sec:'Screens',label:'Detailed idle screen',type:'toggle',def:1},
{idx:14,tab:'display',sec:'Screens',label:'Detailed soldering screen',type:'toggle',def:1},
{idx:8,tab:'display',sec:'Screens',label:'Loop animation',type:'toggle',def:1},
{idx:9,tab:'display',sec:'Screens',label:'Animation speed',type:'select',def:2,opts:[[0,'Off'],[1,'Slow'],[2,'Medium'],[3,'Fast']]},
{idx:31,tab:'display',sec:'Screens',label:'Language code (raw)',type:'number',min:0,max:65535,step:1,def:1033,note:'Raw 16-bit code - no lookup table to a human-readable name yet'},
{idx:21,tab:'advanced',sec:'Calibration',gated:1,label:'Voltage calibration',type:'number',min:360,max:900,step:1,def:630,note:'Hardware calibration factor from the schematic - only change if you know what you are doing'},
{idx:23,tab:'advanced',sec:'Calibration',gated:1,label:'Tip ADC offset',type:'number',min:100,max:2500,step:1,unit:'uV',def:900,note:'Tip-specific calibration value'},
{idx:36,tab:'advanced',sec:'Calibration',gated:1,label:'Trigger CJC calibration on next boot',type:'toggle',def:0,note:'A one-time action, not a persistent setting'},
{idx:54,tab:'advanced',sec:'Calibration',gated:1,label:'Tip type',type:'disabled',def:0,note:'Only one valid value on this hardware - auto tip detection is active'},
{idx:29,tab:'advanced',sec:'Diagnostics (read-only)',label:'Warnings: missing accelerometer',type:'readonly',def:0},
{idx:30,tab:'advanced',sec:'Diagnostics (read-only)',label:'Warnings: missing PD interface',type:'readonly',def:0},
];
const TABS=[['soldering','Soldering'],['sleep','Sleep'],['power','Power'],['display','Display'],['advanced','Advanced']];
let values={},saved={},activeTab='soldering',advancedRevealed=false,connected=false,saving=false,reconnectTimer=null,toastTimeout=null;
FIELDS.forEach(f=>{values[f.idx]=f.def;saved[f.idx]=f.def;});
function cToF(c){return Math.round(c*9/5+32);}
function fToC(f){return Math.round((f-32)*5/9);}
function dirtyKeys(){return Object.keys(values).filter(k=>values[k]!==saved[k]);}
function showToast(msg){const t=document.getElementById('toast');t.textContent=msg;t.style.display='block';clearTimeout(toastTimeout);toastTimeout=setTimeout(()=>{t.style.display='none';},2200);}
function renderField(f,tempF){
const row=document.createElement('div');row.className='row';
const left=document.createElement('div');left.className='row-left';
const lbl=document.createElement('div');lbl.className='label';lbl.textContent=f.label;left.appendChild(lbl);
if(f.note){const n=document.createElement('div');n.className='note';n.textContent=f.note;left.appendChild(n);}
row.appendChild(left);
const right=document.createElement('div');right.className='row-right';
const val=values[f.idx];
if(f.type==='number'){
const isTemp=f.unit==='C';
const dv=isTemp&&tempF?cToF(val):val,dmin=isTemp&&tempF?cToF(f.min):f.min,dmax=isTemp&&tempF?cToF(f.max):f.max;
const input=document.createElement('input');input.type='number';input.value=dv;input.min=dmin;input.max=dmax;input.step=f.step;input.className='input-number';
input.onchange=()=>{const raw=parseFloat(input.value);if(isNaN(raw))return;values[f.idx]=isTemp&&tempF?fToC(raw):raw;render();};
right.appendChild(input);
const unit=document.createElement('span');unit.className='unit';unit.textContent=isTemp?(tempF?'F':'C'):(f.unit||'');right.appendChild(unit);
}else if(f.type==='slider'){
const input=document.createElement('input');input.type='range';input.min=f.min;input.max=f.max;input.step=f.step;input.value=val;input.className='input-slider';
const out=document.createElement('span');out.className='slider-val';out.textContent=val;
input.oninput=()=>{values[f.idx]=parseInt(input.value,10);out.textContent=input.value;};
input.onchange=()=>render();
right.appendChild(input);right.appendChild(out);
}else if(f.type==='select'){
const sel=document.createElement('select');sel.className='input-select';
f.opts.forEach(o=>{const opt=document.createElement('option');opt.value=o[0];opt.textContent=o[1];if(o[0]===val)opt.selected=true;sel.appendChild(opt);});
sel.onchange=()=>{values[f.idx]=parseInt(sel.value,10);render();};
right.appendChild(sel);
}else if(f.type==='toggle'){
const track=document.createElement('div');track.className='toggle'+(val===1?' on':'');
const knob=document.createElement('div');knob.className='knob';track.appendChild(knob);
track.onclick=()=>{values[f.idx]=val===1?0:1;render();};
right.appendChild(track);
}else if(f.type==='readonly'){
const span=document.createElement('span');span.className='readonly';span.textContent=val;right.appendChild(span);
}else if(f.type==='disabled'){
const input=document.createElement('input');input.type='text';input.value=val;input.disabled=true;input.className='input-disabled';right.appendChild(input);
}
row.appendChild(right);
return row;
}
function render(){
document.getElementById('connDot').style.background=connected?'#FF6A2B':'#FF3B2F';
document.getElementById('connLabel').textContent=connected?'Connected':'Disconnected';
document.getElementById('disconnected').style.display=connected?'none':'flex';
if(!connected)return;
const tabsEl=document.getElementById('tabs');tabsEl.innerHTML='';
TABS.forEach(t=>{const b=document.createElement('button');b.textContent=t[1];b.className='tab'+(activeTab===t[0]?' active':'');b.onclick=()=>{activeTab=t[0];render();};tabsEl.appendChild(b);});
const content=document.getElementById('content');content.innerHTML='';
const tempF=values[15]===1;
const hasGated=FIELDS.some(f=>f.tab===activeTab&&f.gated);
if(activeTab==='advanced'&&hasGated&&!advancedRevealed){
const banner=document.createElement('div');banner.className='banner';
banner.innerHTML="<div class='banner-title'>Calibration fields</div><div class='banner-body'>These fields control hardware calibration. Incorrect values can make temperature and voltage readings inaccurate. Only touch these if you know what you are doing.</div>";
const btn=document.createElement('button');btn.className='btn-outline';btn.textContent='Show calibration fields';
btn.onclick=()=>{advancedRevealed=true;render();};
banner.appendChild(btn);content.appendChild(banner);
}
let lastSection=null;
FIELDS.filter(f=>f.tab===activeTab&&(!f.gated||advancedRevealed)).forEach(f=>{
if(f.sec!==lastSection){lastSection=f.sec;const h=document.createElement('h3');h.className='section';h.textContent=f.sec;content.appendChild(h);}
content.appendChild(renderField(f,tempF));
});
const dc=dirtyKeys().length;
document.getElementById('footerStatus').textContent=dc>0?(dc+' change'+(dc===1?'':'s')+' not saved'):'All changes saved';
const saveBtn=document.getElementById('saveBtn');
saveBtn.disabled=!(dc>0)||saving;
saveBtn.textContent=saving?'Saving...':'Save changes';
saveBtn.className=(dc>0&&!saving)?'btn-primary':'btn-primary muted';
const discardBtn=document.getElementById('discardBtn');
discardBtn.disabled=!(dc>0);
discardBtn.className=dc>0?'btn-outline':'btn-outline muted';
}
function discard(){values=Object.assign({},saved);render();}
function save(){
const dirty={};
dirtyKeys().forEach(k=>{dirty[k]=values[k];});
const n=Object.keys(dirty).length;
if(n===0){showToast('No changes to save');return;}
saving=true;render();
fetch('/pinecil/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(dirty)})
.then(r=>r.json())
.then(r=>{
saving=false;
if(r.ok){saved=Object.assign({},values);showToast('Saved '+n+' change'+(n===1?'':'s')+' to the Pinecil');}
else{showToast('Save failed - check BLE write access is set to read-write on the Pinecil itself');}
render();
})
.catch(()=>{saving=false;showToast('Save failed - connection error');render();});
}
function poll(){
fetch('/pinecil/data').then(r=>r.json()).then(d=>{
connected=d.connected;
if(connected&&d.values){Object.keys(d.values).forEach(k=>{values[k]=d.values[k];saved[k]=d.values[k];});}
render();
if(!connected){if(!reconnectTimer)reconnectTimer=setInterval(poll,5000);}
else if(reconnectTimer){clearInterval(reconnectTimer);reconnectTimer=null;}
}).catch(()=>{connected=false;render();if(!reconnectTimer)reconnectTimer=setInterval(poll,5000);});
}
document.getElementById('saveBtn').onclick=save;
document.getElementById('discardBtn').onclick=discard;
poll();
</script></body></html>)PINECILPAGE";

// Bound to GET "/" (see settingsServerBegin()) - this is the page's actual entry point, per
// the user's explicit request, 2026-07-16: the Pinecil config page replaces the plain
// bridge/timezone settings form at the root URL *while a Pinecil is connected*, rather than
// living at its own /pinecil path. `/pinecil/data` and `/pinecil/save` (below) are unchanged
// - they're background fetch() targets the config page's own JS calls, not a page a user
// navigates to directly, so keeping them at that path is invisible either way.
static void handleSettingsRoot() {
  settingsServer.sendHeader("Connection", "close");
  if (connected) {
    settingsServer.send_P(200, "text/html", PINECIL_PAGE_HTML);
    return;
  }
  String html =
      "<!DOCTYPE html><html><head><meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width, initial-scale=1'>"
      "<title>PineCYD settings</title></head><body style='font-family:sans-serif; max-width:420px; margin:2em auto;'>"
      "<h3>PineCYD settings</h3>"
      "<form method='POST' action='/save'>"
      "<label>Usage bridge address (host:port, blank = keep existing):</label><br>"
      "<input type='text' name='bridge' value='" +
      g_bridgeAddr +
      "' style='width:100%; box-sizing:border-box;'><br><br>"
      "<label>Timezone (POSIX TZ string):</label><br>"
      "<input type='text' name='tz' value='" +
      g_timezone +
      "' style='width:100%; box-sizing:border-box;'><br><br>"
      "<input type='submit' value='Save'>"
      "</form></body></html>";
  settingsServer.send(200, "text/html", html);
}

// One BLE read per exposed setting (~20-100ms each on this hardware, per README's own
// measurements) - a few seconds total for all 40 on page load/reconnect. Deliberately not
// optimized further: this page has no fast-read requirement (see handoff doc), unlike the
// dashboard's tip-temp polling.
static void handlePinecilData() {
  settingsServer.sendHeader("Connection", "close");
  JsonDocument doc;
  doc["connected"] = connected;
  if (connected) {
    JsonObject values = doc["values"].to<JsonObject>();
    for (uint8_t idx : SETTINGS_INDICES) {
      uint16_t val;
      if (readSettingU16(idx, val)) {
        values[String(idx)] = val;
      }
    }
  }
  String out;
  serializeJson(doc, out);
  settingsServer.send(200, "application/json", out);
}

// Body is a flat JSON object of {"<index>": <uint16 value>, ...} - only the fields the page
// considers dirty, not a full snapshot (there's no bulk-write characteristic either - see
// this section's header comment). Writes each one individually, then persists with one SAVE
// call if at least one write went through.
static void handlePinecilSave() {
  settingsServer.sendHeader("Connection", "close");
  if (!connected) {
    settingsServer.send(409, "application/json", "{\"ok\":false,\"error\":\"not_connected\"}");
    return;
  }
  JsonDocument         doc;
  DeserializationError err = deserializeJson(doc, settingsServer.arg("plain"));
  if (err) {
    settingsServer.send(400, "application/json", "{\"ok\":false,\"error\":\"bad_json\"}");
    return;
  }

  int  written     = 0;
  bool anyRejected = false;
  for (JsonPair kv : doc.as<JsonObject>()) {
    long idxL = atol(kv.key().c_str());
    if (idxL < 0 || idxL > 255 || !isExposedSettingIndex((uint8_t)idxL)) {
      anyRejected = true; // out-of-scope index (e.g. the BLE toggle, #37) - refuse, don't 500
      continue;
    }
    uint16_t val = (uint16_t)kv.value().as<long>();
    if (writeSettingU16((uint8_t)idxL, val)) {
      written++;
    } else {
      // Most likely cause: the Pinecil's own BluetoothLE setting (#37) is set to read-only -
      // see the handoff doc. Can't distinguish that from a generic BLE hiccup at this layer
      // without deeper NimBLE error-code inspection, so this reports one generic failure
      // either way rather than guessing which.
      anyRejected = true;
    }
  }

  bool savedOk = written > 0 ? saveSettingsToFlash() : true;

  JsonDocument resp;
  resp["ok"]      = !anyRejected && savedOk;
  resp["written"] = written;
  if (anyRejected || !savedOk) resp["error"] = "write_failed";
  String out;
  serializeJson(resp, out);
  settingsServer.send(200, "application/json", out);
}

static void settingsServerBegin() {
  if (settingsServerStarted) return;
  if (WiFi.status() != WL_CONNECTED) return;
  settingsServer.on("/", HTTP_GET, handleSettingsRoot);
  settingsServer.on("/save", HTTP_POST, handleSettingsSave);
  settingsServer.on("/pinecil/data", HTTP_GET, handlePinecilData);
  settingsServer.on("/pinecil/save", HTTP_POST, handlePinecilSave);
  settingsServer.onNotFound(handleSettingsNotFound); // e.g. the browser's automatic favicon.ico request
  settingsServer.begin();
  settingsServerStarted = true;
  Serial.printf("[Settings] Web settings page ready: http://%s.local/\n", OTA_HOSTNAME);
}

// --- Claude usage clock-screen widget (2026-07-15, bridge architecture + limits array
// 2026-07-16) ---
// Ported from a handoff doc originally written for a sibling project's ESPHome/MQTT stack
// (see README.md's "Claude usage zone" section). The device polls a small script on a
// trusted machine (bridge/usage_bridge.py, "host:port" in g_bridgeAddr - see the
// WiFiManagerParameter above) over plain HTTP on the LAN; that script holds the real OAuth
// token and re-serves sanitized fields - this device never sees the token.
//
// The bridge reads Anthropic's `limits` array, not the older flat `five_hour`/`seven_day`/
// ... top-level fields this project used until earlier the same day - confirmed live that
// the flat fields have no per-model breakdown at all (a "Fable"-scoped weekly limit only
// ever showed up in `limits`, never in the flat fields, matching Anthropic's own web usage
// dashboard's "Current session"/"All models"/per-model layout 1:1). Each `limits` entry
// already carries its own readable label - `kind` ("session"/"weekly_all") for the two
// general ones, or the scoped model's own display name ("Fable") for a per-model entry,
// which the bridge picks per-entry (see usage_bridge.py's sanitize()) - so the firmware
// below just displays whatever label string it's given, it doesn't derive one itself.
//
// Slots are NOT bound to specific known entries at compile time - Anthropic could add more
// `limits` entries (e.g. a second scoped model) and they show up automatically, since the
// firmware only cares about a stable per-entry key (bridge's JSON key, e.g.
// "weekly_scoped_fable") for pinning a slot across fetches, not a fixed known-name list (see
// findOrClaimSlot() below). Capacity is capped at USAGE_MAX_ROWS (3, matching today's actual
// screen space budget - explicit user decision, not a technical limit) - an entry beyond
// that cap is silently dropped rather than displacing an already-shown one.
struct UsageLine {
  char        id[32];        // bridge's JSON key (e.g. "weekly_scoped_fable") - stable
                              // identity for pinning this slot across fetches; never shown.
  char        label[24];     // readable text actually shown on screen (bridge's "label"
                              // field, e.g. "session" or "Fable") - see this section's own
                              // comment for where the bridge gets it.
  uint32_t    periodSeconds; // pace-marker window length - the API gives resets_at but not
                              // the window's start, so period_pct (marker position) is
                              // derived from this + resets_at + now. Comes directly from the
                              // bridge's "period_seconds" field (it knows Anthropic's kind/
                              // group grouping directly - see usage_bridge.py), not guessed
                              // device-side.
  float       pct;           // last-fetched utilization, 0-100
  time_t      resetsAt;      // last-fetched reset time, epoch seconds (UTC)
  bool        haveData;      // false until this slot is claimed by a real entry
};

// 1 minute, 2026-07-16 - matches the bridge's own UPSTREAM_CACHE_SECONDS (bridge/
// usage_bridge.py), the actual floor on data freshness now. Under the old direct-to-
// Anthropic design this constant had to weigh heap-fragmentation risk and Anthropic's own
// rate limit (10s got a real HTTP 429) against freshness, which is why it settled at 5 min.
// Neither risk applies to a plain-HTTP LAN request to the bridge - polling faster than the
// bridge's own cache window wouldn't get fresher data anyway, so 1 min is the actual sweet
// spot now, not a dev-only compromise.
static const uint32_t USAGE_FETCH_INTERVAL_MS = 60 * 1000;
static const uint32_t USAGE_STALE_AFTER_MS    = 15 * 60 * 1000; // 15 min, per the handoff doc's stale rule

static UsageLine g_usageLines[3] = {}; // all slots unclaimed until fetchClaudeUsage() fills them
static uint32_t g_lastUsageFetchOkMs = 0; // 0 = never succeeded yet

// Row geometry, shared by buildClockUi() (initial widget creation) and
// updateUsageZoneDisplay() (per-refresh show/hide/reposition) - a single source of truth so
// the two can't drift apart. USAGE_BAND_H is the full 3-row height budget; when fewer than 3
// buckets have data, the visible rows are compacted (no gaps) and the whole stack is
// re-centered within this band, matching the handoff doc's "fewer rows = more centered
// whitespace" 2-line spec rather than leaving fixed gaps where a missing bucket would be.
// LABEL_W widened 32 -> 104, TRACK_W shrunk 200 -> 160, 2026-07-16: labels switched from
// short hand-picked abbreviations ("5T") to raw JSON bucket keys ("seven_day_sonnet", the
// longest of the three) - see UsageLine's own comment. usageRowLabel[i] uses
// LV_LABEL_LONG_CLIP as a safety net either way (see buildClockUi()) so a too-narrow column
// truncates cleanly instead of overlapping the bar track.
//
// LABEL_W 104 -> 94 and TRACK_W 160 -> 154, 2026-07-16 (same day, later): visually confirmed
// on real hardware that 104px left the label column with room to spare, and the pct text
// column (USAGE_PCT_W, now USAGE_RESET_W) is repurposed for the "resets in" countdown, moved
// up beside the bar now that the pct text itself is gone (see updateUsageZoneDisplay()) -
// the bar shrinks/shifts left to free that room rather than widening the zone past 304px.
static const int USAGE_LABEL_W  = 94;
static const int USAGE_TRACK_W  = 154;
static const int USAGE_RESET_W  = 52; // was USAGE_PCT_W - now the "resets in" column, beside the bar
static const int USAGE_ROW_H    = 24; // was 32 - shrunk once the "resets in" line moved up off its own row
static const int USAGE_BAR_H    = 16;
static const int USAGE_ROW_GAP  = 3;
static const int USAGE_MAX_ROWS = 3;
static const int USAGE_BAND_H   = USAGE_MAX_ROWS * USAGE_ROW_H + (USAGE_MAX_ROWS - 1) * USAGE_ROW_GAP;

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

// See UsageLine's own comment for why slots aren't bound to specific known entries at
// compile time. Pins an entry to whichever slot it first claims by its stable id (not its
// displayed label, which can change - e.g. a model's display name), so a row doesn't jump
// position across fetches. Returns nullptr if all USAGE_MAX_ROWS slots are already claimed
// by other entries - that entry is then silently dropped rather than displacing one already
// shown (explicit user decision on row capacity, 2026-07-16 - see README.md/memory).
static UsageLine* findOrClaimSlot(const char* id) {
  for (int i = 0; i < USAGE_MAX_ROWS; i++) {
    if (g_usageLines[i].haveData && strcmp(g_usageLines[i].id, id) == 0) return &g_usageLines[i];
  }
  for (int i = 0; i < USAGE_MAX_ROWS; i++) {
    if (!g_usageLines[i].haveData) {
      strncpy(g_usageLines[i].id, id, sizeof(g_usageLines[i].id) - 1);
      g_usageLines[i].id[sizeof(g_usageLines[i].id) - 1] = '\0';
      return &g_usageLines[i];
    }
  }
  return nullptr;
}

// Fetches usage from the local bridge and updates g_usageLines in place. Runs on its own
// schedule (see loop()) while WiFi is on; a failure just leaves the last-known values in
// place - staleness is tracked separately via g_lastUsageFetchOkMs (last *successful*
// fetch), not fetch attempts, matching the handoff doc's ">15min since last update" rule.
//
// Plain HTTP to a machine on the LAN, 2026-07-16 - no TLS, no token, no heap-fragmentation
// risk here at all (see README.md for the full history of why the earlier direct-to-
// Anthropic HTTPS design needed a fast-fail heap check and a BLE-scan pause around this
// call; neither is needed any more, since there's no ~32KB TLS handshake happening
// on-device for this request).
static void fetchClaudeUsage() {
  if (g_bridgeAddr.isEmpty() || WiFi.status() != WL_CONNECTED) return;

  WiFiClient  client;
  HTTPClient  http;
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  if (!http.begin(client, "http://" + g_bridgeAddr + "/usage")) {
    Serial.println("[Usage] http.begin() failed");
    return;
  }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[Usage] HTTP %d from bridge\n", code);
    http.end();
    return;
  }

  // Buffered into a String and parsed from that, not streamed directly from http.getStream()
  // - confirmed live, 2026-07-15 (against the old direct-to-Anthropic endpoint, but nothing
  // about ArduinoJson's stream-parsing behavior changed with the new URL), that stream-
  // parsing failed with "InvalidInput" even after a successful HTTP 200, cause not fully
  // identified. The response is tiny (a few hundred bytes) so buffering it first costs
  // nothing meaningful, and this also lets a failure log the actual bytes received.
  String responseBody = http.getString();
  http.end();

  JsonDocument         doc;
  DeserializationError err = deserializeJson(doc, responseBody);
  if (err) {
    Serial.printf("[Usage] JSON parse error: %s - response body (%u bytes): %s\n", err.c_str(),
                  responseBody.length(), responseBody.c_str());
    return;
  }

  // Iterates whatever entry keys the bridge actually sent, rather than a fixed known-name
  // list - see UsageLine's own comment. label/period_seconds are trusted straight from the
  // bridge (it derives them from Anthropic's `limits` entries - see usage_bridge.py); this
  // loop only validates the fields it actually needs to render (utilization/resets_at).
  bool anyOk = false;
  for (JsonPair kv : doc.as<JsonObject>()) {
    JsonVariant entry = kv.value();
    if (entry.isNull()) continue;
    float       util          = entry["utilization"] | -1.0f;
    const char* resetsAtStr   = entry["resets_at"] | "";
    const char* labelStr      = entry["label"] | "";
    uint32_t    periodSeconds = entry["period_seconds"] | 0UL;
    if (util < 0 || !resetsAtStr[0] || !labelStr[0] || periodSeconds == 0) continue;
    time_t resetsAt = parseIso8601Utc(resetsAtStr);
    if (resetsAt == 0) continue;

    UsageLine* slot = findOrClaimSlot(kv.key().c_str());
    if (!slot) continue; // all USAGE_MAX_ROWS slots already claimed by other entries

    strncpy(slot->label, labelStr, sizeof(slot->label) - 1);
    slot->label[sizeof(slot->label) - 1] = '\0';
    slot->periodSeconds = periodSeconds;
    // Confirmed live, 2026-07-15: the real API returns utilization already as a percentage
    // (e.g. 43.0 for 43%) - the original example payload the user supplied showed a 0-1
    // fraction (0.42), which turned out not to match the live response. No *100 here.
    slot->pct      = util;
    slot->resetsAt = resetsAt;
    slot->haveData = true;
    anyOk          = true;
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

  // --- Claude usage zone (2026-07-15, see README.md; row count made dynamic 2026-07-15) ---
  // Geometry (track/label/pct column widths, marker/fill logic) is a 1:1 translation of the
  // handoff doc's buildLine()/makeFrame() JS - see that doc for the full derivation. Row gap
  // (8px in the doc) tightened twice after real-hardware feedback (8->5->3) - the user
  // asked for the three bars to sit closer together than the doc's original spacing. All 3
  // slots (five_hour/seven_day/seven_day_sonnet, matching g_usageLines) are pre-created here at their max-row
  // positions, but the zone starts hidden and every row starts wherever this loop puts it -
  // updateUsageZoneDisplay() is what actually decides visibility/position each refresh, based
  // on which buckets the last fetch actually had data for (see its own comment).
  usageZone = lv_obj_create(scr_clock);
  lv_obj_set_style_bg_opa(usageZone, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(usageZone, 0, 0);
  lv_obj_set_style_pad_all(usageZone, 0, 0);
  lv_obj_clear_flag(usageZone, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(usageZone, 304, USAGE_BAND_H);
  // Gap from the date (16) confirmed fine as-is on real hardware - only the group's overall
  // vertical position (label_clock_time's own top margin, above) needed to move, not this.
  // updateUsageZoneDisplay() re-runs this align every refresh with a row-count-dependent
  // offset once there's data; hidden by default here since nothing has been fetched yet.
  lv_obj_align_to(usageZone, label_clock_date, LV_ALIGN_OUT_BOTTOM_MID, 0, 16);
  lv_obj_add_flag(usageZone, LV_OBJ_FLAG_HIDDEN);

  for (int i = 0; i < USAGE_MAX_ROWS; i++) {
    int rowY = i * (USAGE_ROW_H + USAGE_ROW_GAP);

    usageRowLabel[i] = makeLabel(usageZone, 0, rowY + 8, &lv_font_montserrat_12, COLOR_USAGE_ASH, g_usageLines[i].label);
    lv_obj_set_width(usageRowLabel[i], USAGE_LABEL_W - 4);
    lv_label_set_long_mode(usageRowLabel[i], LV_LABEL_LONG_CLIP); // see USAGE_LABEL_W's comment

    usageTrack[i] = lv_obj_create(usageZone);
    lv_obj_set_pos(usageTrack[i], USAGE_LABEL_W, rowY + 3);
    lv_obj_set_size(usageTrack[i], USAGE_TRACK_W, USAGE_BAR_H);
    lv_obj_set_style_radius(usageTrack[i], USAGE_BAR_H / 2, 0);
    lv_obj_set_style_bg_color(usageTrack[i], COLOR_USAGE_SURFACE, 0);
    lv_obj_set_style_bg_opa(usageTrack[i], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(usageTrack[i], 0, 0);
    lv_obj_set_style_pad_all(usageTrack[i], 0, 0);
    lv_obj_clear_flag(usageTrack[i], LV_OBJ_FLAG_SCROLLABLE);

    // Fill is a separate stacked lv_obj, not a plain lv_bar - per-row fill color needs to
    // change (core/alert/desaturated) depending on state, recomputed every refresh in
    // updateUsageZoneDisplay(), which a single shared lv_bar style can't do per-instance as
    // cheaply. Originally a core->hot gradient per the handoff doc's design - dropped to a
    // flat color, 2026-07-15, at the user's explicit request after real, visible gradient
    // banding/striping couldn't be resolved (dithering and gradient caching were both tried
    // live on real hardware, neither fixed it) - the user doesn't want the color-illustrates-
    // usage gradient concept at all, a flat color per state is simpler and sidesteps the
    // rendering issue entirely rather than continuing to chase it.
    usageFill[i] = lv_obj_create(usageZone);
    lv_obj_set_pos(usageFill[i], USAGE_LABEL_W, rowY + 3);
    lv_obj_set_size(usageFill[i], 2, USAGE_BAR_H);
    lv_obj_set_style_radius(usageFill[i], USAGE_BAR_H / 2, 0);
    lv_obj_set_style_bg_color(usageFill[i], COLOR_USAGE_CORE, 0);
    lv_obj_set_style_bg_opa(usageFill[i], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(usageFill[i], 0, 0);
    lv_obj_set_style_pad_all(usageFill[i], 0, 0);
    lv_obj_clear_flag(usageFill[i], LV_OBJ_FLAG_SCROLLABLE);

    usageMarker[i] = lv_obj_create(usageZone);
    lv_obj_set_pos(usageMarker[i], USAGE_LABEL_W, rowY + 3 - 2);
    lv_obj_set_size(usageMarker[i], 2, USAGE_BAR_H + 4);
    lv_obj_set_style_radius(usageMarker[i], 1, 0);
    lv_obj_set_style_bg_color(usageMarker[i], COLOR_USAGE_ASH, 0);
    lv_obj_set_style_bg_opa(usageMarker[i], LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(usageMarker[i], 0, 0);
    lv_obj_set_style_pad_all(usageMarker[i], 0, 0);
    lv_obj_clear_flag(usageMarker[i], LV_OBJ_FLAG_SCROLLABLE);

    // Moved up beside the bar, 2026-07-16, into the column the pct text used to occupy
    // (same x/y this project's own now-removed usagePct[] used) - see USAGE_RESET_W's
    // comment. LV_LABEL_LONG_CLIP as a safety net here too: this column is narrower than
    // the old below-the-bar span, and "23t 59m" is longer than "100%" ever was.
    usageReset[i] = makeLabel(usageZone, USAGE_LABEL_W + USAGE_TRACK_W + 4, rowY + 5, &lv_font_montserrat_12, COLOR_USAGE_ASH, "");
    lv_obj_set_width(usageReset[i], USAGE_RESET_W - 4);
    lv_obj_set_style_text_align(usageReset[i], LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(usageReset[i], LV_LABEL_LONG_CLIP);
  }
}

// Recomputes and applies the usage zone's row count/positions plus every visible row's fill
// width/color, pace-marker position, pct text/color, and resets-in text - called once a
// second alongside the clock tick (see loop()) so the pace marker and countdown stay live
// between fetches, not just on a new fetch. period_pct (pace-marker position) is derived
// here from resetsAt + the known period length + now, not read from the API - see
// fetchClaudeUsage()'s UsageLine comment.
//
// Row count/order (2026-07-15, per the user's explicit next-steps request): built fresh
// every call from which of g_usageLines[] actually haveData, in fixed five_hour/seven_day/
// seven_day_sonnet order with
// any missing bucket skipped rather than shown as an empty placeholder - the visible rows
// are then compacted (no gap left where a missing one would be) and the whole stack
// re-centered in the fixed USAGE_BAND_H budget, matching the handoff doc's "fewer rows = more
// centered whitespace" 2-line spec.
//
// Whole zone hidden - not just desaturated with a "stale" tag - whenever there's no data at
// all OR the last successful fetch is more than USAGE_STALE_AFTER_MS old, 2026-07-16 (see
// README.md): now that the data comes from a bridge script on the user's own Mac rather than
// directly from Anthropic, "stale" routinely means "that Mac is asleep/off," a normal and
// expected state, not a rare failure - showing old percentages as if current in that case
// would be actively misleading, per the user's explicit call ("det vil jo være outdated").
// This replaced an earlier design (dim the bars + a "FORSINKET" corner tag, numbers still
// visible) that this superseded.
static void updateUsageZoneDisplay() {
  time_t now = time(nullptr);

  int visible[USAGE_MAX_ROWS];
  int visCount = 0;
  for (int i = 0; i < USAGE_MAX_ROWS; i++) {
    if (g_usageLines[i].haveData) visible[visCount++] = i;
  }

  bool stale = (g_lastUsageFetchOkMs == 0) || (millis() - g_lastUsageFetchOkMs > USAGE_STALE_AFTER_MS);
  if (visCount == 0 || stale) {
    lv_obj_add_flag(usageZone, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_clear_flag(usageZone, LV_OBJ_FLAG_HIDDEN);

  int zoneH = visCount * USAGE_ROW_H + (visCount - 1) * USAGE_ROW_GAP;
  lv_obj_set_height(usageZone, zoneH);
  int yOffset = 16 + (USAGE_BAND_H - zoneH) / 2;
  lv_obj_align_to(usageZone, label_clock_date, LV_ALIGN_OUT_BOTTOM_MID, 0, yOffset);

  bool shown[USAGE_MAX_ROWS] = {false, false, false};

  for (int k = 0; k < visCount; k++) {
    int        i = visible[k];
    UsageLine& l = g_usageLines[i];
    shown[i]     = true;
    int rowY     = k * (USAGE_ROW_H + USAGE_ROW_GAP);

    lv_obj_clear_flag(usageRowLabel[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(usageTrack[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(usageFill[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(usageMarker[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(usageReset[i], LV_OBJ_FLAG_HIDDEN);

    lv_obj_set_y(usageRowLabel[i], rowY + 8);
    lv_obj_set_y(usageTrack[i], rowY + 3);
    lv_obj_set_y(usageFill[i], rowY + 3);
    lv_obj_set_y(usageReset[i], rowY + 5);

    // Label text is set here, not just once at build time, because which bucket key a slot
    // holds is no longer known at build time (see UsageLine's comment) - it's only known
    // once fetchClaudeUsage() actually claims the slot. Cheap to call every refresh either
    // way (lv_label_set_text() is a no-op internally when the text hasn't changed).
    lv_label_set_text(usageRowLabel[i], l.label);

    long secsLeft = (long)(l.resetsAt - now);
    if (secsLeft < 0) secsLeft = 0;
    float periodPct = 100.0f * (1.0f - (float)secsLeft / (float)l.periodSeconds);
    if (periodPct < 0.0f) periodPct = 0.0f;
    if (periodPct > 100.0f) periodPct = 100.0f;

    // Once resetsAt has actually passed, l.pct is stale - it's still the *previous* period's
    // last-fetched figure (e.g. 98%) until fetchClaudeUsage()'s next 60s cycle confirms the
    // new period's real usage. Showing that stale near-full number past a reset would be
    // actively misleading (the fresh period has used ~nothing yet) - assume 0% locally for
    // display until a real fetch reports a later resetsAt again. l.pct itself is untouched
    // here (only fetchClaudeUsage() ever writes it) - this is a display-only override.
    bool  periodExpired = (l.resetsAt <= now);
    float displayPct    = periodExpired ? 0.0f : l.pct;

    bool critical = displayPct >= 90.0f;
    bool depleted = displayPct >= 100.0f;
    bool overPace = displayPct > periodPct;

    int fillW = (int)(USAGE_TRACK_W * displayPct / 100.0f);
    if (fillW < 2) fillW = 2;
    if (fillW > USAGE_TRACK_W) fillW = USAGE_TRACK_W;
    lv_obj_set_width(usageFill[i], fillW);

    // Flat fill color by state - no gradient (see buildClockUi()'s comment for why). No
    // "stale" branch here any more - the whole zone is hidden before this loop runs at all
    // once data is stale (see this function's own comment), so every row reached here is
    // known-fresh.
    lv_color_t fillColor = (critical || overPace) ? COLOR_USAGE_ALERT : COLOR_USAGE_CORE;
    lv_obj_set_style_bg_color(usageFill[i], fillColor, 0);

    int markerX = (int)(USAGE_TRACK_W * periodPct / 100.0f) - 1;
    if (markerX < 0) markerX = 0;
    lv_obj_set_pos(usageMarker[i], USAGE_LABEL_W + markerX, rowY + 3 - 2);

    // English "Xd Yh" (days/hours) or "Xh Ym" (hours/minutes) - was Danish ("Xd Yt"/"Xt Ym",
    // matching the handoff doc's own "1t 51m"/"4d 9t" examples) until the 2026-07-16
    // English-only pass; "d" is unchanged (means the same in both languages) but Danish
    // "t" (timer/hours) is now "h".
    char resetBuf[24];
    if (secsLeft >= 86400) {
      snprintf(resetBuf, sizeof(resetBuf), "%ldd %ldh", secsLeft / 86400, (secsLeft % 86400) / 3600);
    } else {
      snprintf(resetBuf, sizeof(resetBuf), "%ldh %ldm", secsLeft / 3600, (secsLeft % 3600) / 60);
    }
    lv_label_set_text(usageReset[i], resetBuf);
    lv_obj_set_style_text_color(usageReset[i], depleted ? COLOR_USAGE_TEXT : COLOR_USAGE_ASH, 0);
  }

  for (int i = 0; i < USAGE_MAX_ROWS; i++) {
    if (shown[i]) continue;
    lv_obj_add_flag(usageRowLabel[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usageTrack[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usageFill[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usageMarker[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(usageReset[i], LV_OBJ_FLAG_HIDDEN);
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

  Serial.printf("[BOOT] Free heap after display+LVGL init: %u bytes, largest_block=%u\n", ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
  {
    // One-off measurement, 2026-07-15, to inform whether LV_MEM_SIZE (currently 40KB,
    // lv_conf.h) is over-provisioned for this project's actual widget count, as part of
    // freeing more contiguous headroom for the Claude usage fetch's TLS handshake (see
    // fetchClaudeUsage()) - all three screens (dashboard, clock incl. the usage zone,
    // WiFi config) are built by this point, so this reflects real peak usage, not a guess.
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    Serial.printf("[BOOT] LVGL pool: %u total, %u used (%u%%), %u free, biggest free block %u\n",
                  (unsigned)mon.total_size, (unsigned)(mon.total_size - mon.free_size), mon.used_pct,
                  (unsigned)mon.free_size, (unsigned)mon.free_biggest_size);
  }

  // WiFi + NTP (experimental, see file header) - kicked off once at boot, then left running
  // continuously alongside BLE, including while a Pinecil is connected (see loop(), 2026-07-16 -
  // this used to turn WiFi off for that whole duration; no longer does, see loop()'s comment).
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
  g_timezone   = prefs.getString("tz", DEFAULT_TZ);
  g_bridgeAddr = prefs.getString("bridge_addr", "");
  prefs.end();
  static WiFiManagerParameter tzParam("tz", "Timezone (POSIX TZ string)", g_timezone.c_str(), 63);
  pTzParam = &tzParam;
  wm.addParameter(&tzParam);
  // Pre-filled with the current value, unlike the OAuth token field this replaced - not a
  // secret, so nothing lost by showing it back (see onWifiConfigSaved()'s comment).
  static WiFiManagerParameter bridgeParam("bridge_addr", "Usage bridge address (host:port, blank = keep existing)",
                                           g_bridgeAddr.c_str(), 64);
  pBridgeParam = &bridgeParam;
  wm.addParameter(&bridgeParam);

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
  Serial.printf("[BOOT] Free heap after WiFi init: %u bytes, largest_block=%u\n", ESP.getFreeHeap(),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

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

  // Switch screens on the connected/disconnected transition (see buildClockUi()) - purely a
  // display choice now, 2026-07-16. This used to also toggle WiFi off while a Pinecil was
  // connected (recovers ~83KB heap, avoids WiFi/BT coexistence contention that measurably
  // slowed BLE polling - see README.md, and the 2026-07-15 always-on experiment that was
  // reverted for that same latency cost: cycle_ms ~49-137ms vs. ~54-70ms with the toggle).
  // Removed now that the Pinecil config page (docs/pinecil-config-page/) needs WiFi
  // reachable *while* connected, which the old toggle made impossible by construction. The
  // latency cost is accepted, not overlooked - see README.md; the heap-fragmentation problem
  // this toggle also used to help with is fixed differently now (the usage-bridge
  // architecture), so nothing here is a regression on that front.
  static uint32_t lastWifiAttempt = 0; // shared with the retry block below
  bool            shouldShowClock = !connected;
  if (shouldShowClock != clockScreenActive) {
    clockScreenActive = shouldShowClock;
    lv_scr_load(clockScreenActive ? scr_clock : scr_dashboard);
  }

  // Services the non-blocking captive portal, OTA, and the settings/config webserver -
  // unconditionally every tick now (used to only run while showing the clock screen, back
  // when WiFi was off for the entire time a Pinecil was connected - see above). Near-zero
  // cost when nothing's actually pending, same shape as lv_timer_handler() at the top of
  // loop().
  wm.process();
  if (otaStarted) {
    if (!otaInProgress && millis() - otaStartedAtMs > OTA_WINDOW_MS) {
      Serial.println("[OTA] Listening window closed - freeing its resources (reboot to reopen)");
      ArduinoOTA.end();
      otaStarted = false;
    } else {
      ArduinoOTA.handle();
    }
  }
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
    // Load whichever screen is actually correct, not unconditionally the clock - the portal
    // can in principle now open while a Pinecil is connected (a never-configured device
    // could hit this if a Pinecil connects during that very first autoConnect() attempt),
    // since WiFi no longer turns off for that.
    lv_scr_load(clockScreenActive ? scr_clock : scr_dashboard);
    Serial.println("[WiFi] Config portal closed - resuming Pinecil scan");
    NimBLEDevice::getScan()->start(0, false, true);
  }

  // NTP only needs (re)configuring once per WiFi (re)connection, detected here rather than
  // blocking loop() while (re)connecting the way setup()'s initial connect does. Also where
  // credentials get cached in RAM for connectWifiNow()'s fast path (see g_wifiSsid/g_wifiPsk).
  static bool wifiWasConnected = false;
  bool        wifiNowConnected = (WiFi.status() == WL_CONNECTED);
  if (wifiNowConnected && !wifiWasConnected) {
    Serial.printf("[WiFi] Connected, IP=%s - configuring NTP\n", WiFi.localIP().toString().c_str());
    configTzTime(g_timezone.c_str(), "pool.ntp.org", "time.google.com");
    // Boot-checkpoint largest-block trace, 2026-07-15 (temporary - see USAGE_FETCH_MIN_
    // CONTIGUOUS_HEAP's comment): logs the contiguous headroom before/after each of
    // mDNS/OTA/settings-server init, to find which one (if any) is responsible for the drop
    // from the originally-confirmed ~59-61KB baseline down to a now-reproducible ~45KB.
    Serial.printf("[Heap] Before mDNS/OTA/settings: largest_block=%u free=%u\n",
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), ESP.getFreeHeap());
    if (MDNS.begin(OTA_HOSTNAME)) {
      Serial.printf("[WiFi] mDNS ready: http://%s.local/\n", OTA_HOSTNAME);
    }
    Serial.printf("[Heap] After mDNS: largest_block=%u free=%u\n",
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), ESP.getFreeHeap());
    otaBegin();
    Serial.printf("[Heap] After otaBegin(): largest_block=%u free=%u\n",
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), ESP.getFreeHeap());
    settingsServerBegin();
    Serial.printf("[Heap] After settingsServerBegin(): largest_block=%u free=%u\n",
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), ESP.getFreeHeap());
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
                              // every reconnect from here on (e.g. a real drop-and-retry)
      Serial.println("[WiFi] Cached credentials in RAM for future reconnects");
    }
  } else if (!wifiNowConnected && wifiWasConnected) {
    // A real WiFi drop (router reboot, out of range) - not the old connected-Pinecil toggle,
    // that's gone (see above). OTA's listener and the settings/config webserver don't
    // reliably survive a STA disconnect/reconnect (the same reason the old toggle always
    // reset these, just triggered by something else now) - re-arm them next time
    // wifiNowConnected flips true again, same as a fresh boot would.
    Serial.println("[WiFi] Connection lost - will re-init mDNS/OTA/settings server on reconnect");
    MDNS.end();
    otaStarted            = false;
    settingsServerStarted = false;
  }
  wifiWasConnected = wifiNowConnected;

  // Retry cadence per the user's spec, 2026-07-13: if we're not connected and no portal is
  // open (i.e. a prior attempt's 30s connect timeout and, if triggered, its 120s portal
  // window both ran out unconfigured), try the whole thing again every 2 minutes rather
  // than giving up for the rest of the session. No longer gated to the clock screen,
  // 2026-07-16 - WiFi should keep retrying even while a Pinecil is connected and the
  // dashboard is showing, not just while looking at the clock.
  if (!wifiNowConnected && !wm.getConfigPortalActive() &&
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
      static const char* EN_WEEKDAYS[7] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"}; // tm_wday: 0=Sunday
      static const char* EN_MONTHS[12]  = {"jan", "feb", "mar", "apr", "may", "jun",
                                            "jul", "aug", "sep", "oct", "nov", "dec"};
      lv_label_set_text_fmt(label_clock_date, "%s %d %s", EN_WEEKDAYS[timeinfo.tm_wday], timeinfo.tm_mday,
                             EN_MONTHS[timeinfo.tm_mon]);
    }
    // Recomputed every tick (not just on fetch) so the pace marker and countdown stay live
    // between the 60s fetch cycles below - see updateUsageZoneDisplay()'s own comment.
    updateUsageZoneDisplay();
  }

  // Claude usage fetch cadence (2026-07-15): only while the clock is showing (WiFi on) and
  // a bridge address has been configured. Not tied to the clock tick above - usage doesn't
  // need per-second freshness, and this keeps the two concerns (display refresh vs. network
  // fetch) independently tunable.
  static uint32_t lastUsageFetch = 0;
  if (clockScreenActive && !g_bridgeAddr.isEmpty() && wifiNowConnected &&
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
    // largest_block added 2026-07-15 while chasing why the Claude usage fetch's largest
    // contiguous block dropped from an earlier-confirmed ~59-61KB down to a now-reproducible
    // ~45KB - free heap alone (heap=) doesn't show fragmentation, this does.
    Serial.printf("[HEARTBEAT] uptime_ms=%lu connected=%d disconnects=%u heap=%u min_heap=%u largest_block=%u wifi=%d rssi=%d\n",
                  millis(), connected, disconnectCount, ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), WiFi.status() == WL_CONNECTED,
                  WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  }

  delay(5);
}
