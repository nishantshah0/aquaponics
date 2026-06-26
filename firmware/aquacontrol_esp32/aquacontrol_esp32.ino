/* ============================================================================
   AquaControl — ESP32 firmware
   ENG SCI 1050 Aquaponics Project · Western University

   Reads the real aquaponics sensors and streams telemetry to the AquaControl
   dashboard (index.html) in the exact format it expects:

       {"ph":7.21,"temp":24.1,"level":1048,"tan":8.4}\n

   Two transports, both enabled by the dashboard out of the box:
     1. USB / Serial  — always on. The dashboard's "Connect via USB" button
        (Web Serial API) reads these JSON lines at 115200 baud.
     2. Wi-Fi / MQTT  — optional. Set USE_WIFI to 1 and fill in your creds.
        The dashboard's "Connect via Wi-Fi (MQTT)" button subscribes to the
        same topic.
     3. Wi-Fi / HTTP  — optional. Set USE_HTTP 1 to POST readings to the
        AquaControl Node backend (/server) at /api/telemetry. The backend
        persists history and the dashboard's "Connect to server" button shows
        live data + trend charts. This is the recommended path for a deploy.

   ---------------------------------------------------------------------------
   SENSORS (swap pins/params to match your build)
     - pH:    analog pH board (e.g. DFRobot SEN0161) on GPIO34 (ADC1)
     - Temp:  DS18B20 waterproof probe (OneWire) on GPIO4
     - Level: HC-SR04 ultrasonic (TRIG GPIO5, ECHO GPIO18) -> volume from
              tank geometry, OR swap for a pressure/eTape sensor.
     - TAN:   total ammonia nitrogen. Cheap inline TAN probes don't really
              exist, so by default we ESTIMATE TAN from the daily feed mass
              (TAN = feed_g/day * protein factor). If you have an ion-selective
              ammonia probe, read it on TAN_PIN and set TAN_FROM_PROBE = 1.
   ---------------------------------------------------------------------------
   LIBRARIES (install via Arduino Library Manager)
     - OneWire
     - DallasTemperature
     - PubSubClient            (only if USE_WIFI)
   Board: "ESP32 Dev Module" (esp32 core by Espressif).
   ============================================================================ */

#include <OneWire.h>
#include <DallasTemperature.h>

// ---------------------------------------------------------------------------
// CONFIG
// ---------------------------------------------------------------------------
#define USE_WIFI         0          // 1 = enable Wi-Fi (required for MQTT/HTTP)
#define USE_MQTT         0          // 1 = publish over MQTT  (needs USE_WIFI)
#define USE_HTTP         0          // 1 = POST to Node backend (needs USE_WIFI)
#define TAN_FROM_PROBE   0          // 1 = read analog ammonia probe; 0 = estimate

// Pins
const int PH_PIN     = 34;          // ADC1 — analog pH board
const int TAN_PIN    = 35;          // ADC1 — optional ammonia probe
const int ONE_WIRE   = 4;           // DS18B20 data
const int TRIG_PIN   = 5;           // HC-SR04 trigger
const int ECHO_PIN   = 18;          // HC-SR04 echo

// Tank geometry (matches the design report: 91 x 152 x 76 cm => 1061.9 L)
const float TANK_L_CM      = 152.0; // length
const float TANK_W_CM      = 91.0;  // width
const float TANK_H_CM      = 76.0;  // full water height
const float TANK_FULL_L    = 1061.9;
const float SENSOR_GAP_CM  = 8.0;   // distance from sensor face to full-water line

// pH calibration — two-point. Measure raw volts in pH 4.0 and pH 7.0 buffers.
const float PH_V_AT_7   = 1.50;     // volts at pH 7.00 buffer  (CALIBRATE!)
const float PH_V_AT_4   = 1.95;     // volts at pH 4.00 buffer  (CALIBRATE!)

// TAN estimate model (used when TAN_FROM_PROBE == 0)
// TAN_g_per_day = feed_g_per_day * protein% * 0.092   (Timmons & Ebeling)
float feed_g_per_day = 600.0;       // 40 fish at max feed
const float FEED_PROTEIN  = 0.32;   // 32% protein feed
const float TAN_COEFF     = 0.092;  // g TAN per g protein-N pathway

const unsigned long PUBLISH_MS = 2000;

// ---------------------------------------------------------------------------
OneWire oneWire(ONE_WIRE);
DallasTemperature ds18b20(&oneWire);

#if USE_WIFI
  #include <WiFi.h>
  const char* WIFI_SSID   = "YOUR_WIFI";
  const char* WIFI_PASS   = "YOUR_PASS";
#endif
#if USE_WIFI && USE_MQTT
  #include <PubSubClient.h>
  const char* MQTT_HOST   = "broker.hivemq.com";
  const int   MQTT_PORT   = 1883;
  const char* MQTT_TOPIC  = "aquacontrol/london/telemetry";
  WiFiClient   net;
  PubSubClient mqtt(net);
#endif
#if USE_WIFI && USE_HTTP
  #include <HTTPClient.h>
  // Your deployed backend, e.g. https://aquacontrol.onrender.com  (or http://<pc-ip>:8080)
  const char* HTTP_URL    = "http://192.168.1.50:8080/api/telemetry";
  const char* HTTP_APIKEY = "";       // set if the server has INGEST_KEY
  const char* DEVICE_ID   = "esp32-tank-1";
#endif

unsigned long lastPublish = 0;

// ---------------------------------------------------------------------------
// SENSOR READS
// ---------------------------------------------------------------------------
float readPh() {
  // Median of a few ADC samples -> volts -> pH via two-point calibration.
  long acc = 0;
  const int N = 16;
  for (int i = 0; i < N; i++) { acc += analogRead(PH_PIN); delay(4); }
  float volts = (acc / (float)N) * (3.3 / 4095.0);
  float slope = (4.00 - 7.00) / (PH_V_AT_4 - PH_V_AT_7); // pH per volt
  float ph = 7.00 + (volts - PH_V_AT_7) * slope;
  return constrain(ph, 0.0, 14.0);
}

float readTemp() {
  ds18b20.requestTemperatures();
  float t = ds18b20.getTempCByIndex(0);
  if (t <= -100 || t == DEVICE_DISCONNECTED_C) return NAN; // probe missing
  return t;
}

float readLevelLitres() {
  // Ultrasonic distance (cm) from sensor face to water surface.
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long us = pulseIn(ECHO_PIN, HIGH, 30000UL); // timeout 30 ms (~5 m)
  if (us == 0) return NAN;
  float dist_cm = us * 0.0343 / 2.0;
  float water_h = TANK_H_CM - (dist_cm - SENSOR_GAP_CM);
  water_h = constrain(water_h, 0.0, TANK_H_CM);
  float litres = (TANK_L_CM * TANK_W_CM * water_h) / 1000.0; // cm^3 -> L
  return constrain(litres, 0.0, TANK_FULL_L);
}

float readTan() {
#if TAN_FROM_PROBE
  long acc = 0; const int N = 16;
  for (int i = 0; i < N; i++) { acc += analogRead(TAN_PIN); delay(4); }
  float volts = (acc / (float)N) * (3.3 / 4095.0);
  return volts * 6.0; // map probe volts -> g/day (CALIBRATE to your probe)
#else
  return feed_g_per_day * FEED_PROTEIN * TAN_COEFF; // ~ design value 10.8 g/day
#endif
}

// ---------------------------------------------------------------------------
// OUTPUT
// ---------------------------------------------------------------------------
String buildJson(float ph, float temp, float level, float tan) {
  String s = "{";
  bool first = true;
  auto add = [&](const char* k, float v, int dp) {
    if (isnan(v)) return;                 // omit missing sensors
    if (!first) s += ",";
    s += "\""; s += k; s += "\":";
    s += String(v, dp);
    first = false;
  };
  add("ph", ph, 2);
  add("temp", temp, 1);
  add("level", level, 0);
  add("tan", tan, 1);
  s += "}";
  return s;
}

#if USE_WIFI
void ensureWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 8000) delay(200);
}
#endif
#if USE_WIFI && USE_MQTT
void ensureMqtt() {
  if (mqtt.connected()) return;
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  String id = "aquacontrol-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  mqtt.connect(id.c_str());
}
#endif
#if USE_WIFI && USE_HTTP
void postHttp(const String& json) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(HTTP_URL);
  http.addHeader("Content-Type", "application/json");
  if (HTTP_APIKEY[0]) http.addHeader("x-api-key", HTTP_APIKEY);
  // wrap with device id so the server can tag the row
  String body = json.substring(0, json.length() - 1);   // drop closing brace
  body += String(",\"device\":\"") + DEVICE_ID + "\"}";
  int code = http.POST(body);
  if (code <= 0) Serial.println(String("# HTTP POST failed: ") + http.errorToString(code));
  http.end();
}
#endif

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  analogReadResolution(12);            // 0..4095
  ds18b20.begin();
  Serial.println("# AquaControl ESP32 online. Streaming JSON telemetry @115200.");
#if USE_WIFI
  ensureWifi();
#endif
}

void loop() {
#if USE_WIFI
  ensureWifi();
#endif
#if USE_WIFI && USE_MQTT
  ensureMqtt();
  mqtt.loop();
#endif

  if (millis() - lastPublish >= PUBLISH_MS) {
    lastPublish = millis();

    float ph    = readPh();
    float temp  = readTemp();
    float level = readLevelLitres();
    float tan   = readTan();

    String json = buildJson(ph, temp, level, tan);

    // 1) USB / Serial (read by the dashboard's Web Serial button)
    Serial.println(json);

    // 2) Wi-Fi / MQTT
#if USE_WIFI && USE_MQTT
    if (mqtt.connected()) mqtt.publish(MQTT_TOPIC, json.c_str());
#endif

    // 3) Wi-Fi / HTTP -> Node backend
#if USE_WIFI && USE_HTTP
    postHttp(json);
#endif
  }
}
