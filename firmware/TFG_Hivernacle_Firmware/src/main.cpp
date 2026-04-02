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

const char* FIRMWARE_VERSION = "0.1.9";

// --- CONFIGURACIÓ NÚVOL ---
const char* DEVICE_ID = "H_POL";
const char* API_TOKEN = "uySJYyqvyiLaoKEXDudoplp8tPsv1HVXT3W8U5mXHFz1iEDYw5IfHQV0Whr4XWCS"; 
const char* serverUrl = "https://hivernacle-api.polgussi23.workers.dev/api";
const char* updateCheckUrl = "https://hivernacle-api.polgussi23.workers.dev/api/check-update";

// --- PINS ACTUADORS ---
const int PIN_PUMP_LED = 25;
const int PIN_GROW_LED = 27;
const int PIN_FAN      = 26;
const int PIN_HEATER   = 33;

// --- CANALS PWM (LEDC ESP32) ---
// Canals 0 i 1 per al PID de temperatura.
// Bomba i llums segueixen amb digitalWrite (on/off).
const int PWM_CH_FAN    = 0;
const int PWM_CH_HEATER = 1;
const int PWM_FREQ      = 5000;  // 5 kHz
const int PWM_BITS      = 8;     // resolució 0-255

// --- SENSORS ---
const int PIN_SOIL    = 34;
const int AIR_VALUE   = 3500;
const int WATER_VALUE = 1200;

// ── STRUCT PID ────────────────────────────────────────────────────────────────
struct PIDController {
  // Kp: quanta força apliquem per cada grau d'error
  // Ki: corregeix errors persistents petits (steady-state error)
  // Kd: frena la resposta si l'error canvia ràpid (evita overshoot)
  // ─── Sintonitza aquests valors amb el teu hivernacle real ───
  float Kp = 20.0f;
  float Ki =  0.08f;
  float Kd =  5.0f;

  float integral   = 0.0f;
  float prev_error = 0.0f;
  unsigned long last_time = 0;

  const float OUT_MIN = -255.0f;  // -255 = màxim fred (ventilador ple)
  const float OUT_MAX =  255.0f;  // +255 = màxim calor (calefactor ple)

  // Reset complet: cridar quan canvia el mode o el setpoint
  void reset() {
    integral   = 0.0f;
    prev_error = 0.0f;
    last_time  = 0;
  }

  // Retorna un valor entre -255 i +255:
  //   > 0 → calor → calefactor
  //   < 0 → fred  → ventilador
  //   = 0 → temperatura correcta, tot apagat
  float compute(float setpoint, float current) {
    unsigned long now = millis();

    // Primer cicle: inicialitzem sense calcular
    if (last_time == 0) {
      last_time  = now;
      prev_error = setpoint - current;
      return 0.0f;
    }

    float dt = (now - last_time) / 1000.0f;
    last_time = now;

    // Descartem dt anormals (p.ex. rearranques del watchdog)
    if (dt <= 0.0f || dt > 30.0f) return 0.0f;

    float error      = setpoint - current;
    float derivative = (error - prev_error) / dt;

    // Anti-windup: acumulem integral NOMÉS si la sortida no és saturada
    float tentative = Kp * error + Ki * (integral + error * dt) + Kd * derivative;
    if (tentative > OUT_MIN && tentative < OUT_MAX) {
      integral += error * dt;
    }

    prev_error = error;

    return constrain(Kp * error + Ki * integral + Kd * derivative, OUT_MIN, OUT_MAX);
  }
} pid;
// ─────────────────────────────────────────────────────────────────────────────

struct Config {
  String mode            = "AUTO";
  float  target_t_max    = 30.0;
  float  target_t_min    = 15.0;
  int    target_soil_min = 30;
  int    hour_on         = 8;
  int    hour_off        = 20;
  bool   man_fan    = false;
  bool   man_pump   = false;
  bool   man_light  = false;
  bool   man_heater = false;
} config;

struct ActuatorState {
  bool   fan    = false;
  bool   pump   = false;
  bool   light  = false;
  bool   heater = false;
  String mode   = "AUTO";
} prevState;

const char* ntpServer          = "pool.ntp.org";
const long  gmtOffset_sec      = 3600;
const int   daylightOffset_sec = 3600;

WiFiManager       wifiManager;
Adafruit_BME280   bme;
Adafruit_VEML7700 veml = Adafruit_VEML7700();

bool  bmeFound  = false;
bool  vemlFound = false;
float currentTemp    = 0;
float currentHum     = 0;
int   currentSoilPct = 0;
float currentLux     = 0;

unsigned long lastCloudSend     = 0;
unsigned long lastCloudReceived = 0;
unsigned long lastPidRun        = 0;

const long SEND_DATA_TO_CLOUD    = 60000;
const long GET_CONFIG_FROM_CLOUD = 5000;
const long PID_INTERVAL          = 10000;  // PID cada 10 s

// ── TEMPORITZADORS DE REG ────────────────────────────────────────────────────
// AUTO:   5 s regant, 2 min d'espera perquè l'aigua arribi fins al sensor
// MANUAL: pols únic de 5 s, s'apaga sol
const long WATERING_DURATION        = 5000;
const long WATERING_PAUSE           = 120000;
const long MANUAL_WATERING_DURATION = 5000;
bool          wateringActive    = false;
unsigned long wateringStart     = 0;
unsigned long lastWateringEnd   = 0;
bool          manualPulseActive = false;

// ── HELPERS ──────────────────────────────────────────────────────────────────
int parseHour(String timeStr) {
  return (timeStr.length() >= 2) ? timeStr.substring(0, 2).toInt() : 8;
}

void sendLog(const String& missatge) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(String(serverUrl) + "/log");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Auth-Token", API_TOKEN);
  JsonDocument doc;
  doc["device_id"] = DEVICE_ID;
  doc["missatge"]  = missatge;
  String body; serializeJson(doc, body);
  Serial.print("📝 Log: "); Serial.println(missatge);
  http.POST(body);
  http.end();
}

// Notifica al Worker que apagui manual_pump = 0 a la BBDD.
// Aplica PWM al ventilador i al calefactor. Garanteix que mai estan els dos encesos.
// La web ho veurà al proper refresc i desactivarà el botó automàticament.
void resetPumpCloud() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(String(serverUrl) + "/actuator-reset");
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Auth-Token", API_TOKEN);
  JsonDocument doc;
  doc["device_id"] = DEVICE_ID;
  doc["actuator"]  = "pump";
  String body; serializeJson(doc, body);
  http.POST(body);
  http.end();
}

// Aplica PWM al ventilador i al calefactor. Garanteix que mai estan els dos encesos.
void setPWM(int fan_pwm, int heater_pwm) {
  ledcWrite(PWM_CH_FAN,    constrain(fan_pwm,    0, 255));
  ledcWrite(PWM_CH_HEATER, constrain(heater_pwm, 0, 255));
}
// ─────────────────────────────────────────────────────────────────────────────

void checkFirmwareUpdate() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(updateCheckUrl);
  http.addHeader("Content-Type", "application/json");
  JsonDocument doc;
  doc["device_id"]       = DEVICE_ID;
  doc["current_version"] = FIRMWARE_VERSION;
  String body; serializeJson(doc, body);
  if (http.POST(body) == 200) {
    JsonDocument resp;
    deserializeJson(resp, http.getString());
    if (resp["update_available"]) {
      WiFiClientSecure client; client.setInsecure();
      httpUpdate.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
      esp_task_wdt_delete(NULL);
      httpUpdate.update(client, resp["bin_url"].as<String>());
      esp_task_wdt_add(NULL);
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
  String body; serializeJson(doc, body);
  Serial.print("☁️ Enviant dades... ");
  Serial.println(http.POST(body) == 200 ? "OK" : "Error");
  http.end();
}

void getConfigFromCloud() {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.begin(String(serverUrl) + "/config?device_id=" + String(DEVICE_ID) + "&nocache=" + String(millis()));
  http.addHeader("X-Auth-Token", API_TOKEN);

  // ── Acumulem logs aquí en lloc de cridar sendLog() amb http obert ──────────
  String logMode    = "";
  String logFan     = "";
  String logPump    = "";
  String logLight   = "";
  String logHeater  = "";
  // ─────────────────────────────────────────────────────────────────────────

  if (http.GET() == 200) {
    JsonDocument docIn;
    if (!deserializeJson(docIn, http.getString())) {
      String nouMode = docIn["mode"].as<String>();

      if (nouMode != prevState.mode) {
        pid.reset();
        setPWM(0, 0);
        // ← NO cridem sendLog() aquí, guardem el missatge
        logMode = (nouMode == "MANUAL") ? "🎛️ Activat mode manual" : "⚡ Activat mode automàtic";
        prevState.mode = nouMode;
      }

      config.mode            = nouMode;
      config.man_fan         = docIn["manual"]["fan"];
      config.man_pump        = docIn["manual"]["pump"];
      config.man_light       = docIn["manual"]["light"];
      config.man_heater      = docIn["manual"]["heater"];
      config.target_t_max    = docIn["auto"]["t_max"];
      config.target_t_min    = docIn["auto"]["t_min"];
      config.target_soil_min = docIn["auto"]["soil_min"];
      config.hour_on  = parseHour(docIn["auto"]["light_on"].as<String>());
      config.hour_off = parseHour(docIn["auto"]["light_off"].as<String>());

      if (config.mode == "MANUAL") {
        if (config.man_fan    != prevState.fan)
          logFan    = String("💨 Ventilador ")  + (config.man_fan    ? "activat"   : "desactivat")   + " (mode manual)";
        if (config.man_pump   != prevState.pump)
          logPump   = String("💧 Bomba ")       + (config.man_pump   ? "activada"  : "desactivada")  + " (mode manual)";
        if (config.man_light  != prevState.light)
          logLight  = String("💡 Llums ")       + (config.man_light  ? "activades" : "desactivades") + " (mode manual)";
        if (config.man_heater != prevState.heater)
          logHeater = String("🔥 Calefacció ")  + (config.man_heater ? "activada"  : "desactivada")  + " (mode manual)";
      }

      prevState.fan    = config.man_fan;
      prevState.pump   = config.man_pump;
      prevState.light  = config.man_light;
      prevState.heater = config.man_heater;
    }
  }

  // ── Tanquem la connexió PRIMER, enviem logs DESPRÉS ──────────────────────
  http.end();

  if (logMode.length()   > 0) sendLog(logMode);
  if (logFan.length()    > 0) sendLog(logFan);
  if (logPump.length()   > 0) sendLog(logPump);
  if (logLight.length()  > 0) sendLog(logLight);
  if (logHeater.length() > 0) sendLog(logHeater);
}

// ── CONTROL PRINCIPAL ─────────────────────────────────────────────────────────
void runControl() {
  struct tm timeinfo;
  int currentHour = getLocalTime(&timeinfo) ? timeinfo.tm_hour : 12;

  // ── MANUAL ────────────────────────────────────────────────────────────────
  if (config.mode == "MANUAL") {
    setPWM(config.man_fan ? 255 : 0, config.man_heater ? 255 : 0);
    digitalWrite(PIN_GROW_LED, config.man_light ? HIGH : LOW);

    // Pols de reg manual: quan man_pump passa a true, rega 5 s i s'apaga sol
    if (config.man_pump && !manualPulseActive) {
      manualPulseActive = true;
      wateringActive    = true;
      wateringStart     = millis();
      digitalWrite(PIN_PUMP_LED, HIGH);
      sendLog("💧 Reg manual iniciat (5 s)");
    }
    if (manualPulseActive && wateringActive) {
      if (millis() - wateringStart >= MANUAL_WATERING_DURATION) {
        wateringActive    = false;
        manualPulseActive = false;
        config.man_pump   = false;  // evita re-trigger fins que la web enviï un nou true
        digitalWrite(PIN_PUMP_LED, LOW);
        sendLog("💧 Reg manual acabat");
        resetPumpCloud();           // avisa al Worker → la web desactiva el botó
      }
    }
    return;
  }

  // ── AUTO: PID temperatura (cada PID_INTERVAL ms) ──────────────────────────
  if (millis() - lastPidRun >= PID_INTERVAL) {
    lastPidRun = millis();

    float setpoint = (config.target_t_min + config.target_t_max) / 2.0f;
    float output   = pid.compute(setpoint, currentTemp);

    Serial.printf("🌡 PID · SP=%.1f  T=%.1f  err=%.2f  out=%.1f\n",
                  setpoint, currentTemp, setpoint - currentTemp, output);

    if (output > 0) {
      // Calefactor proporcional
      int pwm = (int)output;
      setPWM(0, pwm);
      if (!prevState.heater) {
        sendLog("🔥 Temperatura baixa (" + String(currentTemp, 1) + "°C, SP " +
                String(setpoint, 1) + "°C). Calefacció al " + String(pwm * 100 / 255) + "%");
        prevState.heater = true;
        prevState.fan    = false;
      }
    } else if (output < -10) {
      // Ventilador proporcional (llindar -10 evita activar per errors mínims)
      int pwm = (int)(-output);
      setPWM(pwm, 0);
      if (!prevState.fan) {
        sendLog("💨 Temperatura alta (" + String(currentTemp, 1) + "°C, SP " +
                String(setpoint, 1) + "°C). Ventilador al " + String(pwm * 100 / 255) + "%");
        prevState.fan    = true;
        prevState.heater = false;
      }
    } else {
      // Zona de confort
      setPWM(0, 0);
      if (prevState.fan || prevState.heater) {
        sendLog("✅ Temperatura estabilitzada a " + String(currentTemp, 1) + "°C");
        prevState.fan    = false;
        prevState.heater = false;
      }
    }
  }

  // ── AUTO: Reg per polsos (5 s ON → 2 min pausa → repetir si cal) ─────────
  unsigned long now = millis();
 
  if (wateringActive) {
    // Bomba encesa: comprovem si ja han passat 5 s
    if (now - wateringStart >= WATERING_DURATION) {
      wateringActive  = false;
      lastWateringEnd = now;
      prevState.pump  = false;
      digitalWrite(PIN_PUMP_LED, LOW);
      sendLog("💧 Cicle de reg acabat. Esperant " + String(WATERING_PAUSE / 1000) + " s");
    }
  } else {
    bool soilDry    = (currentSoilPct < config.target_soil_min);
    bool pauseEnded = (lastWateringEnd == 0 || (now - lastWateringEnd >= WATERING_PAUSE));
 
    if (soilDry && pauseEnded) {
      // Sòl sec i pausa acabada: nou cicle de 5 s
      wateringActive = true;
      wateringStart  = now;
      prevState.pump = true;
      digitalWrite(PIN_PUMP_LED, HIGH);
      sendLog("💧 Sòl sec (" + String(currentSoilPct) + "%). Iniciant cicle de reg (5 s)");
    } else if (!soilDry && prevState.pump) {
      // Sòl ja humit: reiniciem el comptador de pausa
      lastWateringEnd = 0;
      prevState.pump  = false;
      sendLog("💧 Humitat sòl OK (" + String(currentSoilPct) + "%). Reg aturat");
    }
  }

  // ── AUTO: Llum (on/off) ───────────────────────────────────────────────────
  bool isDayTime = (currentHour >= config.hour_on && currentHour < config.hour_off);
  bool newLight  = (isDayTime && currentLux < 250);
  if (newLight != prevState.light) {
    sendLog(newLight
      ? "💡 Poca llum (" + String(currentLux, 0) + " lx). Activant llums"
      : "💡 Llum correcte. Desactivant llums");
    prevState.light = newLight;
  }
  digitalWrite(PIN_GROW_LED, newLight ? HIGH : LOW);
}
// ─────────────────────────────────────────────────────────────────────────────

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Bomba i llums: on/off
  pinMode(PIN_PUMP_LED, OUTPUT); pinMode(PIN_GROW_LED, OUTPUT);
  digitalWrite(PIN_PUMP_LED, LOW); digitalWrite(PIN_GROW_LED, LOW);

  // Ventilador i calefactor: PWM via LEDC
  ledcSetup(PWM_CH_FAN,    PWM_FREQ, PWM_BITS);
  ledcSetup(PWM_CH_HEATER, PWM_FREQ, PWM_BITS);
  ledcAttachPin(PIN_FAN,    PWM_CH_FAN);
  ledcAttachPin(PIN_HEATER, PWM_CH_HEATER);
  setPWM(0, 0);

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

  if (bmeFound)  { currentTemp = bme.readTemperature(); currentHum = bme.readHumidity(); }
  if (vemlFound)   currentLux  = veml.readLux();
  currentSoilPct = constrain(map(analogRead(PIN_SOIL), AIR_VALUE, WATER_VALUE, 0, 100), 0, 100);

  if (millis() - lastCloudReceived > GET_CONFIG_FROM_CLOUD) {
    getConfigFromCloud();
    lastCloudReceived = millis();
    if (millis() - lastCloudSend > SEND_DATA_TO_CLOUD) {
      sendDataToCloud();
      lastCloudSend = millis();
      checkFirmwareUpdate();
    }
  }

  runControl();
  delay(200);
}