#include <WiFi.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <esp_task_wdt.h>
#include <driver/rtc_io.h>

// IMPORT: Introducir credenciales antes de ejecutar 
const char* kWifiNombre = "";
const char* kWifiContrasena = "";

const int kPinTrig = 25;
const int kPinEcho = 26; 
const gpio_num_t kPinBoton = GPIO_NUM_33;

const unsigned long kTiempoEntreMedidas = 200;  // ms
const unsigned long kTiempoSinUso = 60000;      // ms sin peticiones antes de dormir
const uint64_t kTiempoDormido = 60000000;       // us (1 minuto)
const uint32_t kTiempoWatchdog = 8000;          // ms

WebServer servidor(80);

volatile unsigned long inicio_eco = 0;
volatile unsigned long duracion_eco = 0;
volatile bool eco_recibido = false;

// -1: No hay lectura
float distancia = -1;
float distancia_minima = -1;
float distancia_maxima = -1;
float distancia_anterior = -1;  // Última medida antes de dormir, leída de la EEPROM

int arranques = 0;
String motivo_arranque;
unsigned long ultima_medida = 0;
unsigned long ultima_peticion = 0;
bool pedir_dormir = false;

const char kPagina[] = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Medidor ultrasónico</title>
<style>
body { font-family: sans-serif; text-align: center; background: #f2f2f2; margin: 0; padding: 16px; }
.caja { background: white; border-radius: 10px; padding: 16px; margin: 12px auto; max-width: 400px; }
#distancia { font-size: 48px; font-weight: bold; color: #0b7a75; }
td { padding: 6px; text-align: left; }
button { width: 100%; padding: 12px; margin-top: 8px; font-size: 16px; }
</style>
</head>
<body>
<h2>Medidor de distancia ESP32</h2>
<div class="caja"><div id="distancia">--</div></div>
<div class="caja">
<table>
<tr><td>Mínima</td><td id="minima">--</td></tr>
<tr><td>Máxima</td><td id="maxima">--</td></tr>
<tr><td>Última antes de dormir</td><td id="anterior">--</td></tr>
<tr><td>Arranques</td><td id="arranques">--</td></tr>
<tr><td>Motivo del arranque</td><td id="motivo">--</td></tr>
</table>
</div>
<div class="caja">
<p>Se duerme tras 60 s sin uso. Se despierta con el botón o al minuto.</p>
<button onclick="fetch('/dormir')">Dormir ahora</button>
<button onclick="fetch('/bloquear')">Simular bloqueo (watchdog)</button>
</div>
<script>
function cm(valor) {
  if (valor < 0) return '--';
  return valor.toFixed(1) + ' cm';
}
function actualizar() {
  fetch('/datos')
    .then(respuesta => respuesta.json())
    .then(datos => {
      document.getElementById('distancia').textContent = datos.distancia < 0 ? 'Fuera de rango' : cm(datos.distancia);
      document.getElementById('minima').textContent = cm(datos.minima);
      document.getElementById('maxima').textContent = cm(datos.maxima);
      document.getElementById('anterior').textContent = cm(datos.anterior);
      document.getElementById('arranques').textContent = datos.arranques;
      document.getElementById('motivo').textContent = datos.motivo;
    })
    .catch(() => {
      document.getElementById('distancia').textContent = 'Sin conexión';
    });
}
setInterval(actualizar, 500);
</script>
</body>
</html>
)rawliteral";

// Cuando cambia el pin mide que esté alto
void IRAM_ATTR MedirEco() {
  if (digitalRead(kPinEcho) == HIGH) {
    inicio_eco = micros();
  } else {
    duracion_eco = micros() - inicio_eco;
    eco_recibido = true;
  }
}

// EEPROM
void LeerEeprom() {
  EEPROM.begin(16);
  EEPROM.get(0, arranques);
  EEPROM.get(4, distancia_minima);
  EEPROM.get(8, distancia_maxima);
  EEPROM.get(12, distancia_anterior);

  if (arranques < 0) {
    arranques = 0;
    distancia_minima = -1;
    distancia_maxima = -1;
    distancia_anterior = -1;
  }
}

void GuardarEeprom() {
  EEPROM.put(0, arranques);
  EEPROM.put(4, distancia_minima);
  EEPROM.put(8, distancia_maxima);
  if (distancia >= 0) {
    EEPROM.put(12, distancia);
  }
  EEPROM.commit();
}

String MotivoArranque() {
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    return "Botón";
  }
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
    return "Temporizador";
  }
  if (esp_reset_reason() == ESP_RST_TASK_WDT) {
    return "Watchdog";
  }
  return "Encendido";
}

void IniciarWatchdog() {
  esp_task_wdt_config_t configuracion = {
    .timeout_ms = kTiempoWatchdog,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };
  // Si el sistema ya lo había iniciado, solo se cambia su configuración
  if (esp_task_wdt_init(&configuracion) != ESP_OK) {
    esp_task_wdt_reconfigure(&configuracion);
  }
  esp_task_wdt_add(NULL);
}

void ConectarWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(kWifiNombre, kWifiContrasena);
  Serial.print("Conectando al WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    esp_task_wdt_reset();
    delay(500);
    Serial.print(".");
  }
  WiFi.setSleep(true); 
  Serial.println();
  Serial.print("Abre en el navegador: http://");
  Serial.println(WiFi.localIP());
}

void ActualizarDistancia() {
  if (millis() - ultima_medida < kTiempoEntreMedidas) {
    return;
  }

  if (eco_recibido) {
    distancia = duracion_eco / 58.0; //Formula para averiguar distancia (revisar)
  } else {
    distancia = -1;
  }

  // Rango de distancia (Verificado)
  if (distancia < 2 || distancia > 400) {
    distancia = -1;
  }

  if (distancia >= 0) {
    if (distancia_minima < 0 || distancia < distancia_minima) {
      distancia_minima = distancia;
    }
    if (distancia > distancia_maxima) {
      distancia_maxima = distancia;
    }
  }

  eco_recibido = false;
  digitalWrite(kPinTrig, HIGH);
  delayMicroseconds(10);
  digitalWrite(kPinTrig, LOW);
  ultima_medida = millis();
}

// Puente HTML
void EnviarPagina() {
  ultima_peticion = millis();
  servidor.send(200, "text/html", kPagina);
}

void EnviarDatos() {
  ultima_peticion = millis();
  String json = "{";
  json += "\"distancia\":" + String(distancia);
  json += ",\"minima\":" + String(distancia_minima);
  json += ",\"maxima\":" + String(distancia_maxima);
  json += ",\"anterior\":" + String(distancia_anterior);
  json += ",\"arranques\":" + String(arranques);
  json += ",\"motivo\":\"" + motivo_arranque + "\"";
  json += "}";
  servidor.send(200, "application/json", json);
}

void PedirDormir() {
  servidor.send(200, "text/plain", "ok");
  pedir_dormir = true;
}

void Bloquear() {
  servidor.send(200, "text/plain", "ok");
  Serial.println("Bloqueo simulado");
  while (true) {
  }
}
// FIn Puente

void Dormir() {
  Serial.println("Entrando en deep sleep");

  detachInterrupt(digitalPinToInterrupt(kPinEcho));
  GuardarEeprom();

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // Para despertar con botón (AVISO: PUL-UP)
  rtc_gpio_pullup_en(kPinBoton);
  esp_sleep_enable_ext0_wakeup(kPinBoton, 0);
  esp_sleep_enable_timer_wakeup(kTiempoDormido);

  esp_deep_sleep_start();
}

void setup() {
  Serial.begin(115200);
  IniciarWatchdog();

  motivo_arranque = MotivoArranque();
  Serial.println("Motivo del arranque: " + motivo_arranque);

  LeerEeprom();
  arranques++;
  GuardarEeprom();

  pinMode(kPinTrig, OUTPUT);
  pinMode(kPinEcho, INPUT);
  attachInterrupt(digitalPinToInterrupt(kPinEcho), MedirEco, CHANGE);

  ConectarWifi();
  servidor.on("/", EnviarPagina);
  servidor.on("/datos", EnviarDatos);
  servidor.on("/dormir", PedirDormir);
  servidor.on("/bloquear", Bloquear);
  servidor.begin();

  ultima_peticion = millis();
}

void loop() {
  esp_task_wdt_reset();  // Reinicio de watchdog si t < 8s

  servidor.handleClient();
  ActualizarDistancia();

  if (pedir_dormir) {
    delay(200);
    Dormir();
  }
  if (millis() - ultima_peticion > kTiempoSinUso) {
    Dormir();
  }
}
