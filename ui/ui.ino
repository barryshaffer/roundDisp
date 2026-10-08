#include <lvgl.h>
#include <TFT_eSPI.h>
#include <ui.h>
#include <I2C_BM8563.h>
#include <Preferences.h>
#include <WiFi.h>
#include <FS.h>
using fs::FS;
using fs::File;
#include <WebServer.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#define USE_TFT_ESPI_LIBRARY
#include "lv_xiao_round_screen.h"

/*
  IMPORTANT: driver.h must be present in this sketch directory to tell
  Seeed_GFX to use Setup501 (GC9A01 driver for the round display):
  #define BOARD_SCREEN_COMBO 501
*/

// Onboard RTC (PCF8563 / BM8563 at I2C address 0x51 on Wire)
I2C_BM8563 rtc(I2C_BM8563_DEFAULT_ADDRESS, Wire);

// Timezone and NTP Configuration (Initial default: America/Phoenix = MST7)
String ntpServer                     = "pool.ntp.org";
String tzString                      = "MST7";
const unsigned long NTP_SYNC_INTERVAL_MS = 86400000; // 24 hours (24 * 60 * 60 * 1000)
unsigned long lastNtpSync            = 0;

// Day and Month name tables for formatting
const char* const DAYS_OF_WEEK[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};

const char* const MONTHS_OF_YEAR[] = {
    "", "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

// Hardware pins
const int SIGNAL_PIN    = D0;  // SodaStream 3.3V injection signal
const int BACKLIGHT_PIN = D6;  // GC9A01 LCD backlight control

const int32_t MAX_CAPACITY_MG = 410000; // 410 g = 410,000 mg
const int16_t ARC_MAX_RANGE   = 4100;   // 0 to 4100 (1 unit = 100 mg = 0.1 g)

Preferences prefs;

// Wi-Fi and Web Server configuration
String wifiSsid                      = "";
String wifiPass                      = "";
bool isApMode                        = false;
const char* const AP_SSID            = "SodaStream-Setup";
const IPAddress AP_IP(192, 168, 4, 1);
const byte DNS_PORT                  = 53;

DNSServer dnsServer;
WebServer server(80);

// Sleep and Inactivity management
const unsigned long SLEEP_TIMEOUT_MS = 300000; // 5 minutes inactivity timeout
unsigned long lastActivityTime       = 0;
bool isAsleep                        = false;
lv_obj_t * ui_BlankScreen             = NULL;

// Project state variables
uint32_t lifeBottleCount = 0;
uint16_t shotsThisCan    = 0;
int32_t  currentMg       = MAX_CAPACITY_MG;
int16_t  msPerShot       = 2050; // default duration per shot in ms (Screen 4)
int16_t  mgPerShot       = 1470; // default CO2 expended per shot in mg (Screen 5)

// Interrupt pulse timing variables
volatile unsigned long pulseStartTime = 0;
volatile unsigned long lastCycleTime  = 0;
volatile bool newCycleReady           = false;

void IRAM_ATTR onSignalChange()
{
    int pinVal = digitalRead(SIGNAL_PIN);
    unsigned long now = millis();

    if (pinVal == HIGH) {
        // Only latch the start timestamp on the initial transition to HIGH
        if (pulseStartTime == 0) {
            pulseStartTime = now;
        }
    } else { // pinVal == LOW
        if (pulseStartTime > 0) {
            unsigned long duration = now - pulseStartTime;
            // Ignore sub-50ms noise spikes and bounce artifacts
            if (duration >= 50) {
                lastCycleTime = duration;
                newCycleReady = true;
                pulseStartTime = 0;
            } else {
                // False trigger / glitch: reset so it can re-latch cleanly
                pulseStartTime = 0;
            }
        }
    }
}

#if LV_USE_LOG != 0
/* Serial debugging for LVGL */
void my_print(const char * buf)
{
    Serial.printf(buf);
    Serial.flush();
}
#endif

// Calculate day of the week: 0 = Sunday, 1 = Monday, ..., 6 = Saturday
int calculateDayOfWeek(int y, int m, int d)
{
    static int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    y -= m < 3;
    return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}

/**
 * Sets the RTC date and time.
 */
void setRtcTime(int year, int month, int day, int hour, int minute, int second)
{
    I2C_BM8563_DateTypeDef dateStruct;
    dateStruct.year    = year;
    dateStruct.month   = month;
    dateStruct.date    = day;
    dateStruct.weekDay = calculateDayOfWeek(year, month, day);
    rtc.setDate(&dateStruct);

    I2C_BM8563_TimeTypeDef timeStruct;
    timeStruct.hours   = hour;
    timeStruct.minutes = minute;
    timeStruct.seconds = second;
    rtc.setTime(&timeStruct);
}

/**
 * Parses compiler __DATE__ and __TIME__ macros to set RTC automatically.
 */
void syncRtcToCompileTime()
{
    const char *monthsShort[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char monthStr[4] = {0};
    int day = 0, year = 0;
    sscanf(__DATE__, "%3s %d %d", monthStr, &day, &year);

    int month = 1;
    for (int i = 0; i < 12; i++) {
        if (strcmp(monthStr, monthsShort[i]) == 0) {
            month = i + 1;
            break;
        }
    }

    int hour = 0, minute = 0, second = 0;
    sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second);

    setRtcTime(year, month, day, hour, minute, second);
    Serial.printf("RTC synced to compile time: %s %s %d %02d:%02d:%02d\n",
                  DAYS_OF_WEEK[calculateDayOfWeek(year, month, day)],
                  MONTHS_OF_YEAR[month],
                  day, hour, minute, second);
}

/**
 * Initializes the RTC and handles setting the clock if requested or uninitialized.
 */
void initRtc()
{
    rtc.begin();

    I2C_BM8563_DateTypeDef dateStruct;
    I2C_BM8563_TimeTypeDef timeStruct;
    rtc.getDate(&dateStruct);
    rtc.getTime(&timeStruct);

    Serial.printf("RTC initial read from chip: Year=%d Month=%d Date=%d %02d:%02d:%02d\n",
                  dateStruct.year, dateStruct.month, dateStruct.date,
                  timeStruct.hours, timeStruct.minutes, timeStruct.seconds);

    bool needsInit = rtc.getVoltLow() || dateStruct.year < 2024 || dateStruct.year > 2099 ||
                     dateStruct.month < 1 || dateStruct.month > 12;

    if (needsInit) {
        Serial.println("RTC uninitialized or lost power. Syncing to compiler build time as initial fallback...");
        syncRtcToCompileTime();
    } else {
        Serial.println("RTC already running with valid date/time.");
    }

    // Read back to confirm
    rtc.getDate(&dateStruct);
    rtc.getTime(&timeStruct);
    Serial.printf("RTC confirmed active: %s, %s %d, %d at %02d:%02d:%02d\n",
                  DAYS_OF_WEEK[dateStruct.weekDay % 7],
                  MONTHS_OF_YEAR[constrain(dateStruct.month, 1, 12)],
                  dateStruct.date,
                  dateStruct.year,
                  timeStruct.hours, timeStruct.minutes, timeStruct.seconds);
}

/**
 * Reads the RTC and updates currentDayDate and currentTime labels.
 */
void updateRtcDisplay()
{
    I2C_BM8563_DateTypeDef dateStruct;
    I2C_BM8563_TimeTypeDef timeStruct;

    rtc.getDate(&dateStruct);
    rtc.getTime(&timeStruct);

    // Bounds checking
    int month = constrain(dateStruct.month, 1, 12);
    int weekDay = constrain(dateStruct.weekDay, 0, 6);
    int day = constrain(dateStruct.date, 1, 31);

    // 1. Format Day & Date (e.g. "Thursday October 1")
    char dateBuffer[36];
    snprintf(dateBuffer, sizeof(dateBuffer), "%s %s %d",
             DAYS_OF_WEEK[weekDay],
             MONTHS_OF_YEAR[month],
             day);
    lv_label_set_text(ui_currentDayDate, dateBuffer);

    // 2. Format Time in 12-hour AM/PM format (e.g. "06:22 PM")
    int displayHour = timeStruct.hours % 12;
    if (displayHour == 0) displayHour = 12;
    const char * ampm = (timeStruct.hours >= 12) ? "PM" : "AM";

    char timeBuffer[16];
    snprintf(timeBuffer, sizeof(timeBuffer), "%d:%02d %s",
             displayHour,
             timeStruct.minutes,
             ampm);
    lv_label_set_text(ui_currentTime, timeBuffer);
}

/**
 * Synchronizes the onboard BM8563 RTC using an NTP time server and configured POSIX timezone.
 */
bool syncRtcWithNtp()
{
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("NTP Sync skipped: Wi-Fi not connected.");
        return false;
    }

    Serial.printf("Syncing RTC via NTP: Server='%s', Timezone='%s'...\n", ntpServer.c_str(), tzString.c_str());
    configTzTime(tzString.c_str(), ntpServer.c_str());

    struct tm timeinfo;
    // Wait up to 3 seconds for NTP response without blocking LVGL indefinitely
    if (!getLocalTime(&timeinfo, 3000)) {
        Serial.println("Failed to obtain time from NTP server.");
        return false;
    }

    int year   = timeinfo.tm_year + 1900;
    int month  = timeinfo.tm_mon + 1;
    int day    = timeinfo.tm_mday;
    int hour   = timeinfo.tm_hour;
    int minute = timeinfo.tm_min;
    int second = timeinfo.tm_sec;

    setRtcTime(year, month, day, hour, minute, second);
    updateRtcDisplay();

    lastNtpSync = millis();
    Serial.printf("RTC successfully synced with NTP: %04d-%02d-%02d %02d:%02d:%02d\n",
                  year, month, day, hour, minute, second);
    return true;
}

/**
 * Updates the arc gauge, gram label (xxx.x g), percent label, and warning color.
 * Takes remaining CO2 in integer milligrams (0 to 410,000 mg).
 */
void updateGauge(int32_t mg)
{
    if (mg < 0) mg = 0;
    if (mg > MAX_CAPACITY_MG) mg = MAX_CAPACITY_MG;
    currentMg = mg;

    // 1. Update Arc position (0..4100, where 1 unit = 100 mg = 0.1 g)
    lv_arc_set_value(ui_remainingShots, (int16_t)(mg / 100));

    // 2. Format grams with 1 decimal place: e.g. "410.0 g"
    // Note: LVGL's built-in printf has float (%f) disabled by default,
    // so we format using integer whole grams and tenths (%d.%d):
    int wholeGrams = mg / 1000;
    int tenths     = (mg % 1000) / 100;
    lv_label_set_text_fmt(ui_gageValue, "%d.%d g", wholeGrams, tenths);

    // 3. Compute and format Percentage (0 to 100%)
    int percent = (int)((mg * 100LL) / MAX_CAPACITY_MG);
    lv_label_set_text_fmt(ui_percentRemaining, "%d %%", percent);

    // 4. Change color dynamically: RED if below 20%, GREEN otherwise
    if (percent < 20) {
        lv_obj_set_style_arc_color(ui_remainingShots, lv_color_hex(0xFF2222), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    } else {
        lv_obj_set_style_arc_color(ui_remainingShots, lv_color_hex(0x25F214), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    }
}

/**
 * Updates all labels displaying the current canister shot count (Screen 3 and Screen 6).
 */
void updateShotsDisplay(uint16_t shots)
{
    lv_label_set_text_fmt(ui_shotsthisCan, "%u", shots);
    lv_label_set_text_fmt(ui_shotsThisCan6, "%u", shots);
}

/**
 * Load persistent settings from non-volatile storage.
 */
void loadSettings()
{
    prefs.begin("sodastream", false);
    lifeBottleCount = prefs.getUInt("bottles", 0);
    shotsThisCan    = prefs.getUShort("shots", 0);
    currentMg       = prefs.getInt("mg", MAX_CAPACITY_MG);
    msPerShot       = prefs.getShort("ms_shot", 2050);
    mgPerShot       = prefs.getShort("mg_shot", 1470);
    wifiSsid        = prefs.getString("wifi_ssid", "");
    wifiPass        = prefs.getString("wifi_pass", "");
    ntpServer       = prefs.getString("ntp_server", "pool.ntp.org");
    tzString        = prefs.getString("tz_str", "MST7");
    prefs.end();

    if (currentMg < 0 || currentMg > MAX_CAPACITY_MG) {
        currentMg = MAX_CAPACITY_MG;
    }
    if (msPerShot < 2000 || msPerShot > 2100) {
        msPerShot = 2050;
    }
    if (mgPerShot < 1270 || mgPerShot > 1670) {
        mgPerShot = 1470;
    }

    Serial.printf("Settings loaded from NVS: bottles=%u, shots=%u, mg=%d, ms/shot=%d, mg/shot=%d, Wi-Fi='%s', NTP='%s', TZ='%s'\n",
                  lifeBottleCount, shotsThisCan, currentMg, msPerShot, mgPerShot, wifiSsid.c_str(), ntpServer.c_str(), tzString.c_str());
}

/**
 * Persists Wi-Fi network credentials to flash.
 */
void saveWiFiCredentials(const String& ssid, const String& pass)
{
    wifiSsid = ssid;
    wifiPass = pass;
    prefs.begin("sodastream", false);
    prefs.putString("wifi_ssid", wifiSsid);
    prefs.putString("wifi_pass", wifiPass);
    prefs.end();
    Serial.printf("Wi-Fi credentials saved: SSID='%s'\n", wifiSsid.c_str());
}

/**
 * Persists NTP server and Timezone string to flash.
 */
void saveTimeSettings(const String& ntp, const String& tz)
{
    ntpServer = ntp;
    tzString  = tz;
    prefs.begin("sodastream", false);
    prefs.putString("ntp_server", ntpServer);
    prefs.putString("tz_str", tzString);
    prefs.end();
    Serial.printf("Time settings saved: NTP='%s', TZ='%s'\n", ntpServer.c_str(), tzString.c_str());
}

/**
 * Persists runtime cycle counts and remaining CO2 gas to flash.
 */
void saveCycleData()
{
    prefs.begin("sodastream", false);
    prefs.putUInt("bottles", lifeBottleCount);
    prefs.putUShort("shots", shotsThisCan);
    prefs.putInt("mg", currentMg);
    prefs.end();
}

/**
 * Persists canister reset state to flash.
 */
void saveResetData()
{
    prefs.begin("sodastream", false);
    prefs.putUShort("shots", shotsThisCan);
    prefs.putInt("mg", currentMg);
    prefs.end();
}

/**
 * Persists msPerShot slider calibration to flash.
 */
void saveMsPerShot(int16_t val)
{
    msPerShot = val;
    prefs.begin("sodastream", false);
    prefs.putShort("ms_shot", msPerShot);
    prefs.end();
}

/**
 * Persists mgPerShot slider calibration to flash.
 */
void saveMgPerShot(int16_t val)
{
    mgPerShot = val;
    prefs.begin("sodastream", false);
    prefs.putShort("mg_shot", mgPerShot);
    prefs.end();
}

/**
 * Event callback triggered when the user touches or drags the arc on screen.
 */
static void on_arc_value_changed(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        lv_obj_t * target = lv_event_get_target(e);
        int arcVal = lv_arc_get_value(target);
        // Each arc unit is 100 mg (0.1 g) when range is 0..4100
        int32_t mg = (int32_t)arcVal * 100;
        updateGauge(mg);
    } else if (code == LV_EVENT_RELEASED) {
        saveCycleData();
    }
}

/**
 * Event callback triggered when Button2 on Screen 6 is long-pressed.
 * Resets the CO2 gauge to full capacity (410,000 mg = 410.0 g) and resets the shot counter.
 */
static void on_reset_btn_long_press(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_LONG_PRESSED) {
        Serial.println("CO2 Canister Reset triggered!");

        // Reset gauge to full capacity (410,000 mg = 410.0 g)
        currentMg = MAX_CAPACITY_MG;
        updateGauge(currentMg);

        // Reset shots this can counter on Screen 3 & Screen 6
        shotsThisCan = 0;
        updateShotsDisplay(shotsThisCan);

        // Save reset state to NVS
        saveResetData();
    }
}

/**
 * Event callback for Screen 4 Time per Shot slider (ui_mspsSlider).
 */
static void on_msps_slider_event(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * slider = lv_event_get_target(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        msPerShot = (int16_t)lv_slider_get_value(slider);
        lv_label_set_text_fmt(ui_msecPerShot, "%d", msPerShot);
    } else if (code == LV_EVENT_RELEASED) {
        msPerShot = (int16_t)lv_slider_get_value(slider);
        saveMsPerShot(msPerShot);
        Serial.printf("Updated and saved msPerShot: %d ms\n", msPerShot);
    }
}

/**
 * Event callback for Screen 5 Milligrams per Shot slider (ui_mgpsSlider).
 */
static void on_mgps_slider_event(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * slider = lv_event_get_target(e);
    if (code == LV_EVENT_VALUE_CHANGED) {
        mgPerShot = (int16_t)lv_slider_get_value(slider);
        lv_label_set_text_fmt(ui_mgPerShot, "%d", mgPerShot);
    } else if (code == LV_EVENT_RELEASED) {
        mgPerShot = (int16_t)lv_slider_get_value(slider);
        saveMgPerShot(mgPerShot);
        Serial.printf("Updated and saved mgPerShot: %d mg\n", mgPerShot);
    }
}

/**
 * Marks activity (resets both our own timer and LVGL's internal inactivity timer).
 */
void recordActivity()
{
    lastActivityTime = millis();
    lv_disp_trig_activity(NULL);
}

/**
 * Transitions the display into sleep mode:
 * 1. Loads a pure black blank screen to relax the IPS liquid crystals.
 * 2. Turns off the backlight to save power, reduce heat, and prevent LED degradation.
 */
void sleepDisplay()
{
    if (!isAsleep) {
        isAsleep = true;
        lv_disp_load_scr(ui_BlankScreen);
        digitalWrite(BACKLIGHT_PIN, LOW);
        Serial.println("Display sleeping (backlight OFF, blank screen loaded).");
    }
}

/**
 * Wakes the display from sleep mode:
 * 1. Turns the backlight ON.
 * 2. Transitions back to Screen 1 (Dashboard).
 * 3. Refreshes the activity timer.
 */
void wakeDisplay()
{
    if (isAsleep) {
        isAsleep = false;
        digitalWrite(BACKLIGHT_PIN, HIGH);
        lv_disp_load_scr(ui_Screen1);
        recordActivity();
        Serial.println("Display woke up, returned to Screen 1.");
    }
}

/**
 * Event callback for any touch on the blank sleep screen.
 * Safely wakes the display and returns to Screen 1 without accidental button clicks.
 */
static void on_blank_screen_touch(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_CLICKED) {
        wakeDisplay();
    }
}

/**
 * Serves the main web dashboard and configuration form.
 */
void handleRoot()
{
    if (isAsleep) {
        wakeDisplay();
    } else {
        recordActivity();
    }

    int wholeGrams = currentMg / 1000;
    int tenths     = (currentMg % 1000) / 100;
    int percent    = (int)((currentMg * 100LL) / MAX_CAPACITY_MG);

    String netStatus;
    if (isApMode) {
        netStatus = "Mode: Fallback Access Point (SSID: <b>" + String(AP_SSID) + "</b>, IP: <b>" + AP_IP.toString() + "</b>)";
    } else {
        netStatus = "Mode: Connected to <b>" + wifiSsid + "</b> (IP: <b>" + WiFi.localIP().toString() + "</b> | Hostname: <b><a href='http://sodastream.local' style='color:#90CAF9;'>sodastream.local</a></b>)";
    }

    String html;
    html.reserve(2500);
    html += "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>SodaStream CO2 Monitor</title>";
    html += "<style>";
    html += "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#121212;color:#E0E0E0;margin:0;padding:16px;}";
    html += ".card{background:#1E1E1E;border-radius:12px;padding:18px;margin-bottom:16px;max-width:440px;margin-left:auto;margin-right:auto;box-shadow:0 4px 12px rgba(0,0,0,0.4);}";
    html += "h1{font-size:1.35rem;color:#2196F3;margin:0 0 12px 0;text-align:center;}";
    html += "h2{font-size:1.05rem;color:#90CAF9;margin:16px 0 10px 0;border-bottom:1px solid #333;padding-bottom:6px;}";
    html += ".stat{display:flex;justify-content:space-between;margin:8px 0;font-size:0.95rem;}";
    html += ".val{font-weight:bold;color:#4CAF50;}";
    html += "label{display:block;margin:12px 0 4px;font-size:0.85rem;color:#B0BEC5;}";
    html += "input{width:100%;box-sizing:border-box;padding:10px;background:#2A2A2A;border:1px solid #444;border-radius:6px;color:#FFF;font-size:1rem;}";
    html += "button{width:100%;padding:12px;border:none;border-radius:6px;font-weight:bold;font-size:1rem;cursor:pointer;margin-top:14px;}";
    html += ".btn-save{background:#1976D2;color:#FFF;}";
    html += ".btn-save:hover{background:#2196F3;}";
    html += ".btn-reset{background:#D32F2F;color:#FFF;margin-top:8px;}";
    html += ".btn-reset:hover{background:#F44336;}";
    html += ".footer{text-align:center;font-size:0.75rem;color:#757575;max-width:440px;margin:20px auto 0;line-height:1.4;}";
    html += "</style></head><body>";

    html += "<div class='card'>";
    html += "<h1>SodaStream CO2 Monitor</h1>";
    html += "<h2>Current Status</h2>";
    html += "<div class='stat'><span>CO2 Remaining:</span><span class='val'>" + String(wholeGrams) + "." + String(tenths) + " g (" + String(percent) + "%)</span></div>";
    html += "<div class='stat'><span>Lifetime Bottles:</span><span class='val'>" + String(lifeBottleCount) + "</span></div>";
    html += "<div class='stat'><span>Shots This Canister:</span><span class='val'>" + String(shotsThisCan) + "</span></div>";
    html += "<div class='stat'><span>Last Cycle Time:</span><span class='val'>" + String(lastCycleTime) + " ms</span></div>";
    html += "</div>";

    html += "<div class='card'>";
    html += "<form action='/save' method='POST'>";
    html += "<h2>Calibration Settings</h2>";
    html += "<label for='msps'>Time per Shot (2000 - 2100 ms)</label>";
    html += "<input type='number' id='msps' name='msps' min='2000' max='2100' value='" + String(msPerShot) + "' required>";

    html += "<label for='mgps'>Milligrams per Shot (1270 - 1670 mg)</label>";
    html += "<input type='number' id='mgps' name='mgps' min='1270' max='1670' value='" + String(mgPerShot) + "' required>";

    html += "<h2>Time & NTP Settings</h2>";
    html += "<label for='ntp'>NTP Time Server</label>";
    html += "<input type='text' id='ntp' name='ntp' value='" + ntpServer + "' placeholder='pool.ntp.org'>";

    html += "<label for='tz'>Timezone (POSIX Format)</label>";
    html += "<input type='text' id='tz' name='tz' value='" + tzString + "' placeholder='MST7'>";
    html += "<div style='font-size:0.75rem;color:#78909C;margin-top:4px;'>MST7 (Arizona/Phoenix), PST8PDT (Pacific), MST7MDT (Mountain), CST6CDT (Central), EST5EDT (Eastern)</div>";

    html += "<h2>Wi-Fi Setup</h2>";
    html += "<label for='ssid'>Wi-Fi Network Name (SSID)</label>";
    html += "<input type='text' id='ssid' name='ssid' value='" + wifiSsid + "' placeholder='Home Wi-Fi SSID'>";

    html += "<label for='pass'>Wi-Fi Password</label>";
    html += "<input type='password' id='pass' name='pass' placeholder='Leave blank to keep current password'>";

    html += "<button type='submit' class='btn-save'>Save & Apply</button>";
    html += "</form>";
    html += "</div>";

    html += "<div class='card'>";
    html += "<h2>Clock Management</h2>";
    html += "<form action='/sync_clock' method='POST'>";
    html += "<button type='submit' class='btn-save' style='background:#00897B;'>Sync Clock via NTP Now</button>";
    html += "</form>";
    html += "</div>";

    html += "<div class='card'>";
    html += "<h2>Canister Maintenance</h2>";
    html += "<form action='/reset' method='POST' onsubmit=\"return confirm('Reset canister to full 410.0g?');\">";
    html += "<button type='submit' class='btn-reset'>Reset Canister to Full (410g)</button>";
    html += "</form>";
    html += "</div>";

    html += "<div class='footer'>" + netStatus + "</div>";
    html += "</body></html>";

    server.send(200, "text/html", html);
}

/**
 * Handles parameter saving from the web interface.
 */
void handleSave()
{
    if (isAsleep) {
        wakeDisplay();
    } else {
        recordActivity();
    }

    bool wifiUpdated = false;

    // 1. Process msPerShot
    if (server.hasArg("msps")) {
        int val = server.arg("msps").toInt();
        if (val >= 2000 && val <= 2100) {
            saveMsPerShot((int16_t)val);
            lv_slider_set_value(ui_mspsSlider, msPerShot, LV_ANIM_OFF);
            lv_label_set_text_fmt(ui_msecPerShot, "%d", msPerShot);
        }
    }

    // 2. Process mgPerShot
    if (server.hasArg("mgps")) {
        int val = server.arg("mgps").toInt();
        if (val >= 1270 && val <= 1670) {
            saveMgPerShot((int16_t)val);
            lv_slider_set_value(ui_mgpsSlider, mgPerShot, LV_ANIM_OFF);
            lv_label_set_text_fmt(ui_mgPerShot, "%d", mgPerShot);
        }
    }

    // 3. Process NTP and Timezone settings
    if (server.hasArg("ntp") || server.hasArg("tz")) {
        String newNtp = server.hasArg("ntp") ? server.arg("ntp") : ntpServer;
        String newTz  = server.hasArg("tz")  ? server.arg("tz")  : tzString;
        newNtp.trim();
        newTz.trim();
        if (newNtp.length() > 0 && newTz.length() > 0 && (newNtp != ntpServer || newTz != tzString)) {
            saveTimeSettings(newNtp, newTz);
            if (WiFi.status() == WL_CONNECTED) {
                syncRtcWithNtp();
            }
        }
    }

    // 4. Process Wi-Fi credentials
    if (server.hasArg("ssid")) {
        String newSsid = server.arg("ssid");
        newSsid.trim();
        String newPass = server.arg("pass");

        if (newSsid.length() > 0 && (newSsid != wifiSsid || newPass.length() > 0)) {
            if (newPass.length() == 0 && newSsid == wifiSsid) {
                newPass = wifiPass; // Retain existing password
            }
            saveWiFiCredentials(newSsid, newPass);
            wifiUpdated = true;
        }
    }

    if (wifiUpdated) {
        String resp;
        resp += "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
        resp += "<style>body{font-family:sans-serif;background:#121212;color:#FFF;text-align:center;padding:40px;}</style></head><body>";
        resp += "<h2 style='color:#4CAF50;'>Settings Saved!</h2><p>Rebooting to connect to <b>" + wifiSsid + "</b>...</p>";
        resp += "<p>You can reconnect your phone to your home network and access <b>http://sodastream.local</b> in a few moments.</p>";
        resp += "</body></html>";
        server.send(200, "text/html", resp);
        delay(1500);
        ESP.restart();
    } else {
        server.sendHeader("Location", "/");
        server.send(303);
    }
}

/**
 * Handles Canister Reset triggered from the web interface.
 */
void handleReset()
{
    if (isAsleep) {
        wakeDisplay();
    } else {
        recordActivity();
    }

    currentMg = MAX_CAPACITY_MG;
    shotsThisCan = 0;
    updateGauge(currentMg);
    updateShotsDisplay(shotsThisCan);
    saveResetData();

    server.sendHeader("Location", "/");
    server.send(303);
}

/**
 * Handles manual Clock Sync triggered from the web interface.
 */
void handleSyncClock()
{
    if (isAsleep) {
        wakeDisplay();
    } else {
        recordActivity();
    }

    syncRtcWithNtp();
    server.sendHeader("Location", "/");
    server.send(303);
}

/**
 * Handles 404 / Captive Portal redirection.
 */
void handleNotFound()
{
    if (isApMode) {
        // Captive portal: redirect any external requests to the root setup page
        server.sendHeader("Location", String("http://") + AP_IP.toString() + "/");
        server.send(302, "text/plain", "");
    } else {
        server.send(404, "text/plain", "404 Not Found");
    }
}

/**
 * Updates the IP address label on Screen 6 based on current network mode.
 */
void updateIpDisplay()
{
    if (isApMode) {
        lv_label_set_text(ui_ipAddress, "AP: 192.168.4.1");
    } else if (WiFi.status() == WL_CONNECTED) {
        lv_label_set_text_fmt(ui_ipAddress, "IP: %s", WiFi.localIP().toString().c_str());
    } else {
        lv_label_set_text(ui_ipAddress, "IP: Disconnected");
    }
}

/**
 * Initializes Wi-Fi connection with fallback Access Point and Web Server.
 */
void initWiFi()
{
    WiFi.persistent(false);

    if (wifiSsid.length() > 0) {
        Serial.printf("Attempting connection to saved Wi-Fi '%s'...\n", wifiSsid.c_str());
        WiFi.mode(WIFI_STA);
        WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());

        unsigned long startAttempt = millis();
        // Wait up to 10 seconds, pumping LVGL timer so screen and RTC stay active
        while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
            lv_timer_handler();
            delay(100);
        }
    }

    if (WiFi.status() == WL_CONNECTED) {
        isApMode = false;
        Serial.printf("Connected to Wi-Fi! IP: %s\n", WiFi.localIP().toString().c_str());
        if (MDNS.begin("sodastream")) {
            MDNS.addService("http", "tcp", 80);
            Serial.println("mDNS responder active at http://sodastream.local");
        }
        // Update Screen 6 IP address label
        updateIpDisplay();

        // Initial NTP Sync on Wi-Fi connection at boot
        syncRtcWithNtp();
    } else {
        // Fallback to AP Mode
        isApMode = true;
        Serial.println("Wi-Fi connection unavailable. Starting fallback Access Point...");
        WiFi.mode(WIFI_AP);
        WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
        WiFi.softAP(AP_SSID);
        dnsServer.start(DNS_PORT, "*", AP_IP);
        Serial.printf("Access Point '%s' ready at http://%s\n", AP_SSID, AP_IP.toString().c_str());

        // Update Screen 6 IP address label
        updateIpDisplay();
    }

    // Register routes
    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.on("/reset", HTTP_POST, handleReset);
    server.on("/sync_clock", HTTP_POST, handleSyncClock);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("HTTP Web Server listening on port 80.");
}

void setup()
{
    Serial.begin( 115200 );
    delay(1000); // Allow serial monitor time to connect

    String LVGL_Arduino = "Hello Arduino! ";
    LVGL_Arduino += String('V') + lv_version_major() + "." + lv_version_minor() + "." + lv_version_patch();
    Serial.println( LVGL_Arduino );

    lv_init();

    // Set screen rotation (0 to 3) before display & touch init
    screen_rotation = 3;

    // Initializes GC9A01 TFT, backlight, LVGL display buffer, and disp driver
    lv_xiao_disp_init();

    // Initializes CHSC6X touch controller over I2C and LVGL indev driver (also initializes Wire)
    lv_xiao_touch_init();

    // Ensure backlight pin is configured as output and ON
    pinMode(BACKLIGHT_PIN, OUTPUT);
    digitalWrite(BACKLIGHT_PIN, HIGH);

    // Initialize the onboard PCF8563 / BM8563 RTC on the shared I2C bus
    initRtc();

#if LV_USE_LOG != 0
    lv_log_register_print_cb( my_print );
#endif

    // Initialize GPIO and Interrupt for SodaStream Signal
    pinMode(SIGNAL_PIN, INPUT_PULLDOWN);
    attachInterrupt(digitalPinToInterrupt(SIGNAL_PIN), onSignalChange, CHANGE);

    // Load persistent data from NVS
    loadSettings();

    // Initialize the SquareLine Studio generated UI
    ui_init();

    // Create the blank black screen used during sleep
    ui_BlankScreen = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_BlankScreen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ui_BlankScreen, lv_color_hex(0x000000), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ui_BlankScreen, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_add_event_cb(ui_BlankScreen, on_blank_screen_touch, LV_EVENT_ALL, NULL);

    // Ensure Arc range is set to 0..4100 (tenths of a gram)
    lv_arc_set_range(ui_remainingShots, 0, ARC_MAX_RANGE);

    // Hook up touch listener to the arc on Screen 1
    lv_obj_add_event_cb(ui_remainingShots, on_arc_value_changed, LV_EVENT_ALL, NULL);

    // Hook up long-press listener to the Reset Button (ui_Button2) on Screen 6
    lv_obj_add_event_cb(ui_Button2, on_reset_btn_long_press, LV_EVENT_LONG_PRESSED, NULL);

    // Hook up listeners for Screen 4 & Screen 5 calibration sliders
    lv_obj_add_event_cb(ui_mspsSlider, on_msps_slider_event, LV_EVENT_ALL, NULL);
    lv_obj_add_event_cb(ui_mgpsSlider, on_mgps_slider_event, LV_EVENT_ALL, NULL);

    // Populate UI elements from loaded settings
    updateGauge(currentMg);
    lv_label_set_text_fmt(ui_lifeBottleCount, "%u", lifeBottleCount);
    lv_label_set_text(ui_lastCycleTime, "0");
    updateShotsDisplay(shotsThisCan);

    lv_slider_set_value(ui_mspsSlider, msPerShot, LV_ANIM_OFF);
    lv_label_set_text_fmt(ui_msecPerShot, "%d", msPerShot);

    lv_slider_set_value(ui_mgpsSlider, mgPerShot, LV_ANIM_OFF);
    lv_label_set_text_fmt(ui_mgPerShot, "%d", mgPerShot);

    // Initial state of IP address label on Screen 6
    lv_label_set_text(ui_ipAddress, "IP: Connecting...");

    // Initial state of in-cycle indicator on Screen 1 (bright USER_1 if HIGH, default dim if LOW)
    if (digitalRead(SIGNAL_PIN) == HIGH) {
        lv_obj_add_state(ui_incycleIndicator, LV_STATE_USER_1);
    } else {
        lv_obj_clear_state(ui_incycleIndicator, LV_STATE_USER_1);
    }

    // Display current RTC time immediately
    updateRtcDisplay();

    // Initialize activity timestamp
    recordActivity();

    // Start Wi-Fi connection and Web Server
    initWiFi();

    Serial.println( "Setup done" );
}

void loop()
{
    lv_timer_handler(); /* Let LVGL process touch and render frames */

    // Service Web Server and Captive Portal DNS
    server.handleClient();
    if (isApMode) {
        dnsServer.processNextRequest();
    }

    // Sync activity timer whenever user touches any screen
    if (lv_disp_get_inactive_time(NULL) < 50) {
        lastActivityTime = millis();
    }

    // Non-blocking RTC update every 1 second (1000 ms)
    static unsigned long lastRtcUpdate = 0;
    if (millis() - lastRtcUpdate >= 1000) {
        lastRtcUpdate = millis();
        updateRtcDisplay();
    }

    // Periodic NTP sync check (every 24 hours if connected)
    if (WiFi.status() == WL_CONNECTED && (millis() - lastNtpSync >= NTP_SYNC_INTERVAL_MS)) {
        syncRtcWithNtp();
    }

    // Process finished injection cycle
    if (newCycleReady) {
        noInterrupts();
        unsigned long duration = lastCycleTime;
        newCycleReady = false;
        interrupts();

        Serial.printf("Injection pulse ended. Duration: %lu ms\n", duration);

        // 1. Always update Screen 2 Last Cycle Time
        lv_label_set_text_fmt(ui_lastCycleTime, "%lu", duration);

        // 2. Filter: Discard noise if duration < msPerShot
        if (duration >= (unsigned long)msPerShot) {
            uint16_t shots = duration / msPerShot;
            Serial.printf("Calculated shots: %u (msPerShot: %d)\n", shots, msPerShot);

            // 3. Valid Bottle Cycle Requirement: shots >= 2
            if (shots >= 2) {
                lifeBottleCount++;
                shotsThisCan += shots;

                int32_t mgExpended = (int32_t)shots * mgPerShot;
                currentMg = max((int32_t)0, currentMg - mgExpended);

                Serial.printf("Valid bottle cycle: +1 bottle (total %u), +%u shots (total %u), used %d mg (remaining %d mg)\n",
                              lifeBottleCount, shots, shotsThisCan, mgExpended, currentMg);

                // Update UI elements across screens
                updateGauge(currentMg);
                lv_label_set_text_fmt(ui_lifeBottleCount, "%u", lifeBottleCount);
                updateShotsDisplay(shotsThisCan);

                // Persist updated values to flash
                saveCycleData();
            } else {
                Serial.printf("Pulse had only %u shot (< 2 shots needed for bottle). Bottle & CO2 not decremented.\n", shots);
            }
        } else {
            Serial.println("Pulse duration shorter than msPerShot; ignored as noise.");
        }

        // Cycle complete: refresh activity timer so 5-min timeout starts from cycle completion
        recordActivity();
    }

    // Read SodaStream D0 signal state
    int signalVal = digitalRead(SIGNAL_PIN);

    // Update in-cycle indicator on Screen 1 (bright USER_1 when HIGH, default dim when LOW)
    static int lastSignalVal = -1;
    if (signalVal != lastSignalVal) {
        lastSignalVal = signalVal;
        if (ui_incycleIndicator != NULL) {
            if (signalVal == HIGH) {
                lv_obj_add_state(ui_incycleIndicator, LV_STATE_USER_1);
            } else {
                lv_obj_clear_state(ui_incycleIndicator, LV_STATE_USER_1);
            }
        }
    }

    // Manage Sleep & Wake states
    if (isAsleep) {
        // If the valve starts firing or a cycle finishes while asleep, wake up immediately
        if (signalVal == HIGH || newCycleReady) {
            wakeDisplay();
        }
    } else {
        // Keep activity alive while valve is actively firing
        if (signalVal == HIGH) {
            recordActivity();
        }

        // Enter sleep if both touch and valve/cycle have been inactive for SLEEP_TIMEOUT_MS
        if ((millis() - lastActivityTime >= SLEEP_TIMEOUT_MS) &&
            (lv_disp_get_inactive_time(NULL) >= SLEEP_TIMEOUT_MS)) {
            sleepDisplay();
        }
    }

    delay(5); // Small yield for background OS tasks
}
