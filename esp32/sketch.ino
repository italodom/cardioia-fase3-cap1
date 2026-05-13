/*
 * =====================================================================
 *  CardioIA - Fase 3, Capítulo 1
 *  Sistema vestível de monitoramento cardíaco (protótipo IoT)
 * =====================================================================
 *
 *  Camadas implementadas:
 *    - Edge Computing : leitura local, buffer offline, alerta no LED
 *    - Cloud          : transmissão MQTT (broker.hivemq.com)
 *
 *  Hardware simulado no Wokwi:
 *    - DHT22  (GPIO 15) : temperatura e umidade
 *    - Botão  (GPIO 4)  : simula cada batimento cardíaco
 *    - Switch (GPIO 5)  : simula conectividade Wi-Fi (online/offline)
 *    - LED    (GPIO 2)  : indicador visual de alerta
 *
 *  Limites clínicos de alerta (definidos pelo grupo):
 *    - BPM  > 120  -> taquicardia
 *    - Temp > 38°C -> febre
 * =====================================================================
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include "DHT.h"

// ---------- Pinos ----------
#define PIN_DHT     15
#define PIN_BTN      4
#define PIN_LED      2
#define PIN_SWITCH   5

// ---------- Sensor DHT22 ----------
#define DHT_TYPE DHT22
DHT dht(PIN_DHT, DHT_TYPE);

// ---------- Wi-Fi / MQTT ----------
const char* WIFI_SSID      = "Wokwi-GUEST";
const char* WIFI_PASS      = "";
const char* MQTT_BROKER    = "broker.hivemq.com";
const int   MQTT_PORT      = 1883;
const char* MQTT_TOPIC     = "cardioia/italo/sinais";
const char* MQTT_CLIENT_ID = "cardioia-esp32-italo";

WiFiClient   wifiClient;
PubSubClient mqtt(wifiClient);

// ---------- Resiliência offline (Edge Computing) ----------
// Buffer circular em RAM substitui o SPIFFS (volátil em simuladores).
// Modelo de negócio: 50 amostras x 5s = ~4 min de operação offline,
// suficiente para travessias típicas (elevador, túnel, banheiro) em
// um wearable cardio de uso doméstico.
#define BUFFER_SIZE 50
struct Leitura {
  unsigned long timestamp;
  float         temperatura;
  float         umidade;
  int           bpm;
};
Leitura buffer[BUFFER_SIZE];
int bufferInicio = 0;
int bufferFim    = 0;
int bufferCount  = 0;

// ---------- BPM ----------
volatile int     contadorBatimentos = 0;
volatile unsigned long ultimoClick  = 0;
unsigned long    ultimoCalculoBpm   = 0;
int              bpmAtual           = 0;
const unsigned long JANELA_BPM_MS   = 10000;  // janela de 10s

// ---------- Timing de leitura ----------
unsigned long ultimaLeitura = 0;
const unsigned long INTERVALO_LEITURA_MS = 5000;

// ---------- Limites de alerta ----------
const float LIMITE_TEMP = 38.0;
const int   LIMITE_BPM  = 120;

// ---------- Flag de conectividade simulada ----------
bool wifiSimulado = false;

// =====================================================================
// ISR do botão de batimento (debounce simples)
// =====================================================================
void IRAM_ATTR onBotaoPressionado() {
  unsigned long agora = millis();
  if (agora - ultimoClick > 150) {
    contadorBatimentos++;
    ultimoClick = agora;
  }
}

// =====================================================================
// Buffer circular - política drop-oldest
//   Em sinal vital, manter a janela mais recente é mais valioso para o
//   cardiologista do que preservar dados de minutos atrás.
// =====================================================================
void armazenarBuffer(Leitura l) {
  buffer[bufferFim] = l;
  bufferFim = (bufferFim + 1) % BUFFER_SIZE;
  if (bufferCount < BUFFER_SIZE) {
    bufferCount++;
  } else {
    bufferInicio = (bufferInicio + 1) % BUFFER_SIZE;
  }
}

// =====================================================================
// Conexão Wi-Fi (Wokwi)
// =====================================================================
void conectarWiFi() {
  Serial.print("[WiFi] Conectando a ");
  Serial.println(WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  int tentativas = 0;
  while (WiFi.status() != WL_CONNECTED && tentativas < 20) {
    delay(250);
    Serial.print(".");
    tentativas++;
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("[WiFi] Conectado. IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("[WiFi] Falha na conexao");
  }
}

// =====================================================================
// Conexão MQTT
// =====================================================================
bool conectarMQTT() {
  if (mqtt.connected()) return true;
  Serial.print("[MQTT] Conectando ao broker... ");
  if (mqtt.connect(MQTT_CLIENT_ID)) {
    Serial.println("OK");
    return true;
  }
  Serial.print("falhou, rc=");
  Serial.println(mqtt.state());
  return false;
}

// =====================================================================
// Formata uma Leitura em JSON
// =====================================================================
String formatarJson(Leitura l) {
  String json = "{";
  json += "\"ts\":"   + String(l.timestamp)        + ",";
  json += "\"temp\":" + String(l.temperatura, 2)   + ",";
  json += "\"umid\":" + String(l.umidade, 2)       + ",";
  json += "\"bpm\":"  + String(l.bpm);
  json += "}";
  return json;
}

// =====================================================================
// Sincronização do buffer com a nuvem
//   - Serial.println cumpre o papel de "envio para cloud" da Parte 1
//   - mqtt.publish cumpre o papel de transmissão real da Parte 2
//   Aborta se um publish falhar (evita perda silenciosa).
// =====================================================================
void sincronizarBuffer() {
  if (bufferCount == 0) return;
  if (!conectarMQTT()) return;

  Serial.print("[SYNC] Enviando ");
  Serial.print(bufferCount);
  Serial.println(" leitura(s) pendente(s)...");

  while (bufferCount > 0) {
    Leitura l = buffer[bufferInicio];
    String payload = formatarJson(l);

    Serial.print("[CLOUD-SERIAL] ");
    Serial.println(payload);

    if (mqtt.publish(MQTT_TOPIC, payload.c_str())) {
      bufferInicio = (bufferInicio + 1) % BUFFER_SIZE;
      bufferCount--;
    } else {
      Serial.println("[MQTT] Falha no publish, abortando sync");
      break;
    }
    delay(50);
  }

  if (bufferCount == 0) {
    Serial.println("[SYNC] Buffer esvaziado com sucesso");
  }
}

// =====================================================================
// Verificação de alerta no Edge
//   A decisão crítica é feita localmente para não depender da rede.
// =====================================================================
void verificarAlerta(Leitura l) {
  bool alerta = (l.bpm > LIMITE_BPM) || (l.temperatura > LIMITE_TEMP);
  digitalWrite(PIN_LED, alerta ? HIGH : LOW);
  if (alerta) {
    Serial.print("[ALERTA] ");
    if (l.bpm > LIMITE_BPM) {
      Serial.print("BPM=" + String(l.bpm) + " > " + String(LIMITE_BPM) + " ");
    }
    if (l.temperatura > LIMITE_TEMP) {
      Serial.print("Temp=" + String(l.temperatura, 1) + "C > " + String(LIMITE_TEMP, 1) + "C");
    }
    Serial.println();
  }
}

// =====================================================================
// setup()
// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== CardioIA Fase 3 Cap 1 - boot ===");

  pinMode(PIN_LED,    OUTPUT);
  pinMode(PIN_SWITCH, INPUT_PULLUP);
  pinMode(PIN_BTN,    INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_BTN), onBotaoPressionado, FALLING);

  dht.begin();
  conectarWiFi();
  mqtt.setServer(MQTT_BROKER, MQTT_PORT);
}

// =====================================================================
// loop()
// =====================================================================
void loop() {
  // Switch fechado (LOW) = "online" - simulação da conectividade exigida
  // pelo enunciado da Parte 1 (variável booleana).
  wifiSimulado = (digitalRead(PIN_SWITCH) == LOW);

  unsigned long agora = millis();

  // ---- Cálculo de BPM a cada janela ----
  if (agora - ultimoCalculoBpm >= JANELA_BPM_MS) {
    noInterrupts();
    int batidas = contadorBatimentos;
    contadorBatimentos = 0;
    interrupts();
    bpmAtual = batidas * (60000 / JANELA_BPM_MS);
    ultimoCalculoBpm = agora;
  }

  // ---- Leitura periódica ----
  if (agora - ultimaLeitura >= INTERVALO_LEITURA_MS) {
    ultimaLeitura = agora;

    float temp = dht.readTemperature();
    float umid = dht.readHumidity();
    if (isnan(temp) || isnan(umid)) {
      Serial.println("[DHT22] Falha de leitura");
      return;
    }

    Leitura l = { agora, temp, umid, bpmAtual };
    armazenarBuffer(l);

    Serial.printf("[LEITURA] T=%.1fC | U=%.1f%% | BPM=%d | buffer=%d/%d | wifi=%s\n",
                  l.temperatura, l.umidade, l.bpm,
                  bufferCount, BUFFER_SIZE,
                  wifiSimulado ? "ON" : "OFF");

    verificarAlerta(l);

    // Sincroniza buffer se a simulação indica online
    if (wifiSimulado) {
      sincronizarBuffer();
    }
  }

  if (mqtt.connected()) mqtt.loop();
}
