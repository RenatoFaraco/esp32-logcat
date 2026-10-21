# PRD — esp32-logcat v1

> Sistema de logs para firmware embarcado, inspirado no `adb logcat` do Android.
> Status: rascunho v1 · Autor: Renato Faraco · Out/2026

## 1. Problema

Equipamento em campo trava, reinicia ou se comporta mal, e quando chega na bancada
não há registro do que aconteceu. `printf` na serial só ajuda com o cabo conectado,
some no reset e não diz *por que* a placa reiniciou.

## 2. Objetivo

Um componente de logging reutilizável em todo produto (FSI-TECH e projetos pessoais) que:

- registra eventos com nível, tag, timestamp e contexto de boot;
- **sobrevive a reset e falta de energia** (persistência em flash);
- registra **por que** o dispositivo reiniciou;
- tem uma ferramenta no PC para **ver, filtrar e baixar** os logs, como o `adb logcat`.

## 3. Escopo v1

### Firmware (ESP32-C3, PlatformIO, framework ESP-IDF, C++17)

| ID | Requisito |
|----|-----------|
| F1 | Capturar **todo** log do sistema (`ESP_LOGx` do app e do próprio IDF) via `esp_log_set_vprintf`, sem exigir API nova no código da aplicação |
| F2 | Níveis E/W/I/D/V e tags; nível ajustável **por tag em tempo de execução** |
| F3 | Logging não bloqueante: quem loga só enfileira; uma tarefa de baixa prioridade formata, envia e grava |
| F4 | Buffer circular em RAM com as últimas N linhas (consultável via comando) |
| F5 | Persistência em partição LittleFS dedicada, com **rotação de arquivos** e limite de tamanho total |
| F6 | Política de flush: periódica (ex.: 5 s) **+ imediata em ERROR** + manual via comando |
| F7 | Registro de boot: contador de boots (NVS), motivo do reset (`esp_reset_reason`), versão do firmware |
| F8 | "Último suspiro": últimas linhas em RAM `RTC_NOINIT` para recuperar o que aconteceu antes de um panic/watchdog |
| F9 | Console via USB-Serial-JTAG com comandos: `info`, `log dump`, `log tail`, `log clear`, `log level <tag> <nível>`, `reboot`, `crash <panic\|wdt>` (teste) |

### Formato da linha (texto legível e parseável)

```
<boot>:<seq> <uptime_ms> <L> <TAG>: <mensagem>
0042:000137 0000123456 W PRESSURE: leitura fora da faixa (4.71 V)
```

- `boot`: contador de boots → correlaciona logs entre resets
- `seq`: sequência dentro do boot → detecta perda de linhas
- Só ASCII imprimível (`0x20`–`0x7E`), uma linha por registro terminada em `\n` (ADR-006)

### Ferramenta host (`esp-logcat`, Python 3.12, CLI)

| ID | Requisito |
|----|-----------|
| H1 | `esp-logcat stream` — stream ao vivo, colorido por nível |
| H2 | Filtros estilo logcat: `esp-logcat stream "*:W" "PRESSURE:D"` |
| H3 | `esp-logcat dump` — baixa os logs persistidos da flash para arquivo |
| H4 | `esp-logcat shell` — envia comandos do console |
| H5 | `esp-logcat replay <arquivo>` — reproduz um log salvo com os mesmos filtros (desenvolvimento sem hardware) |
| H6 | Salvar sessão em arquivo com timestamp do PC ao lado do uptime do dispositivo |

## 4. Fora do escopo v1 (backlog)

- Envio para nuvem via MQTT (v2 — base de telemetria/monitoramento de drift, ponte com MLOps)
- Formato binário/tokenizado (v2)
- Core dump completo em flash (v1.1)
- Criptografia/assinatura dos logs (relevante para valor probatório, v3)
- Port para CH32V003 (RAM/flash insuficientes para esta arquitetura; avaliar versão mínima)

## 5. Restrições e decisões

- **Não bloquear tarefas críticas.** Logar não pode alterar o timing da aquisição de sinal.
- **Logs em ISR:** `ESP_LOGx` não pode ser usado em interrupção; usar `ESP_DRAM_LOGx` ou enfileirar evento.
- **Desgaste da flash:** gravações em lote; partição limitada (proposta 512 KB).
- **Falta de energia:** o que está só na RAM é perdido. O intervalo de flush define a janela máxima de perda — trade-off documentado, não bug.
- **Arquitetura:** núcleo em **C++ puro sem dependência do IDF** (formatação, parser, ring buffer, filtros, rotação), testável no PC (`env:native`). Adaptadores separados fazem a ponte com IDF/FreeRTOS/LittleFS (ports & adapters).

### Tabela de partições proposta (4 MB, validar no M0)

```
# Name,   Type, SubType,  Offset,  Size
nvs,      data, nvs,      0x9000,  0x6000
phy_init, data, phy,      0xf000,  0x1000
factory,  app,  factory,  0x10000, 0x180000
logs,     data, littlefs, ,        0x80000
coredump, data, coredump, ,        0x10000
```

## 6. Hardware de desenvolvimento

| Item | Qtd | Uso no projeto |
|------|-----|----------------|
| Placa ESP32-C3 genérica, **duas USB-C** | 2 | Alvo. Uma porta = USB-Serial-JTAG nativo (gravação, debug, console); outra = ponte USB-UART na UART0 (canal de log secundário). Identificar portas e flash na chegada |
| Módulo USB-UART CH343P | 1 | Canal extra / ler UART de outros dispositivos (alta velocidade) |
| Módulo USB-UART CP2102 | 1 | Reserva / segundo canal externo |
| Analisador lógico 24 MHz, 8 canais (PulseView/sigrok) | 1 | Critério de aceite 3 (jitter da tarefa crítica via GPIO) |

Regras: conversores externos sempre em **3,3 V** nos sinais; ligar só TX/RX/GND cruzados.
Chegada prevista: ~11/nov/2026. Até lá: M0, M1, M3 e o núcleo do M4/M5 em `env:native`.

## 7. Critérios de aceite v1

1. Após `crash panic`, o próximo boot mostra motivo `PANIC` e as últimas linhas antes do crash.
2. Cortando a alimentação, perdem-se no máximo os logs do último intervalo de flush; logs de nível ERROR nunca se perdem se o flush terminou.
3. Logar 1000 linhas/s numa tarefa não atrasa em mais de X µs uma tarefa periódica de 1 kHz (medir com analisador lógico via toggle de GPIO, confirmado por medição interna com `esp_timer`; X definido no M2).
4. A partição de logs nunca ultrapassa o limite; arquivos mais antigos são descartados.
5. Testes do núcleo C++ (native) e da ferramenta host (pytest) rodando no GitHub Actions.

## 8. Marcos

| Marco | Entrega |
|-------|---------|
| M0 | Repositório, `platformio.ini` (esp32c3 + native), CI compilando e rodando testes |
| M1 | Núcleo C++: `LogRecord`, formatador, parser, ring buffer — com testes native |
| M2 | Hook do `esp_log` + tarefa escritora + saída USB; medição de impacto no timing |
| M3 | Host: parser, cores, filtros, `replay` (pode ser feito antes do hardware chegar) |
| M4 | Persistência LittleFS + rotação + registro de boot |
| M5 | Console e comandos; `esp-logcat dump` e `esp-logcat shell` |
| M6 | Último suspiro (RTC) + testes de crash e corte de energia |
| M7 | (stretch) Core dump em flash |
