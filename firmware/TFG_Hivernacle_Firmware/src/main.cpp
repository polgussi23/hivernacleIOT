#include <Arduino.h>
#include <Wire.h>
#include <WiFiManager.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME280.h>
#include "Adafruit_VEML7700.h"
#include "time.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <HTTPUpdate.h>
#include <WiFiClientSecure.h>
#include <esp_task_wdt.h>

#define WDT_TIMEOUT 15

const char* FIRMWARE_VERSION = "0.1.6";

// --- CONFIGURACIÓ NÚVOL ---
const char* DEVICE_ID = "H_POL";
const char* API_TOKEN = "uySJYyqvyiLaoKEXDudoplp8tPsv1HVXT3W8U5mXHFz1iEDYw5IfHQV0Whr4XWCS"; 
const char* serverUrl = "https://hivernacle-api.polgussi23.workers.dev/api";
const char* updateCheckUrl = "https://hivernacle-api.polgussi23.workers.dev/api/check-update";

// --- CONFIGURACIÓ ACTUADORS ---
const int PIN_PUMP_LED = 25;
const int PIN_GROW_LED = 27;
const int PIN_FAN = 26;
const int PIN_HEATER = 33;

// --- CONFIGURACIÓ SENSORS ---
const int PIN_SOIL = 34;
const int AIR_VALUE = 3500;
const int WATER_VALUE = 1200;

struct Config {
  String mode = "AUTO";
  float target_t_max = 30.0;
  float target_t_min = 15.0;
  int target_soil_min = 30;
  int hour_on = 8;
  int hour_off = 20;
  bool man_fan = false;
  bool man_pump = false;
  bool man_light = false;
  bool man_heater = false;
} config;

// ── ESTAT ANTERIOR D'ACTUADORS (per detectar canvis i generar logs) ──────────
struct ActuatorState {
  bool fan    = false;
  bool pump   = false;
  bool light  = false;
  bool heater = false;
  String mode = "AUTO";
} prevState;
// ─────────────────────────────────────────────────────────────────────────────

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600;
const int   daylightOffset_sec = 3600;

WiFiManager wifiManager;
Adafruit_BME280 bme;
Adafruit_VEML7700 veml = Adafruit_VEML7700();

bool bmeFound = false;
bool vemlFound = false;
float currentTemp = 0;
float currentHum = 0;
int currentSoilPct = 0;
float currentLux = 0;

unsigned long lastCloudSend = 0;
unsigned long lastCloudReceived = 0;
const long SEND_DATA_TO_CLOUD    = 60000;
const long GET_CONFIG_FROM_CLOUD = 5000;

int parseHour(String timeStr) {
  if (timeStr.length() >= 2) return timeStr.substring(0, 2).toInt();
  return 8;
}

// ── NOU: Enviar un missatge de log al Worker ──────────────────────────────────
void sendLog(const String& missatge) {
  if (WiFi.status() != WL_CONNECTED) return;

  String logUrl = String(serverUrl) + "/log";
  HTTPClient http;
  http.begin(logUrl);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Auth-Token", API_TOKEN);

  JsonDocument doc;
  doc["device_id"] = DEVICE_ID;
  doc["missatge"]  = missatge;
  String body;
  serializeJson(doc, body);

  Serial.print("📝 Log: "); Serial.println(missatge);
  int code = http.POST(body);
  if (code != 200) Serial.printf("   ⚠️ Error enviant log: %d\n", code);
  http.end();
}
// ─────────────────────────────────────────────────────────────────────────────

void checkFirmwareUpdate() {
  if (WiFi.status() != WL_CONNECTED) return;
  Serial.println("Buscant actualitzacions de firmware...");

  HTTPClient http;
  http.begin(updateCheckUrl);
  http.addHeader("Content-Type", "application/json");
  
  JsonDocument doc;
  doc["device_id"] = DEVICE_ID;
  doc["current_version"] = FIRMWARE_VERSION;
  String jsonString;
  serializeJson(doc, jsonString);
  
  int httpCode = http.POST(jsonString);
  if (httpCode == 200) {
    String payload = http.getString();
    JsonDocument responseDoc;
    deserializeJson(responseDoc, payload);

    bool updateAvailable = responseDoc["update_available"];
    if (updateAvailable) {
      String newVersion = responseDoc["new_version"].as<String>();
      String binUrl     = responseDoc["bin_url"].as<String>();
      Serial.println("Actualització trobada! Versió: " + newVersion);

      WiFiClientSecure client;
      client.setInsecure();
      httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

      esp_task_wdt_delete(NULL);
      t_httpUpdate_return ret = httpUpdate.update(client, binUrl);
      esp_task_wdt_add(NULL);

      switch (ret) {
        case HTTP_UPDATE_FAILED:
          Serial.printf("❌ Error: (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
          break;
        case HTTP_UPDATE_OK:
          Serial.println("✅ Actualització OK! Reiniciant...");
          break;
        default: break;
      }
    } else {
      Serial.println("✅ Ja tens l'última versió.");
    }
  }
  http.end();
}

void sendDataToCloud() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(String(serverUrl) + "/upload");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Auth-Token", API_TOKEN);

  JsonDocument doc;
  doc["device_id"] = DEVICE_ID;
  doc["temp"]  = currentTemp;
  doc["hum"]   = currentHum;
  doc["light"] = currentLux;
  doc["soil"]  = currentSoilPct;

  String jsonString;
  serializeJson(doc, jsonString);

  Serial.print("☁️ Enviant dades (Sensors)... ");
  int code = http.POST(jsonString);
  Serial.println(code == 200 || code == 201 ? "OK!" : "Error: " + String(code));
  http.end();
}

void getConfigFromCloud() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String fetchUrl = String(serverUrl) + "/config?device_id=" + String(DEVICE_ID) + "&nocache=" + String(millis());
  http.begin(fetchUrl);
  http.addHeader("X-Auth-Token", API_TOKEN);

  Serial.print("📥 Comprovant ordres... ");
  int httpCode = http.GET();

  if (httpCode == 200) {
    String response = http.getString();
    JsonDocument docIn;
    DeserializationError error = deserializeJson(docIn, response);

    if (!error) {
      // ── Detectar canvi de mode ────────────────────────────────────────
      String nouMode = docIn["mode"].as<String>();
      if (nouMode != prevState.mode) {
        if (nouMode == "MANUAL") sendLog("🎛️ Activat mode manual");
        else                     sendLog("⚡ Activat mode automàtic");
        prevState.mode = nouMode;
      }

      config.mode        = nouMode;
      config.man_fan     = docIn["manual"]["fan"];
      config.man_pump    = docIn["manual"]["pump"];
      config.man_light   = docIn["manual"]["light"];
      config.man_heater  = docIn["manual"]["heater"];
      config.target_t_max    = docIn["auto"]["t_max"];
      config.target_t_min    = docIn["auto"]["t_min"];
      config.target_soil_min = docIn["auto"]["soil_min"];
      config.hour_on  = parseHour(docIn["auto"]["light_on"].as<String>());
      config.hour_off = parseHour(docIn["auto"]["light_off"].as<String>());

      // ── Detectar canvis d'actuadors manuals ──────────────────────────
      if (config.mode == "MANUAL") {
        if (config.man_fan    != prevState.fan)    sendLog(String("💨 Ventilador ") + (config.man_fan    ? "activat" : "desactivat") + " (mode manual)");
        if (config.man_pump   != prevState.pump)   sendLog(String("💧 Bomba ")      + (config.man_pump   ? "activada" : "desactivada") + " (mode manual)");
        if (config.man_light  != prevState.light)  sendLog(String("💡 Llums ")      + (config.man_light  ? "activades" : "desactivades") + " (mode manual)");
        if (config.man_heater != prevState.heater) sendLog(String("🔥 Calefacció ") + (config.man_heater ? "activada" : "desactivada") + " (mode manual)");
      }

      // Guardem l'estat actual com a "anterior" per al proper cicle
      prevState.fan    = config.man_fan;
      prevState.pump   = config.man_pump;
      prevState.light  = config.man_light;
      prevState.heater = config.man_heater;
      // ─────────────────────────────────────────────────────────────────

      Serial.println("OK! Mode: " + config.mode);
    } else {
      Serial.print("ERROR JSON: "); Serial.println(error.c_str());
    }
  } else {
    Serial.print("Error rebent: "); Serial.println(httpCode);
  }
  http.end();
}

void runAutoControl() {
  struct tm timeinfo;
  bool timeSynced = getLocalTime(&timeinfo);
  int currentHour = timeSynced ? timeinfo.tm_hour : 12;

  if (config.mode == "MANUAL") {
    digitalWrite(PIN_FAN,      config.man_fan    ? HIGH : LOW);
    digitalWrite(PIN_PUMP_LED, config.man_pump   ? HIGH : LOW);
    digitalWrite(PIN_GROW_LED, config.man_light  ? HIGH : LOW);
    digitalWrite(PIN_HEATER,   config.man_heater ? HIGH : LOW);
  } else {
    // ── Control temperatura ───────────────────────────────────────────
    bool newFan    = false;
    bool newHeater = false;

    if (currentTemp > config.target_t_max) {
      newFan = true; newHeater = false;
    } else if (currentTemp < config.target_t_min) {
      newFan = false; newHeater = true;
    }

    // ── Log si canvia l'estat del ventilador en AUTO ──────────────────
    bool curFanPin    = digitalRead(PIN_FAN)    == HIGH;
    bool curHeaterPin = digitalRead(PIN_HEATER) == HIGH;
    bool curPumpPin   = digitalRead(PIN_PUMP_LED) == HIGH;
    bool curLightPin  = digitalRead(PIN_GROW_LED) == HIGH;

    if (newFan != curFanPin) {
      sendLog(newFan
        ? "💨 Temperatura alta (" + String(currentTemp, 1) + "°C). Activant ventilador"
        : "💨 Temperatura OK. Aturant ventilador");
    }
    if (newHeater != curHeaterPin) {
      sendLog(newHeater
        ? "🔥 Temperatura baixa (" + String(currentTemp, 1) + "°C). Activant calefacció"
        : "🔥 Temperatura OK. Aturant calefacció");
    }

    digitalWrite(PIN_FAN,    newFan    ? HIGH : LOW);
    digitalWrite(PIN_HEATER, newHeater ? HIGH : LOW);

    // ── Control reg ───────────────────────────────────────────────────
    bool newPump = (currentSoilPct < config.target_soil_min);
    if (newPump != curPumpPin) {
      sendLog(newPump
        ? "💧 Sòl sec (" + String(currentSoilPct) + "%). Activant bomba"
        : "💧 Humitat sòl OK. Aturant bomba");
    }
    digitalWrite(PIN_PUMP_LED, newPump ? HIGH : LOW);

    // ── Control llum ──────────────────────────────────────────────────
    bool isDayTime = (currentHour >= config.hour_on && currentHour < config.hour_off);
    bool newLight  = (isDayTime && currentLux < 250);
    if (newLight != curLightPin) {
      sendLog(newLight
        ? "💡 Poca llum (" + String(currentLux, 0) + " lx). Activant llums"
        : "💡 Llum correcte. Desactivant llums");
    }
    digitalWrite(PIN_GROW_LED, newLight ? HIGH : LOW);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(PIN_PUMP_LED, OUTPUT); pinMode(PIN_GROW_LED, OUTPUT);
  pinMode(PIN_FAN, OUTPUT);      pinMode(PIN_HEATER, OUTPUT);
  digitalWrite(PIN_PUMP_LED, LOW); digitalWrite(PIN_GROW_LED, LOW);
  digitalWrite(PIN_FAN, LOW);      digitalWrite(PIN_HEATER, LOW);
  pinMode(PIN_SOIL, INPUT);

  wifiManager.autoConnect("HIVERNACLE-SETUP");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Wire.begin();
  
  if (bme.begin(0x76) || bme.begin(0x77)) bmeFound = true;
  if (veml.begin()) {
    vemlFound = true;
    veml.setGain(VEML7700_GAIN_1);
    veml.setIntegrationTime(VEML7700_IT_800MS);
  }

  Serial.println("✅ SISTEMA CONNECTAT I LLEST");

  esp_task_wdt_init(WDT_TIMEOUT, true);
  esp_task_wdt_add(NULL);
}

void loop() {
  esp_task_wdt_reset();

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(); WiFi.reconnect(); delay(5000); return;
  }

  if (bmeFound) { currentTemp = bme.readTemperature(); currentHum = bme.readHumidity(); }
  if (vemlFound) currentLux = veml.readLux();

  int rawSoil = analogRead(PIN_SOIL);
  currentSoilPct = constrain(map(rawSoil, AIR_VALUE, WATER_VALUE, 0, 100), 0, 100);

  if (millis() - lastCloudReceived > GET_CONFIG_FROM_CLOUD) {
    getConfigFromCloud();
    lastCloudReceived = millis();

    if (millis() - lastCloudSend > SEND_DATA_TO_CLOUD) {
      sendDataToCloud();
      lastCloudSend = millis();
      checkFirmwareUpdate();
    }
  }

  runAutoControl();
  delay(200);
}