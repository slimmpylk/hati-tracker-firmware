#define TINY_GSM_MODEM_SIM7600
#include <TinyGsmClient.h>
#include <ArduinoHttpClient.h>
#include <esp_task_wdt.h>

// ---- PINS ----
#define MODEM_TX        26
#define MODEM_RX        27
#define MODEM_PWRKEY    4
#define MODEM_POWER_ON  25
#define MODEM_RST       5
#define BAT_ADC         35
#define LED_PIN         12

// ---- NETWORK ----
const char APN[]  = "internet";
const char USER[] = "";
const char PASS[] = "";

// ---- TRACCAR ----
const char TRACCAR_HOST[] = "demo.traccar.org";
const int  TRACCAR_PORT   = 5055;
const char DEVICE_ID[]    = "hati-tracker-001";

// ---- TIMING ----
const int  GPS_TIMEOUT_MS     = 120000;
const int  INTERVAL_MOVING_MS = 10000;
const int  INTERVAL_STILL_MS  = 60000;
const float MOVING_SPEED_KMH  = 1.5;

// ---- GLOBALS ----
HardwareSerial modemSerial(1);
TinyGsm        modem(modemSerial);
TinyGsmClient  gsmClient(modem);
bool           isMoving = false;

// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Hati Tracker Booting ===");

  // Watchdog — auto restart if hung >2 min
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 120000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_task_wdt_reconfigure(&wdt_config);
  esp_task_wdt_add(NULL);

  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  powerOnModem();
  modemSerial.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(1000);

  if (!waitForModem()) {
    Serial.println("Modem not responding — restarting!");
    ESP.restart();
  }

  esp_task_wdt_reset();

  modem.init();
  String info = modem.getModemInfo();
  Serial.print("Modem: ");
  Serial.println(info);

  // Wait for network
  Serial.print("Waiting for network");
  int tries = 0;
  while (!modem.waitForNetwork(10000L)) {
    Serial.print(".");
    blinkLED(2);
    esp_task_wdt_reset();
    if (++tries > 12) {
      Serial.println("\nNetwork timeout — restarting!");
      ESP.restart();
    }
  }
  Serial.println(" OK");
  Serial.print("Operator: ");
  Serial.println(modem.getOperator());

  esp_task_wdt_reset();

  Serial.print("Connecting to APN...");
  if (!modem.gprsConnect(APN, USER, PASS)) {
    Serial.println(" FAILED — restarting!");
    ESP.restart();
  }
  Serial.print(" OK  IP: ");
  Serial.println(modem.localIP());

  Serial.println("Enabling GPS...");
  modem.enableGPS();

  digitalWrite(LED_PIN, LOW);
  Serial.println("=== Boot complete ===\n");
  esp_task_wdt_reset();
}

// ============================================================
void loop() {
  esp_task_wdt_reset();
  digitalWrite(LED_PIN, HIGH);

  float lat = 0, lon = 0, speed = 0, alt = 0, accuracy = 0;
  int   vsat = 0, usat = 0;
  int   year = 0, month = 0, day = 0, hour = 0, mi = 0, sec = 0;
  bool  gotGPS = false;

  Serial.print("Getting GPS fix");
  unsigned long gpsStart = millis();

  while (millis() - gpsStart < (unsigned long)GPS_TIMEOUT_MS) {
    esp_task_wdt_reset();
    if (modem.getGPS(&lat, &lon, &speed, &alt,
                     &vsat, &usat, &accuracy,
                     &year, &month, &day,
                     &hour, &mi, &sec)) {
      gotGPS = true;
      break;
    }
    delay(2000);
    Serial.print(".");
  }

  if (gotGPS) {
    Serial.println(" GOT FIX");
    Serial.printf("  Lat: %.6f  Lon: %.6f\n", lat, lon);
    Serial.printf("  Alt: %.1fm  Speed: %.1f km/h\n", alt, speed);
    Serial.printf("  Sats: %d/%d  Accuracy: %.1fm\n", usat, vsat, accuracy);
    Serial.printf("  Time: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                  year, month, day, hour, mi, sec);

    float battery = readBattery();
    Serial.printf("  Battery: %.0f%%\n", battery);

    isMoving = (speed > MOVING_SPEED_KMH);

    char ts[32];
    snprintf(ts, sizeof(ts), "%04d-%02d-%02dT%02d:%02d:%02dZ",
             year, month, day, hour, mi, sec);

    sendToTraccar(lat, lon, speed, alt, usat, battery, ts);

  } else {
    Serial.println(" no fix — trying LBS fallback...");
    tryLBSFallback();
  }

  digitalWrite(LED_PIN, LOW);

  int interval = isMoving ? INTERVAL_MOVING_MS : INTERVAL_STILL_MS;
  Serial.printf("Sleeping %d sec...\n\n", interval / 1000);

  unsigned long sleepStart = millis();
  while (millis() - sleepStart < (unsigned long)interval) {
    esp_task_wdt_reset();
    delay(5000);
  }
}

// ============================================================
void sendToTraccar(float lat, float lon, float speed,
                   float alt, int sats, float battery,
                   const char* timestamp) {
  Serial.print("Sending to Traccar...");

  if (!modem.isGprsConnected()) {
    Serial.print("reconnecting GPRS...");
    modem.gprsConnect(APN, USER, PASS);
    delay(3000);
  }

  String path = "/?id=";        path += DEVICE_ID;
  path += "&lat=";               path += String(lat, 6);
  path += "&lon=";               path += String(lon, 6);
  path += "&speed=";             path += String(speed, 1);
  path += "&altitude=";          path += String(alt, 1);
  path += "&batt=";              path += String(battery, 0);
  path += "&satellites=";        path += String(sats);
  path += "&timestamp=";         path += timestamp;

  HttpClient http(gsmClient, TRACCAR_HOST, TRACCAR_PORT);
  http.setTimeout(10000);
  int err = http.get(path);

  if (err == 0) {
    int status = http.responseStatusCode();
    Serial.printf(" HTTP %d\n", status);
    blinkLED(status == 200 ? 1 : 3);
  } else {
    Serial.printf(" FAILED (err %d)\n", err);
    blinkLED(5);
  }
  http.stop();
}

// ============================================================
void tryLBSFallback() {
  while (modemSerial.available()) modemSerial.read();
  modemSerial.println("AT+CLBS=1,1");
  delay(3000);

  String resp = "";
  unsigned long t = millis();
  while (millis() - t < 4000) {
    if (modemSerial.available())
      resp += (char)modemSerial.read();
  }

  if (resp.indexOf("+CLBS: 0") >= 0) {
    int start = resp.indexOf("+CLBS: 0,") + 9;
    float lbsLat = resp.substring(start).toFloat();
    int comma1 = resp.indexOf(',', start) + 1;
    float lbsLon = resp.substring(comma1).toFloat();
    if (lbsLat != 0 && lbsLon != 0) {
      Serial.printf("LBS fix: %.4f, %.4f\n", lbsLat, lbsLon);
      float battery = readBattery();
      sendToTraccar(lbsLat, lbsLon, 0, 0, 0, battery, "");
    }
  } else {
    Serial.println("LBS also failed");
  }
}

// ============================================================
float readBattery() {
  long sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += analogRead(BAT_ADC);
    delay(10);
  }
  float voltage = (sum / 10.0 / 4095.0) * 3.3 * 2.0;
  float pct = ((voltage - 3.0) / (4.2 - 3.0)) * 100.0;
  return constrain(pct, 0.0, 100.0);
}

// ============================================================
bool waitForModem() {
  Serial.print("Syncing modem");
  for (int i = 0; i < 20; i++) {
    while (modemSerial.available()) modemSerial.read();
    modemSerial.println("AT");
    delay(500);
    String resp = "";
    unsigned long t = millis();
    while (millis() - t < 500) {
      if (modemSerial.available())
        resp += (char)modemSerial.read();
    }
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
  Serial.println("Modem powered on!");
}

// ============================================================
void blinkLED(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_PIN, HIGH); delay(150);
    digitalWrite(LED_PIN, LOW);  delay(150);
  }
}