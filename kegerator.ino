#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <HX711_ADC.h>

/*
   Keg scale
   ---------
   The reported weight is the weight of the BEER, not of the keg.

   That needs two separate things, done in this order:

     1. Calibration factor - how many raw HX711 counts one kg produces.
        Measured once by putting the empty keg (known mass) on the scale
        while the scale is zeroed with nothing on it.

     2. Tare offset - the raw reading of the empty keg, so that the empty
        keg itself reads 0 kg and only the contents are measured.

   Both values are stored in NVS (Preferences), so they survive a reboot.
   Do NOT tare automatically at boot: if the device restarts with a full
   keg on the platform, that would silently become "0 kg".

   Serial commands (115200 baud):
     c - calibrate (asks you to empty the scale, then place the empty keg)
     z - re-zero: treat whatever is on the scale right now as 0 kg
     i - print current calibration and reading
     x - erase stored calibration
*/

// ---------- Hardware ----------
const uint8_t HX711_DOUT_PIN = 3;
const uint8_t HX711_SCK_PIN  = 4;

// ---------- Keg / scale configuration ----------
// Known mass of the empty keg. This is the calibration reference.
const float EMPTY_KEG_KG = 12.3f;

// Readings below this (and negative readings) are reported as 0 kg.
const float ZERO_DEADBAND_KG = 0.05f;

// Only used until the first calibration has been done.
const float DEFAULT_CAL_FACTOR = 726685.375f;

const unsigned long REPORT_INTERVAL_MS = 5000;
// The library averages 18 samples. At the HX711 default of 10 SPS,
// 3000 ms refills the whole moving average window.
const unsigned long SETTLE_MS = 3000;

const char* NR_API_URL = "https://metric-api.newrelic.com/metric/v1";
const char* NR_LICENSE_KEY = "KEY HERE";
const char* NTP_URL = "pool.ntp.org";
const char* WIFI_SSID = "SSID HERE";
const char* WIFI_PASSWORD = "PASS HERE";

HX711_ADC LoadCell(HX711_DOUT_PIN, HX711_SCK_PIN);
Preferences prefs;

float calFactor = DEFAULT_CAL_FACTOR;  // raw counts per kg
long tareOffset = 0;                   // raw counts of the empty keg
bool scaleCalibrated = false;
bool timeSynced = false;
unsigned long report_t = 0;

unsigned long get_current_timestamp() {
  time_t now;
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time from NTP");
    return 0;
  }
  time(&now);
  return now;
}

// Let the moving average fill up with the load that is on the scale right now.
// (Deliberately not using refreshDataSet() from the library; this is simpler
// and cannot block forever.)
void settle(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    LoadCell.update();
    delay(5);
  }
}

bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return true;
  }

  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi connection failed");
    return false;
  }
  Serial.println("Connected to WiFi!");
  return true;
}

bool ensureTime() {
  if (timeSynced) {
    return true;
  }

  Serial.print("Synchronising time via NTP");
  configTime(0, 0, NTP_URL);
  struct tm timeinfo;
  unsigned long start = millis();
  while (!getLocalTime(&timeinfo, 500) && millis() - start < 10000) {
    Serial.print(".");
  }
  Serial.println();

  if (!getLocalTime(&timeinfo, 500)) {
    Serial.println("NTP sync failed");
    return false;
  }
  timeSynced = true;
  Serial.println("Time synchronised successfully!");
  return true;
}

// ---------- Persistent calibration ----------

void loadScaleSettings() {
  prefs.begin("kegerator", true);
  if (prefs.isKey("calFactor") && prefs.isKey("tareOffset")) {
    calFactor = prefs.getFloat("calFactor", DEFAULT_CAL_FACTOR);
    tareOffset = prefs.getLong("tareOffset", 0);
    scaleCalibrated = true;
  }
  prefs.end();
}

void saveScaleSettings() {
  prefs.begin("kegerator", false);
  prefs.putFloat("calFactor", calFactor);
  prefs.putLong("tareOffset", tareOffset);
  prefs.end();
}

void printScaleStatus() {
  Serial.printf("Calibration: %s\n", scaleCalibrated ? "stored" : "NOT SET (send 'c' to calibrate)");
  Serial.printf("  calFactor : %.3f counts/kg\n", calFactor);
  Serial.printf("  tareOffset: %ld counts (empty keg)\n", tareOffset);
  Serial.printf("  reading   : %.3f kg (liquid)\n", LoadCell.getData());
}

void printHelp() {
  Serial.println("Commands: c=calibrate, z=re-zero current load, i=info, x=erase calibration");
}

// Block until the user presses Enter, keeping the scale updated meanwhile.
void waitForEnter(const char* message) {
  Serial.println(message);
  Serial.flush();
  while (Serial.available()) Serial.read();  // drop stale input
  while (!Serial.available()) {
    LoadCell.update();
    delay(10);
  }
  while (Serial.available()) Serial.read();  // drop the newline
  Serial.println();
}

// Calibrate using the empty keg as the known mass, then zero the empty keg.
void runCalibration() {
  Serial.println();
  Serial.println("=== Scale calibration ===");
  Serial.printf("Known empty-keg mass: %.2f kg\n", EMPTY_KEG_KG);
  Serial.println("The scale must be stable and on a level surface.");

  float oldCalFactor = calFactor;
  long oldTareOffset = tareOffset;

  // Step 1: zero with NOTHING on the scale. The calibration slope is
  // (counts with keg - counts at zero) / kg, so a true zero is required.
  waitForEnter("1) Remove everything from the scale, then press Enter.");
  LoadCell.tare();  // blocking
  if (LoadCell.getTareTimeoutFlag()) {
    Serial.println("ERROR: tare timed out. Check the HX711 wiring.");
    LoadCell.setCalFactor(oldCalFactor);
    LoadCell.setTareOffset(oldTareOffset);
    return;
  }
  Serial.printf("   Zero offset: %ld counts\n", LoadCell.getTareOffset());

  // Step 2: derive the calibration factor from the known empty keg.
  waitForEnter("2) Place the EMPTY keg on the scale, then press Enter.");
  settle(SETTLE_MS);
  float newCalFactor = LoadCell.getNewCalibration(EMPTY_KEG_KG);

  if (newCalFactor <= 0) {
    Serial.println("ERROR: negative calibration factor - the load cell is connected in reverse.");
    Serial.println("       Uncomment LoadCell.setReverseOutput() in setup() and recalibrate.");
    LoadCell.setCalFactor(oldCalFactor);
    LoadCell.setTareOffset(oldTareOffset);
    return;
  }

  Serial.printf("   New calibration factor: %.3f counts/kg\n", newCalFactor);
  Serial.printf("   Check: empty keg reads %.2f kg (expected %.2f kg)\n",
                LoadCell.getData(), EMPTY_KEG_KG);

  // Keep step 2's result, then zero the empty keg that is still on the scale.
  calFactor = newCalFactor;

  // Step 3: tare the empty keg so only the contents are measured.
  Serial.println("3) Zeroing the empty keg (leave it on the scale)...");
  LoadCell.tare();  // blocking
  if (LoadCell.getTareTimeoutFlag()) {
    Serial.println("ERROR: tare timed out. Check the HX711 wiring.");
    return;
  }

  tareOffset = LoadCell.getTareOffset();
  scaleCalibrated = true;
  saveScaleSettings();

  Serial.printf("   Empty-keg offset: %ld counts\n", tareOffset);
  Serial.printf("   Empty keg now reads: %.3f kg\n", LoadCell.getData());
  Serial.println("Calibration saved. A full keg will now show only the liquid weight.");
  Serial.println("=========================");
  Serial.println();
}

// Treat whatever is on the scale right now as 0 kg.
void zeroWithCurrentLoad() {
  if (!scaleCalibrated) {
    Serial.println("Not calibrated yet - send 'c' first.");
    return;
  }

  Serial.println("Zeroing with the current load: this load now reads 0 kg.");
  LoadCell.tare();  // blocking
  if (LoadCell.getTareTimeoutFlag()) {
    Serial.println("ERROR: tare timed out. Check the HX711 wiring.");
    return;
  }

  tareOffset = LoadCell.getTareOffset();
  saveScaleSettings();
  printScaleStatus();
}

void eraseCalibration() {
  prefs.begin("kegerator", false);
  prefs.clear();
  prefs.end();

  scaleCalibrated = false;
  calFactor = DEFAULT_CAL_FACTOR;
  tareOffset = 0;
  LoadCell.setCalFactor(calFactor);
  LoadCell.setTareOffset(tareOffset);
  Serial.println("Stored calibration erased.");
}

void handleSerialInput() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == 'c') {
      runCalibration();
    } else if (c == 'z') {
      zeroWithCurrentLoad();
    } else if (c == 'i') {
      printScaleStatus();
      printHelp();
    } else if (c == 'x') {
      eraseCalibration();
    }
  }
}

// ---------- Reporting ----------

void publishWeight(float weightKg) {
  if (!ensureWiFi()) {
    return;
  }
  if (!ensureTime()) {
    return;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.begin(client, NR_API_URL);

  http.addHeader("Content-Type", "application/json");
  http.addHeader("Api-Key", NR_LICENSE_KEY);

  unsigned long current_time = get_current_timestamp();

  JsonDocument doc;
  JsonArray rootArray = doc.to<JsonArray>();

  JsonObject firstElement = rootArray.add<JsonObject>();
  JsonArray metricsArray = firstElement.createNestedArray("metrics");

  JsonObject metricObject = metricsArray.add<JsonObject>();
  metricObject["name"] = "kegeratortesting.weight";
  metricObject["type"] = "gauge";
  metricObject["value"] = weightKg;
  metricObject["timestamp"] = current_time;

  String requestBody;
  serializeJson(rootArray, requestBody);

  Serial.println("Sending metric to NR");
  int httpResponseCode = http.POST(requestBody);

  // Debug print of the response status and headers
  Serial.println("Response:");
  if (httpResponseCode > 0) {
    Serial.printf("HTTP Status Code: %d\n", httpResponseCode);
    String responseBody = http.getString();
    Serial.println("Response Payload:");
    Serial.println(responseBody);
  } else {
    Serial.printf("Error on sending POST request: %s\n", http.errorToString(httpResponseCode).c_str());
  }

  http.end();
}

void setup()
{
  Serial.begin(115200);

  Serial.println();
  Serial.println();
  Serial.println();

  // Give background processes a few seconds to complete
  for (uint8_t t = 4; t > 0; t--)
  {
    Serial.printf("WAIT %d...\n", t);
    Serial.flush();
    delay(1000);
  }

  // Set up the HX711 before touching the network, so the scale works offline.
  LoadCell.begin();
  // dotare = false: never make whatever is on the scale at boot the zero point.
  LoadCell.start(2000, false);
  if (LoadCell.getSignalTimeoutFlag() || LoadCell.getTareTimeoutFlag()) {
    Serial.println("WARNING: no valid data from the HX711 - check wiring and pin designations.");
  }

  loadScaleSettings();
  LoadCell.setCalFactor(calFactor);
  LoadCell.setTareOffset(tareOffset);
  LoadCell.setReverseOutput();
  settle(SETTLE_MS);

  printScaleStatus();
  if (!scaleCalibrated) {
    Serial.println("Calibrate once with the empty keg: send 'c'.");
  }
  printHelp();

  ensureWiFi();
  ensureTime();

  Serial.println("Startup complete!");
}

void loop()
{
  LoadCell.update();
  handleSerialInput();

  if (millis() - report_t >= REPORT_INTERVAL_MS)
  {
    report_t = millis();

    float weight = LoadCell.getData();
    if (weight < ZERO_DEADBAND_KG) {
      weight = 0;  // empty keg or slight zero drift
    }

    Serial.printf("Weight value: %f\n", weight);

    if (!scaleCalibrated) {
      Serial.println("Scale not calibrated - not publishing. Send 'c'.");
      return;
    }

    publishWeight(weight);
  }
}
