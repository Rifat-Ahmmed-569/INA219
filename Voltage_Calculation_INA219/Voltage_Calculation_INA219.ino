
/*
  ESP32 GPS Tracker - INA219 Calibration Portal
  ------------------------------------------------
  I2C SDA: GPIO 21
  I2C SCL: GPIO 22

  Wi-Fi AP: INA219-Calibrator
  Password: TrackerCal123

  Open: http://192.168.4.1

  Libraries:
    - Adafruit INA219
    - ESP32 WebServer (built-in)
    - Preferences (built-in)

  Calibration is stored persistently in ESP32 NVS.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <Preferences.h>
#include <Adafruit_INA219.h>

// ---------------- PIN CONFIGURATION ----------------

#define INA219_SDA 21
#define INA219_SCL 22

// ---------------- WIFI ACCESS POINT ----------------

const char* AP_SSID = "INA219-Calibrator";
const char* AP_PASSWORD = "TrackerCal123";

// ---------------- OBJECTS ----------------

WebServer server(80);
Preferences preferences;
Adafruit_INA219 ina219;

// ---------------- STATE ----------------

bool ina219Ready = false;

float voltageFactor = 1.000000f;
float lastRawVoltage = 0.0f;
float lastCurrent_mA = 0.0f;
float lastPower_mW = 0.0f;
float lastReferenceVoltage = 0.0f;

unsigned long lastReadMs = 0;
const unsigned long READ_INTERVAL_MS = 500;

// ---------------- DASHBOARD ----------------

const char PAGE[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>INA219 Calibration</title>
<style>
body {
  margin: 0;
  padding: 20px;
  background: #101722;
  color: #edf3ff;
  font-family: Arial, sans-serif;
}
main { max-width: 560px; margin: auto; }
h2 { margin-bottom: 5px; }
.muted { color: #9eafc7; font-size: 14px; }
.card {
  background: #1b2737;
  border: 1px solid #34445a;
  border-radius: 12px;
  padding: 16px;
  margin-top: 14px;
}
.value { font-size: 28px; font-weight: bold; margin-top: 8px; }
label { display: block; margin: 14px 0 7px; }
input {
  width: 100%;
  box-sizing: border-box;
  background: #101722;
  border: 1px solid #526780;
  border-radius: 8px;
  padding: 13px;
  color: white;
  font-size: 18px;
}
button {
  width: 100%;
  padding: 13px;
  margin-top: 12px;
  border: 0;
  border-radius: 8px;
  background: #62d5ba;
  color: #10251f;
  font-size: 16px;
  font-weight: bold;
}
.secondary { background: #34445a; color: white; }
.status { margin-top: 12px; white-space: pre-wrap; }
</style>
</head>
<body>
<main>
<h2>INA219 Calibration</h2>
<p class="muted">ESP32 local battery-monitoring setup</p>

<div class="card">
  <div class="muted">INA219 status</div>
  <div id="status" class="value">Connecting...</div>
</div>

<div class="card">
  <div class="muted">Raw battery voltage</div>
  <div class="value"><span id="raw">--</span> V</div>

  <div class="muted">Calibrated battery voltage</div>
  <div class="value"><span id="cal">--</span> V</div>

  <div class="muted">Current</div>
  <div class="value"><span id="current">--</span> mA</div>

  <div class="muted">Power</div>
  <div class="value"><span id="power">--</span> mW</div>
</div>

<div class="card">
  <h3>Voltage calibration</h3>
  <p class="muted">
    Measure the battery voltage with your multimeter.
    Enter the reading in volts. Measure directly across
    the battery terminals, using the same battery state
    and operating conditions as the live reading.
  </p>

  <label for="reference">Multimeter voltage (V)</label>
  <input id="reference" type="number"
         min="2.5" max="5.0" step="0.001"
         placeholder="Example: 3.842">

  <button onclick="calibrate()">Save calibration</button>
  <button class="secondary" onclick="resetCalibration()">
    Reset calibration to 1.000000
  </button>

  <div class="status" id="message"></div>
</div>

<div class="card">
  <div class="muted">Saved correction factor</div>
  <div class="value" id="factor">--</div>
  <p class="muted">
    This factor is saved in non-volatile storage and
    survives power loss.
  </p>
</div>
</main>

<script>
async function refresh() {
  try {
    const response = await fetch('/api');
    const d = await response.json();

    document.getElementById('status').textContent =
      d.ready ? 'INA219 online' : 'INA219 unavailable';

    document.getElementById('raw').textContent =
      Number(d.raw).toFixed(3);

    document.getElementById('cal').textContent =
      Number(d.calibrated).toFixed(3);

    document.getElementById('current').textContent =
      Number(d.current).toFixed(1);

    document.getElementById('power').textContent =
      Number(d.power).toFixed(1);

    document.getElementById('factor').textContent =
      Number(d.factor).toFixed(6);

  } catch (e) {
    document.getElementById('status').textContent =
      'Connection lost';
  }
}

async function calibrate() {
  const input = document.getElementById('reference');
  const value = input.value;

  if (!value || !Number.isFinite(Number(value))) {
    document.getElementById('message').textContent =
      'Enter a valid multimeter reading.';
    return;
  }

  const body = new URLSearchParams();
  body.set('reference', value);

  try {
    const response = await fetch('/calibrate', {
      method: 'POST',
      headers: {'Content-Type':
        'application/x-www-form-urlencoded'},
      body
    });

    const result = await response.json();

    document.getElementById('message').textContent =
      result.message;

    await refresh();
  } catch (e) {
    document.getElementById('message').textContent =
      'Could not save calibration.';
  }
}

async function resetCalibration() {
  if (!confirm('Reset saved calibration?')) return;

  try {
    const response = await fetch('/reset', {method: 'POST'});
    const result = await response.json();

    document.getElementById('message').textContent =
      result.message;

    await refresh();
  } catch (e) {
    document.getElementById('message').textContent =
      'Reset failed.';
  }
}

refresh();
setInterval(refresh, 1000);
</script>
</body>
</html>
)HTML";

// ---------------- SENSOR READING ----------------

void readINA219() {
  if (!ina219Ready) return;

  float busVoltage = ina219.getBusVoltage_V();
  float shuntVoltage_mV = ina219.getShuntVoltage_mV();

  // Estimate voltage at the battery side of the shunt.
  float rawVoltage =
      busVoltage + (shuntVoltage_mV / 1000.0f);

  float current = ina219.getCurrent_mA();
  float power = ina219.getPower_mW();

  if (isfinite(rawVoltage) &&
      rawVoltage >= 0.0f &&
      rawVoltage < 30.0f) {
    lastRawVoltage = rawVoltage;
  }

  if (isfinite(current)) {
    lastCurrent_mA = current;
  }

  if (isfinite(power)) {
    lastPower_mW = power;
  }
}

// ---------------- SERIAL CALIBRATION OUTPUT ----------------

void printCalibration() {
  Serial.println();
  Serial.println("========================================");
  Serial.println(" SAVED INA219 VOLTAGE CALIBRATION");
  Serial.println("========================================");

  Serial.printf("Raw voltage:       %.6f V\n", lastRawVoltage);
  Serial.printf("Multimeter voltage: %.6f V\n",
                lastReferenceVoltage);

  Serial.printf("Voltage factor:     %.8f\n", voltageFactor);

  Serial.printf("Calibrated voltage: %.6f V\n",
                lastRawVoltage * voltageFactor);

  Serial.println();
  Serial.println("COPY INTO YOUR MAIN GPS PROJECT:");
  Serial.println("----------------------------------------");

  Serial.printf(
      "const float INA219_VOLTAGE_FACTOR = %.8ff;\n",
      voltageFactor
  );

  Serial.println();
  Serial.println("Apply it to the raw battery-side voltage:");
  Serial.println(
      "float batteryVoltage = rawBatteryVoltage * "
      "INA219_VOLTAGE_FACTOR;"
  );

  Serial.println("----------------------------------------");
  Serial.println("Current calibration is separate.");
  Serial.println("========================================");
}

// ---------------- HTTP HANDLERS ----------------

void handleRoot() {
  server.send_P(200, "text/html", PAGE);
}

void handleAPI() {
  readINA219();

  float calibratedVoltage = lastRawVoltage * voltageFactor;

  String json = "{";
  json += "\"ready\":";
  json += (ina219Ready ? "true" : "false");
  json += ",\"raw\":";
  json += String(lastRawVoltage, 6);
  json += ",\"calibrated\":";
  json += String(calibratedVoltage, 6);
  json += ",\"current\":";
  json += String(lastCurrent_mA, 3);
  json += ",\"power\":";
  json += String(lastPower_mW, 3);
  json += ",\"factor\":";
  json += String(voltageFactor, 8);
  json += "}";

  server.send(200, "application/json", json);
}

void handleCalibrate() {
  if (!ina219Ready) {
    server.send(503, "application/json",
                "{\"message\":\"INA219 unavailable.\"}");
    return;
  }

  if (!server.hasArg("reference")) {
    server.send(400, "application/json",
                "{\"message\":\"Missing reference voltage.\"}");
    return;
  }

  float reference = server.arg("reference").toFloat();

  // Intended for a single-cell Li-Po tracker battery.
  if (!isfinite(reference) ||
      reference < 2.5f || reference > 5.0f) {
    server.send(400, "application/json",
                "{\"message\":\"Voltage must be between 2.5 and 5.0 V.\"}");
    return;
  }

  readINA219();

  if (lastRawVoltage < 2.5f || lastRawVoltage > 5.0f) {
    server.send(400, "application/json",
                "{\"message\":\"Raw voltage out of expected range.\"}");
    return;
  }

  float newFactor = reference / lastRawVoltage;

  // Reject implausibly large corrections.
  if (!isfinite(newFactor) ||
      newFactor < 0.80f || newFactor > 1.20f) {
    server.send(400, "application/json",
                "{\"message\":\"Correction exceeds 20%. Check wiring and readings.\"}");
    return;
  }

  voltageFactor = newFactor;
  lastReferenceVoltage = reference;

  preferences.putFloat("vfactor", voltageFactor);
  preferences.putFloat("vref", lastReferenceVoltage);

  Serial.println();
  Serial.println("NEW VOLTAGE CALIBRATION SAVED");
  printCalibration();

  server.send(200, "application/json",
              "{\"message\":\"Calibration saved to ESP32 NVS. Check Serial Monitor for the reusable code.\"}");
}

void handleReset() {
  voltageFactor = 1.0f;
  lastReferenceVoltage = 0.0f;

  preferences.putFloat("vfactor", voltageFactor);
  preferences.putFloat("vref", lastReferenceVoltage);

  Serial.println("Calibration reset to 1.00000000");

  server.send(200, "application/json",
              "{\"message\":\"Calibration reset to 1.000000.\"}");
}

// ---------------- SETUP ----------------

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("ESP32 INA219 Calibration Portal");

  // Load saved calibration.
  preferences.begin("ina219cal", false);

  voltageFactor = preferences.getFloat("vfactor", 1.0f);
  lastReferenceVoltage = preferences.getFloat("vref", 0.0f);

  if (!isfinite(voltageFactor) ||
      voltageFactor < 0.80f ||
      voltageFactor > 1.20f) {
    voltageFactor = 1.0f;
  }

  // Initialize I2C and sensor.
  Wire.begin(INA219_SDA, INA219_SCL);
  Wire.setClock(100000);

  ina219Ready = ina219.begin();

  if (ina219Ready) {
    // Confirm the breakout's shunt rating before use.
    ina219.setCalibration_32V_2A();
    Serial.println("INA219 detected.");
  } else {
    Serial.println("WARNING: INA219 not detected.");
  }

  // Start local Wi-Fi access point.
  WiFi.mode(WIFI_AP);
  bool apStarted = WiFi.softAP(AP_SSID, AP_PASSWORD);

  if (apStarted) {
    Serial.printf("Wi-Fi SSID: %s\n", AP_SSID);
    Serial.printf("Wi-Fi password: %s\n", AP_PASSWORD);
    Serial.print("Dashboard: http://");
    Serial.println(WiFi.softAPIP());
  } else {
    Serial.println("ERROR: Wi-Fi AP failed to start.");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api", HTTP_GET, handleAPI);
  server.on("/calibrate", HTTP_POST, handleCalibrate);
  server.on("/reset", HTTP_POST, handleReset);

  server.begin();

  readINA219();

  Serial.printf("Saved factor: %.8f\n", voltageFactor);
  Serial.printf("Raw voltage: %.6f V\n", lastRawVoltage);

  if (lastReferenceVoltage > 0.0f) {
    printCalibration();
  }
}

// ---------------- LOOP ----------------

void loop() {
  server.handleClient();

  if (millis() - lastReadMs >= READ_INTERVAL_MS) {
    lastReadMs = millis();
    readINA219();
  }
}
