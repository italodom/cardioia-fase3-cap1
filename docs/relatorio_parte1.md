# Relatório Parte 1 — Edge Computing no ESP32

## 1. Visão geral

Este protótipo simula um *wearable* cardiológico do projeto **CardioIA** executado no Wokwi com ESP32. O foco da Parte 1 é o **Edge Computing**: coletar, processar e proteger leituras de sinais vitais localmente, garantindo que nenhum dado seja perdido mesmo em janelas de queda de conectividade. A motivação clínica é clara — em um dispositivo cardio, a decisão sobre disparar um alerta de taquicardia ou febre **não pode** depender da disponibilidade da rede.

## 2. Hardware simulado

| Componente      | Função                                          | Pino    |
|-----------------|-------------------------------------------------|---------|
| DHT22           | Temperatura e umidade corporais/ambientais      | GPIO 15 |
| Botão (pull-up) | Simula cada batimento cardíaco                  | GPIO 4  |
| Slide switch    | Simula o estado da conectividade Wi-Fi          | GPIO 5  |
| LED vermelho    | Indicador visual de alerta clínico              | GPIO 2  |

O grupo cumpre o requisito de **dois sensores distintos**: DHT22 (obrigatório) + botão tratado como sensor de batimento, exatamente conforme sugerido no enunciado ("um botão de pressão, que você aperta x vezes por minuto").

## 3. Fluxo de funcionamento

1. **Coleta periódica** — a cada 5 s o firmware lê temperatura e umidade do DHT22. Em paralelo, uma **ISR (interrupt service routine)** no botão incrementa o contador de batimentos a cada borda de descida, com debounce de 150 ms.
2. **Cálculo de BPM** — a cada janela de 10 s, o contador é multiplicado por 6 para extrapolar para um intervalo de 60 s, gerando o BPM atual. A janela curta dá resposta rápida; uma janela maior estabilizaria o valor, mas atrasaria o alerta.
3. **Empacotamento** — cada leitura compõe um `struct Leitura {timestamp, temp, umid, bpm}` que é gravado no buffer.
4. **Decisão local de alerta** — antes de qualquer transmissão, o ESP32 avalia `BPM > 120` ou `Temp > 38 °C` e acende o LED imediatamente. **Esse é o ponto central do Edge Computing em saúde**: a resposta crítica fica sob o controle do dispositivo e não introduz latência de rede.
5. **Transmissão** — quando o slide switch indica "online" (`wifiSimulado = true`), o buffer é drenado: cada leitura pendente é impressa via `Serial.println` (papel da nuvem simulada exigido pela Parte 1) **e** publicada via MQTT no broker HiveMQ (já antecipando a integração da Parte 2).

## 4. Estratégia de resiliência offline

O enunciado autoriza o uso do **Monitor Serial como alternativa de resiliência** dada a volatilidade do SPIFFS em simuladores. Optamos por um **buffer circular em RAM** que abstrai esse comportamento e mantém o mesmo padrão lógico que seria adotado em hardware real.

- **Tamanho do buffer**: 50 amostras. Com leitura a cada 5 s, isso cobre ~4 minutos de queda contínua de Wi-Fi. Esse dimensionamento foi escolhido alinhado ao **modelo de negócio** do CardioIA: um wearable doméstico precisa sobreviver a perdas curtas de cobertura — elevadores, túneis, banheiros — sem perder o sinal vital. Cenários de queda mais longos demandariam armazenamento persistente, e o caminho natural seria substituir o array por SPIFFS/SD em hardware real.
- **Política drop-oldest**: se o buffer encher antes de o link voltar, a leitura mais antiga é descartada. Em um sinal vital, **manter a janela mais recente é clinicamente mais valioso** do que preservar dados de muitos minutos atrás. O cardiologista quer ver o que aconteceu próximo ao evento.
- **Reentrega segura**: a função `sincronizarBuffer()` itera do índice `bufferInicio` até esvaziar o buffer. Se um `mqtt.publish()` falhar no meio do processo, a função aborta sem descartar a leitura corrente — isso evita perda silenciosa caso a rede caia novamente durante a sincronização.
- **Indicação local**: o `Serial.println` com prefixos (`[LEITURA]`, `[ALERTA]`, `[CLOUD-SERIAL]`, `[MQTT]`, `[SYNC]`) permite auditoria do comportamento em tempo real, mesmo sem o dashboard.

## 5. Como reproduzir

1. Abrir o link do projeto no Wokwi (ver `README.md`).
2. Iniciar a simulação.
3. Clicar repetidamente no botão vermelho para "gerar" batimentos — segurar em torno de 2 cliques/s para chegar próximo do limite de 120 BPM.
4. Alternar o slide switch para reproduzir os cenários online/offline e validar a resiliência.
5. Acompanhar o Monitor Serial: leituras periódicas, alertas, prefixos `[CLOUD-SERIAL]` (Parte 1) e `[MQTT]` (Parte 2).

## 6. Conclusão

A Parte 1 demonstra, em um cenário mínimo, o valor do Edge Computing em aplicações críticas de saúde: a decisão de alertar é local e imediata, o armazenamento é resiliente a quedas momentâneas de conexão, e a sincronização com a nuvem é oportunista — disparada apenas quando há link disponível. Essa arquitetura é diretamente transferível para um wearable real, bastando trocar o buffer em RAM por SPIFFS/SD e adicionar autenticação ao broker MQTT.
