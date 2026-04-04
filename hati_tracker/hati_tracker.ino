// ============================================================
// Hati Tracker — LilyGO T-A7670E
// Finland | GPS+GLONASS+Galileo | Telia 4G | Traccar
// Walk: 5s updates | Run (>6km/h): 1s updates
// ============================================================

#define TINY_GSM_MODEM_SIM7600
#include <TinyGsmClient.h>
#include <ArduinoHttpClient.h>
#include <esp_task_wdt.h>

// ---- PINS --------------------------------------------------
#define MODEM_TX        26
#define MODEM_RX        27
#define MODEM_PWRKEY    4
#define MODEM_POWER_ON  25
#define MODEM_RST       5
#define BAT_ADC         35
#define LED_PIN         12

// ---- NETWORK -----------------------------------------------
const char APN[]  = "internet";
const char USER[] = "";
const char PASS[] = "";

// ---- TRACCAR -----------------------------------------------
const char TRACCAR_HOST[] = "demo.traccar.org";
const int  TRACCAR_PORT   = 5055;
const char DEVICE_ID[]    = "hati-tracker-001";

// ---- TIMING ------------------------------------------------
const unsigned long GPS_TIMEOUT_MS      = 90000; // 90s to get fix
const unsigned long GPS_POLL_MS         = 500;   // check every 0.5s
const float         SPEED_THRESHOLD_KMH = 6.0;  // walk vs run
const unsigned long INTERVAL_WALK_MS    = 5000;  // 5s walking
const unsigned long INTERVAL_RUN_MS     = 1000;  // 1s running

// ---- GLOBALS -----------------------------------------------
HardwareSerial modemSerial(1);
TinyGsm        modem(modemSerial);
TinyGsmClient  gsmClient(modem);
int            failCount = 0;

// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n==============================");
  Serial.println("     Hati Tracker Booting");
  Serial.println("==============================");

  // Watchdog — restart if hung >3 min
  esp_task_wdt_config_t wdt = {
    .timeout_ms     = 180000,
    .idle_core_mask = 0,
    .trigger_panic  = true
  };
  esp_task_wdt_reconfigure(&wdt);
  esp_task_wdt_add(NULL);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  // Boot modem
  powerOnModem();
  modemSerial.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(1000);

  if (!waitForModem()) {
    Serial.println("Modem dead — restarting!");
    ESP.restart();
  }
  esp_task_wdt_reset();

  modem.init();
  Serial.print("Modem: ");
  Serial.println(modem.getModemInfo());
  esp_task_wdt_reset();

  // Network
  Serial.print("Network");
  int ntries = 0;
  while (!modem.waitForNetwork(10000L)) {
    Serial.print(".");
    blinkLED(2);
    esp_task_wdt_reset();
    if (++ntries > 18) {
      Serial.println(" timeout — restarting!");
      ESP.restart();
    }
  }
  Serial.println(" OK");
  Serial.print("Operator: ");
  Serial.println(modem.getOperator());
  Serial.print("Signal:   ");
  Serial.println(modem.getSignalQuality());
  esp_task_wdt_reset();

  // 4G data
  Serial.print("APN...");
  if (!modem.gprsConnect(APN, USER, PASS)) {
    Serial.println(" FAILED — restarting!");
    ESP.restart();
  }
  Serial.print(" OK  IP: ");
  Serial.println(modem.localIP());
  esp_task_wdt_reset();

  // GPS setup
  setupGPS();

  digitalWrite(LED_PIN, LOW);
  Serial.println("\n=== Ready ===\n");
  esp_task_wdt_reset();
}

// ============================================================
void setupGPS() {
  Serial.println("Configuring GPS...");

  // GPS(1) + GLONASS(2) + Galileo(8) = 11 — optimal for Finland
  sendATCmd("AT+CGNSSMODE=11", 1000);

  // Enable GPS
  modem.enableGPS();
  delay(1000);

  // Download AGPS data over 4G — cuts cold start from 2min to ~10s
  Serial.println("Downloading AGPS data (10s)...");
  sendATCmd("AT+AGPS", 12000);
  Serial.println("AGPS done!");
}

// ============================================================
void loop() {
  esp_task_wdt_reset();
  digitalWrite(LED_PIN, HIGH);

  // GPS variables
  float lat = 0, lon = 0, spd = 0, alt = 0, acc = 0;
  int   vsat = 0, usat = 0;
  int   yr = 0, mo = 0, dy = 0, hr = 0, mi = 0, sc = 0;
  bool  gotFix = false;

  // Poll for GPS fix
  Serial.print("GPS");
  unsigned long t0 = millis();

  while (millis() - t0 < GPS_TIMEOUT_MS) {
    esp_task_wdt_reset();

    if (modem.getGPS(&lat, &lon, &spd, &alt,
                     &vsat, &usat, &acc,
                     &yr, &mo, &dy,
                     &hr, &mi, &sc)) {

      // Sanity check — must be valid Finland coordinates
      if (lat > 59.0 && lat < 71.0 &&
          lon > 19.0 && lon < 32.0) {
        gotFix = true;
        break;
      }
    }

    delay(GPS_POLL_MS);

    // Progress every 5 seconds
    if ((millis() - t0) % 5000 < GPS_POLL_MS + 100) {
      Serial.printf("..%lus", (millis() - t0) / 1000);
    }
  }

  // ---- Got GPS fix ----------------------------------------
  if (gotFix) {
    Serial.println(" FIX!");
    Serial.printf("  Lat:  %.6f\n",      lat);
    Serial.printf("  Lon:  %.6f\n",      lon);
    Serial.printf("  Alt:  %.1f m\n",    alt);
    Serial.printf("  Spd:  %.2f km/h\n", spd);
    Serial.printf("  Sats: %d/%d\n",     usat, vsat);
    Serial.printf("  Acc:  %.1f m\n",    acc);
    Serial.printf("  Time: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                  yr, mo, dy, hr, mi, sc);

    float batt = readBattery();
    Serial.printf("  Batt: %.0f%%\n", batt);

    char ts[32];
    snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             yr, mo, dy, hr, mi, sc);

    bool sent = sendToTraccar(lat, lon, spd, alt,
                               usat, batt, ts, false);
    if (sent) {
      failCount = 0;
      blinkLED(1);
    } else {
      failCount++;
      blinkLED(5);
      if (failCount >= 5) {
        Serial.println("5 failures — reconnecting GPRS...");
        modem.gprsDisconnect();
        delay(2000);
        modem.gprsConnect(APN, USER, PASS);
        failCount = 0;
      }
    }

  // ---- No GPS fix — try cell tower fallback ---------------
  } else {
    Serial.println(" no fix");
    tryLBSFallback();
  }

  digitalWrite(LED_PIN, LOW);

  // Adaptive rate: walk or run
  unsigned long interval;
  if (spd >= SPEED_THRESHOLD_KMH) {
    interval = INTERVAL_RUN_MS;
    Serial.println("Mode: RUN (1s)");
  } else {
    interval = INTERVAL_WALK_MS;
    Serial.println("Mode: WALK (5s)");
  }

  Serial.printf("Signal: %d | Next in %lus\n\n",
                modem.getSignalQuality(), interval / 1000);

  sleepWithWatchdog(interval);
}

// ============================================================
bool sendToTraccar(float lat, float lon, float spd,
                   float alt, int sats, float batt,
                   const char* ts, bool isLBS) {
  Serial.print("Traccar...");

  if (!modem.isGprsConnected()) {
    Serial.print("(reconnect) ");
    modem.gprsConnect(APN, USER, PASS);
    delay(3000);
  }

  String path = "/?id=";      path += DEVICE_ID;
  path += "&lat=";             path += String(lat, 6);
  path += "&lon=";             path += String(lon, 6);
  path += "&speed=";           path += String(spd, 2);
  path += "&altitude=";        path += String(alt, 1);
  path += "&batt=";            path += String(batt, 0);
  path += "&satellites=";      path += String(sats);
  path += "&accuracy=";        path += String(isLBS ? 500 : 5);
  if (strlen(ts) > 0) {
    path += "&timestamp=";     path += ts;
  }

  HttpClient http(gsmClient, TRACCAR_HOST, TRACCAR_PORT);
  http.setTimeout(15000);
  int err = http.get(path);

  if (err == 0) {
    int status = http.responseStatusCode();
    Serial.printf(" HTTP %d\n", status);
    http.stop();
    return (status == 200);
  }

  Serial.printf(" err %d\n", err);
  http.stop();
  return false;
}

// ============================================================
void tryLBSFallback() {
  Serial.print("LBS...");
  while (modemSerial.available()) modemSerial.read();
  modemSerial.println("AT+CLBS=1,1");

  String resp = "";
  unsigned long t = millis();
  while (millis() - t < 6000) {
    if (modemSerial.available())
      resp += (char)modemSerial.read();
  }

  if (resp.indexOf("+CLBS: 0") >= 0) {
    int s1   = resp.indexOf("+CLBS: 0,") + 9;
    float la = resp.substring(s1).toFloat();
    int s2   = resp.indexOf(',', s1) + 1;
    float lo = resp.substring(s2).toFloat();
    if (la != 0 && lo != 0) {
      Serial.printf(" %.4f, %.4f\n", la, lo);
      float batt = readBattery();
      sendToTraccar(la, lo, 0, 0, 0, batt, "", true);
    }
  } else {
    Serial.println(" failed");
  }
}

// ============================================================
float readBattery() {
  // 20 samples, average middle 10 to remove outliers
  int r[20];
  for (int i = 0; i < 20; i++) { r[i] = analogRead(BAT_ADC); delay(5); }
  for (int i = 0; i < 19; i++)
    for (int j = i+1; j < 20; j++)
      if (r[j] < r[i]) { int t = r[i]; r[i] = r[j]; r[j] = t; }
  long sum = 0;
  for (int i = 5; i < 15; i++) sum += r[i];
  float voltage = (sum / 10.0 / 4095.0) * 3.3 * 2.0;
  return constrain(((voltage - 3.0) / (4.2 - 3.0)) * 100.0, 0.0, 100.0);
}

// ============================================================
void sleepWithWatchdog(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    esp_task_wdt_reset();
    delay(500);
  }
}

// ============================================================
void sendATCmd(const char* cmd, int waitMs) {
  while (modemSerial.available()) modemSerial.read();
  modemSerial.println(cmd);
  delay(waitMs);
  while (modemSerial.available()) modemSerial.read();
}

// ============================================================
bool waitForModem() {
  Serial.print("Sync modem");
  for (int i = 0; i < 20; i++) {
    while (modemSerial.available()) modemSerial.read();
    modemSerial.println("AT");
    delay(500);
    String resp = "";
    unsigned long t = millis();
    while (millis() - t < 500)
      if (modemSerial.available())
        resp += (char)modemSerial.read();
    if (resp.indexOf("OK") >= 0) {
      Serial.println(" OK!");
      return true;
    }
    Serial.print(".");
    esp_task_wdt_reset();
  }
  Serial.println(" FAILED");
  return false;
}

// ============================================================
void powerOnModem() {
  Serial.println("Powering on modem...");
  pinMode(MODEM_PWRKEY,   OUTPUT);
  pinMode(MODEM_POWER_ON, OUTPUT);
  pinMode(MODEM_RST,      OUTPUT);
  digitalWrite(MODEM_POWER_ON, HIGH);
  digitalWrite(MODEM_RST, HIGH);
  delay(100);
  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(1200);
  digitalWrite(MODEM_PWRKEY, LOW);
  delay(8000);
  Serial.println("Modem up!");
}

// ============================================================
void blinkLED(int n) {
  for (int i = 0; i < n; i++) {
    digitalWrite(LED_PIN, HIGH); delay(150);
    digitalWrite(LED_PIN, LOW);  delay(150);
  }
}