#include <Arduino.h>

// Definim els "handlers" de les tasques (opcional, però bona pràctica per controlar-les després)
TaskHandle_t TaskSensorsHandle;
TaskHandle_t TaskActuatorsHandle;

// --- TASCA 1: Simulació de lectura de sensors (S'executarà al Core 1) ---
void TaskSensors(void *pvParameters) {
  Serial.print("TaskSensors corrent al nucli: ");
  Serial.println(xPortGetCoreID());

  for(;;) { // Bucle infinit de la tasca (equivalent al loop, però local)
    // Aquí en el futur llegiràs el VEML7700 i el BME280
    Serial.println("[SENSOR] Llegint dades de llum i temperatura...");
    
    // A FreeRTOS usem vTaskDelay en lloc de delay()
    // 2000ms de pausa sense bloquejar la CPU
    vTaskDelay(2000 / portTICK_PERIOD_MS); 
  }
}

// --- TASCA 2: Control d'Actuadors / LED (S'executarà al Core 0 o on decideixi l'Scheduler) ---
void TaskActuators(void *pvParameters) {
  Serial.print("TaskActuators corrent al nucli: ");
  Serial.println(xPortGetCoreID());

  pinMode(LED_BUILTIN, OUTPUT);

  for(;;) {
    digitalWrite(LED_BUILTIN, HIGH);
    vTaskDelay(200 / portTICK_PERIOD_MS); // LED ON 200ms
    digitalWrite(LED_BUILTIN, LOW);
    vTaskDelay(800 / portTICK_PERIOD_MS); // LED OFF 800ms
    // Serial.println("[ACTUADOR] Heartbeat LED");
  }
}

void setup() {
  Serial.begin(115200);
  
  // Creem la Tasca de Sensors
  xTaskCreatePinnedToCore(
    TaskSensors,      // Funció de la tasca
    "TaskSensors",    // Nom (per depuració)
    10000,            // Mida de la pila (Stack size) en paraules
    NULL,             // Paràmetres
    1,                // Prioritat (1 = baixa)
    &TaskSensorsHandle, // Handle
    1);               // Nucli (Core 1 és on sol córrer el codi d'usuari)

  // Creem la Tasca d'Actuadors
  xTaskCreatePinnedToCore(
    TaskActuators,
    "TaskActuators",
    10000,
    NULL,
    1,
    &TaskActuatorsHandle,
    0);               // Nucli (Core 0 sol gestionar Wi-Fi, però podem posar-hi coses)
}

void loop() {
  // En un sistema RTOS ben dissenyat, el loop() es pot deixar buit 
  // o fer-se servir només per a tasques de manteniment de molt baixa prioritat.
  // Tota la lògica està a les Tasks.
  vTaskDelete(NULL); // Opcional: Esborrem la tasca "loop" per alliberar memòria
}