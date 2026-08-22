/*
  AutismBuds firmware — ESP32-WROOM + MAX30102 + Bluetooth earbuds (A2DP)

  What it does:
    1. Reads the MAX30102 over I2C, runs beat detection, computes a smoothed BPM.
    2. Sends {bpm, finger_detected, ir, ts} as JSON to the backend over a WebSocket
       (ws://BACKEND_HOST:BACKEND_PORT/ws/device) about once a second.
    3. Listens on that same WebSocket for {"cmd":"ALERT_ON"} / {"cmd":"ALERT_OFF"}
       from the backend and, when told to, streams a short alert tone to the
       Bluetooth earbuds via A2DP.

  Wiring (MAX30102 -> ESP32-WROOM, I2C):
    VIN -> 3V3      GND -> GND
    SCL -> GPIO22   SDA -> GPIO21   INT -> GPIO4 (optional)

  Libraries required (Arduino Library Manager unless noted):
    - SparkFun MAX3010x Pulse and Proximity Sensor Library
    - ArduinoJson (Benoit Blanchon)
    - arduinoWebSockets (Markus Sattler / Links2004)
    - ESP32-A2DP (pschatzmann)
    - ESP32 board package (espressif/arduino-esp32) via Boards Manager

  Before uploading: copy secrets.h.example -> secrets.h and fill in your WiFi +
  backend + earbud details.
*/

#include <WiFi.h>
#include <Wire.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "MAX30105.h"
#include "heartRate.h"
#include "BluetoothA2DPSource.h"
#include "secrets.h"

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

MAX30105 particleSensor;
WebSocketsClient webSocket;
BluetoothA2DPSource a2dpSource;

const byte RATE_SIZE = 4;          // running average window for BPM smoothing
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;
float beatsPerMinute = 0;
int beatAvg = 0;

const long FINGER_THRESHOLD = 50000;  // IR value above this ~= finger present
unsigned long lastSend = 0;
const unsigned long SEND_INTERVAL_MS = 1000;

volatile bool alertActive = false;
unsigned long alertStartedAt = 0;
const unsigned long ALERT_MAX_MS = 4000;  // hard local cap, backend also enforces one
const double TONE_FREQ_HZ = 880.0;        // alert tone pitch

// ---------------------------------------------------------------------------
// Bluetooth A2DP: generates the alert tone (silence when no alert active)
// ---------------------------------------------------------------------------

int32_t get_audio_frames(Frame* frames, int32_t frame_count) {
  static double phase = 0.0;
  const double sampleRate = 44100.0;

  if (!alertActive) {
    for (int i = 0; i < frame_count; i++) {
      frames[i].channel1 = 0;
      frames[i].channel2 = 0;
    }
    return frame_count;
  }

  for (int i = 0; i < frame_count; i++) {
    int16_t sample = (int16_t)(sin(phase) * 12000);  // moderate volume sine wave
    frames[i].channel1 = sample;
    frames[i].channel2 = sample;
    phase += 2.0 * PI * TONE_FREQ_HZ / sampleRate;
    if (phase > 2.0 * PI) phase -= 2.0 * PI;
  }
  return frame_count;
}

void startAlert() {
  alertActive = true;
  alertStartedAt = millis();
}

void stopAlert() {
  alertActive = false;
}

// ---------------------------------------------------------------------------
// WebSocket: handle commands from the backend
// ---------------------------------------------------------------------------

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.println("[WS] connected to backend");
      break;
    case WStype_DISCONNECTED:
      Serial.println("[WS] disconnected from backend");
      break;
    case WStype_TEXT: {
      StaticJsonDocument<128> doc;
      DeserializationError err = deserializeJson(doc, payload, length);
      if (err) return;
      const char* cmd = doc["cmd"] | "";
      if (strcmp(cmd, "ALERT_ON") == 0) {
        Serial.println("[ALERT] ON");
        startAlert();
      } else if (strcmp(cmd, "ALERT_OFF") == 0) {
        Serial.println("[ALERT] OFF");
        stopAlert();
      }
      break;
    }
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

  // --- Bluetooth A2DP source (streams to the earbuds) ---
  a2dpSource.start(EARBUDS_NAME, get_audio_frames);
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

  // local safety net: auto-clear an alert if the backend's ALERT_OFF never arrives
  if (alertActive && millis() - alertStartedAt > ALERT_MAX_MS) {
    stopAlert();
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
