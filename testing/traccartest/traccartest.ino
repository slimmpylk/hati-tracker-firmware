// ============================================================
// Hati Tracker — Traccar 4G Test Only
// LilyGO T-A7670E / A7670E
//
// PURPOSE:
//   Test 4G -> HTTP -> Traccar connection without GPS.
//
// LED STATUS:
//   RED1 GPIO 12:
//     - Blinks while booting / connecting
//     - Solid ON when 4G data is connected
//
//   RED2 GPIO 2:
//     - Blinks once when Traccar send succeeds
//     - Blinks three times when Traccar send fails
// ============================================================

#define TINY_GSM_MODEM_SIM7600

#include <TinyGsmClient.h>
#include <ArduinoHttpClient.h>

// ---- MODEM PINS --------------------------------------------
#define MODEM_TX        26
#define MODEM_RX        27
#define MODEM_PWRKEY    4
#define MODEM_POWER_ON  25
#define MODEM_RST       5

// ---- LED PINS ----------------------------------------------
#define LED_RED1        12
#define LED_RED2        2

// ---- NETWORK -----------------------------------------------
const char APN[]  = "internet";
const char USER[] = "";
const char PASS[] = "";

// ---- TRACCAR -----------------------------------------------
// For demo Traccar, create a device with this exact Unique ID.
const char TRACCAR_HOST[] = "demo.traccar.org";
const int  TRACCAR_PORT   = 5055;
const char DEVICE_ID[]    = "hati-tracker-001";

// ---- TEST POSITION -----------------------------------------
// Fake test position near Tampere railway station.
float testLat = 61.498100;
float testLon = 23.760000;

// Send every 10 seconds.
const unsigned long SEND_INTERVAL_MS = 10000;

// ---- GLOBALS -----------------------------------------------
HardwareSerial modemSerial(1);
TinyGsm        modem(modemSerial);
TinyGsmClient  gsmClient(modem);

bool has4G = false;
unsigned long lastSendMs = 0;
unsigned long lastRed1Blink = 0;
bool red1State = false;

// ============================================================
void setLED(int pin, bool on) {
  digitalWrite(pin, on ? HIGH : LOW);
}

// ============================================================
void updateStatusLED() {
  if (has4G) {
    setLED(LED_RED1, true);
    return;
  }

  unsigned long now = millis();

  if (now - lastRed1Blink >= 500) {
    red1State = !red1State;
    setLED(LED_RED1, red1State);
    lastRed1Blink = now;
  }
}

// ============================================================
void blinkRed2(int count) {
  for (int i = 0; i < count; i++) {
    setLED(LED_RED2, true);
    delay(150);
    setLED(LED_RED2, false);
    delay(150);
  }
}

// ============================================================
void powerOnModem() {
  Serial.println("Powering on modem...");

  pinMode(MODEM_PWRKEY, OUTPUT);
  pinMode(MODEM_POWER_ON, OUTPUT);
  pinMode(MODEM_RST, OUTPUT);

  digitalWrite(MODEM_POWER_ON, HIGH);
  digitalWrite(MODEM_RST, HIGH);

  delay(100);

  digitalWrite(MODEM_PWRKEY, HIGH);
  delay(1200);
  digitalWrite(MODEM_PWRKEY, LOW);

  unsigned long start = millis();

  while (millis() - start < 8000) {
    updateStatusLED();
    delay(100);
  }

  Serial.println("Modem power sequence done.");
}

// ============================================================
bool waitForModem() {
  Serial.print("Sync modem");

  for (int i = 0; i < 20; i++) {
    while (modemSerial.available()) {
      modemSerial.read();
    }

    modemSerial.println("AT");
    delay(500);

    String resp = "";
    unsigned long start = millis();

    while (millis() - start < 500) {
      while (modemSerial.available()) {
        resp += (char)modemSerial.read();
      }

      updateStatusLED();
      delay(10);
    }

    if (resp.indexOf("OK") >= 0) {
      Serial.println(" OK!");
      return true;
    }

    Serial.print(".");
  }

  Serial.println(" FAILED");
  return false;
}

// ============================================================
String urlEncode(const char* str) {
  String enc = "";
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
bool connect4G() {
  Serial.println();
  Serial.println("Connecting to mobile network...");

  Serial.print("Network");

  for (int i = 0; i < 20; i++) {
    if (modem.waitForNetwork(10000L)) {
      Serial.println(" OK");
      break;
    }

    Serial.print(".");
    updateStatusLED();

    if (i == 19) {
      Serial.println(" FAILED");
      return false;
    }
  }

  Serial.print("Operator: ");
  Serial.println(modem.getOperator());

  Serial.print("Signal:   ");
  Serial.println(modem.getSignalQuality());

  Serial.print("APN...");

  if (!modem.gprsConnect(APN, USER, PASS)) {
    Serial.println(" FAILED");
    return false;
  }

  has4G = true;
  updateStatusLED();

  Serial.print(" OK  IP: ");
  Serial.println(modem.localIP());

  return true;
}

// ============================================================
bool sendTestPositionToTraccar() {
  Serial.println();
  Serial.println("Sending test position to Traccar...");

  if (!modem.isGprsConnected()) {
    Serial.println("GPRS not connected. Reconnecting...");

    has4G = false;
    updateStatusLED();

    if (!modem.gprsConnect(APN, USER, PASS)) {
      Serial.println("GPRS reconnect failed.");
      return false;
    }

    has4G = true;
    updateStatusLED();

    Serial.print("Reconnected. IP: ");
    Serial.println(modem.localIP());
  }

  // Move the fake point slightly every send so Traccar shows updates.
  testLat += 0.00005;
  testLon += 0.00005;

  String path = "/?id=";
  path += DEVICE_ID;

  path += "&lat=";
  path += String(testLat, 6);

  path += "&lon=";
  path += String(testLon, 6);

  path += "&speed=0";
  path += "&altitude=100";
  path += "&accuracy=999";
  path += "&batt=100";
  path += "&satellites=0";

  Serial.print("GET ");
  Serial.println(path);

  HttpClient http(gsmClient, TRACCAR_HOST, TRACCAR_PORT);
  http.setTimeout(15000);

  int err = http.get(path);

  if (err != 0) {
    Serial.print("HTTP client error: ");
    Serial.println(err);
    http.stop();
    return false;
  }

  int status = http.responseStatusCode();
  String response = http.responseBody();

  Serial.print("HTTP status: ");
  Serial.println(status);

  Serial.print("Response body: ");
  Serial.println(response);

  http.stop();

  return status >= 200 && status < 300;
}

// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("======================================");
  Serial.println("   Hati Tracker — Traccar Test Only");
  Serial.println("======================================");

  pinMode(LED_RED1, OUTPUT);
  pinMode(LED_RED2, OUTPUT);

  setLED(LED_RED1, false);
  setLED(LED_RED2, false);

  // LED boot test
  setLED(LED_RED1, true);
  setLED(LED_RED2, true);
  delay(1000);
  setLED(LED_RED1, false);
  setLED(LED_RED2, false);

  powerOnModem();

  modemSerial.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(1000);

  if (!waitForModem()) {
    Serial.println("Modem did not respond. Restarting ESP...");
    delay(2000);
    ESP.restart();
  }

  modem.init();

  Serial.print("Modem: ");
  Serial.println(modem.getModemInfo());

  if (!connect4G()) {
    Serial.println("4G connection failed. Restarting ESP...");
    delay(3000);
    ESP.restart();
  }

  Serial.println();
  Serial.println("4G ready. Starting Traccar test sends.");
  Serial.println();

  // Send once immediately.
  bool ok = sendTestPositionToTraccar();

  if (ok) {
    Serial.println("TRACCAR TEST SEND: SUCCESS");
    blinkRed2(1);
  } else {
    Serial.println("TRACCAR TEST SEND: FAILED");
    blinkRed2(3);
  }

  lastSendMs = millis();
}

// ============================================================
void loop() {
  updateStatusLED();

  unsigned long now = millis();

  if (now - lastSendMs >= SEND_INTERVAL_MS) {
    bool ok = sendTestPositionToTraccar();

    if (ok) {
      Serial.println("TRACCAR TEST SEND: SUCCESS");
      blinkRed2(1);
    } else {
      Serial.println("TRACCAR TEST SEND: FAILED");
      blinkRed2(3);
    }

    lastSendMs = now;
  }

  delay(100);
}
