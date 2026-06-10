/*
 * app_http_trigger.cpp — HTTP Request Sender, retro pixel-display style
 *
 * Widgets:
 *   button  — 1–3 stacked horizontal pills, each fires its own URL
 *   toggle  — two stacked pills (ON / OFF)
 *   slider  — vertical pill-bar column
 *   display — 1–3 fetched values, refresh on tap
 *
 * Screen layout configured via /setup/http-trigger.txt (INI-style sections).
 * WiFi credentials still live in /setup/setup.txt next to the launcher config.
 */

#include "app_http_trigger.h"
#include "app_common.h"
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <SD_MMC.h>
#include <FS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "canvas/Arduino_Canvas.h"
#include "pin_config.h"
#include "HWCDC.h"
#include <Adafruit_XCA9554.h>
#include "TouchDrvFT6X36.hpp"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// Root-CA bundle embedded by the ESP32 Arduino build (Mozilla roots, ~140 kB).
// Lets WiFiClientSecure verify public-CA HTTPS endpoints (CoinGecko, GitHub,
// Home Assistant Cloud, Tado, Pushover, …) without shipping a custom bundle.
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[]   asm("_binary_x509_crt_bundle_end");

// USB MSC is only available in TinyUSB mode (USBMode=default + CDCOnBoot=cdc).
#if !ARDUINO_USB_MODE && SOC_USB_OTG_SUPPORTED
  #define REMOTE_HAS_USB_MSC 1
  #include <USB.h>
  #include <USBMSC.h>
#else
  #define REMOTE_HAS_USB_MSC 0
#endif

extern USBCDC USBSerial;
extern Arduino_Canvas *g_canvas;

// ── Constants ────────────────────────────────────────────────────────────────
#define MAX_SCREENS  16
#define MAX_ITEMS    3
#define BOOT_BTN     0
#define DW           368
#define DH           448

#define COL_BG       0x0000
#define COL_WHITE    0xFFFF
#define COL_DIM      0x4208
#define COL_FAINT    0x18C3
#define COL_GREY     0x7BEF
#define COL_LGREY    0xC618
#define COL_GREEN    0x2FE4
#define COL_RED      0xF800
#define COL_DIVIDER  0x2945
#define COL_DOTS     0x6B6D
#define COL_AMBER    0xFD20

// canvas layout
#define MARGIN_X      22
#define TOP_Y         40
#define BOTTOM_Y      405
#define WIDGET_W      (DW - MARGIN_X * 2 - 14)
#define DROP_OFFSET_X 4
#define DROP_OFFSET_Y 10
#define COL_SHADOW    0x4208

#define RESPONSE_BANNER_MS  2200
#define SLIDER_DEBOUNCE_MS  1000

// Unified hero text size — pixelated, applied to every value/label that sits
// inside a pill (buttons, toggle labels, slider value, display values).
#define HERO_SX        4
#define HERO_SY        5
#define HERO_PX        1
#define HERO_CHAR_W    25     // sx*6 + px
#define HERO_CHAR_H    41     // sy*8 + px

// Swipe gesture thresholds
#define SWIPE_MIN_PX   60
#define SWIPE_MAX_MS   600
#define MODE_LOCK_PX   18

// Default periodic refresh — overridable via INFO_REFRESH_M in setup.txt
#define REFRESH_DEFAULT_MIN  15

// ── Globals ──────────────────────────────────────────────────────────────────
static Arduino_Canvas   *canvas = nullptr;
static Adafruit_XCA9554  expander;
static TouchDrvFT6X36    s_touch;
static Preferences       s_prefs;

enum ScreenType { SC_NONE = 0, SC_BUTTON, SC_TOGGLE, SC_SLIDER, SC_DISPLAY };

// One request endpoint with its own runtime state. For BUTTON / DISPLAY each
// sub-element on a screen is an Item. For TOGGLE / SLIDER the screen carries
// the config in its own fields and items[0] is just the runtime carrier (so
// the banner / busy logic stays uniform).
struct Item {
    // Config
    char     label[40];
    char     method[8];
    char     url[200];
    char     body[160];
    char     headers[3][120];
    char     jsonPath[64];        // display only
    char     unit[10];            // display / slider
    uint8_t  decimals;            // display / slider
    // Optional status polling (buttons): GET URL + json_path that yields a
    // bool. White when true/unknown, grey when false.
    char     statusUrl[200];
    char     statusJsonPath[64];
    volatile bool statusValue;    // last-known polled bool
    volatile bool statusKnown;    // becomes true after the first successful poll
    // Runtime
    char     displayCache[64];
    uint32_t lastFetchMs;
    int      lastStatus;
    char     lastResponse[40];
    bool     hasResponse;
    uint32_t responseShownAt;
    volatile bool busy;
};

struct Screen {
    ScreenType type;
    char       label[40];          // header text

    // BUTTON / DISPLAY have 1..MAX_ITEMS. TOGGLE / SLIDER use items[0] only
    // as a runtime carrier; their config sits in dedicated fields below.
    Item       items[MAX_ITEMS];
    int        nItems;

    // TOGGLE config
    char       labelOn[16];
    char       labelOff[16];
    char       urlOn[200];
    char       urlOff[200];
    char       bodyOn[160];
    char       bodyOff[160];
    char       tMethod[8];
    char       tHeaders[3][120];
    bool       toggleOn;
    // Optional status polling: when set, server-truth feeds back into
    // toggleOn so the active-half always reflects reality.
    char       statusUrl[200];
    char       statusJsonPath[64];

    // SLIDER config
    int        minVal;
    int        maxVal;
    int        step;
    int        sliderValue;
};

static Screen   s_screens[MAX_SCREENS];
static int      s_nScreens  = 0;
static int      s_screenIdx = 0;

static char     cfg_ssid[3][64] = {};
static char     cfg_pass[3][64] = {};
static bool     s_wifiOk = false;

// touch / drag state
static bool     s_touchHeld = false;
static bool     s_dragActive = false;
static int      s_pendingSlider = INT32_MIN;
static bool     s_releaseScheduled  = false;
static uint32_t s_releaseScheduledAt = 0;

// Button press state — index of the pressed item (-1 = none)
static int      s_btnPressing = -1;

// Touch gesture state — used to disambiguate swipe vs. tap/drag.
enum TouchMode { TM_UNKNOWN, TM_VERTICAL, TM_HORIZONTAL };
static int16_t   s_touchStartX = 0;
static int16_t   s_touchStartY = 0;
static int16_t   s_touchLastX  = 0;
static int16_t   s_touchLastY  = 0;
static uint32_t  s_touchStartMs = 0;
static TouchMode s_touchMode    = TM_UNKNOWN;

// Periodic display-refresh
static uint32_t s_refreshIntervalMs = REFRESH_DEFAULT_MIN * 60UL * 1000UL;
static uint32_t s_lastRefreshMs     = 0;

// HTTPS cert verification — default ON. Users with self-signed-cert servers
// on their LAN can opt out with HTTPS_INSECURE = true in setup.txt.
static bool     s_httpsInsecure     = false;

// BOOT short detection — ISR-latched
static volatile bool     s_bootShortPending = false;
static volatile uint32_t s_bootLastIsrMs    = 0;
static void IRAM_ATTR bootISR() {
    uint32_t t = millis();
    if (t - s_bootLastIsrMs > 30) {
        s_bootShortPending = true;
        s_bootLastIsrMs    = t;
    }
}

static bool     s_dirty = true;
static uint32_t s_lastDraw = 0;

// ── Tiny string helpers ─────────────────────────────────────────────────────
static void copyStr(char *dst, size_t cap, const char *src) {
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = '\0';
}

static void trimEnds(char *s) {
    if (!s) return;
    char *p = s;
    while (*p == ' ' || *p == '\t') p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r')) {
        s[--n] = '\0';
    }
}

static ScreenType parseType(const char *s) {
    if (!strcasecmp(s, "button"))  return SC_BUTTON;
    if (!strcasecmp(s, "toggle"))  return SC_TOGGLE;
    if (!strcasecmp(s, "slider"))  return SC_SLIDER;
    if (!strcasecmp(s, "display")) return SC_DISPLAY;
    return SC_NONE;
}

// ── WiFi credentials + global settings from /setup/setup.txt ────────────────
static void readSetupGlobals() {
    File f = SD_MMC.open("/setup/setup.txt");
    if (!f) return;
    char line[200];
    while (f.available()) {
        int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = '\0';
        auto trySet = [&](const char *key, char *dst, size_t cap) {
            const char *kp = strstr(line, key);
            if (!kp) return;
            if (kp != line && !isspace((unsigned char)kp[-1])) return;
            char after = kp[strlen(key)];
            if (isalnum((unsigned char)after) || after == '_') return;
            const char *v = kp + strlen(key);
            while (*v == ' ' || *v == '=') v++;
            if (*v == '"') v++;
            size_t i = 0;
            while (*v && *v != '\n' && *v != '\r' && *v != '"' && *v != '#' && i < cap - 1) {
                dst[i++] = *v++;
            }
            while (i > 0 && (dst[i-1] == ' ' || dst[i-1] == '\t')) i--;
            dst[i] = '\0';
        };
        trySet("SSID",      cfg_ssid[0], 64);
        trySet("PASSWORD",  cfg_pass[0], 64);
        trySet("SSID2",     cfg_ssid[1], 64);
        trySet("PASSWORD2", cfg_pass[1], 64);
        trySet("SSID3",     cfg_ssid[2], 64);
        trySet("PASSWORD3", cfg_pass[2], 64);
        // Refresh interval (minutes, 1..1440). Default 15.
        char buf[16] = "";
        trySet("INFO_REFRESH_M", buf, sizeof(buf));
        if (buf[0]) {
            int mins = atoi(buf);
            if (mins >= 1 && mins <= 1440) {
                s_refreshIntervalMs = (uint32_t)mins * 60UL * 1000UL;
            }
        }
        // HTTPS_INSECURE = true/yes/on/1 → skip cert verification (LAN with
        // self-signed certs). Default false (verify against the embedded
        // Mozilla root CA bundle).
        buf[0] = '\0';
        trySet("HTTPS_INSECURE", buf, sizeof(buf));
        if (buf[0]) {
            if (!strcasecmp(buf, "true") || !strcasecmp(buf, "yes") ||
                !strcasecmp(buf, "on")   || !strcasecmp(buf, "1")) {
                s_httpsInsecure = true;
            }
        }
    }
    f.close();
}

// ── INI-style parser for /setup/http-trigger.txt ──────────────────────────────────
// Sections recognised:
//   [screen]            — opens a new screen; `type` is required
//   [button]            — only under a button screen; up to 3 per screen
//   [value]             — only under a display screen; up to 3 per screen
// Keys are case-insensitive (documented form is lower-case). Comments use
// `#` or `;` and may appear at line start. Optional quotes around values
// are stripped. Indentation is purely cosmetic.
static bool readConfig() {
    File f = SD_MMC.open("/setup/http-trigger.txt");
    if (!f) return false;

    char line[400];
    enum Section { SEC_NONE, SEC_SCREEN, SEC_ITEM };
    Section  sec      = SEC_NONE;
    Screen  *curS     = nullptr;
    Item    *curI     = nullptr;
    bool     warned   = false;

    while (f.available()) {
        int n = f.readBytesUntil('\n', line, sizeof(line) - 1);
        line[n] = '\0';
        trimEnds(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') continue;

        // Section header
        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (!end) continue;
            *end = '\0';
            char *name = line + 1;
            trimEnds(name);
            // Lowercase
            for (char *c = name; *c; c++) *c = tolower((unsigned char)*c);

            if (!strcmp(name, "screen")) {
                if (s_nScreens >= MAX_SCREENS) { curS = nullptr; curI = nullptr; sec = SEC_NONE; continue; }
                curS = &s_screens[s_nScreens++];
                memset(curS, 0, sizeof(Screen));
                curI = nullptr;
                sec  = SEC_SCREEN;
            } else if (!strcmp(name, "button")) {
                if (!curS || curS->type != SC_BUTTON || curS->nItems >= MAX_ITEMS) {
                    curI = nullptr; sec = SEC_NONE; continue;
                }
                curI = &curS->items[curS->nItems++];
                memset(curI, 0, sizeof(Item));
                // Inherit screen-level shared method/headers if set on items[0]'s prototype
                // (we use curS->tMethod / tHeaders to stash screen-level defaults during parse)
                if (curS->tMethod[0])     copyStr(curI->method,     sizeof(curI->method),     curS->tMethod);
                if (curS->tHeaders[0][0]) copyStr(curI->headers[0], sizeof(curI->headers[0]), curS->tHeaders[0]);
                if (curS->tHeaders[1][0]) copyStr(curI->headers[1], sizeof(curI->headers[1]), curS->tHeaders[1]);
                if (curS->tHeaders[2][0]) copyStr(curI->headers[2], sizeof(curI->headers[2]), curS->tHeaders[2]);
                sec = SEC_ITEM;
            } else if (!strcmp(name, "value")) {
                if (!curS || curS->type != SC_DISPLAY || curS->nItems >= MAX_ITEMS) {
                    curI = nullptr; sec = SEC_NONE; continue;
                }
                curI = &curS->items[curS->nItems++];
                memset(curI, 0, sizeof(Item));
                if (curS->tMethod[0])     copyStr(curI->method,     sizeof(curI->method),     curS->tMethod);
                if (curS->tHeaders[0][0]) copyStr(curI->headers[0], sizeof(curI->headers[0]), curS->tHeaders[0]);
                if (curS->tHeaders[1][0]) copyStr(curI->headers[1], sizeof(curI->headers[1]), curS->tHeaders[1]);
                if (curS->tHeaders[2][0]) copyStr(curI->headers[2], sizeof(curI->headers[2]), curS->tHeaders[2]);
                sec = SEC_ITEM;
            } else {
                if (!warned) { USBSerial.printf("http-trigger.txt: unknown section [%s]\n", name); warned = true; }
                curI = nullptr; sec = SEC_NONE;
            }
            continue;
        }

        // Key = value
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trimEnds(key);
        trimEnds(val);
        // Strip wrapping quotes
        size_t vlen = strlen(val);
        if (vlen >= 2 && val[0] == '"' && val[vlen-1] == '"') {
            val[vlen-1] = '\0';
            val++;
        }
        // Lowercase the key
        for (char *c = key; *c; c++) *c = tolower((unsigned char)*c);

        if (sec == SEC_NONE || !curS) continue;

        if (sec == SEC_SCREEN) {
            // Generic
            if (!strcmp(key, "type"))           { curS->type = parseType(val); continue; }
            if (!strcmp(key, "label"))          { copyStr(curS->label,   sizeof(curS->label),   val); continue; }
            // Shared method / headers — apply to subsequent items in this screen
            if (!strcmp(key, "method"))         { copyStr(curS->tMethod,     sizeof(curS->tMethod),     val); }
            if (!strcmp(key, "header"))         { copyStr(curS->tHeaders[0], sizeof(curS->tHeaders[0]), val); }
            if (!strcmp(key, "header2"))        { copyStr(curS->tHeaders[1], sizeof(curS->tHeaders[1]), val); }
            if (!strcmp(key, "header3"))        { copyStr(curS->tHeaders[2], sizeof(curS->tHeaders[2]), val); }
            // Toggle
            if (curS->type == SC_TOGGLE) {
                if (!strcmp(key, "label_on"))         copyStr(curS->labelOn,        sizeof(curS->labelOn),        val);
                if (!strcmp(key, "label_off"))        copyStr(curS->labelOff,       sizeof(curS->labelOff),       val);
                if (!strcmp(key, "url_on"))           copyStr(curS->urlOn,          sizeof(curS->urlOn),          val);
                if (!strcmp(key, "url_off"))          copyStr(curS->urlOff,         sizeof(curS->urlOff),         val);
                if (!strcmp(key, "body_on"))          copyStr(curS->bodyOn,         sizeof(curS->bodyOn),         val);
                if (!strcmp(key, "body_off"))         copyStr(curS->bodyOff,        sizeof(curS->bodyOff),        val);
                if (!strcmp(key, "status_url"))       copyStr(curS->statusUrl,      sizeof(curS->statusUrl),      val);
                if (!strcmp(key, "status_json_path")) copyStr(curS->statusJsonPath, sizeof(curS->statusJsonPath), val);
            }
            // Slider — config goes into items[0] for URL/body/headers/method,
            // and into screen fields for min/max/step. items[0].unit / decimals
            // describe how to format sliderValue.
            if (curS->type == SC_SLIDER) {
                if (curS->nItems == 0) {
                    memset(&curS->items[0], 0, sizeof(Item));
                    curS->nItems = 1;
                }
                Item &si = curS->items[0];
                if (!strcmp(key, "min"))        curS->minVal = atoi(val);
                if (!strcmp(key, "max"))        curS->maxVal = atoi(val);
                if (!strcmp(key, "step"))       curS->step   = atoi(val);
                if (!strcmp(key, "url"))        copyStr(si.url,        sizeof(si.url),        val);
                if (!strcmp(key, "body"))       copyStr(si.body,       sizeof(si.body),       val);
                if (!strcmp(key, "method"))     copyStr(si.method,     sizeof(si.method),     val);
                if (!strcmp(key, "header"))     copyStr(si.headers[0], sizeof(si.headers[0]), val);
                if (!strcmp(key, "header2"))    copyStr(si.headers[1], sizeof(si.headers[1]), val);
                if (!strcmp(key, "header3"))    copyStr(si.headers[2], sizeof(si.headers[2]), val);
                if (!strcmp(key, "unit"))       copyStr(si.unit,       sizeof(si.unit),       val);
                if (!strcmp(key, "decimals"))   si.decimals = (uint8_t)atoi(val);
            }
            continue;
        }

        if (sec == SEC_ITEM && curI) {
            if (!strcmp(key, "label"))             copyStr(curI->label,          sizeof(curI->label),          val);
            else if (!strcmp(key, "method"))         copyStr(curI->method,         sizeof(curI->method),         val);
            else if (!strcmp(key, "url"))            copyStr(curI->url,            sizeof(curI->url),            val);
            else if (!strcmp(key, "body"))           copyStr(curI->body,           sizeof(curI->body),           val);
            else if (!strcmp(key, "header"))         copyStr(curI->headers[0],     sizeof(curI->headers[0]),     val);
            else if (!strcmp(key, "header2"))        copyStr(curI->headers[1],     sizeof(curI->headers[1]),     val);
            else if (!strcmp(key, "header3"))        copyStr(curI->headers[2],     sizeof(curI->headers[2]),     val);
            else if (!strcmp(key, "json_path"))      copyStr(curI->jsonPath,       sizeof(curI->jsonPath),       val);
            else if (!strcmp(key, "unit"))           copyStr(curI->unit,           sizeof(curI->unit),           val);
            else if (!strcmp(key, "decimals"))       curI->decimals = (uint8_t)atoi(val);
            else if (!strcmp(key, "status_url"))     copyStr(curI->statusUrl,      sizeof(curI->statusUrl),      val);
            else if (!strcmp(key, "status_json_path"))copyStr(curI->statusJsonPath,sizeof(curI->statusJsonPath), val);
        }
    }
    f.close();

    // Post-process / defaults / validation
    int kept = 0;
    for (int i = 0; i < s_nScreens; i++) {
        Screen &s = s_screens[i];
        if (s.type == SC_NONE) continue;
        if (s.type == SC_BUTTON  && s.nItems == 0) { USBSerial.printf("http-trigger.txt: button screen %d has no [button]\n", i); continue; }
        if (s.type == SC_DISPLAY && s.nItems == 0) { USBSerial.printf("http-trigger.txt: display screen %d has no [value]\n",  i); continue; }
        if (s.type == SC_TOGGLE) {
            if (!s.labelOn[0])  strcpy(s.labelOn,  "ON");
            if (!s.labelOff[0]) strcpy(s.labelOff, "OFF");
            if (!s.tMethod[0])  strcpy(s.tMethod,  "POST");
            s.nItems = 1;                              // runtime carrier
            memset(&s.items[0], 0, sizeof(Item));
        }
        if (s.type == SC_SLIDER) {
            if (s.minVal == 0 && s.maxVal == 0) s.maxVal = 100;
            if (s.step <= 0) s.step = 1;
            if (s.nItems == 0) { memset(&s.items[0], 0, sizeof(Item)); s.nItems = 1; }
            if (!s.items[0].method[0]) strcpy(s.items[0].method, "POST");
        }
        if (s.type == SC_BUTTON || s.type == SC_DISPLAY) {
            // Per-item method default
            const char *defM = (s.type == SC_DISPLAY) ? "GET" : "POST";
            for (int k = 0; k < s.nItems; k++) {
                if (!s.items[k].method[0]) strcpy(s.items[k].method, defM);
            }
        }
        if (kept != i) s_screens[kept] = s;
        kept++;
    }
    s_nScreens = kept;
    return s_nScreens > 0;
}

// ── Persistence ──────────────────────────────────────────────────────────────
static void prefsLoadStates() {
    s_prefs.begin("http-trigger", true);
    for (int i = 0; i < s_nScreens; i++) {
        Screen &s = s_screens[i];
        char key[16];
        if (s.type == SC_TOGGLE) {
            snprintf(key, sizeof(key), "t%d", i);
            s.toggleOn = s_prefs.getBool(key, false);
        } else if (s.type == SC_SLIDER) {
            snprintf(key, sizeof(key), "s%d", i);
            s.sliderValue = s_prefs.getInt(key, s.minVal);
        }
    }
    s_prefs.end();
}

static void prefsSaveToggle(int idx) {
    s_prefs.begin("http-trigger", false);
    char key[16]; snprintf(key, sizeof(key), "t%d", idx);
    s_prefs.putBool(key, s_screens[idx].toggleOn);
    s_prefs.end();
}

static void prefsSaveSlider(int idx) {
    s_prefs.begin("http-trigger", false);
    char key[16]; snprintf(key, sizeof(key), "s%d", idx);
    s_prefs.putInt(key, s_screens[idx].sliderValue);
    s_prefs.end();
}

// ── WiFi ─────────────────────────────────────────────────────────────────────
static bool wifiConnect() {
    WifiCred list[3] = {
        { cfg_ssid[0], cfg_pass[0] },
        { cfg_ssid[1], cfg_pass[1] },
        { cfg_ssid[2], cfg_pass[2] },
    };
    return wifi_try_connect(list, 3) >= 0;
}

// ── Splash text helper ───────────────────────────────────────────────────────
static void showSplash(const char *l1, const char *l2 = nullptr) {
    canvas->fillScreen(COL_BG);
    canvas->setTextColor(COL_WHITE); canvas->setTextSize(2);
    int16_t y = DH / 2 - (l2 ? 18 : 8);
    int16_t tw = (int16_t)(strlen(l1) * 12);
    canvas->setCursor((DW - tw) / 2, y); canvas->print(l1);
    if (l2) {
        canvas->setTextColor(COL_GREY);
        tw = (int16_t)(strlen(l2) * 12);
        canvas->setCursor((DW - tw) / 2, y + 28); canvas->print(l2);
    }
    canvas->flush();
}

// ── {value} substitution ─────────────────────────────────────────────────────
static void substValue(char *out, size_t cap, const char *tmpl, const char *value) {
    size_t oi = 0;
    for (const char *p = tmpl; *p && oi < cap - 1; ) {
        if (p[0] == '{' && p[1] == 'v' && p[2] == 'a' && p[3] == 'l' &&
            p[4] == 'u' && p[5] == 'e' && p[6] == '}') {
            for (const char *v = value; *v && oi < cap - 1; v++)
                out[oi++] = *v;
            p += 7;
        } else {
            out[oi++] = *p++;
        }
    }
    out[oi] = '\0';
}

// ── JSON path extractor ──────────────────────────────────────────────────────
static bool extractByPath(const char *body, const char *path, char *out, size_t cap) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) return false;

    JsonVariant cur = doc.as<JsonVariant>();
    char part[32]; size_t pi = 0;
    const char *p = path;
    while (true) {
        if (*p == '.' || *p == '\0') {
            part[pi] = '\0';
            if (pi > 0) {
                bool numeric = true;
                for (size_t k = 0; k < pi; k++)
                    if (!isdigit((unsigned char)part[k])) { numeric = false; break; }
                if (numeric) cur = cur[atoi(part)];
                else         cur = cur[(const char *)part];
            }
            pi = 0;
            if (*p == '\0') break;
            p++;
            continue;
        }
        if (pi < sizeof(part) - 1) part[pi++] = *p;
        p++;
    }

    if (cur.isNull()) return false;
    if (cur.is<const char *>()) {
        strncpy(out, cur.as<const char *>(), cap - 1);
        out[cap - 1] = '\0';
    } else if (cur.is<float>() || cur.is<double>()) {
        snprintf(out, cap, "%g", cur.as<double>());
    } else if (cur.is<int>() || cur.is<long>()) {
        snprintf(out, cap, "%ld", cur.as<long>());
    } else if (cur.is<bool>()) {
        strncpy(out, cur.as<bool>() ? "true" : "false", cap - 1);
        out[cap - 1] = '\0';
    } else {
        serializeJson(cur, out, cap);
    }
    return true;
}

// ── HTTP executor ────────────────────────────────────────────────────────────
struct ReqSpec {
    const char *method;
    const char *url;
    const char *body;
    const char *headers[3];
};

// Parse a free-form server response as a boolean. Accepts true/false, yes/no,
// on/off (case-insensitive), as well as numeric (non-zero → true).
static bool parseBool(const char *s) {
    if (!s) return false;
    while (*s == ' ' || *s == '\t' || *s == '"' || *s == '\'') s++;
    if (*s == '\0') return false;
    if (!strncasecmp(s, "true",  4)) return true;
    if (!strncasecmp(s, "false", 5)) return false;
    if (!strncasecmp(s, "yes",   3)) return true;
    if (!strncasecmp(s, "no",    2)) return false;
    if (!strncasecmp(s, "on",    2)) return true;
    if (!strncasecmp(s, "off",   3)) return false;
    char *end;
    long v = strtol(s, &end, 10);
    return (end != s) && (v != 0);
}

// Runs the request synchronously and writes status / response into the Item.
// `valueOut` is filled (when non-null) with the JSON-path-extracted or raw
// body. If `jsonPathOverride` is set it takes precedence over `it.jsonPath`.
// `silent=true` skips writes to lastStatus / lastResponse / hasResponse —
// used by status polls so they don't clobber the action banner.
static bool execRequest(Item &it, const ReqSpec &req, char *valueOut, size_t valueCap,
                        const char *jsonPathOverride = nullptr, bool silent = false) {
    if (WiFi.status() != WL_CONNECTED) {
        if (!silent) {
            it.lastStatus = -1;
            copyStr(it.lastResponse, sizeof(it.lastResponse), "NO WIFI");
            it.hasResponse = true;
            it.responseShownAt = millis();
        }
        return false;
    }
    if (!req.url || !req.url[0]) {
        if (!silent) {
            it.lastStatus = -2;
            copyStr(it.lastResponse, sizeof(it.lastResponse), "NO URL");
            it.hasResponse = true;
            it.responseShownAt = millis();
        }
        return false;
    }

    HTTPClient http;
    WiFiClientSecure secureClient;
    bool isHttps = (strncmp(req.url, "https://", 8) == 0);
    if (isHttps) {
        if (s_httpsInsecure) {
            // Opt-in: user set HTTPS_INSECURE=true in setup.txt because their
            // LAN endpoint presents a self-signed certificate.
            secureClient.setInsecure();
        } else {
            // Default: verify the server certificate against the embedded
            // Mozilla root CA bundle. Protects bearer tokens against LAN MITM.
            secureClient.setCACertBundle(rootca_crt_bundle_start,
                                         rootca_crt_bundle_end - rootca_crt_bundle_start);
        }
        http.begin(secureClient, req.url);
    } else {
        http.begin(req.url);
    }
    http.setTimeout(5000);
    http.setConnectTimeout(3000);

    for (int i = 0; i < 3; i++) {
        if (!req.headers[i] || !req.headers[i][0]) continue;
        const char *colon = strchr(req.headers[i], ':');
        if (!colon) continue;
        size_t nameLen = colon - req.headers[i];
        char name[64];
        if (nameLen >= sizeof(name)) nameLen = sizeof(name) - 1;
        memcpy(name, req.headers[i], nameLen); name[nameLen] = '\0';
        const char *val = colon + 1;
        while (*val == ' ') val++;
        http.addHeader(name, val);
    }

    int code;
    const char *m = req.method ? req.method : "GET";
    if (!strcasecmp(m, "GET"))         code = http.GET();
    else if (!strcasecmp(m, "DELETE")) code = http.sendRequest("DELETE");
    else code = http.sendRequest(m, (uint8_t *)(req.body ? req.body : ""),
                                 req.body ? strlen(req.body) : 0);

    String resp = (code > 0) ? http.getString() : String();
    http.end();

    if (!silent) {
        it.lastStatus = code;
        if (resp.length() > 0) {
            size_t copyLen = resp.length();
            if (copyLen >= sizeof(it.lastResponse)) copyLen = sizeof(it.lastResponse) - 1;
            memcpy(it.lastResponse, resp.c_str(), copyLen);
            it.lastResponse[copyLen] = '\0';
            for (char *c = it.lastResponse; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
        } else {
            it.lastResponse[0] = '\0';
        }
        it.hasResponse = true;
        it.responseShownAt = millis();
    }

    if (valueOut && valueCap > 0) {
        valueOut[0] = '\0';
        if (code == 200 && resp.length() > 0) {
            const char *jp = jsonPathOverride ? jsonPathOverride : it.jsonPath;
            if (jp && jp[0]) extractByPath(resp.c_str(), jp, valueOut, valueCap);
            else {
                String trimmed = resp; trimmed.trim();
                copyStr(valueOut, valueCap, trimmed.c_str());
            }
        }
    }

    return code == 200;
}

// ── Display value formatter (unit + decimals) ───────────────────────────────
// Inserts a space between the value and the unit ("0 mm" instead of "0mm").
static void fmtDisplayValue(const Item &it, const char *raw, char *out, size_t cap) {
    bool isNum = raw[0] != '\0';
    for (const char *c = raw; *c; c++) {
        if (!isdigit((unsigned char)*c) && *c != '.' && *c != '-' && *c != '+' && *c != 'e' && *c != 'E') {
            isNum = false; break;
        }
    }
    if (isNum && it.decimals > 0) {
        double v = atof(raw);
        if (it.unit[0]) snprintf(out, cap, "%.*f %s", it.decimals, v, it.unit);
        else            snprintf(out, cap, "%.*f",    it.decimals, v);
    } else if (isNum && it.unit[0]) {
        snprintf(out, cap, "%s %s", raw, it.unit);
    } else {
        snprintf(out, cap, "%s", raw);
    }
}

// ── Async HTTP worker ────────────────────────────────────────────────────────
// HTTP is performed on a separate FreeRTOS task pinned to core 0 so the main
// UI loop (LoopCore=1) keeps responding to BOOT/PWR/touch even while a request
// is in flight. Trigger functions snapshot the request and enqueue a job; the
// worker calls execRequest, then flips s_dirty so the UI refreshes.
// Job kinds
#define JK_ACTION   0
#define JK_DISPLAY  1
#define JK_STATUS   2

struct HttpJob {
    int     screenIdx;
    int     itemIdx;
    uint8_t kind;            // JK_*
    char    method[8];
    char    url[256];
    char    body[200];
    char    headers[3][140];
    char    jsonPath[64];    // override for DISPLAY / STATUS extraction
};

static QueueHandle_t s_jobQueue   = nullptr;
static TaskHandle_t  s_workerTask = nullptr;

static void enqueueStatusPoll(int screenIdx, int itemIdx);  // fwd

static void httpWorkerTask(void *) {
    HttpJob job;
    char    raw[80];
    for (;;) {
        if (xQueueReceive(s_jobQueue, &job, portMAX_DELAY) != pdTRUE) continue;
        if (job.screenIdx < 0 || job.screenIdx >= s_nScreens) continue;
        Screen &s = s_screens[job.screenIdx];
        int idx = job.itemIdx;
        if (idx < 0) idx = 0;
        if (idx >= MAX_ITEMS) continue;
        Item &it = s.items[idx];

        ReqSpec req = { job.method, job.url, job.body,
                        { job.headers[0], job.headers[1], job.headers[2] } };
        raw[0] = '\0';

        if (job.kind == JK_STATUS) {
            // Silent poll — extract bool, feed into Screen.toggleOn (toggle)
            // or Item.statusValue (button). No banner side-effects.
            execRequest(it, req, raw, sizeof(raw), job.jsonPath, /*silent=*/true);
            if (raw[0]) {
                bool b = parseBool(raw);
                if (s.type == SC_TOGGLE) s.toggleOn = b;
                else                     { it.statusValue = b; it.statusKnown = true; }
                if (job.screenIdx == s_screenIdx) s_dirty = true;
            }
            continue;
        }

        if (job.kind == JK_DISPLAY) {
            execRequest(it, req, raw, sizeof(raw), job.jsonPath, /*silent=*/false);
            if (it.lastStatus == 200 && raw[0]) {
                fmtDisplayValue(it, raw, it.displayCache, sizeof(it.displayCache));
                it.hasResponse = false;   // value is shown inline
            }
            it.lastFetchMs = millis();
            it.busy = false;
            s_dirty = true;
            continue;
        }

        // JK_ACTION
        execRequest(it, req, nullptr, 0, nullptr, /*silent=*/false);
        it.busy = false;
        s_dirty = true;

        // Auto-poll status after an action so the visual catches up quickly.
        if (s.type == SC_TOGGLE && s.statusUrl[0])      enqueueStatusPoll(job.screenIdx, 0);
        else if (s.type == SC_BUTTON && it.statusUrl[0]) enqueueStatusPoll(job.screenIdx, idx);
    }
}

static void enqueueJob(int screenIdx, int itemIdx,
                       const char *method, const char *url, const char *body,
                       const char *h0, const char *h1, const char *h2,
                       uint8_t kind,
                       const char *jsonPath = nullptr) {
    if (!s_jobQueue) return;
    HttpJob j = {};
    j.screenIdx = screenIdx;
    j.itemIdx   = itemIdx;
    j.kind      = kind;
    if (method)   copyStr(j.method,     sizeof(j.method),     method);
    if (url)      copyStr(j.url,        sizeof(j.url),        url);
    if (body)     copyStr(j.body,       sizeof(j.body),       body);
    if (h0)       copyStr(j.headers[0], sizeof(j.headers[0]), h0);
    if (h1)       copyStr(j.headers[1], sizeof(j.headers[1]), h1);
    if (h2)       copyStr(j.headers[2], sizeof(j.headers[2]), h2);
    if (jsonPath) copyStr(j.jsonPath,   sizeof(j.jsonPath),   jsonPath);

    // Only action/display use the busy indicator; status polls are silent.
    if (kind != JK_STATUS) {
        Item &it = s_screens[screenIdx].items[itemIdx];
        it.busy        = true;
        it.hasResponse = false;
        if (xQueueSend(s_jobQueue, &j, 0) != pdTRUE) {
            it.busy = false;     // queue full — don't get stuck visually
        }
        s_dirty = true;
    } else {
        xQueueSend(s_jobQueue, &j, 0);    // silent — drop if full
    }
}

// Status polls share the action item's headers (so bearer-auth etc. work).
// Always GET, no body, no JSON_PATH from the action — uses statusJsonPath.
static void enqueueStatusPoll(int screenIdx, int itemIdx) {
    if (screenIdx < 0 || screenIdx >= s_nScreens) return;
    Screen &s = s_screens[screenIdx];
    const char *url, *jp, *h0, *h1, *h2;
    if (s.type == SC_TOGGLE) {
        if (!s.statusUrl[0]) return;
        url = s.statusUrl; jp = s.statusJsonPath;
        h0 = s.tHeaders[0]; h1 = s.tHeaders[1]; h2 = s.tHeaders[2];
        itemIdx = 0;
    } else if (s.type == SC_BUTTON && itemIdx >= 0 && itemIdx < s.nItems) {
        Item &it = s.items[itemIdx];
        if (!it.statusUrl[0]) return;
        url = it.statusUrl; jp = it.statusJsonPath;
        h0 = it.headers[0]; h1 = it.headers[1]; h2 = it.headers[2];
    } else {
        return;
    }
    enqueueJob(screenIdx, itemIdx, "GET", url, nullptr, h0, h1, h2, JK_STATUS, jp);
}

static void httpWorkerInit() {
    if (s_workerTask) return;
    // Queue depth sized for periodic refresh: up to ~5 display screens × 3
    // values = 15 simultaneous fetches need to fit without dropping.
    s_jobQueue = xQueueCreate(16, sizeof(HttpJob));
    xTaskCreatePinnedToCore(httpWorkerTask, "http", 8192, nullptr, 5, &s_workerTask, 0);
}

// ── Trigger functions ───────────────────────────────────────────────────────
static void triggerButton(int screenIdx, int btnIdx) {
    Screen &s = s_screens[screenIdx];
    if (btnIdx < 0 || btnIdx >= s.nItems) return;
    Item &it = s.items[btnIdx];
    enqueueJob(screenIdx, btnIdx, it.method, it.url, it.body,
               it.headers[0], it.headers[1], it.headers[2], JK_ACTION);
}

static void triggerToggleTo(int screenIdx, bool wantOn) {
    Screen &s = s_screens[screenIdx];
    const char *url  = wantOn ? s.urlOn  : s.urlOff;
    const char *body = wantOn ? s.bodyOn : s.bodyOff;
    s.toggleOn = wantOn;
    prefsSaveToggle(screenIdx);
    enqueueJob(screenIdx, 0, s.tMethod, url, body,
               s.tHeaders[0], s.tHeaders[1], s.tHeaders[2], JK_ACTION);
}

static void triggerSlider(int screenIdx) {
    Screen &s = s_screens[screenIdx];
    Item &it = s.items[0];
    char vbuf[16]; snprintf(vbuf, sizeof(vbuf), "%d", s.sliderValue);
    char url[256], body[200], h0[140], h1[140], h2[140];
    substValue(url,  sizeof(url),  it.url,        vbuf);
    substValue(body, sizeof(body), it.body,       vbuf);
    substValue(h0,   sizeof(h0),   it.headers[0], vbuf);
    substValue(h1,   sizeof(h1),   it.headers[1], vbuf);
    substValue(h2,   sizeof(h2),   it.headers[2], vbuf);
    prefsSaveSlider(screenIdx);
    enqueueJob(screenIdx, 0, it.method, url, body, h0, h1, h2, JK_ACTION);
}

static void triggerDisplayValue(int screenIdx, int valueIdx) {
    Screen &s = s_screens[screenIdx];
    if (valueIdx < 0 || valueIdx >= s.nItems) return;
    Item &it = s.items[valueIdx];
    enqueueJob(screenIdx, valueIdx, it.method, it.url, it.body,
               it.headers[0], it.headers[1], it.headers[2], JK_DISPLAY,
               it.jsonPath);
}

static void triggerDisplayAll(int screenIdx) {
    Screen &s = s_screens[screenIdx];
    for (int k = 0; k < s.nItems; k++) triggerDisplayValue(screenIdx, k);
}

// Fire all configured info-fetches: display values (every n on display screens)
// plus optional status polls for buttons/toggles that have a status_url set.
static void pollAllInfo() {
    if (!s_wifiOk) return;
    for (int i = 0; i < s_nScreens; i++) {
        Screen &s = s_screens[i];
        if (s.type == SC_DISPLAY) {
            triggerDisplayAll(i);
        } else if (s.type == SC_TOGGLE) {
            if (s.statusUrl[0]) enqueueStatusPoll(i, 0);
        } else if (s.type == SC_BUTTON) {
            for (int k = 0; k < s.nItems; k++) {
                if (s.items[k].statusUrl[0]) enqueueStatusPoll(i, k);
            }
        }
    }
}

// ── Drawing helpers ──────────────────────────────────────────────────────────
static void drawScreenDots() {
    if (s_nScreens <= 1) return;
    int gap = 14;
    int totalW = (s_nScreens - 1) * gap;
    int x0 = DW / 2 - totalW / 2;
    int y = 14;
    for (int i = 0; i < s_nScreens; i++) {
        int x = x0 + i * gap;
        if (i == s_screenIdx) canvas->fillCircle(x, y, 3, COL_WHITE);
        else                  canvas->drawCircle(x, y, 2, COL_DOTS);
    }
}

static void drawHeaderLabel(const char *label) {
    if (!label || !label[0]) return;
    canvas->setTextSize(2);
    canvas->setTextColor(COL_LGREY);
    int16_t tw = (int16_t)(strlen(label) * 12);
    if (tw > DW - 60) tw = DW - 60;
    canvas->setCursor((DW - tw) / 2, 36);
    canvas->print(label);
    canvas->drawFastHLine(40, 60, DW - 80, COL_DIVIDER);
}

static void drawPill(int16_t bx, int16_t by, int16_t bw, int16_t bh,
                     uint16_t fill, uint16_t border, bool shadow) {
    int16_t r = (bw < bh ? bw : bh) / 2;
    if (shadow) {
        canvas->fillRoundRect(bx + DROP_OFFSET_X, by + DROP_OFFSET_Y,
                              bw, bh, r, COL_SHADOW);
    }
    canvas->fillRoundRect(bx, by, bw, bh, r, fill);
    if (border) canvas->drawRoundRect(bx, by, bw, bh, r, border);
}

// Truncate `src` to fit `maxW` pixels at HERO_CHAR_W, appending ".." if cut.
static void truncToWidth(const char *src, int16_t maxW, char *out, size_t cap) {
    if (cap == 0) return;
    int srcLen   = (int)strlen(src);
    int maxChars = maxW / HERO_CHAR_W;
    if (maxChars <= 0) { out[0] = '\0'; return; }
    if ((int)cap - 1 < maxChars) maxChars = (int)cap - 1;
    if (srcLen <= maxChars) { copyStr(out, cap, src); return; }
    int keep = maxChars - 1;
    if (keep < 1) {
        memcpy(out, src, (size_t)maxChars);
        out[maxChars] = '\0';
        return;
    }
    memcpy(out, src, (size_t)keep);
    out[keep]     = '.';
    out[keep + 1] = '\0';
}

// Render `text` centered in pill bounds at HERO_SIZE, truncating to fit.
static void drawHeroIn(int16_t bx, int16_t by, int16_t bw, int16_t bh,
                       const char *text, uint16_t col) {
    char buf[48];
    truncToWidth(text, bw - 32, buf, sizeof(buf));
    canvas->setTextSize(HERO_SX, HERO_SY, HERO_PX);
    canvas->setTextColor(col);
    int16_t tw = (int16_t)strlen(buf) * HERO_CHAR_W;
    canvas->setCursor(bx + (bw - tw) / 2, by + (bh - HERO_CHAR_H) / 2);
    printUtf8(canvas, buf);
}

// Pick the freshest item with hasResponse on this screen for the banner.
static const Item *freshestResponse(const Screen &s) {
    const Item *best = nullptr;
    for (int k = 0; k < s.nItems; k++) {
        const Item &it = s.items[k];
        if (!it.hasResponse) continue;
        if (!best || it.responseShownAt > best->responseShownAt) best = &it;
    }
    return best;
}

static void drawResponseBanner(int idx) {
    const Screen &s = s_screens[idx];
    const Item *it = freshestResponse(s);
    if (!it) return;
    if ((millis() - it->responseShownAt) >= RESPONSE_BANNER_MS) return;

    int16_t y = DH - 90;
    int16_t bh = 36;
    canvas->fillRect(20, y, DW - 40, bh, 0x0841);
    canvas->drawRect(20, y, DW - 40, bh,
                     it->lastStatus == 200 ? COL_GREEN :
                     it->lastStatus  >  0  ? COL_AMBER : COL_RED);
    canvas->setTextSize(2);
    canvas->setTextColor(it->lastStatus == 200 ? COL_GREEN :
                         it->lastStatus  >  0  ? COL_AMBER : COL_RED);
    char head[24];
    if (it->lastStatus > 0) snprintf(head, sizeof(head), "%d", it->lastStatus);
    else                    snprintf(head, sizeof(head), "ERR");
    canvas->setCursor(28, y + 10);
    canvas->print(head);
    char body[24];
    copyStr(body, sizeof(body), it->lastResponse);
    if (body[0]) {
        canvas->setTextColor(COL_LGREY);
        int16_t bx = 28 + (int16_t)strlen(head) * 12 + 12;
        canvas->setCursor(bx, y + 10);
        canvas->print(body);
    }
}

// Geometry for stacked pills with 1..3 elements. All counts use the slim
// single-button aspect (~32 % of widget width) and the stack is centered
// vertically in the work area — so two/three pills don't get stretched fat.
static int16_t pillRowY(int k, int count, int16_t &heightOut) {
    int16_t avail = BOTTOM_Y - 70 - DROP_OFFSET_Y;
    int16_t gap   = (count == 3) ? 16 : 22;
    int16_t bh    = (WIDGET_W * 32) / 100;            // slim pill — ~100 px
    int16_t total = bh * count + (count > 1 ? gap * (count - 1) : 0);
    // Safety net: if the stack exceeds the work area, shrink the pills.
    if (total > avail && count > 0) {
        bh = (avail - (count > 1 ? gap * (count - 1) : 0)) / count;
        total = bh * count + (count > 1 ? gap * (count - 1) : 0);
    }
    int16_t y0 = 70 + (avail - total) / 2;
    heightOut = bh;
    return y0 + k * (bh + gap);
}

// ── Widget renderers ────────────────────────────────────────────────────────
static void drawButton(int idx) {
    const Screen &s = s_screens[idx];
    drawHeaderLabel(s.label);

    int16_t bw = WIDGET_W;
    int16_t bx = MARGIN_X;
    int16_t bh;
    for (int k = 0; k < s.nItems; k++) {
        int16_t by = pillRowY(k, s.nItems, bh);
        const Item &it = s.items[k];

        bool pressing       = (s_btnPressing == k);
        bool statusInactive = it.statusKnown && !it.statusValue;
        uint16_t fill, border, txCol;
        if (it.busy)             { fill = COL_AMBER; border = COL_AMBER; txCol = COL_BG;    }
        else if (pressing)       { fill = 0x18C3;    border = COL_WHITE; txCol = COL_WHITE; }
        else if (statusInactive) { fill = COL_BG;    border = COL_DIM;   txCol = COL_DIM;   }
        else                     { fill = COL_BG;    border = COL_WHITE; txCol = COL_WHITE; }
        drawPill(bx, by, bw, bh, fill, border, true);
        drawHeroIn(bx, by, bw, bh, it.label[0] ? it.label : "button", txCol);
    }
}

static void drawToggleHalf(int16_t bx, int16_t by, int16_t bw, int16_t bh,
                           const char *label, bool active, bool busy) {
    uint16_t fill, border, txCol;
    bool shadow = !active;
    if (busy && active) { fill = COL_AMBER; border = COL_AMBER; txCol = COL_BG; }
    else if (active)    { fill = COL_BG;    border = COL_DIM;   txCol = COL_DIM; }
    else                { fill = COL_BG;    border = COL_WHITE; txCol = COL_WHITE; }

    drawPill(bx, by, bw, bh, fill, border, shadow);
    drawHeroIn(bx, by, bw, bh, label, txCol);
}

static void drawToggle(int idx) {
    const Screen &s = s_screens[idx];
    drawHeaderLabel(s.label);

    int16_t bx = MARGIN_X;
    int16_t bw = WIDGET_W;
    int16_t bh;
    int16_t y0 = pillRowY(0, 2, bh);
    int16_t y1 = pillRowY(1, 2, bh);

    bool busy = s.items[0].busy;
    drawToggleHalf(bx, y0, bw, bh,
                   s.labelOn[0]  ? s.labelOn  : "ON",
                   /*active=*/  s.toggleOn, busy);
    drawToggleHalf(bx, y1, bw, bh,
                   s.labelOff[0] ? s.labelOff : "OFF",
                   /*active=*/ !s.toggleOn, busy);
}

// Slider — vertical column of pill bars, fill from the bottom.
#define SLIDER_BARS    14
#define SLIDER_BAR_W   240
#define SLIDER_BAR_H   12
#define SLIDER_GAP     6
#define SLIDER_X       ((DW - SLIDER_BAR_W) / 2)
#define SLIDER_TOP     126
#define SLIDER_BOTTOM  (SLIDER_TOP + SLIDER_BARS * (SLIDER_BAR_H + SLIDER_GAP) - SLIDER_GAP)

static int sliderLitBars(const Screen &s) {
    int range = s.maxVal - s.minVal;
    if (range <= 0) return 0;
    int v = s.sliderValue - s.minVal;
    if (v < 0) v = 0;
    int lit = (v * SLIDER_BARS + range / 2) / range;
    if (lit < 0) lit = 0;
    if (lit > SLIDER_BARS) lit = SLIDER_BARS;
    return lit;
}

static void drawSlider(int idx) {
    const Screen &s = s_screens[idx];
    drawHeaderLabel(s.label);

    char vbuf[24];
    int displayVal = s_pendingSlider != INT32_MIN ? s_pendingSlider : s.sliderValue;
    if (s.items[0].unit[0]) snprintf(vbuf, sizeof(vbuf), "%d %s", displayVal, s.items[0].unit);
    else                    snprintf(vbuf, sizeof(vbuf), "%d",    displayVal);
    canvas->setTextSize(HERO_SX, HERO_SY, HERO_PX);
    canvas->setTextColor(COL_WHITE);
    int16_t tw = (int16_t)strlen(vbuf) * HERO_CHAR_W;
    canvas->setCursor((DW - tw) / 2, 72);
    canvas->print(vbuf);

    Screen previewS = s;
    if (s_pendingSlider != INT32_MIN) previewS.sliderValue = s_pendingSlider;
    int lit = sliderLitBars(previewS);

    const int16_t barR = SLIDER_BAR_H / 2;
    for (int i = 0; i < SLIDER_BARS; i++) {
        int y = SLIDER_TOP + i * (SLIDER_BAR_H + SLIDER_GAP);
        int fromBottom = SLIDER_BARS - 1 - i;
        bool isLit = fromBottom < lit;
        if (isLit) {
            uint8_t t = (uint8_t)((SLIDER_BARS - 1 - fromBottom) * 255 / (SLIDER_BARS - 1));
            uint16_t col = blend565(COL_WHITE, COL_DIM, t);
            canvas->fillRoundRect(SLIDER_X, y, SLIDER_BAR_W, SLIDER_BAR_H, barR, col);
        } else {
            canvas->drawRoundRect(SLIDER_X, y, SLIDER_BAR_W, SLIDER_BAR_H, barR, COL_FAINT);
        }
    }

    char mbuf[12];
    canvas->setTextSize(1);
    canvas->setTextColor(COL_GREY);
    snprintf(mbuf, sizeof(mbuf), "%d", s.maxVal);
    canvas->setCursor(SLIDER_X + SLIDER_BAR_W + 8, SLIDER_TOP);
    canvas->print(mbuf);
    snprintf(mbuf, sizeof(mbuf), "%d", s.minVal);
    canvas->setCursor(SLIDER_X + SLIDER_BAR_W + 8, SLIDER_BOTTOM - 8);
    canvas->print(mbuf);
}

// Render one display value as a pill (no shadow, grey border). For multi-row
// displays a small label sits at the top of the pill; for single-value the
// pill just holds the hero value, centered.
static void drawDisplayPill(int16_t bx, int16_t by, int16_t bw, int16_t bh,
                            const Item &it, bool withLabel) {
    drawPill(bx, by, bw, bh, COL_BG, COL_DIM, /*shadow=*/false);

    if (withLabel && it.label[0]) {
        canvas->setTextSize(2);
        canvas->setTextColor(COL_LGREY);
        char lbuf[20];
        copyStr(lbuf, sizeof(lbuf), it.label);
        // truncate label to fit the pill at size 2 (12 px / char)
        int maxLChars = (bw - 16) / 12;
        if ((int)strlen(lbuf) > maxLChars && maxLChars > 1) {
            lbuf[maxLChars - 1] = '.';
            lbuf[maxLChars]     = '\0';
        }
        int16_t lw = (int16_t)strlen(lbuf) * 12;
        canvas->setCursor(bx + (bw - lw) / 2, by + 10);
        canvas->print(lbuf);
    }

    const char *val = it.displayCache[0] ? it.displayCache
                     : (it.busy ? "..." : "--");
    uint16_t txCol = (it.lastStatus != 0 && it.lastStatus != 200) ? COL_RED : COL_WHITE;

    if (withLabel) {
        // Value sits at the bottom of the pill, below the label
        char buf[48];
        truncToWidth(val, bw - 32, buf, sizeof(buf));
        canvas->setTextSize(HERO_SX, HERO_SY, HERO_PX);
        canvas->setTextColor(txCol);
        int16_t tw  = (int16_t)strlen(buf) * HERO_CHAR_W;
        int16_t valY = by + bh - HERO_CHAR_H - 12;
        canvas->setCursor(bx + (bw - tw) / 2, valY);
        printUtf8(canvas, buf);
    } else {
        drawHeroIn(bx, by, bw, bh, val, txCol);
    }
}

static void drawDisplay(int idx) {
    const Screen &s = s_screens[idx];
    drawHeaderLabel(s.label);

    int16_t bw = WIDGET_W;
    int16_t bx = MARGIN_X;
    int16_t bh;
    bool multi = (s.nItems > 1);
    for (int k = 0; k < s.nItems; k++) {
        int16_t by = pillRowY(k, s.nItems, bh);
        drawDisplayPill(bx, by, bw, bh, s.items[k], /*withLabel=*/multi);
    }
}

// Hardware-button label on the right edge — just rotated text, no bg.
static void drawHwButtonLabel(uint8_t button, const char *action) {
    int16_t len = (int16_t)strlen(action);
    if (len == 0) return;
    const uint8_t stride = 2, pxSz = 1;
    const int16_t charW = 6 * stride;
    const int16_t charH = 8 * stride;
    const int16_t padX = 4, padY = 4;
    int16_t pillW = charH + padX * 2;
    int16_t pillH = len * charW + padY * 2;
    int16_t btnY  = (button == 0) ? BOOT_BTN_Y_P : PWR_BTN_Y_P;
    int16_t px    = LCD_WIDTH - pillW - 4;
    int16_t py    = btnY - pillH / 2;
    drawTextRot(canvas, px + padX, py + padY, action, HUD_PILL_TX, stride, pxSz);
}

// ── Master draw ──────────────────────────────────────────────────────────────
static void drawScreen() {
    canvas->fillScreen(COL_BG);
    const Screen &s = s_screens[s_screenIdx];

    switch (s.type) {
        case SC_BUTTON:  drawButton(s_screenIdx);  break;
        case SC_TOGGLE:  drawToggle(s_screenIdx);  break;
        case SC_SLIDER:  drawSlider(s_screenIdx);  break;
        case SC_DISPLAY: drawDisplay(s_screenIdx); break;
        default: break;
    }

    drawScreenDots();
    drawResponseBanner(s_screenIdx);

    draw_battery_g(canvas, DW, DH);
    draw_watermark_g(canvas, DW, DH);
    drawHwButtonLabel(0, "prev");
    drawHwButtonLabel(1, "next");

    canvas->flush();
    s_dirty = false;
    s_lastDraw = millis();
}

// ── Touch ───────────────────────────────────────────────────────────────────
static bool readTouch(int16_t &tx, int16_t &ty, bool &down) {
    bool present = s_touch.getPoint(&tx, &ty, 1);
    if (present && tx == 0 && ty == 0) present = false;
    down = present;
    bool fresh = present && !s_touchHeld;
    s_touchHeld = present;
    return fresh;
}

static void handleSliderTouch(int idx, int16_t ty) {
    Screen &s = s_screens[idx];
    int top = SLIDER_TOP - SLIDER_GAP;
    int bot = SLIDER_BOTTOM + SLIDER_GAP;
    if (ty < top) ty = top;
    if (ty > bot) ty = bot;
    int fromTop = ty - top;
    int totalH  = bot - top;
    int range   = s.maxVal - s.minVal;
    int v = s.maxVal - (fromTop * range + totalH / 2) / totalH;
    if (s.step > 1) {
        v = ((v - s.minVal + s.step / 2) / s.step) * s.step + s.minVal;
    }
    if (v < s.minVal) v = s.minVal;
    if (v > s.maxVal) v = s.maxVal;
    if (v != s_pendingSlider) {
        s_pendingSlider = v;
        s_dirty = true;
    }
}

// Return which pill row (0..nItems-1) is under `ty`, or -1.
static int pillHitTest(const Screen &s, int16_t ty) {
    int16_t bh;
    for (int k = 0; k < s.nItems; k++) {
        int16_t y = pillRowY(k, s.nItems, bh);
        if (ty >= y && ty < y + bh) return k;
    }
    return -1;
}

// ── USB Mass Storage ────────────────────────────────────────────────────────
// Exposes the SD card as a removable drive when plugged into a PC, so
// /setup/http-trigger.txt and /setup/setup.txt are editable from the host.
#if REMOTE_HAS_USB_MSC
extern "C" {
#include "diskio.h"
}
static constexpr uint8_t REMOTE_PDRV = 0;
static USBMSC s_msc;

static int32_t mscOnWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize) {
    (void)offset;
    uint32_t secSize = SD_MMC.sectorSize();
    if (!secSize) return -1;
    uint32_t count = bufsize / secSize;
    if (disk_write(REMOTE_PDRV, buffer, lba, count) != RES_OK) return -1;
    return bufsize;
}

static int32_t mscOnRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
    (void)offset;
    uint32_t secSize = SD_MMC.sectorSize();
    if (!secSize) return -1;
    uint32_t count = bufsize / secSize;
    if (disk_read(REMOTE_PDRV, (uint8_t *)buffer, lba, count) != RES_OK) return -1;
    return bufsize;
}

static bool mscOnStartStop(uint8_t, bool, bool) { return true; }

static void initUsbMsc() {
    s_msc.vendorID("Pixels");
    s_msc.productID("HTTP Trigger");
    s_msc.productRevision("1.0");
    s_msc.onRead(mscOnRead);
    s_msc.onWrite(mscOnWrite);
    s_msc.onStartStop(mscOnStartStop);
    s_msc.begin(SD_MMC.numSectors(), SD_MMC.sectorSize());
    s_msc.mediaPresent(true);
    USB.begin();
}
#else
static void initUsbMsc() {}
#endif

// ── Public API ──────────────────────────────────────────────────────────────
void app_http_trigger_setup(Arduino_SH8601 * /*passed_gfx*/) {
    if (!expander.begin(0x20)) USBSerial.println("XCA9554 init failed");
    expander.pinMode(1, OUTPUT); expander.digitalWrite(1, LOW);
    expander.pinMode(2, OUTPUT); expander.digitalWrite(2, LOW);
    delay(20);
    expander.digitalWrite(1, HIGH);
    expander.digitalWrite(2, HIGH);

    canvas = g_canvas;
    canvas->setRotation(0);

    memset(s_screens, 0, sizeof(s_screens));
    s_nScreens      = 0;
    s_screenIdx     = 0;
    s_touchHeld     = false;
    s_dragActive    = false;
    s_pendingSlider = INT32_MIN;
    s_btnPressing   = -1;
    s_dirty         = true;

    pinMode(BOOT_BTN, INPUT_PULLUP);
    pinMode(TP_INT,   INPUT_PULLUP);
    attachInterrupt(BOOT_BTN, bootISR, FALLING);
    s_touch.begin(Wire, 0x38, IIC_SDA, IIC_SCL);
    for (int i = 0; i < 3; i++) {
        int16_t dx = 0, dy = 0;
        s_touch.getPoint(&dx, &dy, 1);
        delay(8);
    }

    showSplash("HTTP Trigger", "Reading config...");

    SD_MMC.setPins(SDMMC_CLK, SDMMC_CMD, SDMMC_DATA);
    if (!SD_MMC.begin("/sdcard", true)) {
        showSplash("SD card failed!", "Check card");
        delay(5000); return;
    }
    initUsbMsc();

    readSetupGlobals();
    if (!readConfig()) {
        showSplash("No http-trigger.txt", "Add /setup/http-trigger.txt");
        delay(8000); return;
    }
    USBSerial.printf("Loaded %d screens\n", s_nScreens);

    prefsLoadStates();

    if (!cfg_ssid[0][0]) {
        showSplash("No SSID!", "Add SSID to setup.txt");
        delay(8000); return;
    }
    showSplash("Connecting WiFi...", cfg_ssid[0]);
    s_wifiOk = wifiConnect();
    if (!s_wifiOk) {
        showSplash("WiFi failed!", "Check credentials");
        delay(3000);
    } else {
        USBSerial.println("WiFi connected");
    }

    httpWorkerInit();

    // Boot-time fetch so values aren't empty for the first refresh interval.
    // Also seeds status polls for any button / toggle that has a status_url.
    pollAllInfo();
    s_lastRefreshMs = millis();
    s_dirty = true;
}

static void changeScreen(int delta) {
    common_activity();
    s_screenIdx = (s_screenIdx + delta + s_nScreens) % s_nScreens;
    s_pendingSlider    = INT32_MIN;
    s_dragActive       = false;
    s_releaseScheduled = false;
    s_btnPressing      = -1;
    s_touchMode        = TM_UNKNOWN;
    s_dirty            = true;
    Screen &s = s_screens[s_screenIdx];
    // Clear stale banner state across all items — nothing else fires here.
    for (int k = 0; k < s.nItems; k++) s.items[k].hasResponse = false;
}

void app_http_trigger_loop() {
    common_tick();
    if (s_nScreens == 0) { delay(100); return; }

    if (s_bootShortPending) { s_bootShortPending = false; changeScreen(-1); }
    if (common_consume_pwr_short())                  changeScreen(+1);

    // ── Touch read + gesture-mode tracking ──────────────────────────────────
    int16_t tx = 0, ty = 0;
    bool    down;
    bool    prevDown = s_touchHeld;
    bool    fresh = readTouch(tx, ty, down);
    bool    released = prevDown && !down;
    Screen &cur   = s_screens[s_screenIdx];
    uint32_t now  = millis();

    if (fresh) {
        s_touchStartX  = tx;
        s_touchStartY  = ty;
        s_touchLastX   = tx;
        s_touchLastY   = ty;
        s_touchStartMs = now;
        s_touchMode    = TM_UNKNOWN;
    }
    if (down) {
        int16_t dxa = abs(tx - s_touchStartX);
        int16_t dya = abs(ty - s_touchStartY);
        if (s_touchMode == TM_UNKNOWN && (dxa > MODE_LOCK_PX || dya > MODE_LOCK_PX)) {
            s_touchMode = (dxa > dya) ? TM_HORIZONTAL : TM_VERTICAL;
        }
        s_touchLastX = tx;
        s_touchLastY = ty;
    }

    // Swipe detection happens at release. If it fires, we skip the widget's
    // own release handling for this gesture.
    bool handledAsSwipe = false;
    if (released) {
        int16_t dx = s_touchLastX - s_touchStartX;
        int16_t dy = s_touchLastY - s_touchStartY;
        uint32_t dur = now - s_touchStartMs;
        bool isSwipe = (abs(dx) > SWIPE_MIN_PX && abs(dx) > 2 * abs(dy) && dur < SWIPE_MAX_MS);
        if (isSwipe) {
            handledAsSwipe = true;
            if (dx < 0) changeScreen(+1);   // swipe left → next
            else        changeScreen(-1);   // swipe right → prev
        }
    }

    // ── Widget interaction ──────────────────────────────────────────────────
    if (cur.type == SC_SLIDER) {
        // Drag is engaged only while the gesture is (or is becoming) vertical.
        if (down && s_touchMode != TM_HORIZONTAL) {
            common_activity();
            if (!s_dragActive) {
                s_dragActive       = true;
                s_releaseScheduled = false;
                if (s_pendingSlider == INT32_MIN) s_pendingSlider = cur.sliderValue;
            }
            handleSliderTouch(s_screenIdx, ty);
        } else if (down && s_touchMode == TM_HORIZONTAL && s_dragActive) {
            // Mode locked to horizontal mid-drag — cancel slider engagement.
            s_dragActive    = false;
            s_pendingSlider = INT32_MIN;
            s_dirty         = true;
        } else if (released && s_dragActive && !handledAsSwipe) {
            s_dragActive = false;
            if (s_pendingSlider != INT32_MIN && s_pendingSlider != cur.sliderValue) {
                s_releaseScheduled   = true;
                s_releaseScheduledAt = now;
            } else {
                s_pendingSlider = INT32_MIN;
            }
            s_dirty = true;
        }
        if (s_releaseScheduled && (now - s_releaseScheduledAt) >= SLIDER_DEBOUNCE_MS) {
            s_releaseScheduled = false;
            cur.sliderValue    = s_pendingSlider;
            triggerSlider(s_screenIdx);
            s_pendingSlider = INT32_MIN;
        }
    } else if (cur.type == SC_BUTTON) {
        int hit = (tx >= MARGIN_X && tx <= MARGIN_X + WIDGET_W)
                  ? pillHitTest(cur, ty) : -1;
        if (fresh && hit >= 0 && !cur.items[hit].busy) {
            common_activity();
            s_btnPressing = hit;
            s_dirty = true;
        }
        // Cancel press-feedback if finger leaves the pill or swipes — but
        // only while the finger is still down. On release, tx/ty drop to 0
        // and hit goes to -1, which we must NOT treat as "left the pill".
        if (s_btnPressing >= 0 && down &&
            (hit != s_btnPressing || s_touchMode == TM_HORIZONTAL)) {
            s_btnPressing = -1;
            s_dirty = true;
        }
        if (released && s_btnPressing >= 0 && !handledAsSwipe) {
            int k = s_btnPressing;
            s_btnPressing = -1;
            s_dirty = true;
            triggerButton(s_screenIdx, k);
        }
    } else if (cur.type == SC_TOGGLE) {
        // Toggle fires on release (not on fresh) so a swipe-from-toggle
        // doesn't accidentally trigger it.
        if (released && !handledAsSwipe && s_touchMode != TM_HORIZONTAL) {
            common_activity();
            int16_t bh;
            int16_t y0 = pillRowY(0, 2, bh);
            int16_t y1 = pillRowY(1, 2, bh);
            int16_t splitY = (y0 + bh + y1) / 2;
            bool wantOn = (s_touchStartY < splitY);
            triggerToggleTo(s_screenIdx, wantOn);
        }
    } else if (cur.type == SC_DISPLAY) {
        // Manual refresh on tap (release without swipe).
        if (released && !handledAsSwipe && s_touchMode != TM_HORIZONTAL) {
            common_activity();
            triggerDisplayAll(s_screenIdx);
        }
    }

    // ── Periodic refresh ────────────────────────────────────────────────────
    if (s_wifiOk && s_refreshIntervalMs > 0 &&
        (now - s_lastRefreshMs) >= s_refreshIntervalMs) {
        s_lastRefreshMs = now;
        pollAllInfo();
    }

    // Banner expiry — flip dirty when any item's banner times out
    for (int k = 0; k < cur.nItems; k++) {
        Item &it = cur.items[k];
        if (it.hasResponse && (now - it.responseShownAt) >= RESPONSE_BANNER_MS) {
            it.hasResponse = false;
            s_dirty = true;
        }
    }

    if (s_dirty) drawScreen();
    delay(5);
}
