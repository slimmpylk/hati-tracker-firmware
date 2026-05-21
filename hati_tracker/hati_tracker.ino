// ============================================================
// Hati Tracker — LilyGO T-A7670E / A7670E
// GNSS via AT+CGNSSINFO + AGPS via AT+CAGPS + Traccar OsmAnd
//
// Safe-to-commit version:
//   - Put private server/device/APN values in secrets.h
//   - Commit secrets.example.h, not secrets.h
//
// LED STATUS:
//   RED1 GPIO 12
//     - Blinks while booting / connecting 4G
//     - Solid ON when 4G is connected
//
//   RED2 GPIO 2
//     - OFF before GNSS search starts
//     - Blinks while searching GNSS fix
//     - Solid ON when GNSS fix is valid
//
// When everything works: both LEDs are solid ON.
// ============================================================

#define TINY_GSM_MODEM_SIM7600

#include <TinyGsmClient.h>
#include <ArduinoHttpClient.h>
#include <esp_task_wdt.h>
#include <ctype.h>
#include <math.h>
#include "secrets.h"

// ---- MODEM PINS --------------------------------------------
#define MODEM_TX        26
#define MODEM_RX        27
#define MODEM_PWRKEY    4
#define MODEM_POWER_ON  25
#define MODEM_RST       5
#define BAT_ADC         35

// ---- LED PINS ----------------------------------------------
#define LED_RED1        12
#define LED_RED2        2

// ---- DEBUG --------------------------------------------------
#define DEBUG_SERIAL    1

#if DEBUG_SERIAL
  #define LOG(x)        Serial.print(x)
  #define LOGLN(x)      Serial.println(x)
  #define LOGF(...)     Serial.printf(__VA_ARGS__)
#else
  #define LOG(x)
  #define LOGLN(x)
  #define LOGF(...)
#endif

// ---- TIMING ------------------------------------------------
const unsigned long GPS_SEARCH_TIMEOUT_MS = 300000;  // 5 min max search window
const unsigned long GPS_POLL_MS           = 1000;    // read CGNSSINFO every 1s
const unsigned long WALK_INTERVAL_MS      = 5000;
const unsigned long RUN_INTERVAL_MS       = 1000;
const unsigned long MIN_SEND_INTERVAL_MS  = 900;
const unsigned long RED1_BLINK_MS         = 500;
const unsigned long RED2_BLINK_MS         = 1000;

const float RUN_THRESHOLD_KMH = 6.0;

// ---- BATTERY -----------------------------------------------
// On USB-only power this may read 0. Disable until board battery ADC is verified.
const bool  ENABLE_BATTERY_ADC = false;
const float BATTERY_DIVIDER    = 2.0;
const float BATTERY_CAL        = 1.00;

// ---- GLOBALS -----------------------------------------------
HardwareSerial modemSerial(1);
TinyGsm        modem(modemSerial);
TinyGsmClient  gsmClient(modem);

bool has4G            = false;
bool gpsSearchStarted = false;
bool hasGPSFix        = false;

unsigned long lastSendMs    = 0;
unsigned long lastRed1Blink = 0;
unsigned long lastRed2Blink = 0;
bool red1State = false;
bool red2State = false;
int  failCount = 0;

struct GpsFix {
  float lat = 0;
  float lon = 0;
  float speedKmh = 0;
  float altitude = 0;
  float accuracy = 50;
  int visibleSats = 0;
  int usedSats = 0;
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  bool valid = false;
};

// ============================================================
void setLED(int pin, bool on) {
  digitalWrite(pin, on ? HIGH : LOW);
}

void updateLEDs() {
  unsigned long now = millis();

  if (has4G) {
    red1State = true;
    setLED(LED_RED1, true);
  } else if (now - lastRed1Blink >= RED1_BLINK_MS) {
    red1State = !red1State;
    setLED(LED_RED1, red1State);
    lastRed1Blink = now;
  }

  if (!gpsSearchStarted) {
    red2State = false;
    setLED(LED_RED2, false);
  } else if (hasGPSFix) {
    red2State = true;
    setLED(LED_RED2, true);
  } else if (now - lastRed2Blink >= RED2_BLINK_MS) {
    red2State = !red2State;
    setLED(LED_RED2, red2State);
    lastRed2Blink = now;
  }
}

void delayWithTasks(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    esp_task_wdt_reset();
    updateLEDs();
    delay(50);
  }
}

// ============================================================
void setupWatchdog() {
  esp_task_wdt_config_t wdt = {
    .timeout_ms     = 180000,
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic  = true
  };

  esp_err_t result = esp_task_wdt_init(&wdt);
  if (result == ESP_ERR_INVALID_STATE) {
    esp_task_wdt_reconfigure(&wdt);
  }
  esp_task_wdt_add(NULL);
}

// ============================================================
String sendAT(const char* cmd, unsigned long waitMs, bool echo = true) {
  while (modemSerial.available()) modemSerial.read();
  modemSerial.println(cmd);

  String resp;
  unsigned long start = millis();
  while (millis() - start < waitMs) {
    while (modemSerial.available()) resp += (char)modemSerial.read();
    esp_task_wdt_reset();
    updateLEDs();
    delay(10);
  }

#if DEBUG_SERIAL
  if (echo) {
    LOGLN();
    LOG("CMD: "); LOGLN(cmd);
    LOG("RESP: "); LOGLN(resp.length() ? resp : "(no response)");
    LOGLN("----");
  }
#endif

  return resp;
}

String urlEncode(const char* str) {
  String enc;
  char buf[4];
  while (*str) {
    char c = *str++;
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      enc += c;
    } else {
      snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
      enc += buf;
    }
  }
  return enc;
}

// ============================================================
void powerOnModem() {
  LOGLN("Powering on modem...");

  pinMode(MODEM_PWRKEY, OUTPUT);
  pinMode(MODEM_POWER_ON, OUTPUT);
  pinMode(MODEM_RST, OUTPUT);

  digitalWrite(MODEM_POWER_ON, HIGH);
  digitalWrite(MODEM_RST, HIGH);
  delay(100);

  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(1200);
  digitalWrite(MODEM_PWRKEY, LOW);

  // A7670E sometimes needs more than 8 seconds before AT responds.
  delayWithTasks(15000);
  LOGLN("Modem power sequence done.");
}

bool waitForModem() {
  LOG("Sync modem");

  for (int i = 0; i < 40; i++) {
    String resp = sendAT("AT", 500, false);
    if (resp.indexOf("OK") >= 0) {
      LOGLN(" OK!");
      return true;
    }
    LOG(".");
    delayWithTasks(250);
  }

  LOGLN(" FAILED");
  return false;
}

bool connect4G() {
  LOG("Network");

  for (int i = 0; i < 20; i++) {
    if (modem.waitForNetwork(10000L)) {
      LOGLN(" OK");
      break;
    }
    LOG(".");
    if (i == 19) {
      LOGLN(" FAILED");
      return false;
    }
    esp_task_wdt_reset();
    updateLEDs();
  }

  LOG("Operator: "); LOGLN(modem.getOperator());
  LOG("Signal:   "); LOGLN(modem.getSignalQuality());

  LOG("APN...");
  if (!modem.gprsConnect(APN, APN_USER, APN_PASS)) {
    LOGLN(" FAILED");
    return false;
  }

  has4G = true;
  updateLEDs();

  LOG(" OK  IP: "); LOGLN(modem.localIP());
  return true;
}

// ============================================================
bool waitGnssReady(unsigned long timeoutMs) {
  LOGLN("Waiting for GNSS READY...");

  unsigned long start = millis();
  while (millis() - start < timeoutMs) {
    while (modemSerial.available()) {
      String line = modemSerial.readStringUntil('\n');
      line.trim();
      if (line.length()) {
        LOG("GNSS async: "); LOGLN(line);
      }
      if (line.indexOf("+CGNSSPWR: READY") >= 0) {
        LOGLN("GNSS READY received.");
        return true;
      }
    }
    delayWithTasks(100);
  }

  LOGLN("GNSS READY not seen, continuing anyway.");
  return false;
}

void tryAGPS() {
  if (!modem.isGprsConnected()) {
    has4G = false;
    updateLEDs();
    if (!modem.gprsConnect(APN, APN_USER, APN_PASS)) {
      LOGLN("AGPS skipped: GPRS reconnect failed.");
      return;
    }
    has4G = true;
    updateLEDs();
  }

  LOGLN("AGPS: AT+CAGPS...");
  String r = sendAT("AT+CAGPS", 12000);

  if (r.indexOf("+AGPS: success") >= 0) {
    LOGLN("AGPS: success.");
  } else if (r.indexOf("+AGPS:") >= 0) {
    LOGLN("AGPS: returned status/error.");
  } else if (r.indexOf("ERROR") >= 0) {
    LOGLN("AGPS: unsupported/failed.");
  } else {
    LOGLN("AGPS: no clear response.");
  }
}

void setupGNSS() {
  LOGLN();
  LOGLN("--- GNSS Setup ---");

  sendAT("AT+CGNSSPWR?", 1000);
  sendAT("AT+CGNSSPWR=0", 1000);
  delayWithTasks(1000);

  // Do not use AT+CGNSSMODE=11. This firmware returned ERROR for it.
  sendAT("AT+CGNSSPWR=1", 2000);
  waitGnssReady(30000);
  sendAT("AT+CGNSSPWR?", 1000);

  // Confirm command path. Product line may arrive with this response.
  sendAT("AT+CGNSSINFO", 3000);

  // This works on your A7670E and significantly improved first fix.
  tryAGPS();

  sendAT("AT+CGNSSINFO", 3000);

  LOGLN("--- GNSS Setup done ---");
  LOGLN();
}

// ============================================================
bool parseCGNSSINFO(const String &resp, GpsFix &fix) {
  int p = resp.indexOf("+CGNSSINFO:");
  if (p < 0) return false;

  int start = p + strlen("+CGNSSINFO:");
  int end = resp.indexOf('\n', start);
  if (end < 0) end = resp.indexOf('\r', start);
  if (end < 0) end = resp.length();

  String line = resp.substring(start, end);
  line.trim();

  if (line.length() == 0 || line.startsWith(",,,,") || line.indexOf(",,,,") >= 0) {
    return false;
  }

  String f[24];
  int count = 0;
  int fieldStart = 0;

  while (count < 24) {
    int comma = line.indexOf(',', fieldStart);
    if (comma < 0) {
      f[count++] = line.substring(fieldStart);
      break;
    }
    f[count++] = line.substring(fieldStart, comma);
    fieldStart = comma + 1;
  }

  if (count < 13 || f[5].length() == 0 || f[7].length() == 0) {
    return false;
  }

  fix.lat = f[5].toFloat();
  fix.lon = f[7].toFloat();
  if (f[6] == "S") fix.lat = -fix.lat;
  if (f[8] == "W") fix.lon = -fix.lon;

  if (!(fix.lat > 59.0 && fix.lat < 71.0 && fix.lon > 19.0 && fix.lon < 32.0)) {
    return false;
  }

  String date = f[9];   // ddmmyy
  String time = f[10];  // hhmmss.ss

  if (date.length() >= 6) {
    fix.day   = date.substring(0, 2).toInt();
    fix.month = date.substring(2, 4).toInt();
    fix.year  = 2000 + date.substring(4, 6).toInt();
  }

  if (time.length() >= 6) {
    fix.hour   = time.substring(0, 2).toInt();
    fix.minute = time.substring(2, 4).toInt();
    fix.second = time.substring(4, 6).toInt();
  }

  fix.altitude = f[11].toFloat();
  fix.speedKmh = f[12].toFloat();

  fix.visibleSats = f[1].toInt();
  int usedA = f[3].toInt();
  int usedB = f[4].toInt();
  fix.usedSats = usedA > usedB ? usedA : usedB;
  if (fix.usedSats <= 0 && fix.visibleSats > 0) fix.usedSats = fix.visibleSats;

  // Use a DOP-like field as rough accuracy estimate.
  if (count > 15) {
    float dop = f[15].toFloat();
    fix.accuracy = dop > 0 ? dop * 5.0 : 50.0;
  } else {
    fix.accuracy = 50.0;
  }

  if (fix.accuracy <= 0 || isnan(fix.accuracy) || fix.accuracy > 9999) {
    fix.accuracy = 50.0;
  }

  fix.valid = true;
  return true;
}

bool readGpsFix(GpsFix &fix) {
  String resp = sendAT("AT+CGNSSINFO", 1500, DEBUG_SERIAL);
  return parseCGNSSINFO(resp, fix);
}

// ============================================================
float readBatteryPercent() {
  if (!ENABLE_BATTERY_ADC) return 100.0;

  int r[20];
  for (int i = 0; i < 20; i++) {
    r[i] = analogRead(BAT_ADC);
    delay(5);
  }

  for (int i = 0; i < 19; i++) {
    for (int j = i + 1; j < 20; j++) {
      if (r[j] < r[i]) {
        int tmp = r[i];
        r[i] = r[j];
        r[j] = tmp;
      }
    }
  }

  long sum = 0;
  for (int i = 5; i < 15; i++) sum += r[i];

  float v = (sum / 10.0 / 4095.0) * 3.3 * BATTERY_DIVIDER * BATTERY_CAL;
  return constrain(((v - 3.0) / (4.2 - 3.0)) * 100.0, 0.0, 100.0);
}

// ============================================================
bool sendToTraccar(const GpsFix &fix) {
  if (!modem.isGprsConnected()) {
    LOGLN("GPRS reconnect...");
    has4G = false;
    updateLEDs();

    if (!modem.gprsConnect(APN, APN_USER, APN_PASS)) {
      LOGLN("GPRS reconnect failed.");
      return false;
    }

    has4G = true;
    updateLEDs();
    delayWithTasks(3000);
  }

  float batt = readBatteryPercent();
  int battInt = constrain((int)round(batt), 0, 100);

  // Traccar OsmAnd speed is commonly interpreted as knots.
  // A7670E CGNSSINFO speed appears to be km/h from your train test.
  float speedKnots = fix.speedKmh * 0.539957;

  float accuracy = fix.accuracy;
  if (accuracy <= 0 || isnan(accuracy) || accuracy > 9999) accuracy = 50.0;

  char ts[32];
  snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02dZ",
           fix.year, fix.month, fix.day,
           fix.hour, fix.minute, fix.second);

  String path = "/?id=";
  path += DEVICE_ID;
  path += "&lat=";        path += String(fix.lat, 6);
  path += "&lon=";        path += String(fix.lon, 6);
  path += "&speed=";      path += String(speedKnots, 2);
  path += "&altitude=";   path += String(fix.altitude, 1);
  path += "&batt=";       path += String(battInt);
  path += "&satellites="; path += String(fix.usedSats);
  path += "&accuracy=";   path += String(accuracy, 1);
  path += "&valid=true";
  path += "&timestamp=";  path += urlEncode(ts);

  LOG("Traccar GET "); LOGLN(path);

  HttpClient http(gsmClient, TRACCAR_HOST, TRACCAR_PORT);
  http.setTimeout(15000);

  int err = http.get(path);
  if (err != 0) {
    LOG("Traccar client err "); LOGLN(err);
    http.stop();
    return false;
  }

  int status = http.responseStatusCode();
  LOG("Traccar HTTP "); LOGLN(status);
  http.stop();

  return status >= 200 && status < 300;
}

// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  LOGLN();
  LOGLN("==============================");
  LOGLN("     Hati Tracker Booting");
  LOGLN("==============================");

  pinMode(LED_RED1, OUTPUT);
  pinMode(LED_RED2, OUTPUT);
  setLED(LED_RED1, false);
  setLED(LED_RED2, false);

  analogReadResolution(12);
  analogSetPinAttenuation(BAT_ADC, ADC_11db);

  setupWatchdog();

  setLED(LED_RED1, true);
  setLED(LED_RED2, true);
  delay(1000);
  setLED(LED_RED1, false);
  setLED(LED_RED2, false);

  powerOnModem();
  modemSerial.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delayWithTasks(1000);

  if (!waitForModem()) {
    LOGLN("Modem dead — restarting!");
    ESP.restart();
  }

  modem.init();
  LOG("Modem: "); LOGLN(modem.getModemInfo());

  if (!connect4G()) {
    LOGLN("4G failed — restarting!");
    ESP.restart();
  }

  setupGNSS();

  gpsSearchStarted = true;
  hasGPSFix = false;
  updateLEDs();

  LOGLN("Ready — searching GNSS fix.");
}

// ============================================================
void loop() {
  esp_task_wdt_reset();
  updateLEDs();

  GpsFix fix;
  bool gotFix = false;
  unsigned long start = millis();
  unsigned long lastPrint = 0;

  while (millis() - start < GPS_SEARCH_TIMEOUT_MS) {
    esp_task_wdt_reset();
    updateLEDs();

    if (readGpsFix(fix)) {
      gotFix = true;
      hasGPSFix = true;
      updateLEDs();
      break;
    }

    delayWithTasks(GPS_POLL_MS);

    unsigned long elapsed = millis() - start;
    if (elapsed - lastPrint >= 10000) {
      lastPrint = elapsed;
      LOG("Searching GNSS... "); LOG(elapsed / 1000); LOGLN("s");
    }
  }

  if (!gotFix) {
    hasGPSFix = false;
    updateLEDs();
    LOGLN("No GNSS fix in this window.");
    delayWithTasks(1000);
    return;
  }

  LOGF("GPS FIX lat=%.6f lon=%.6f speed=%.2f km/h sats=%d/%d acc=%.1f\n",
       fix.lat, fix.lon, fix.speedKmh, fix.usedSats, fix.visibleSats, fix.accuracy);

  unsigned long interval = fix.speedKmh >= RUN_THRESHOLD_KMH ? RUN_INTERVAL_MS : WALK_INTERVAL_MS;
  if (interval < MIN_SEND_INTERVAL_MS) interval = MIN_SEND_INTERVAL_MS;

  unsigned long now = millis();
  if (now - lastSendMs >= interval) {
    if (sendToTraccar(fix)) {
      failCount = 0;
      lastSendMs = now;
    } else {
      failCount++;
      if (failCount >= 5) {
        LOGLN("Too many send failures; reconnecting GPRS.");
        modem.gprsDisconnect();
        has4G = false;
        updateLEDs();
        delayWithTasks(2000);
        has4G = modem.gprsConnect(APN, APN_USER, APN_PASS);
        updateLEDs();
        failCount = 0;
      }
    }
  }

  delayWithTasks(200);
}
