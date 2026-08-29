/*
  AutismBuds firmware — "laptop audio" build (ESP32-WROOM + MAX30102 only)

  Why this build exists:
    The original build had the ESP32 itself pair with the Bluetooth earbuds
    (A2DP source) and play the alert tone. That pairing was unreliable, so in
    this build the ESP32 does ONE job — read the sensor and send readings to
    the backend over WiFi. The earbuds instead pair directly with the laptop
    (normal OS Bluetooth pairing, not done by this sketch), and the laptop
    (running backend/main2.py) plays the alert tone through them.

  What it does:
    1. Reads the MAX30102 over I2C, runs beat detection, computes a smoothed BPM.
    2. Sends {bpm, finger_detected, ir, ts} as JSON to the backend over a WebSocket
       (ws://BACKEND_HOST:BACKEND_PORT/ws/device) about once a second.
    That's it — no Bluetooth/A2DP code on the ESP32 in this build.

  Wiring (MAX30102 -> ESP32-WROOM, I2C):
    VIN -> 3V3      GND -> GND
    SCL -> GPIO22   SDA -> GPIO21   INT -> GPIO4 (optional)

  Libraries required (Arduino Library Manager unless noted):
    - SparkFun MAX3010x Pulse and Proximity Sensor Library
    - ArduinoJson (Benoit Blanchon)
    - arduinoWebSockets (Markus Sattler / Links2004)
    - ESP32 board package (espressif/arduino-esp32) via Boards Manager
      (no ESP32-A2DP library needed for this build)

  Setup steps for this build:
    1. Pair the Bluetooth earbuds with the LAPTOP directly (System Settings ->
       Bluetooth on Mac, or Settings -> Bluetooth on Windows) and make sure
       they're selected as the laptop's output device.
    2. Run `python main2.py` (see backend/) on the laptop — it plays the
       alert tone locally through whatever output device is selected.
    3. Fill in the WiFi / backend values below and upload this sketch.

  Before uploading: fill in the WiFi / backend values just below.
*/

#include <WiFi.h>
#include <Wire.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "MAX30105.h"
#include "heartRate.h"

// ---------------------------------------------------------------------------
// Config — fill these in for your setup before uploading.
// ---------------------------------------------------------------------------

#define WIFI_SSID     "YourWiFiName"
#define WIFI_PASSWORD "YourWiFiPassword"

// The laptop running the FastAPI backend (main2.py), on the same WiFi network.
// Find it with `ipconfig` (Windows) / `ifconfig` or `ipconfig getifaddr en0` (Mac).
#define BACKEND_HOST  "192.168.1.100"
#define BACKEND_PORT  8000
#define BACKEND_PATH  "/ws/device"

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

MAX30105 particleSensor;
WebSocketsClient webSocket;

const byte RATE_SIZE = 4;          // running average window for BPM smoothing
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;
float beatsPerMinute = 0;
int beatAvg = 0;

const long FINGER_THRESHOLD = 50000;  // IR value above this ~= finger present
unsigned long lastSend = 0;
const unsigned long SEND_INTERVAL_MS = 1000;

// ---------------------------------------------------------------------------
// WebSocket: connection lifecycle logging
// (The laptop drives the alert sound now, so the ESP32 doesn't need to act
// on ALERT_ON/ALERT_OFF commands — but we still log anything it sends.)
// ---------------------------------------------------------------------------

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[WS] connected to backend");
      break;
    case WStype_DISCONNECTED:
      Serial.println("[WS] disconnected from backend");
      break;
    case WStype_TEXT:
      Serial.print("[WS] message from backend: ");
      Serial.write(payload, length);
      Serial.println();
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Setup
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);

  // --- MAX30102 ---
  Wire.begin(21, 22);  // SDA, SCL
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("MAX30102 not found. Check wiring.");
    while (1) delay(1000);
  }
  particleSensor.setup();               // default: red+IR, sampleAverage=4, ledMode=2
  particleSensor.setPulseAmplitudeRed(0x0A);
  particleSensor.setPulseAmplitudeGreen(0);

  // --- WiFi ---
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi connected, IP: ");
  Serial.println(WiFi.localIP());

  // --- WebSocket to backend ---
  webSocket.begin(BACKEND_HOST, BACKEND_PORT, BACKEND_PATH);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(3000);
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void loop() {
  webSocket.loop();

  long irValue = particleSensor.getIR();
  bool fingerDetected = irValue > FINGER_THRESHOLD;

  if (fingerDetected && checkForBeat(irValue)) {
    long delta = millis() - lastBeat;
    lastBeat = millis();
    beatsPerMinute = 60.0 / (delta / 1000.0);

    if (beatsPerMinute > 20 && beatsPerMinute < 255) {
      rates[rateSpot++] = (byte)beatsPerMinute;
      rateSpot %= RATE_SIZE;
      int total = 0;
      for (byte i = 0; i < RATE_SIZE; i++) total += rates[i];
      beatAvg = total / RATE_SIZE;
    }
  }

  if (millis() - lastSend >= SEND_INTERVAL_MS) {
    lastSend = millis();

    StaticJsonDocument<128> doc;
    doc["bpm"] = fingerDetected ? beatAvg : 0;
    doc["ir"] = irValue;
    doc["finger_detected"] = fingerDetected;
    doc["ts"] = millis();

    String json;
    serializeJson(doc, json);
    webSocket.sendTXT(json);
  }
}
