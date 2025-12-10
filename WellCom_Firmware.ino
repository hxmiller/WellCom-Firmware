/*
   WellCom – ESP32-C3 Wellness Communication Device
   Target board: Seeed XIAO ESP32-C3

   Hardware
   --------
   - LEDs (active HIGH):
       GREEN  -> D2 (GPIO4)
       RED    -> D3 (GPIO5)
   - Buttons (wired to GND, INPUT_PULLUP):
       WELL   -> D4 (GPIO6)
       ILL    -> D5 (GPIO7)
       RESET  -> D6 (GPIO21)

   Persistent data in NVS ("wellcom" namespace)
   --------------------------------------------
   - ssid, pwd              : Wi-Fi credentials
   - to_phone, from_phone   : 11-digit US phone numbers (leading '1')
   - to_name, from_name     : names used inside SMS text
   - tz_id, tz_hours        : timezone selection + optional custom UTC offset
   - device_name            : human-friendly device ID (written by init sketch)
   - last_msg_ymd           : last WELL/ILL day (YYYYMMDD)
   - last_msg_hour/min      : last WELL/ILL local time (HH/MM)
   - last_none_ymd          : last automatic “None” reminder day (YYYYMMDD)

   Backend / Twilio architecture
   -----------------------------
   - The device does NOT talk to Twilio directly.
   - All SMS are sent via HTTPS POST to BACKEND_URL (Heroku Flask app).
   - JSON body includes: device_name, firmware version, to, from, message.
   - The Heroku backend owns the Twilio SID/Auth Token and real secrets.
   - TWILIO_FROM_NUMBER here is only for logging / identification.

   Boot sequence
   -------------
   1. Turn both LEDs ON for 1 second (power-on sanity check).
   2. Read device_name from NVS:
        - If missing → RED on, GREEN off, globalStatus = false, stop.
   3. Load Wi-Fi / phone / timezone config and last-sent state from NVS.
      If PRODUCTION_MODE == false, clear last-sent state in NVS (test bench mode).
   4. If config is valid:
        - Connect to Wi-Fi.
        - Set timezone from tz_id/tz_hours and fetch time via NTP.
        - On success: globalStatus = true and immediately send a “Test” SMS
          to from_phone to confirm the device has restarted.
      Otherwise or on failure:
        - Enter config-portal AP mode (SSID: "WellCom", open network).
        - Green/Red LEDs alternate to indicate AP / setup mode.
        - HTTP config form is served at http://192.168.4.1/.

   Main loop behaviour
   -------------------
   - Always:
       * updateGreenBlink() for non-blocking send animation.
       * handleButtons() to process WELL / ILL / RESET presses.
   - If Wi-Fi mode is AP:
       * updateApBlink() to alternate GREEN/RED.
       * server.handleClient() to serve the config pages.
   - Normal station mode (Wi-Fi STA) with globalStatus == true:
       * handleDailyLogic():
           - After 10:00 local time:
               · If a WELL/ILL message has already been sent today → do nothing.
               · Else if a “None” reminder has already been sent today → do nothing.
               · Else send a “None” reminder SMS to BOTH to_phone and from_phone
                 and record last_none_ymd in NVS.

   Button mapping
   --------------
   - RESET (highest priority, always active):
       <  5 seconds : rebootDevice()
       5–10 seconds : enter config portal (Perform_setup → AP + web form)
       > 10 seconds : perform OTA firmware update (Perform_download from OTA_FIRMWARE_URL)
   - WELL:
       < 0.5 seconds : blink GREEN solid for 2 seconds (local acknowledgement only)
       ≥ 0.5 seconds : send “Well” SMS to to_phone
   - ILL:
       < 0.5 seconds : blink RED solid for 2 seconds (local acknowledgement only)
       ≥ 0.5 seconds : send “Ill” SMS to to_phone

   SMS message types
   -----------------
   - "Test" : sent on successful boot; goes to from_phone only.
   - "Well" : user says they are OK; goes to to_phone only,
              using a randomly chosen friendly template.
   - "Ill"  : urgent “not feeling well” message; goes to to_phone only,
              with stronger wording.
   - "None" : automatic daily reminder if no WELL/ILL by 10:00 AM;
              sent to BOTH to_phone and from_phone.

   LED behaviour during sending
   ----------------------------
   - For "Ill":
       * 5-second RED pre-blink (blocking) before contacting backend.
   - For "Test", "Well", "None":
       * 5-second GREEN pre-blink using non-blocking timer.
   - After all backend calls succeed:
       * GREEN solid for 3 seconds, then OFF.
   - On any failure (no Wi-Fi or non-200/201 HTTP status):
       * RED LED turns ON and stays on, and globalStatus is set to false
         so no further texts are sent until reboot.

   OTA firmware update
   -------------------
   - Triggered by holding RESET > 10 seconds.
   - Downloads firmware from OTA_FIRMWARE_URL over HTTPS into the OTA partition.
   - On success: prints status and reboots into the new image.
   - On error: aborts update, lights RED, sets globalStatus = false.
*/


#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Update.h>
#include <time.h>

// =================== PIN DEFINITIONS (ADJUST AS NEEDED) ===================

// For XIAO ESP32C3 (from pins_arduino.h):
// D2 -> GPIO4
// D3 -> GPIO5
// D4 -> GPIO6
// D5 -> GPIO7
// D6 -> GPIO21

// LEDs (assume active HIGH, wired: GPIO -> resistor -> LED -> GND)
const uint8_t PIN_LED_GREEN = D2;  // Green LED on board pin D2 (GPIO4)
const uint8_t PIN_LED_RED   = D3;  // Red LED   on board pin D3 (GPIO5)

// Buttons (wiring: button -> pin, other side -> GND, use INPUT_PULLUP)
const uint8_t PIN_BTN_WELL  = D4;  // WELL button on D4 (GPIO6)
const uint8_t PIN_BTN_ILL   = D5;  // ILL  button on D5 (GPIO7)
const uint8_t PIN_BTN_RESET = D6;  // RESET button on D6 (GPIO21)

// =================== TWILIO CONFIG (FILL IN YOUR REAL VALUES) ===================

// Twilio REST API          
const char *TWILIO_FROM_NUMBER = "+18667247232";      //E.164 format

// Twilio endpoint
const char *TWILIO_API_HOST = "api.twilio.com";
const int   TWILIO_API_PORT = 443;

// =================== OTA / DRIVEHQ CONFIG (SKELETON) ===================

// Backend URL (Heroku – for SMS backend)
const char* BACKEND_URL =
  "https://wellcom-backend-c1cd087ec64f.herokuapp.com/api/v1/send_sms";

// Placeholder firmware URL for OTA (update this when you have a real .bin URL)
const char* OTA_FIRMWARE_URL =
  "https://raw.githubusercontent.com/hxmiller/WellCom-Firmware/main/WellCom_latest.bin";

// =================== TIME / NTP CONFIG ===================

const char *NTP_SERVER = "pool.ntp.org";

// =================== GLOBAL STATE ===================

Preferences prefs;

String gDeviceName;  // global device name read from flash

// Define a firmware version string:
#define WELLCOM_FIRMWARE_VERSION "1.1.1.2"  // update as you release new versions

bool globalStatus = false;

// Not-so-secret version string in code:
//const char* Version = "WellCom_v1.1 2025 11 08";   // update as you release new firmware

// For button long/short press timing
unsigned long wellPressStart  = 0;
unsigned long illPressStart   = 0;
unsigned long resetPressStart = 0;

bool wellWasPressed  = false;
bool illWasPressed   = false;
bool resetWasPressed = false;

// We will ignore new button events while processing a previous one
bool busyProcessingButton = false;

// Thresholds
const unsigned long SHORT_PRESS_MS      = 500;  // 1/2 second
const unsigned long RESET_SHORT_MS      = 5000;  // 5 seconds
const unsigned long RESET_MEDIUM_MS     = 10000; // 10 seconds

// Non-blocking green LED blinking during Send_text
bool greenBlinkActive       = false;
unsigned long lastGreenBlinkMs = 0;
unsigned long greenBlinkIntervalMs = 300;
bool greenBlinkState        = false;

// Track last days (YYYYMMDD) for user messages and "None" reminders
int lastMsgYMD  = 0;   // last Well/Ill day
int lastNoneYMD = 0;   // last day we sent "None"

// Track last Well/Ill message time (local HH:MM)
int lastMsgHour = -1;
int lastMsgMin  = -1;

int lastTenAmDebugYMD = 0;  // last day we printed the 10:00 AM debug info

// AP mode LED alternating blink
bool apBlinkState = false;
unsigned long lastApBlinkMs = 0;
const unsigned long AP_BLINK_INTERVAL_MS = 250;  // 1/4 second

// =================== MODE: PRODUCTION vs TEST ===================
const bool PRODUCTION_MODE = true;   // set to false while bench-testing

// =================== CONFIG STRUCT ===================

struct WellComConfig {
  String wifi_ssid;
  String wifi_pwd;
  String to_phone;    // 11-digit, starting with '1'
  String from_phone;  // 11-digit, starting with '1'
  String to_name;
  String from_name;
  String timezone_id;        // e.g. "US_Central", "US_Eastern", "Custom"
  int    tz_custom_hours;    // e.g. -6, +1, only used if timezone_id == "Custom"
};

WellComConfig config;

// =================== FORWARD DECLARATIONS ===================

void loadConfig();
void saveConfig();
bool hasValidConfig();

bool connectWiFiAndTime();
void Perform_setup();
void Perform_download();
void Send_text(const String &type);
bool twilioSendSMS(const String &toNumber, const String &message);

void startGreenBlinkNonBlocking();
void stopGreenBlink();
void updateGreenBlink();

void handleButtons();
void handleResetButtonAction(unsigned long pressMs);
void handleDailyLogic();

bool isAfterHourMinute(int targetHour, int targetMinute);
void rebootDevice();

int getTodayYMD();
String normalizePhone(const String &raw);
bool isAllDigits(const String &s);

// Config portal
WebServer server(80);
void startConfigPortal();
void handleRoot();
void handleSave();

// Random WELL messages
String getRandomWellMessage(const String &toName, const String &fromName);

// =================== SETUP ===================

void setup() {

  Serial.begin(115200);
  delay(200);

  pinMode(PIN_LED_GREEN, OUTPUT);
  pinMode(PIN_LED_RED, OUTPUT);
  pinMode(PIN_BTN_WELL, INPUT_PULLUP);
  pinMode(PIN_BTN_ILL, INPUT_PULLUP);
  pinMode(PIN_BTN_RESET, INPUT_PULLUP);

  // Step 1–3: turn both LEDs on, wait 1s
  digitalWrite(PIN_LED_GREEN, HIGH);
  digitalWrite(PIN_LED_RED, HIGH);
  delay(1000);

  globalStatus = false;

  // Read device_name from NVS
  prefs.begin("wellcom", false);  // true = read-only
  gDeviceName = prefs.getString("device_name", "");

  if (gDeviceName.length() == 0) {
    Serial.println("ERROR: device_name not found in NVS. Run init sketch!");
    // You might blink RED here or refuse to send messages.
    digitalWrite(PIN_LED_RED, HIGH);
    digitalWrite(PIN_LED_GREEN, LOW);
    globalStatus = false;
    return;
  } else {
    Serial.print("Device name loaded from NVS: ");
    Serial.println(gDeviceName);
  }

  /*
  config.wifi_ssid   = "xxx";
  config.wifi_pwd    = "xxx";
  config.to_phone    = "xxx";   // MUST be 11 digits
  config.from_phone  = "xxx";   // MUST be 11 digits
  config.to_name     = "xxx";
  config.from_name   = "xxx";
  config.timezone_id    = "US_Central";
  config.tz_custom_hours = -6;
  saveConfig();  // write these to flash
  */

  loadConfig();

  // Load last message / reminder info from NVS (0 / -1 = none yet)
  lastMsgYMD   = prefs.getInt("last_msg_ymd", 0);
  lastNoneYMD  = prefs.getInt("last_none_ymd", 0);
  lastMsgHour  = prefs.getInt("last_msg_hour", -1);
  lastMsgMin   = prefs.getInt("last_msg_min", -1);

  if (!PRODUCTION_MODE) {
    Serial.println("TEST MODE: clearing last-sent state (Well/Ill/None) in RAM + NVS");
    lastMsgYMD  = 0;
    lastNoneYMD = 0;
    lastMsgHour = -1;
    lastMsgMin  = -1;

    prefs.putInt("last_msg_ymd",  0);
    prefs.putInt("last_msg_hour", -1);
    prefs.putInt("last_msg_min",  -1);
    prefs.putInt("last_none_ymd", 0);
  }

  Serial.print("Last user message day (YMD): ");
  Serial.println(lastMsgYMD);
  Serial.print("Last user message time (HH:MM): ");
  Serial.print(lastMsgHour);
  Serial.print(":");
  Serial.println(lastMsgMin < 10 && lastMsgMin >= 0 ? "0" + String(lastMsgMin) : String(lastMsgMin));
  Serial.print("Last 'None' reminder day (YMD): ");
  Serial.println(lastNoneYMD);

  if (hasValidConfig()) {
    Serial.println("Config found; attempting WiFi + time...");
    bool ok = connectWiFiAndTime();
    if (ok) {
      Serial.println("Connected to Wi-Fi");
      globalStatus = true;          // <-- so Send_text() will run
      Send_text("Test");            // optional, but matches your design
    } else {
      // WiFi/time failed → go to AP config (unchanged)
      Serial.println("WiFi/time failed. Entering AP config mode.");
      digitalWrite(PIN_LED_GREEN, LOW);
      digitalWrite(PIN_LED_RED, LOW);
      globalStatus = false;
      Perform_setup();
      return;  // nothing more to do in setup()
    }
  }
}

// =================== LOOP ===================

void loop() {
  updateGreenBlink();   // non-blocking blink handler (for send)

  // Always allow RESET to work
  handleButtons();

  // If we're in config portal mode (AP), blink LEDs + serve web page
  if (WiFi.getMode() == WIFI_AP) {
    updateApBlink();
    server.handleClient();
    delay(10);
    return;
  }

  // Normal operation (station mode)
  if (globalStatus) {
    handleDailyLogic();
  }

  delay(10);
}

// =================== CONFIG LOAD / SAVE ===================

void loadConfig() {
  config.wifi_ssid   = prefs.getString("ssid", "");
  config.wifi_pwd    = prefs.getString("pwd", "");
  config.to_phone    = prefs.getString("to_phone", "");
  config.from_phone  = prefs.getString("from_phone", "");
  config.to_name     = prefs.getString("to_name", "");
  config.from_name   = prefs.getString("from_name", "");
  config.timezone_id   = prefs.getString("tz_id", "US_Central"); // default
  config.tz_custom_hours = prefs.getInt("tz_hours", -6);      
}

void saveConfig() {
  prefs.putString("ssid",       config.wifi_ssid);
  prefs.putString("pwd",        config.wifi_pwd);
  prefs.putString("to_phone",   config.to_phone);
  prefs.putString("from_phone", config.from_phone);
  prefs.putString("to_name",    config.to_name);
  prefs.putString("from_name",  config.from_name);
  prefs.putString("tz_id",      config.timezone_id);
  prefs.putInt("tz_hours",      config.tz_custom_hours);
}

bool hasValidConfig() {
  return config.wifi_ssid.length() > 0 &&
         config.wifi_pwd.length() > 0 &&
         config.to_phone.length() == 11 &&
         config.from_phone.length() == 11 &&
         config.to_name.length() > 0 &&
         config.from_name.length() > 0;
}

// =================== WIFI + TIME ===================

String getTzStringFromConfig() {
  // US zones with automatic DST using POSIX TZ rules
  if (config.timezone_id == "US_Eastern") {
    return "EST5EDT,M3.2.0/2,M11.1.0/2";
  } else if (config.timezone_id == "US_Central") {
    return "CST6CDT,M3.2.0/2,M11.1.0/2";
  } else if (config.timezone_id == "US_Mountain") {
    return "MST7MDT,M3.2.0/2,M11.1.0/2";
  } else if (config.timezone_id == "US_Pacific") {
    return "PST8PDT,M3.2.0/2,M11.1.0/2";
  } else if (config.timezone_id == "US_Alaska") {
    return "AKST9AKDT,M3.2.0/2,M11.1.0/2";
  } else if (config.timezone_id == "US_Hawaii") {
    // Hawaii – no DST
    return "HST10";
  } else if (config.timezone_id == "Custom") {
    // Custom fixed offset from UTC, no DST, user enters hours like -3, +2, etc.
    int h = config.tz_custom_hours;   // e.g. -6
    int posixOffset = -h;             // POSIX offset is "hours WEST of UTC"

    // Example: h = -6 (UTC-6) -> posixOffset = 6 -> "UTC6"
    String tz = "UTC";
    tz += String(posixOffset);
    return tz;
  }

  // Fallback: US Central
  return "CST6CDT,M3.2.0/2,M11.1.0/2";
}

bool connectWiFiAndTime() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);

  Serial.printf("Connecting to SSID '%s'...\n", config.wifi_ssid.c_str());
  WiFi.begin(config.wifi_ssid.c_str(), config.wifi_pwd.c_str());

  unsigned long start = millis();
  const unsigned long timeout = 20000; // 20s
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeout) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection failed.");
    return false;
  }

  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());

  // Time with timezone + DST using TZ rules
  String tz = getTzStringFromConfig();
  Serial.print("Setting timezone: ");
  Serial.println(tz);

  // This sets TZ and starts NTP using the given server
  configTzTime(tz.c_str(), NTP_SERVER);

  Serial.println("Waiting for time...");
  time_t now = time(nullptr);
  int tries = 0;
  while (now < 8 * 3600 * 2 && tries < 20) { // check until time > 1970-ish
    delay(500);
    now = time(nullptr);
    tries++;
  }

  if (now < 8 * 3600 * 2) {
    Serial.println("Failed to get time from NTP.");
    return false;
  }

  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  Serial.printf("Current time: %s", asctime(&timeinfo));
  return true;
}

// =================== BUTTON HANDLING ===================

void handleButtons() {
  // Debounce-ish with simple state machine & timestamps
  bool wellPressed  = digitalRead(PIN_BTN_WELL)  == LOW;
  bool illPressed   = digitalRead(PIN_BTN_ILL)   == LOW;
  bool resetPressed = digitalRead(PIN_BTN_RESET) == LOW;
  unsigned long now = millis();

  // RESET BUTTON (highest priority)
  if (resetPressed && !resetWasPressed) {
    resetWasPressed = true;
    resetPressStart = now;
  } else if (!resetPressed && resetWasPressed) {
    // released
    resetWasPressed = false;
    unsigned long pressMs = now - resetPressStart;
    handleResetButtonAction(pressMs);
  }

  // In AP mode, WELL/ILL are ignored; only RESET is active
  if (WiFi.getMode() == WIFI_AP) {
    return;
  }

  if (busyProcessingButton) {
    // ignore other buttons while we are processing something
    return;
  }

  // WELL BUTTON
  if (wellPressed && !wellWasPressed) {
    wellWasPressed = true;
    wellPressStart = now;
  } else if (!wellPressed && wellWasPressed) {
    wellWasPressed = false;
    unsigned long pressMs = now - wellPressStart;

    busyProcessingButton = true;

    if (pressMs < SHORT_PRESS_MS) {
      // Short press: blink green for 2s
      Serial.println("WELL short press -> green LED blink");
      digitalWrite(PIN_LED_GREEN, HIGH);
      delay(2000);
      digitalWrite(PIN_LED_GREEN, LOW);
    } else {
      // Long press: send "Well"
      Serial.println("WELL long press -> Send_text(\"Well\")");
      Send_text("Well");
    }

    busyProcessingButton = false;
  }

  // ILL BUTTON
  if (illPressed && !illWasPressed) {
    illWasPressed = true;
    illPressStart = now;
  } else if (!illPressed && illWasPressed) {
    illWasPressed = false;
    unsigned long pressMs = now - illPressStart;

    busyProcessingButton = true;

    if (pressMs < SHORT_PRESS_MS) {
      // Short press: blink red for 2s
      Serial.println("ILL short press -> red LED blink");
      digitalWrite(PIN_LED_RED, HIGH);
      delay(2000);
      digitalWrite(PIN_LED_RED, LOW);
    } else {
      // Long press: send "Ill"
      Serial.println("ILL long press -> Send_text(\"Ill\")");
      Send_text("Ill");
    }

    busyProcessingButton = false;
  }
}

// =================== RESET BUTTON ACTIONS ===================

void handleResetButtonAction(unsigned long pressMs) {
  Serial.printf("RESET pressed for %lu ms\n", pressMs);

  if (pressMs < RESET_SHORT_MS) {
    // <5s -> reboot
    Serial.println("RESET: short press -> reboot");
    rebootDevice();
  } else if (pressMs < RESET_MEDIUM_MS) {
    // 5–10s -> Perform_setup (AP + config portal)
    Serial.println("RESET: medium press -> Perform_setup (config portal)");
    Perform_setup();
  } else {
    // >10s -> Perform_download (OTA)
    Serial.println("RESET: long press -> Perform_download (OTA)");
    Perform_download();
  }
}

// =================== DAILY LOGIC ===================

String formatYMD(int ymd) {
  if (ymd <= 0) return String("(none)");
  int year  = ymd / 10000;
  int month = (ymd / 100) % 100;
  int day   = ymd % 100;

  char buf[11];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
  return String(buf);
}

void handleDailyLogic() {
  time_t now = time(nullptr);
  if (now == 0) return; // no time yet

  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  int todayYMD = getTodayYMD();
  if (todayYMD == 0) return;

  // 1) Only care if it's after 10:00 AM local time
  if (!isAfterHourMinute(10, 0)) {
    return;
  }

  // Figure out if this is the first time we're doing the 10 AM logic today
  bool firstTimeToday = (lastTenAmDebugYMD != todayYMD);
  if (firstTimeToday) {
    lastTenAmDebugYMD = todayYMD;

    // --- DEBUG HEADER: we are at the 10:00 logic (first time this day) ---
    Serial.println("========== 10:00 AM LOGIC (first time today) ==========");
    Serial.print("Now local: ");
    Serial.print(asctime(&timeinfo)); // includes newline

    Serial.print("Today YMD: ");
    Serial.println(formatYMD(todayYMD));

    // Last Well/Ill message info
    Serial.print("Last Well/Ill message day: ");
    Serial.println(formatYMD(lastMsgYMD));
    Serial.print("Last Well/Ill time (HH:MM, 24h): ");
    if (lastMsgHour >= 0 && lastMsgMin >= 0) {
      Serial.print(lastMsgHour);
      Serial.print(":");
      if (lastMsgMin < 10) Serial.print("0");
      Serial.println(lastMsgMin);
    } else {
      Serial.println("(none recorded)");
    }

    Serial.print("Last 'None' reminder day: ");
    Serial.println(formatYMD(lastNoneYMD));
  }

  // 2) If we've already sent a "None" reminder today, do nothing
  if (lastNoneYMD == todayYMD) {
    if (firstTimeToday) {
      Serial.println("Decision: Already sent 'None' today -> DO NOT send another.");
      Serial.println("====================================");
    }
    return;
  }

  // 3) If we already have a Well/Ill message today, do nothing
  if (lastMsgYMD == todayYMD) {
    if (firstTimeToday) {
      Serial.println("Decision: A Well/Ill message has already been sent today -> DO NOT send 'None'.");
      Serial.println("====================================");
    }
    return;
  }

  // 4) Otherwise, no user message yet today, and no "None" yet -> send "None"
  if (firstTimeToday) {
    Serial.println("Decision: No message yet today and no 'None' sent -> SENDING 'None' now.");
    Serial.println("====================================");
  }
  Send_text("None");
}

bool isAfterHourMinute(int targetHour, int targetMinute) {
  time_t now = time(nullptr);
  if (now == 0) return false;
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  if (timeinfo.tm_hour > targetHour) return true;
  if (timeinfo.tm_hour < targetHour) return false;
  // same hour
  return timeinfo.tm_min >= targetMinute;
}

// =================== GREEN LED NON-BLOCKING BLINK ===================

void startGreenBlinkNonBlocking() {
  greenBlinkActive = true;
  greenBlinkState  = false;
  lastGreenBlinkMs = millis();
  digitalWrite(PIN_LED_RED, LOW);
}

void stopGreenBlink() {
  greenBlinkActive = false;
  greenBlinkState  = false;
  digitalWrite(PIN_LED_GREEN, LOW);
}

void updateApBlink() {
  if (WiFi.getMode() != WIFI_AP) return;  // only blink in AP mode

  unsigned long now = millis();
  if (now - lastApBlinkMs >= AP_BLINK_INTERVAL_MS) {
    lastApBlinkMs = now;
    apBlinkState = !apBlinkState;

    // Alternate: green on / red off, then green off / red on
    digitalWrite(PIN_LED_GREEN, apBlinkState ? HIGH : LOW);
    digitalWrite(PIN_LED_RED,   apBlinkState ? LOW  : HIGH);
  }
}


void updateGreenBlink() {
  if (!greenBlinkActive) return;
  unsigned long now = millis();
  if (now - lastGreenBlinkMs >= greenBlinkIntervalMs) {
    lastGreenBlinkMs = now;
    greenBlinkState = !greenBlinkState;
    digitalWrite(PIN_LED_GREEN, greenBlinkState ? HIGH : LOW);
  }
}

// =================== PERFORM_SETUP (CONFIG PORTAL) ===================

void Perform_setup() {
  // Turn off WiFi station, start AP
  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP("WellCom", nullptr); // open AP
  if (!ok) {
    Serial.println("Failed to start AP.");
    return;
  }

  IPAddress ip = WiFi.softAPIP();
  Serial.print("Config portal AP started at: ");
  Serial.println(ip);

  startConfigPortal(); // sets handlers and starts server
}

// Start web server with config form
void startConfigPortal() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);

  // Anything else → go back to "/"
  server.onNotFound([]() {
    server.sendHeader("Location", "/");
    server.send(302, "text/plain", "");
  });

  server.begin();
  Serial.println("Web server started for config.");
}

void handleRoot() {
  // Build the <option> list with "selected" based on current config.timezone_id
  String tzOptions;

  auto addOption = [&](const char* value, const char* label) {
    tzOptions += "<option value='";
    tzOptions += value;
    tzOptions += "'";
    if (config.timezone_id == value) {
      tzOptions += " selected";
    }
    tzOptions += ">";
    tzOptions += label;
    tzOptions += "</option>\n";
  };

  addOption("US_Eastern",  "US Eastern (ET)");
  addOption("US_Central",  "US Central (CT)");
  addOption("US_Mountain", "US Mountain (MT)");
  addOption("US_Pacific",  "US Pacific (PT)");
  addOption("US_Alaska",   "US Alaska (AKT)");
  addOption("US_Hawaii",   "US Hawaii (HST)");
  addOption("Custom",      "Custom offset from UTC");

  String html =
    "<!DOCTYPE html>\n"
    "<html>\n"
    "<head>\n"
    "  <meta charset='UTF-8'>\n"
    "  <meta name='viewport' content='width=device-width, initial-scale=1.3'>\n"
    "  <title>WellCom Configuration</title>\n"
    "  <style>\n"
    "    body { font-family: Arial, sans-serif; margin: 16px; font-size: 22px; }\n"
    "    h1 { font-size: 28px; margin-bottom: 10px; }\n"
    "    label { display: block; margin-top: 14px; }\n"
    "    input[type='text'], input[type='password'], select {\n"
    "      width: 100%;\n"
    "      max-width: 480px;\n"
    "      padding: 10px;\n"
    "      font-size: 22px;\n"
    "      box-sizing: border-box;\n"
    "      margin-top: 4px;\n"
    "    }\n"
    "    button {\n"
    "      margin-top: 24px;\n"
    "      padding: 12px 20px;\n"
    "      font-size: 22px;\n"
    "    }\n"
    "    p { font-size: 20px; }\n"
    "  </style>\n"
    "  <script>\n"
    "    function togglePwd() {\n"
    "      var x = document.getElementById('pwd');\n"
    "      if (!x) return;\n"
    "      x.type = (x.type === 'password') ? 'text' : 'password';\n"
    "    }\n"
    "  </script>\n"
    "</head>\n"
    "<body>\n"
    "  <h1>WellCom Device Setup</h1>\n"
    "  <form method='POST' action='/save'>\n"
    "    <label>Wi-Fi SSID:<br>\n"
    "      <input type='text' name='ssid' value='" + config.wifi_ssid + "'>\n"
    "    </label>\n"
    "    <label>Wi-Fi Password:<br>\n"
    "      <input type='password' id='pwd' name='pwd' value='" + config.wifi_pwd + "'>\n"
    "    </label>\n"
    "    <label style='margin-top:8px;'>\n"
    "      <input type='checkbox' onclick='togglePwd()'> Show password\n"
    "    </label>\n"
    "    <label>To Phone (10 or 11 digits):<br>\n"
    "      <input type='text' name='to_phone' value='" + config.to_phone + "'>\n"
    "    </label>\n"
    "    <label>From Phone (10 or 11 digits):<br>\n"
    "      <input type='text' name='from_phone' value='" + config.from_phone + "'>\n"
    "    </label>\n"
    "    <label>To Name:<br>\n"
    "      <input type='text' name='to_name' value='" + config.to_name + "'>\n"
    "    </label>\n"
    "    <label>From Name:<br>\n"
    "      <input type='text' name='from_name' value='" + config.from_name + "'>\n"
    "    </label>\n"
    "    <label>Time Zone:<br>\n"
    "      <select name='tz_id'>\n" +
           tzOptions +
    "      </select>\n"
    "    </label>\n"
    "    <label>Custom offset from UTC (hours, e.g. -6 for UTC-6, 2 for UTC+2):<br>\n"
    "      <input type='text' name='tz_offset' value='" + String(config.tz_custom_hours) + "'>\n"
    "    </label>\n"
    "    <button type='submit'>Save &amp; Reboot</button>\n"
    "  </form>\n"
    "  <p>After saving, the device will reboot and send a Test message.</p>\n"
    "</body>\n"
    "</html>\n";

  server.send(200, "text/html", html);
}

void handleSave() {
  // Read form fields
  String ssid         = server.arg("ssid");
  String pwd          = server.arg("pwd");
  String rawToPhone   = server.arg("to_phone");
  String rawFromPhone = server.arg("from_phone");
  String toName       = server.arg("to_name");
  String fromName     = server.arg("from_name");

  String tzId         = server.arg("tz_id");
  String tzOffsetStr  = server.arg("tz_offset");

  // Normalize phone numbers
  String toPhone   = normalizePhone(rawToPhone);
  String fromPhone = normalizePhone(rawFromPhone);

  // Basic validation
  String errorMsg;

  if (ssid.length() == 0 || pwd.length() == 0 ||
      toName.length() == 0 || fromName.length() == 0) {
    errorMsg = "SSID, password, To Name and From Name are required.";
  } else if (toPhone.length() != 11 || fromPhone.length() != 11) {
    errorMsg = "Phone numbers must be 10 or 11 digits (US).";
  }

  // Parse custom offset hours (integer) – even if not used, keep a sane value
  int tzHours = 0;
  if (tzOffsetStr.length() > 0) {
    tzHours = tzOffsetStr.toInt();  // if invalid, this becomes 0
  }

  if (errorMsg.length() > 0) {
    String resp =
      "<!DOCTYPE html>\n"
      "<html>\n"
      "<head>\n"
      "  <meta charset='UTF-8'>\n"
      "  <meta name='viewport' content='width=device-width, initial-scale=1.3'>\n"
      "  <title>WellCom Configuration Error</title>\n"
      "  <style>\n"
      "    body { font-family: Arial, sans-serif; margin: 16px; font-size: 22px; }\n"
      "    h1 { font-size: 28px; margin-bottom: 10px; }\n"
      "    p  { font-size: 20px; }\n"
      "    a  { font-size: 20px; }\n"
      "  </style>\n"
      "</head>\n"
      "<body>\n"
      "  <h1>Error</h1>\n"
      "  <p>" + errorMsg + "</p>\n"
      "  <p><a href=\"/\">Back</a></p>\n"
      "</body>\n"
      "</html>\n";

    server.send(400, "text/html", resp);
    return;
  }


  // Save into config struct
  config.wifi_ssid   = ssid;
  config.wifi_pwd    = pwd;
  config.to_phone    = toPhone;
  config.from_phone  = fromPhone;
  config.to_name     = toName;
  config.from_name   = fromName;

  // Time zone config
  if (tzId.length() == 0) {
    tzId = "US_Central";
  }
  config.timezone_id    = tzId;
  config.tz_custom_hours = tzHours;  // used only if tzId == "Custom"

  // Write to NVS
  saveConfig();

 // Confirmation page with larger font (same style as error page)
  String resp =
    "<!DOCTYPE html>\n"
    "<html>\n"
    "<head>\n"
    "  <meta charset='UTF-8'>\n"
    "  <meta name='viewport' content='width=device-width, initial-scale=1.3'>\n"
    "  <title>WellCom Configuration Saved</title>\n"
    "  <style>\n"
    "    body { font-family: Arial, sans-serif; margin: 16px; font-size: 22px; }\n"
    "    h1 { font-size: 28px; margin-bottom: 10px; }\n"
    "    p  { font-size: 20px; }\n"
    "  </style>\n"
    "</head>\n"
    "<body>\n"
    "  <h1>Saved</h1>\n"
    "  <p>Device will reboot now and send a Test message.</p>\n"
    "</body>\n"
    "</html>\n";

  server.send(200, "text/html", resp);


  delay(1000);
  rebootDevice();
}

// =================== NORMALIZE PHONE ===================
// Keep digits only; ensure 11-digit US number with leading "1"
String normalizePhone(const String &raw) {
  String digits;
  for (size_t i = 0; i < raw.length(); i++) {
    if (isdigit(raw[i])) digits += raw[i];
  }

  if (digits.length() == 10) {
    digits = "1" + digits;
  } else if (digits.length() == 11 && digits[0] != '1') {
    // Let it fail later in validation
  }
  return digits;
}

// =================== PERFORM_DOWNLOAD (OTA) ===================

void Perform_download() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Perform_download: WiFi not connected. Aborting.");
    digitalWrite(PIN_LED_GREEN, LOW);
    digitalWrite(PIN_LED_RED, HIGH);
    globalStatus = false;
    return;
  }

  Serial.println("");
  Serial.println("");
  Serial.println("Starting OTA firmware download...");
  Serial.println("Starting OTA firmware download...");
  Serial.println("Starting OTA firmware download...");
  Serial.println("");
  Serial.println("");

  WiFiClientSecure client;
  client.setInsecure(); // For quick start. Later: use real cert.

  HTTPClient https;
  if (!https.begin(client, OTA_FIRMWARE_URL)) {
    Serial.println("HTTPS begin failed.");
    digitalWrite(PIN_LED_GREEN, LOW);
    digitalWrite(PIN_LED_RED, HIGH);
    globalStatus = false;
    return;
  }

  int httpCode = https.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("HTTP GET failed, code: %d\n", httpCode);
    https.end();
    digitalWrite(PIN_LED_GREEN, LOW);
    digitalWrite(PIN_LED_RED, HIGH);
    globalStatus = false;
    return;
  }

  int contentLength = https.getSize();
  WiFiClient *stream = https.getStreamPtr();

  if (contentLength <= 0) {
    Serial.println("Content length is invalid.");
    https.end();
    digitalWrite(PIN_LED_GREEN, LOW);
    digitalWrite(PIN_LED_RED, HIGH);
    globalStatus = false;
    return;
  }

  Serial.printf("Firmware size: %d bytes\n", contentLength);

  if (!Update.begin(contentLength)) {
    Serial.println("Not enough space for OTA.");
    https.end();
    digitalWrite(PIN_LED_GREEN, LOW);
    digitalWrite(PIN_LED_RED, HIGH);
    globalStatus = false;
    return;
  }

  uint8_t buff[1024];
  int written = 0;

  while (https.connected() && written < contentLength) {
    size_t available = stream->available();
    if (available) {
      int len = stream->readBytes(buff, (available > sizeof(buff)) ? sizeof(buff) : available);
      if (len > 0) {
        if (Update.write(buff, len) != len) {
          Serial.println("OTA write error.");
          Update.abort();
          https.end();
          digitalWrite(PIN_LED_GREEN, LOW);
          digitalWrite(PIN_LED_RED, HIGH);
          globalStatus = false;
          return;
        }
        written += len;
      }
    }
    delay(1);
  }

  if (!Update.end()) {
    Serial.printf("Update failed. Error: %s\n", Update.errorString());
    https.end();
    digitalWrite(PIN_LED_GREEN, LOW);
    digitalWrite(PIN_LED_RED, HIGH);
    globalStatus = false;
    return;
  }

  https.end();

  if (!Update.isFinished()) {
    Serial.println("Update not finished?");
    digitalWrite(PIN_LED_GREEN, LOW);
    digitalWrite(PIN_LED_RED, HIGH);
    globalStatus = false;
    return;
  }

  Serial.println("OTA update successful. Rebooting...");
  rebootDevice();
}

String getCurrentLocalTime12h() {
  time_t now = time(nullptr);
  if (now == 0) {
    return "??:??";
  }

  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  int hour24 = timeinfo.tm_hour;
  int minute = timeinfo.tm_min;

  int hour12 = hour24 % 12;
  if (hour12 == 0) {
    hour12 = 12;  // 0 or 12 -> 12
  }

  const char *ampm = (hour24 >= 12) ? "PM" : "AM";

  char buf[10];
  snprintf(buf, sizeof(buf), "%d:%02d%s", hour12, minute, ampm);
  return String(buf);
}

int getTodayYMD() {
  time_t now = time(nullptr);
  if (now == 0) return 0;  // time not set

  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  int year  = timeinfo.tm_year + 1900;
  int month = timeinfo.tm_mon + 1;
  int day   = timeinfo.tm_mday;

  // Pack into YYYYMMDD integer
  return year * 10000 + month * 100 + day;
}

// =================== SEND_TEXT ===================
void Send_text(const String &type) {
  if (!globalStatus) {
    Serial.println("Send_text called but globalStatus == false, aborting.");
    digitalWrite(PIN_LED_RED, HIGH);
    digitalWrite(PIN_LED_GREEN, LOW);
    return;
  }

    // Make sure RED and GREEn are off before we start the "sending" indication
  digitalWrite(PIN_LED_RED, LOW);
  digitalWrite(PIN_LED_GREEN, LOW);

  if (type == "Ill") {
    // ----- 5-second RED pre-blink for Ill messages -----
    Serial.println("ILL message: red pre-blink before sending");
    unsigned long blinkStart = millis();
    bool redState = false;
    while (millis() - blinkStart < 5000) {   // 5000 ms = 5 seconds
      redState = !redState;
      digitalWrite(PIN_LED_RED, redState ? HIGH : LOW);
      delay(300);                            // about 300 ms per toggle
    }
    digitalWrite(PIN_LED_RED, LOW);          // make sure it's off
    // ----- end red pre-blink -----
  } else {
    // ----- existing GREEN pre-blink for other message types -----
    Serial.println("About to call startGreenBlinkNonBlocking (pre-blink)");
    startGreenBlinkNonBlocking();

    unsigned long blinkStart = millis();
    while (millis() - blinkStart < 5000) {   // 5000 ms = 5 seconds
      updateGreenBlink();                    // toggle green as needed
      delay(10);
    }
    // We'll still call stopGreenBlink() later after sending
    // ----- end green pre-blink -----
  }

  // Build a time prefix like: "Local time 13:45. "
  String timePrefix = getCurrentLocalTime12h() + " ";

  String message;
  if (type == "Test") {
    // Build labels that include names + phone numbers
    String toLabel   = config.to_name   + " (+" + config.to_phone   + ")";
    String fromLabel = config.from_name + " (+" + config.from_phone + ")";

    message = timePrefix +
              "This is a system test confirming the WellCom Device has restarted and is functioning properly." +
              "  Messages will be sent to: " + toLabel + "  from: " + fromLabel + "." +
              "  WellCom Device: " + gDeviceName + "." + 
              "  Firmware version: " + String(WELLCOM_FIRMWARE_VERSION) + ".";
  }
  else if (type == "None") {
    message = timePrefix +
              "Hello " + config.to_name + " and " + config.from_name +
              ". This is a reminder from WellCom letting you know that " +
              config.from_name +
              " has not sent a wellness communication text yet today.  I hope everyone is well.";
  } else if (type == "Ill") {
    message = timePrefix +
              "IMPORTANT MESSAGE. Hello " + config.to_name + ". " +
              config.from_name +
              " just initiated a message on WellCom to let you know they are NOT FEELING WELL today. "
              "You should contact them and/or emergency responders in their area immediately.";
  } else if (type == "Well") {
    // Random well message, but still prefixed with the time
    message = timePrefix + getRandomWellMessage(config.to_name, config.from_name);
  } else {
    message = timePrefix + "Unknown message type.";
  }

  bool okAll = true;

  // For "None" – send to BOTH to_phone and from_phone
  if (type == "None") {
    if (!twilioSendSMS("+" + config.to_phone, message)) {
      okAll = false;
    }
    if (!twilioSendSMS("+" + config.from_phone, message)) {
      okAll = false;
    }
  } 
  // For "Test" – send to from_phone
  else if (type == "Test") {
    if (!twilioSendSMS("+" + config.from_phone, message)) {
      okAll = false;
    }
  }
  else {
    // "Ill" and "Well" -> send only to to_phone
    if (!twilioSendSMS("+" + config.to_phone, message)) {
      okAll = false;
    }
  }

  // Stop blinking now that sending is done
  stopGreenBlink();

    if (okAll) {
    int todayYMD = getTodayYMD();

    if (type == "Well" || type == "Ill") {
      // A real communication from the user
      lastMsgYMD = todayYMD;

      // Get current local time for debugging / persistence
      time_t now = time(nullptr);
      struct tm timeinfo;
      localtime_r(&now, &timeinfo);
      lastMsgHour = timeinfo.tm_hour;
      lastMsgMin  = timeinfo.tm_min;

      prefs.putInt("last_msg_ymd",  lastMsgYMD);
      prefs.putInt("last_msg_hour", lastMsgHour);
      prefs.putInt("last_msg_min",  lastMsgMin);

      Serial.print("Recorded last user message day: ");
      Serial.println(formatYMD(lastMsgYMD));
      Serial.print("Recorded last user message time (local): ");
      Serial.print(lastMsgHour);
      Serial.print(":");
      if (lastMsgMin < 10) Serial.print("0");
      Serial.println(lastMsgMin);
    } else if (type == "None") {
      // Reminder that no message was sent
      lastNoneYMD = todayYMD;
      prefs.putInt("last_none_ymd", lastNoneYMD);
      Serial.print("Recorded last 'None' reminder day: ");
      Serial.println(formatYMD(lastNoneYMD));
    }

    // Success pattern: solid green 3s
    digitalWrite(PIN_LED_GREEN, HIGH);
    delay(3000);
    digitalWrite(PIN_LED_GREEN, LOW);
  } else {
    // Failure pattern: red on indefinitely, globalStatus false
    digitalWrite(PIN_LED_RED, HIGH);
    globalStatus = false;
  }
}

// =================== TWILIO SMS ===================

bool twilioSendSMS(const String &toNumber, const String &message) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("twilioSendSMS: WiFi not connected.");
    return false;
  }

  Serial.println();
  Serial.println("========== twilioSendSMS() REAL HTTP CALL ==========");
  Serial.print("Backend URL: ");
  Serial.println(BACKEND_URL);

  Serial.print("Device name: ");
  Serial.println(gDeviceName);

  Serial.print("Firmware version: ");
  Serial.println(WELLCOM_FIRMWARE_VERSION);

  Serial.print("To number: ");
  Serial.println(toNumber);

  Serial.print("Configured Twilio FROM (on server side): ");
  Serial.println(TWILIO_FROM_NUMBER);

  // ---- Build JSON body for backend ----
  String jsonBody = "{";
  jsonBody += "\"device_name\":\"" + gDeviceName + "\",";
  jsonBody += "\"firmware\":\"" + String(WELLCOM_FIRMWARE_VERSION) + "\",";
  jsonBody += "\"to\":\"" + toNumber + "\",";
  jsonBody += "\"from\":\"" + String(TWILIO_FROM_NUMBER) + "\",";
  jsonBody += "\"message\":\"" + message + "\"";
  jsonBody += "}";

  Serial.println("POST body to backend:");
  Serial.println("--------------------");
  Serial.println(jsonBody);
  Serial.println("--------------------");

  WiFiClientSecure client;   // secure client for HTTPS
  client.setInsecure();      // accept Heroku's cert without validation for now

  HTTPClient http;
  if (!http.begin(client, BACKEND_URL)) {
    Serial.println("HTTPS begin failed.");
    return false;
  }


  http.addHeader("Content-Type", "application/json");

  int httpCode = http.POST(jsonBody);
  Serial.printf("Backend HTTP status: %d\n", httpCode);

  String payload = http.getString();
  Serial.println("Backend response payload:");
  Serial.println(payload);

  http.end();

  // Treat 200 or 201 as success
  if (httpCode == 200 || httpCode == 201) {
    Serial.println("twilioSendSMS: Backend reported success.");
    Serial.println("=========================================\n");
    return true;
  } else {
    Serial.println("twilioSendSMS: Backend reported FAILURE.");
    Serial.println("=========================================\n");
    return false;
  }
}

// =================== RANDOM WELL MESSAGES ===================

String getRandomWellMessage(const String &toName, const String &fromName) {
  static const char *templates[] = {
    "Hello %TO%. %FROM% just checked in with WellCom to say they are feeling well today.",
    "Hi %TO%! This is WellCom. %FROM% wants you to know they're doing well today.",
    "Greetings %TO%. A quick WellCom message from %FROM%: all is well today.",
    "Hello %TO%. %FROM% reports they are feeling fine today. Thanks for caring!",
    "Hi %TO%. WellCom update: %FROM% says they are okay today.",
    "Hello %TO%. Just a friendly note from WellCom that %FROM% is doing well."
  };

  const int count = sizeof(templates) / sizeof(templates[0]);
  static bool seeded = false;
  if (!seeded) {
    seeded = true;
    randomSeed((uint32_t)millis() ^ (uint32_t)time(nullptr));
  }

  int idx = random(count);
  String s = templates[idx];
  s.replace("%TO%", toName);
  s.replace("%FROM%", fromName);
  return s;
}

// =================== REBOOT DEVICE ===================

void rebootDevice() {
  Serial.println("Rebooting...");
  delay(500);
  ESP.restart();
}

