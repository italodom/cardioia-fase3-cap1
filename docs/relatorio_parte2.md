# Relatório Parte 2 — Fog/Cloud Computing e Visualização

## 1. Arquitetura geral

A solução integra as três camadas clássicas de IoT em saúde:

```
┌──────────────────────────────────────────────┐
│ EDGE                                         │
│ ESP32 + DHT22 + Botão (BPM) + Switch + LED   │
│  - leitura periódica (5 s)                   │
│  - cálculo de BPM por janela (10 s)          │
│  - buffer circular RAM (resiliência offline) │
│  - decisão local de alerta (LED)             │
└────────────────────┬─────────────────────────┘
                     │  MQTT (broker.hivemq.com:1883)
                     │  tópico: cardioia/italo/sinais
                     ▼
┌──────────────────────────────────────────────┐
│ FOG                                          │
│ Node-RED (Docker, localhost:1880)            │
│  - subscribe MQTT                            │
│  - parse JSON                                │
│  - regra clínica de alerta                   │
└────────────────────┬─────────────────────────┘
                     │
                     ▼
┌──────────────────────────────────────────────┐
│ VISUALIZAÇÃO                                 │
│ Dashboard em http://localhost:1880/ui        │
│  - gráfico de BPM (linha, 10 min)            │
│  - gauge de temperatura (20–45 °C)           │
│  - indicador textual de alerta (verde/vermelho)│
└──────────────────────────────────────────────┘
```

Essa separação reflete fielmente o modelo de negócio do CardioIA: o **edge** decide o que é urgente, o **fog** centraliza e enriquece o sinal, a **cloud** expõe a informação aos profissionais.

## 2. Fluxo de comunicação MQTT

| Item                | Valor                                |
|---------------------|--------------------------------------|
| Broker              | `broker.hivemq.com`                  |
| Porta               | `1883` (TCP, sem TLS)                |
| Cliente ESP32       | `cardioia-esp32-italo`               |
| Tópico de publicação| `cardioia/italo/sinais`              |
| Tipo de payload     | JSON                                 |
| Estrutura           | `{ts, temp, umid, bpm}`              |
| QoS                 | 0                                    |

### Decisões e justificativas

- **Broker público (HiveMQ)** — escolhido por eliminar cadastro e TLS, adequado a um protótipo acadêmico. Em produção, o caminho natural é migrar para **HiveMQ Cloud** com usuário/senha e porta TLS 8883, mantendo a mesma topologia.
- **QoS 0** — suficiente porque a garantia de entrega já é resolvida na camada de aplicação, no **buffer de resiliência do edge**: se uma mensagem se perder no broker, a próxima sincronização re-publica tudo o que ainda estiver pendente no ESP32. Usar QoS 1/2 dobraria o tráfego sem ganho real neste arranjo.
- **Payload JSON** — simplifica o parse no Node-RED (`datatype: "json"` no nó `mqtt in`), interoperabilidade com Grafana via plugin MQTT, e expansão futura (basta acrescentar campos como `bateria`, `gps`, `id_paciente`).
- **Tópico identificado** — incluímos um identificador (`italo`) para não colidir com outros usuários do broker público. Em produção, o caminho seria um UUID por paciente: `cardioia/<paciente_uuid>/sinais`.

### Sequência típica de uma transmissão

1. ESP32 lê DHT22 e calcula o BPM da janela atual.
2. Empilha a leitura no buffer circular.
3. Se `wifiSimulado == true`, percorre o buffer e publica cada item no tópico.
4. Node-RED recebe a mensagem, faz o parse JSON, e roteia para três destinos paralelos: gráfico de BPM, gauge de temperatura, função de detecção de alerta.
5. O dashboard atualiza-se em tempo real (websocket interno do Node-RED Dashboard).

## 3. Configuração do dashboard (Node-RED)

O arquivo `flow.json` exportado é importável via `Menu → Import` no Node-RED. Estrutura dos nós:

| Nó                  | Tipo            | Função                                                                  |
|---------------------|-----------------|-------------------------------------------------------------------------|
| `MQTT CardioIA`     | `mqtt in`       | Subscribe em `cardioia/italo/sinais`, `datatype: json`                  |
| `Extrair campos`    | `function`      | Copia `bpm`, `temp`, `umid` para o `msg` (facilita roteamento)          |
| `BPM -> chart`      | `function`      | Formata `{payload: bpm, topic: 'BPM'}` para o gráfico                   |
| `Gráfico BPM`       | `ui_chart`      | Linha, eixo Y 0–180, retenção de 10 min                                 |
| `Temp -> gauge`     | `function`      | Repassa apenas `temp` para o gauge                                      |
| `Gauge Temperatura` | `ui_gauge`      | 20–45 °C, faixas verde/amarela/vermelha em 37/38                        |
| `Detectar alerta`   | `function`      | Aplica regras clínicas, gera texto e cor                                |
| `Indicador alerta`  | `ui_text`       | Texto grande, verde quando OK, vermelho quando alerta ativo             |

### Regras de alerta definidas pelo grupo

| Condição           | Significado clínico  | Ação                                  |
|--------------------|----------------------|---------------------------------------|
| `BPM > 120`        | Taquicardia          | LED no ESP32 + texto vermelho no UI   |
| `Temp > 38 °C`     | Febre                | LED no ESP32 + texto vermelho no UI   |
| Caso contrário     | Sinais normais       | LED apagado + texto verde no UI       |

As regras estão **duplicadas propositalmente em duas camadas** (edge e fog). O edge responde em milissegundos sem depender da rede; o fog persiste o evento e permitiria, em uma próxima iteração, encaminhar via REST/e-mail para o cardiologista — exatamente o conceito explorado no "Ir Além 1" do enunciado.

## 4. Reprodução passo a passo

### 4.1 Subir o Node-RED (Docker)

```bash
docker run -d --name nodered -p 1880:1880 -v $HOME/.node-red:/data nodered/node-red
```

### 4.2 Instalar o pacote de dashboard

1. Abrir `http://localhost:1880`.
2. `Menu → Manage Palette → Install`.
3. Buscar `node-red-dashboard` e instalar.

### 4.3 Importar o flow

1. `Menu → Import → Select a file to import`.
2. Apontar para `flow.json` deste repositório.
3. Clicar em **Deploy** (canto superior direito).

### 4.4 Subir o ESP32 no Wokwi

1. Abrir o link do Wokwi (ver `README.md`).
2. Confirmar que `libraries.txt` contém `DHT sensor library`, `Adafruit Unified Sensor` e `PubSubClient`.
3. Iniciar a simulação.
4. Garantir que o slide switch está em "ON" para que o ESP32 publique no broker.

### 4.5 Validar o dashboard

1. Abrir `http://localhost:1880/ui`.
2. Verificar que o gráfico de BPM começa a receber pontos.
3. Pressionar o botão repetidamente até o BPM ultrapassar 120 — o indicador deve virar **vermelho** com a mensagem `ALERTA: TAQUICARDIA`.
4. No Wokwi, clicar no DHT22 e arrastar a temperatura para acima de 38 °C — o gauge deve mudar de cor e o indicador deve sinalizar **febre**.

## 5. Limitações e próximos passos

- **Broker público é multi-tenant**: qualquer demo em ambiente público deveria usar um tópico com UUID. Em produção, HiveMQ Cloud com credenciais e TLS.
- **Sem persistência histórica**: o gráfico do Node-RED é volátil. O próximo passo é encadear `InfluxDB + Grafana Cloud` para análise retroativa — o formato JSON já está pronto para isso.
- **Alertas estáticos**: as regras atuais usam apenas limiares. O "Ir Além 2" do enunciado complementaria com IA em séries temporais (regressão logística vs. rede neuromórfica LIF) para detectar **padrões de arritmia**, não só extremos.
- **Sem autenticação**: o ESP32 conecta-se ao broker sem credenciais. Em produção, certificados X.509 por dispositivo são o padrão para wearables médicos.

## 6. Conclusão

A Parte 2 fecha o ciclo IoT em saúde: captura no edge, transmissão via MQTT, processamento e regras no fog (Node-RED), visualização em tempo real. A arquitetura é direta, observável e modular — qualquer um dos blocos pode ser substituído (Grafana no lugar do Node-RED, AWS IoT no lugar do HiveMQ) sem alterar o firmware do ESP32, o que demonstra que as decisões de protocolo e formato de payload foram acertadas.
