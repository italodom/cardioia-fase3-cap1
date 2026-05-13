"""
CardioIA - Simulador do ESP32 em Python
========================================

Publica no broker MQTT exatamente o mesmo payload JSON que o firmware
do ESP32 (sketch.ino) publicaria. Serve como plano alternativo quando o
servidor de build do Wokwi (free tier) esta indisponivel.

O comportamento eh fiel ao firmware:
  - Le "DHT22" simulado (temperatura/umidade com ruido)
  - Conta "batimentos" simulados (BPM aleatorio em torno de 75-130)
  - Implementa o mesmo buffer circular (50 amostras, drop-oldest)
  - Simula queda de Wi-Fi periodica (a cada 30s alterna online/offline)
  - Quando "online", envia o buffer acumulado via MQTT
  - Imprime alertas no console quando bpm > 120 ou temp > 38C

Como rodar:
  pip install paho-mqtt
  python simulador_esp32.py
"""

import json
import random
import time
from collections import deque

import paho.mqtt.client as mqtt

# ---------- Config (mesma do sketch.ino) ----------
MQTT_BROKER    = "broker.hivemq.com"
MQTT_PORT      = 1883
MQTT_TOPIC     = "cardioia/italo/sinais"
MQTT_CLIENT_ID = "cardioia-py-italo"

BUFFER_SIZE         = 50
INTERVALO_LEITURA_S = 5
INTERVALO_WIFI_S    = 30        # alterna online/offline a cada 30s
LIMITE_TEMP         = 38.0
LIMITE_BPM          = 120

# ---------- Estado ----------
buffer = deque(maxlen=BUFFER_SIZE)   # drop-oldest automatico
wifi_simulado = True


def gerar_leitura() -> dict:
    """Gera uma leitura sintetica com ruido realista."""
    return {
        "ts":   int(time.time() * 1000),
        "temp": round(random.gauss(36.8, 0.6), 2),     # febre as vezes
        "umid": round(random.gauss(55.0, 5.0), 2),
        "bpm":  random.randint(60, 135),                # taquicardia as vezes
    }


def verificar_alerta(l: dict) -> None:
    avisos = []
    if l["bpm"] > LIMITE_BPM:
        avisos.append(f"BPM={l['bpm']} > {LIMITE_BPM}")
    if l["temp"] > LIMITE_TEMP:
        avisos.append(f"Temp={l['temp']:.1f}C > {LIMITE_TEMP}C")
    if avisos:
        print(f"  [ALERTA] {' | '.join(avisos)}")


def sincronizar_buffer(client: mqtt.Client) -> None:
    """Drena o buffer publicando cada leitura no broker."""
    if not buffer:
        return
    print(f"  [SYNC] Enviando {len(buffer)} leitura(s) pendente(s)...")
    while buffer:
        leitura = buffer.popleft()
        payload = json.dumps(leitura)
        result = client.publish(MQTT_TOPIC, payload)
        if result.rc != mqtt.MQTT_ERR_SUCCESS:
            print("  [MQTT] Falha no publish, abortando sync")
            buffer.appendleft(leitura)
            return
        print(f"  [CLOUD-MQTT] {payload}")
        time.sleep(0.05)
    print("  [SYNC] Buffer esvaziado")


def on_connect(client, userdata, flags, reason_code, properties=None):
    if reason_code == 0:
        print(f"[MQTT] Conectado ao broker {MQTT_BROKER}:{MQTT_PORT}")
    else:
        print(f"[MQTT] Falha na conexao, rc={reason_code}")


def main() -> None:
    global wifi_simulado

    client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id=MQTT_CLIENT_ID,
    )
    client.on_connect = on_connect
    client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
    client.loop_start()

    print("=== CardioIA - Simulador do ESP32 (Python) ===")
    print(f"Publicando em '{MQTT_TOPIC}'")
    print("Ctrl+C para parar\n")

    inicio = time.time()

    try:
        while True:
            # Alterna o estado simulado de Wi-Fi a cada INTERVALO_WIFI_S
            wifi_simulado = (int((time.time() - inicio) / INTERVALO_WIFI_S) % 2 == 0)

            leitura = gerar_leitura()
            buffer.append(leitura)

            wifi_str = "ON " if wifi_simulado else "OFF"
            print(f"[LEITURA] T={leitura['temp']:.1f}C | U={leitura['umid']:.1f}% | "
                  f"BPM={leitura['bpm']:3d} | buffer={len(buffer)}/{BUFFER_SIZE} | "
                  f"wifi={wifi_str}")

            verificar_alerta(leitura)

            if wifi_simulado:
                sincronizar_buffer(client)

            time.sleep(INTERVALO_LEITURA_S)

    except KeyboardInterrupt:
        print("\n[FIM] Encerrado pelo usuario")
    finally:
        client.loop_stop()
        client.disconnect()


if __name__ == "__main__":
    main()
