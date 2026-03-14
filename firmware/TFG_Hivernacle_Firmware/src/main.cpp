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

const char* FIRMWARE_VERSION = "0.1.1";

// --- CONFIGURACIÓ NÚVOL ---
const char* DEVICE_ID = "H_POL";
const char* API_TOKEN = "uySJYyqvyiLaoKEXDudoplp8tPsv1HVXT3W8U5mXHFz1iEDYw5IfHQV0Whr4XWCS"; 
const char* serverUrl = "https://hivernacle-api.polgussi23.workers.dev/api/upload";
const char* updateCheckUrl = "https://hivernacle-api.polgussi23.workers.dev/api/check-update";

// --- CONFIGURACIÓ ACTUADORS ---
const int PIN_PUMP_LED = 26; // Pin bomba aigua
const int PIN_GROW_LED = 27; // Pin Llum
const int PIN_FAN = 25; // Pin ventiladors

// --- CONFIGURACIÓ SENSORS ---
const int PIN_SOIL = 34;
const int AIR_VALUE = 3500;
const int WATER_VALUE = 1200;

struct Config {
  String mode = "AUTO"; // "AUTO" o "MANUAL"
  
  // Objectius AUTO (Valors per defecte)
  float target_t_max = 30.0;
  float target_t_min = 15.0;
  int target_soil_min = 30;
  int hour_on = 8;
  int hour_off = 20;

  // Ordres MANUALS
  bool man_fan = false;
  bool man_pump = false;
  bool man_light = false;
} config;

// --- SERVIDOR HORARI ---
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600;
const int   daylightOffset_sec = 3600;

// --- OBJECTES ---
WiFiManager wifiManager;
Adafruit_BME280 bme;
Adafruit_VEML7700 veml = Adafruit_VEML7700();

// --- VARIABLES GLOBALS ---
bool bmeFound = false;
bool vemlFound = false;
float currentTemp = 0;
float currentHum = 0;
int currentSoilPct = 0;
float currentLux = 0;

// Temporitzador per no enviar dades massa sovint
unsigned long lastCloudSend = 0;
const long CLOUD_INTERVAL = 60000; // Enviar cada 15 segons (per proves) / Producció podria ser cada 60 segons? 

// Funció per a extreure l'hora d'un string
int parseHour(String timeStr){
  if(timeStr.length() >= 2){
    return timeStr.substring(0,2).toInt();
  }
  return 8;
}

// Funció per a comprovar versió de firmware i actualitzar si cal
void checkFirmwareUpdate() {
  if(WiFi.status() != WL_CONNECTED) return;
  
  Serial.println("Buscant actualitzacions de firmware...");

  HTTPClient http;
  http.begin(updateCheckUrl);
  http.addHeader("Content-Type", "application/json");
  
  // Enviem la versió actual al servidor
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
    String newVersion = responseDoc["new_version"].as<String>();
    String binUrl = responseDoc["bin_url"].as<String>();

    if (updateAvailable) {
      Serial.println("Actualització trobada! Versió: " + newVersion);
      Serial.println("Descarregant des de: " + binUrl);
      
      // Iniciem l'actualització
      WiFiClientSecure client;
      client.setInsecure(); // Saltem validació SSL per simplicitat
      
      t_httpUpdate_return ret = httpUpdate.update(client, binUrl);

      switch (ret) {
        case HTTP_UPDATE_FAILED:
          Serial.printf("❌ Error Actualització: (%d): %s\n", httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
          break;
        case HTTP_UPDATE_NO_UPDATES:
          Serial.println("⚠️ No hi ha updates (o error de lògica)");
          break;
        case HTTP_UPDATE_OK:
          Serial.println("✅ Actualització OK! Reiniciant...");
          break;
      }
    } else {
      Serial.println("✅ Ja tens l'última versió.");
    }
  } else {
    Serial.printf("Error connectant al servidor d'updates: %d\n", httpCode);
  }
  http.end();
}

// Funció per enviar dades al Worker (API)
void sendDataToCloud() {
  if(WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.setTimeout(3600);

  http.begin(serverUrl);
  
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Auth-Token", API_TOKEN);

  // Es crea el paquet JSON amb les dades a enviar a la API
  JsonDocument doc;
  doc["device_id"] = DEVICE_ID;
  doc["temp"] = currentTemp;
  doc["hum"]  = currentHum;
  doc["light"] = currentLux;
  doc["soil"]  = currentSoilPct;

  String jsonString;
  serializeJson(doc, jsonString);

  Serial.print("☁️ Enviant dades al núvol... ");
  int httpResponseCode = http.POST(jsonString);

  if (httpResponseCode == 200) {
    String response = http.getString();
    Serial.println("OK! Actualitzant config...");
    
    JsonDocument docIn;
    DeserializationError error = deserializeJson(docIn, response);

    if(!error){
      // Guardem la nova configuració a la struct "Config"
      config.mode = docIn["mode"].as<String>();
      
      // Configuracions manuals
      config.man_fan = docIn["manual"]["fan"];
      config.man_pump = docIn["manual"]["pump"];
      config.man_light = docIn["manual"]["light"];

      // Configuracions Auto
      config.target_t_max = docIn["auto"]["t_max"];
      config.target_t_min = docIn["auto"]["t_min"];
      config.target_soil_min = docIn["auto"]["soil_min"];

      config.hour_on = parseHour(docIn["auto"]["light_on"].as<String>());
      config.hour_off = parseHour(docIn["auto"]["light_off"].as<String>());

      Serial.println("    -> Mode: " + config.mode + " | Planta Max Temp: " + String(config.target_t_max));
    } else{
      Serial.print("ERROR JSON: "); Serial.println(error.c_str());
    }
  } else {
    Serial.print("Error enviant: "); Serial.println(httpResponseCode);
  }
  
  http.end();
}

// Funció per a la lògica de control entre sensors i actuadors
void runAutoControl() {
  struct tm timeinfo;
  bool timeSynced = getLocalTime(&timeinfo);
  int currentHour = timeSynced ? timeinfo.tm_hour : 12;

  Serial.println("LÒGICA (" + config.mode + "): ");

  // Cas "MANUAL": L'usuari està manant des de la web
  if(config.mode == "MANUAL"){
    digitalWrite(PIN_FAN, config.man_fan ? HIGH : LOW);
    digitalWrite(PIN_PUMP_LED, config.man_pump ? HIGH : LOW);
    digitalWrite(PIN_GROW_LED, config.man_light ? HIGH : LOW);

    Serial.printf("Fan: %d | Pump: %d | Light: %d\n", config.man_fan, config.man_pump, config.man_light);
  }
  // Cas "AUTO": L'ESP32 decideix segons els objectius de la plantació 
  else{
    // Lògica Temperatura
    if(currentTemp > config.target_t_max){
      digitalWrite(PIN_FAN, HIGH);
      Serial.print("CALOR -> Fan ON | ");
    } else{
      digitalWrite(PIN_FAN, LOW);
      Serial.print("TEMP OK | ");
    }
    // Lògica Reg
    if (currentSoilPct < config.target_soil_min){
      digitalWrite(PIN_PUMP_LED, HIGH);
      Serial.print("SEC -> Regant | ");
    }
    else{
      digitalWrite(PIN_PUMP_LED, LOW);
      Serial.print("Humitat Sòl OK | ");
    }

    // Lògica Llum
    bool isDayTime = (currentHour >= config.hour_on && currentHour < config.hour_off);
    if (isDayTime && currentLux < 250){
      digitalWrite(PIN_GROW_LED, HIGH);
      Serial.print("FALTA LLUM -> Light ON");
    }
    else{
      digitalWrite(PIN_GROW_LED, LOW);
      Serial.print("Llum OK / Nit");
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  pinMode(PIN_PUMP_LED, OUTPUT);
  pinMode(PIN_GROW_LED, OUTPUT);
  pinMode(PIN_FAN, OUTPUT);

  digitalWrite(PIN_PUMP_LED, LOW);
  digitalWrite(PIN_GROW_LED, LOW);
  digitalWrite(PIN_FAN, LOW);
  
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

  // Configuració Watchdog
  Serial.println("Activant Watchdog...");
  esp_task_wdt_init(WDT_TIMEOUT, true); // true = reinicia
  esp_task_wdt_add(NULL); // Afegeix el fil actual (loop principal) al vigilant
}

void loop() {
  // 1. Reconnexió Wi-Fi
  if (WiFi.status() != WL_CONNECTED) {
      WiFi.disconnect(); WiFi.reconnect(); delay(5000); return;
  }
  
  // 2. LLEGIR DADES
  if (bmeFound) {
    currentTemp = bme.readTemperature();
    currentHum = bme.readHumidity();
  }
  if (vemlFound) currentLux = veml.readLux();
  
  int rawSoil = analogRead(PIN_SOIL);
  currentSoilPct = map(rawSoil, AIR_VALUE, WATER_VALUE, 0, 100);
  currentSoilPct = constrain(currentSoilPct, 0, 100);
  

  // 3. ENVIAR AL NÚVOL (Cada 60 segons)
  if (millis() - lastCloudSend > CLOUD_INTERVAL) {
    // Enviem
    sendDataToCloud();
    lastCloudSend = millis();

    checkFirmwareUpdate(); // En un futur es pot posar que es comprovi cada hora, no cada minut
  }

  // 4. EXECUTAR CONTROL INTEL·LIGENT
  runAutoControl();
  delay(200); // Petit retard per no saturar el processador
}