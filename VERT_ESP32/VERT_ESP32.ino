/*
  ===========================================================================
   V.E.R.T. - Versatile Eco-farming Robotic Technology
   ESP32 Main Controller Firmware
  ===========================================================================

  HARDWARE ON THIS BUS:
    - ESP32 DevKit V1              (main board, powered via HW-131 5V rail)
    - 5x Soil moisture sensors     (analog)
    - 2x pH sensor modules         (analog)
    - 1x DHT22                     (temperature + humidity)
    - 2x Limit switches            (door OPEN / door CLOSED)
    - 1x L298N driver              (Channel A -> 12V door motor)
    - 1x Relay module              (switches the 12V DC water pump)
    - 12V/5A adapter               (feeds L298N motor supply / pump supply)
    - HW-131 breadboard PSU        (feeds ESP32 5V/3V3 rail)

  LIBRARIES REQUIRED (Sketch > Include Library > Manage Libraries):
    - "DHT sensor library" by Adafruit   (also installs "Adafruit Unified Sensor")
    - Everything else (WiFi, WebServer, LittleFS, Preferences, ESPmDNS) ships
      with the ESP32 board package, nothing else to install.

  BEFORE UPLOADING:
    1. Set WIFI_SSID / WIFI_PASSWORD below.
    2. Tools > ESP32 Sketch Data Upload (installed via the "ESP32FS" plugin,
       or in Arduino IDE 2.x the "ESP32 Sketch Data Upload" command from the
       command palette) to push the /data folder (index.html, style.css,
       script.js) into LittleFS. Do this BEFORE or AFTER flashing the sketch,
       order doesn't matter, but it must be done at least once.
    3. Calibrate the soil and pH sensors (see CALIBRATION section below) -
       the numbers baked in are reasonable defaults but every sensor batch
       and soil type behaves a little differently.

  ===========================================================================
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <ESPmDNS.h>

// ===========================================================================
// --- WiFi credentials ---
// ===========================================================================
const char* WIFI_SSID = "Airbox-7113";
const char* WIFI_PASSWORD = "14594540";
const char* MDNS_NAME = "vert";  // reachable at http://vert.local

// ===========================================================================
// --- Pin definitions ---
// ===========================================================================
// Analog sensors
// NOTE: this is a 30-pin Doit ESP32 DevKit V1. GPIO36 ("VP") and GPIO39 ("VN")
// exist on the chip but this board doesn't break them out to a header pin -
// its rightmost usable pin is GPIO35. SOIL5 and PH1 were moved to free ADC2
// pins instead. They'll share ADC2 with the WiFi radio like PH2 already does,
// which readAnalogSafe() already handles with median-of-3 sampling.
#define SOIL1_PIN 32  // ADC1_CH4
#define SOIL2_PIN 33  // ADC1_CH5
#define SOIL3_PIN 34  // ADC1_CH6 (input only)
#define SOIL4_PIN 35  // ADC1_CH7 (input only)
#define SOIL5_PIN 2   // ADC2_CH2 - boot-strapping pin (must be low/floating at boot); \
                      // fine for a high-impedance analog sensor, but disconnect it if you ever see boot issues
#define PH1_PIN 12    // ADC2_CH5 - boot-strapping pin (MTDI, selects flash voltage, \
                      // must NOT be pulled high at boot) - same caveat as above
#define PH2_PIN 25    // ADC2_CH8 - shares ADC2 with WiFi, see readAnalogSafe()

// DHT22
#define DHTPIN 4
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);

// Limit switches (wired NO to GND, using internal pullups)
#define LIMIT_DOOR_OPEN_PIN 16
#define LIMIT_DOOR_CLOSED_PIN 17 z0
// L298N - drives the door motor (this is now the only thing on the L298N)
#define MOTOR_IN1 18
#define MOTOR_IN2 19
#define MOTOR_ENA 23

// Irrigation pump - single-channel relay module, not the L298N
#define PUMP_RELAY_PIN 27

// Status LEDs (optional, wire through a ~220ohm resistor)
#define LED_DOOR_PIN 26
#define LED_PUMP_PIN 21

// Grow light bulb (relay-driven, not a direct GPIO->bulb connection)
#define BULB_PIN 15

  // ===========================================================================
  // --- Calibration constants (ADJUST THESE FOR YOUR HARDWARE) ---
  // ===========================================================================
  // Soil moisture: raw ADC (0-4095). Typical capacitive sensor reads HIGH when
  // dry and LOW when fully submerged in water. Put each sensor in dry air and
  // in a cup of water, note the readings, and update these two numbers.
  const int SOIL_DRY_RAW = 3000;  // raw value in dry air
const int SOIL_WET_RAW = 1200;    // raw value fully in water

// pH sensor: needs a 2-point calibration with pH 4.0 and pH 7.0 buffer
// solutions. Measure the millivolt reading in each buffer and fill these in.
// pH = 7.0 - ((measuredMillivolts - NEUTRAL_MV) / MV_PER_PH_UNIT)
const float PH_NEUTRAL_MV = 1500.0;  // mV reading at pH 7.0 buffer
const float PH_MV_PER_UNIT = 180.0;  // mV change per 1.0 pH unit (from buffer test)

// Relay modules (grow light bulb, irrigation pump) are almost always
// ACTIVE-LOW: pulling the control pin LOW energizes the relay and turns the
// load ON; HIGH turns it OFF. That's the opposite of "HIGH = on", which is
// why the bulb used to come on by itself and turning it "on" in the UI
// switched it off. If your relay board is one of the less-common active-HIGH
// ones, set this to false instead.
const bool RELAY_ACTIVE_LOW = true;

// Door motor safety timeout - if it runs this long without hitting a limit
// switch, something's wrong (jam, broken switch) - stop and flag an error.
const unsigned long MOTOR_TIMEOUT_MS = 15000;

// How often we refresh sensor readings
const unsigned long SENSOR_INTERVAL_MS = 2000;

// Debounce for limit switches
const unsigned long DEBOUNCE_MS = 50;

// ===========================================================================
// --- State ---
// ===========================================================================
WebServer server(80);
Preferences prefs;

enum DoorState { DOOR_UNKNOWN,
                 DOOR_OPEN,
                 DOOR_CLOSED,
                 DOOR_OPENING,
                 DOOR_CLOSING,
                 DOOR_ERROR };
DoorState doorState = DOOR_UNKNOWN;
unsigned long doorMoveStartedAt = 0;

bool autoIrrigation = true;
bool pumpState = false;
bool bulbState = false;

// Farmer-configurable thresholds (persisted in flash)
float soilMinPct = 30.0;  // irrigate when average soil moisture drops below this
float soilMaxPct = 70.0;  // stop irrigating once it rises above this
float tempMinC = 18.0;    // just used for on-screen alerts
float tempMaxC = 32.0;
float phMin = 5.5;
float phMax = 7.5;

// Live sensor values
float soilPct[5] = { 0, 0, 0, 0, 0 };
float soilAvgPct = 0;
float phValue[2] = { 7.0, 7.0 };
float temperatureC = NAN;
float humidityPct = NAN;

unsigned long lastSensorRead = 0;
unsigned long lastLimitCheck = 0;

// ===========================================================================
// --- Helpers ---
// ===========================================================================

// Median-of-3 read to smooth out ADC noise (particularly useful on GPIO25
// which shares ADC2 with the WiFi radio and can occasionally glitch).
int readAnalogSafe(uint8_t pin) {
  int a = analogRead(pin);
  delayMicroseconds(200);
  int b = analogRead(pin);
  delayMicroseconds(200);
  int c = analogRead(pin);
  // return the median of the three
  if (a > b) {
    int t = a;
    a = b;
    b = t;
  }
  if (b > c) {
    int t = b;
    b = c;
    c = t;
  }
  if (a > b) {
    int t = a;
    a = b;
    b = t;
  }
  return b;
}

float rawToSoilPct(int raw) {
  float pct = map(raw, SOIL_DRY_RAW, SOIL_WET_RAW, 0, 100);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

float rawToPh(int raw) {
  float mv = raw * (3300.0 / 4095.0);  // ESP32 ADC ref ~3.3V, adjust if you added attenuation
  float ph = 7.0 - ((mv - PH_NEUTRAL_MV) / PH_MV_PER_UNIT);
  return ph;
}

// Writes the correct electrical level to a relay control pin for the
// requested logical on/off state, honoring RELAY_ACTIVE_LOW above.
void writeRelay(uint8_t pin, bool on) {
  int level = RELAY_ACTIVE_LOW ? (on ? LOW : HIGH) : (on ? HIGH : LOW);
  digitalWrite(pin, level);
}

void loadSettings() {
  prefs.begin("vert", true);  // read-only
  soilMinPct = prefs.getFloat("soilMin", soilMinPct);
  soilMaxPct = prefs.getFloat("soilMax", soilMaxPct);
  tempMinC = prefs.getFloat("tempMin", tempMinC);
  tempMaxC = prefs.getFloat("tempMax", tempMaxC);
  phMin = prefs.getFloat("phMin", phMin);
  phMax = prefs.getFloat("phMax", phMax);
  autoIrrigation = prefs.getBool("autoIrr", autoIrrigation);
  prefs.end();
}

void saveSettings() {
  prefs.begin("vert", false);  // read-write
  prefs.putFloat("soilMin", soilMinPct);
  prefs.putFloat("soilMax", soilMaxPct);
  prefs.putFloat("tempMin", tempMinC);
  prefs.putFloat("tempMax", tempMaxC);
  prefs.putFloat("phMin", phMin);
  prefs.putFloat("phMax", phMax);
  prefs.putBool("autoIrr", autoIrrigation);
  prefs.end();
}

// ===========================================================================
// --- Motor / door control ---
// ===========================================================================
void motorStop() {
  digitalWrite(MOTOR_IN1, LOW);
  digitalWrite(MOTOR_IN2, LOW);
  digitalWrite(MOTOR_ENA, LOW);
}

void motorRunOpenDirection() {
  digitalWrite(MOTOR_IN1, HIGH);
  digitalWrite(MOTOR_IN2, LOW);
  digitalWrite(MOTOR_ENA, HIGH);
}

void motorRunCloseDirection() {
  digitalWrite(MOTOR_IN1, LOW);
  digitalWrite(MOTOR_IN2, HIGH);
  digitalWrite(MOTOR_ENA, HIGH);
}

void commandDoorOpen() {
  if (doorState == DOOR_OPEN || doorState == DOOR_OPENING) return;
  doorState = DOOR_OPENING;
  doorMoveStartedAt = millis();
  motorRunOpenDirection();
}

void commandDoorClose() {
  if (doorState == DOOR_CLOSED || doorState == DOOR_CLOSING) return;
  doorState = DOOR_CLOSING;
  doorMoveStartedAt = millis();
  motorRunCloseDirection();
}

void serviceDoor() {
  bool openHit = (digitalRead(LIMIT_DOOR_OPEN_PIN) == LOW);
  bool closedHit = (digitalRead(LIMIT_DOOR_CLOSED_PIN) == LOW);

  if (doorState == DOOR_OPENING) {
    if (openHit) {
      motorStop();
      doorState = DOOR_OPEN;
    } else if (millis() - doorMoveStartedAt > MOTOR_TIMEOUT_MS) {
      motorStop();
      doorState = DOOR_ERROR;
    }
  } else if (doorState == DOOR_CLOSING) {
    if (closedHit) {
      motorStop();
      doorState = DOOR_CLOSED;
    } else if (millis() - doorMoveStartedAt > MOTOR_TIMEOUT_MS) {
      motorStop();
      doorState = DOOR_ERROR;
    }
  } else if (doorState == DOOR_UNKNOWN) {
    // At boot, if we happen to already be sitting on a limit switch, trust it.
    if (openHit) doorState = DOOR_OPEN;
    else if (closedHit) doorState = DOOR_CLOSED;
  }

  digitalWrite(LED_DOOR_PIN, (doorState == DOOR_OPEN) ? HIGH : LOW);
}

// ===========================================================================
// --- Pump control ---
// ===========================================================================
void setPump(bool on) {
  pumpState = on;
  writeRelay(PUMP_RELAY_PIN, on);
  digitalWrite(LED_PUMP_PIN, on ? HIGH : LOW);  // status LED is plain active-high, not the relay
}

// ===========================================================================
// --- Grow light control ---
// ===========================================================================
void setBulb(bool on) {
  bulbState = on;
  writeRelay(BULB_PIN, on);
}

void serviceIrrigation() {
  if (!autoIrrigation) return;  // manual mode, farmer controls pump directly
  if (soilAvgPct < soilMinPct && !pumpState) {
    setPump(true);
  } else if (soilAvgPct >= soilMaxPct && pumpState) {
    setPump(false);
  }
}

// ===========================================================================
// --- Sensor sampling ---
// ===========================================================================
void readAllSensors() {
  int raw1 = readAnalogSafe(SOIL1_PIN);
  int raw2 = readAnalogSafe(SOIL2_PIN);
  int raw3 = readAnalogSafe(SOIL3_PIN);
  int raw4 = readAnalogSafe(SOIL4_PIN);
  int raw5 = readAnalogSafe(SOIL5_PIN);

  // CALIBRATION: watch these raw values while a sensor sits in dry air and
  // fully submerged in water, then set SOIL_DRY_RAW / SOIL_WET_RAW to match.
  // Remove this line once calibration is done.
  Serial.printf("Soil raw -> S1:%4d S2:%4d S3:%4d S4:%4d S5:%4d\n", raw1, raw2, raw3, raw4, raw5);

  soilPct[0] = rawToSoilPct(raw1);
  soilPct[1] = rawToSoilPct(raw2);
  soilPct[2] = rawToSoilPct(raw3);
  soilPct[3] = rawToSoilPct(raw4);
  soilPct[4] = rawToSoilPct(raw5);
  soilAvgPct = (soilPct[0] + soilPct[1] + soilPct[2] + soilPct[3] + soilPct[4]) / 5.0;

  int rawPh1 = readAnalogSafe(PH1_PIN);
  int rawPh2 = readAnalogSafe(PH2_PIN);  // ADC2 pin, see readAnalogSafe() note
  // CALIBRATION: dip the probe in pH 4.0 buffer, note the mV reading below,
  // then in pH 7.0 buffer, note that mV reading too. Then:
  //   PH_NEUTRAL_MV  = the mV you saw in the pH 7.0 buffer
  //   PH_MV_PER_UNIT = (mV in pH4 buffer - mV in pH7 buffer) / 3.0
  // (3.0 because pH 7.0 -> 4.0 is a 3-unit swing). Update both constants near
  // the top of this file, then remove this debug line.
  float mvPh1 = rawPh1 * (3300.0 / 4095.0);
  float mvPh2 = rawPh2 * (3300.0 / 4095.0);
  Serial.printf("pH raw -> P1:%4d (%.0fmV) P2:%4d (%.0fmV)\n", rawPh1, mvPh1, rawPh2, mvPh2);
  phValue[0] = rawToPh(rawPh1);
  phValue[1] = rawToPh(rawPh2);

  float t = dht.readTemperature();
  float h = dht.readHumidity();
  // DEBUG: remove once DHT22 is confirmed reading reliably. NaN here means
  // the DHT library's read() failed - almost always a wiring/pin/power issue
  // (wrong DHTPIN, sensor not powered, or a flaky connection), not a code bug.
  Serial.printf("DHT raw -> temp:%s humidity:%s\n",
                isnan(t) ? "NaN" : String(t, 1).c_str(),
                isnan(h) ? "NaN" : String(h, 1).c_str());
  if (!isnan(t)) temperatureC = t;
  if (!isnan(h)) humidityPct = h;
}

// ===========================================================================
// --- Web server: static files from LittleFS ---
// ===========================================================================
String contentTypeFor(const String& path) {
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".js")) return "application/javascript";
  return "text/plain";
}

bool serveFile(String path) {
  if (path.endsWith("/")) path += "index.html";
  if (!LittleFS.exists(path)) return false;
  File f = LittleFS.open(path, "r");
  server.streamFile(f, contentTypeFor(path));
  f.close();
  return true;
}

void handleNotFound() {
  if (!serveFile(server.uri())) {
    server.send(404, "text/plain", "Not found");
  }
}

// ===========================================================================
// --- Web server: API endpoints ---
// ===========================================================================
String doorStateToString() {
  switch (doorState) {
    case DOOR_OPEN: return "open";
    case DOOR_CLOSED: return "closed";
    case DOOR_OPENING: return "opening";
    case DOOR_CLOSING: return "closing";
    case DOOR_ERROR: return "error";
    default: return "unknown";
  }
}

void handleData() {
  String json = "{";
  json += "\"soil\":[" + String(soilPct[0], 1) + "," + String(soilPct[1], 1) + "," + String(soilPct[2], 1) + "," + String(soilPct[3], 1) + "," + String(soilPct[4], 1) + "],";
  json += "\"soilAvg\":" + String(soilAvgPct, 1) + ",";
  json += "\"ph\":[" + String(phValue[0], 2) + "," + String(phValue[1], 2) + "],";
  json += "\"temp\":" + String(isnan(temperatureC) ? -1 : temperatureC, 1) + ",";
  json += "\"humidity\":" + String(isnan(humidityPct) ? -1 : humidityPct, 1) + ",";
  json += "\"doorState\":\"" + doorStateToString() + "\",";
  json += "\"pumpOn\":" + String(pumpState ? "true" : "false") + ",";
  json += "\"autoIrrigation\":" + String(autoIrrigation ? "true" : "false") + ",";
  json += "\"bulbOn\":" + String(bulbState ? "true" : "false") + ",";
  json += "\"soilMin\":" + String(soilMinPct, 1) + ",";
  json += "\"soilMax\":" + String(soilMaxPct, 1) + ",";
  json += "\"tempMin\":" + String(tempMinC, 1) + ",";
  json += "\"tempMax\":" + String(tempMaxC, 1) + ",";
  json += "\"phMin\":" + String(phMin, 2) + ",";
  json += "\"phMax\":" + String(phMax, 2);
  json += "}";
  server.send(200, "application/json", json);
}

void handleSetThresholds() {
  if (server.hasArg("soilMin")) soilMinPct = server.arg("soilMin").toFloat();
  if (server.hasArg("soilMax")) soilMaxPct = server.arg("soilMax").toFloat();
  if (server.hasArg("tempMin")) tempMinC = server.arg("tempMin").toFloat();
  if (server.hasArg("tempMax")) tempMaxC = server.arg("tempMax").toFloat();
  if (server.hasArg("phMin")) phMin = server.arg("phMin").toFloat();
  if (server.hasArg("phMax")) phMax = server.arg("phMax").toFloat();
  saveSettings();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handlePump() {
  if (autoIrrigation) {
    server.send(409, "application/json", "{\"ok\":false,\"reason\":\"auto mode is on\"}");
    return;
  }
  if (server.hasArg("state")) {
    String s = server.arg("state");
    setPump(s == "on");
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleBulb() {
  if (server.hasArg("state")) {
    String s = server.arg("state");
    setBulb(s == "on");
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleMode() {
  if (server.hasArg("auto")) {
    autoIrrigation = server.arg("auto") == "1";
    saveSettings();
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleDoor() {
  if (server.hasArg("action")) {
    String a = server.arg("action");
    if (a == "open") commandDoorOpen();
    else if (a == "close") commandDoorClose();
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

// ===========================================================================
// --- WiFi diagnostics ---
// ===========================================================================
// Prints every 2.4GHz network the ESP32 can see. ESP32 radios are 2.4GHz-only,
// so if your target SSID doesn't show up here, it's either out of range or
// only being broadcast on 5GHz (common on phone hotspots / dual-band routers
// that reuse the same SSID for both bands).
void scanNetworks() {
  Serial.println("Scanning for WiFi networks...");
  int n = WiFi.scanNetworks();
  if (n < 0) {
    // WIFI_SCAN_FAILED (-2) or WIFI_SCAN_RUNNING (-1) - usually means the scan
    // started too soon after WiFi.mode()/disconnect(). Give it a moment and retry once.
    Serial.printf("  Scan did not start (code %d) - retrying...\n", n);
    delay(1000);
    n = WiFi.scanNetworks();
  }
  if (n < 0) {
    Serial.printf("  Scan failed again (code %d).\n", n);
  } else if (n == 0) {
    Serial.println("  No networks found.");
  } else {
    for (int i = 0; i < n; i++) {
      Serial.printf("  %2d: %-32s RSSI=%4d dBm  ch=%2d  %s\n",
                    i + 1,
                    WiFi.SSID(i).c_str(),
                    WiFi.RSSI(i),
                    WiFi.channel(i),
                    (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) ? "open" : "secured");
    }
  }
  WiFi.scanDelete();
}

// ===========================================================================
// --- Setup ---
// ===========================================================================
void setup() {
  Serial.begin(115200);
  Serial.println("### BUILD MARKER: VERT_ESP32 rev A ###");

  pinMode(LIMIT_DOOR_OPEN_PIN, INPUT_PULLUP);
  pinMode(LIMIT_DOOR_CLOSED_PIN, INPUT_PULLUP);

  pinMode(MOTOR_IN1, OUTPUT);
  pinMode(MOTOR_IN2, OUTPUT);
  pinMode(MOTOR_ENA, OUTPUT);
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  pinMode(LED_DOOR_PIN, OUTPUT);
  pinMode(LED_PUMP_PIN, OUTPUT);
  pinMode(BULB_PIN, OUTPUT);
  motorStop();
  setPump(false);
  setBulb(false);

  dht.begin();

  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed - did you run 'ESP32 Sketch Data Upload'?");
  }

  loadSettings();

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);
  scanNetworks();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
    delay(400);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("Connected! IP address: ");
    Serial.println(WiFi.localIP());
    if (MDNS.begin(MDNS_NAME)) {
      Serial.print("Reachable at http://");
      Serial.print(MDNS_NAME);
      Serial.println(".local");
    }
  } else {
    Serial.println();
    Serial.println("WiFi connect failed - check credentials. Continuing offline.");
  }

  server.on("/data", HTTP_GET, handleData);
  server.on("/setThresholds", HTTP_POST, handleSetThresholds);
  server.on("/pump", HTTP_GET, handlePump);
  server.on("/bulb", HTTP_GET, handleBulb);
  server.on("/mode", HTTP_GET, handleMode);
  server.on("/door", HTTP_GET, handleDoor);
  server.onNotFound(handleNotFound);
  server.begin();

  readAllSensors();  // populate initial values before first web request
}

// ===========================================================================
// --- Main loop ---
// ===========================================================================
void loop() {
  server.handleClient();
  serviceDoor();  // check limit switches every loop, no delay, for fast response

  unsigned long now = millis();
  if (now - lastSensorRead >= SENSOR_INTERVAL_MS) {
    lastSensorRead = now;
    readAllSensors();
    serviceIrrigation();
  }
}