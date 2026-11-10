# PRD — esp32-logcat v2: componente ESP-IDF multichip

> Status: **rascunho**. Vira "aprovado" quando a v2 começar, junto com o ADR-009 ("aceita").
> Fontes: ADR-009 e ROADMAP, "Depois da v1". Nada aqui é requisito novo: só o que já foi
> decidido, organizado como requisito. Formato no fio: [`spec/wire-format.md`](../spec/wire-format.md),
> o mesmo da v1.

## 1. Problema

A v1 roda só no ESP32-C3. O chip muda de produto para produto, e um logcat preso a um chip só
serve a um produto. Quem adota o projeto precisa dele no chip que já usa.

## 2. Objetivo

O mesmo logcat da v1, publicado no IDF Component Registry e funcionando nos principais chips da
família Espressif, sem mudar o formato nem a ferramenta de PC.

## 3. Escopo

| ID | Requisito | Origem |
|----|-----------|--------|
| V2-1 | Componente ESP-IDF publicado no IDF Component Registry, com núcleo e adaptadores separados (núcleo sem IDF, ADR-003) | ADR-009 §1, §6 |
| V2-2 | Alvos: ESP32-C3, ESP32, ESP32-S3, ESP32-C6, ESP32-P4; cada um com bring-up registrado no `hardware.md` | ADR-009 §2 |
| V2-3 | Recursos detectados pelas *capabilities* do SoC (`SOC_*`), não por `CONFIG_IDF_TARGET_*` | ADR-009 §3 |
| V2-4 | Opções pelo Kconfig (núcleo da `WriterTask`, profundidade da fila, rótulo da partição…), lidas como `constexpr` | ADR-009 §4 |
| V2-5 | Um `Sink` por periférico de saída: `UsbJtagSink`, `UartSink` e um sink USB-OTG CDC | ADR-009 §5 |
| V2-6 | Dual-core: afinidade da `WriterTask` configurável (padrão `tskNO_AFFINITY`), criação estática; último suspiro com um único escritor, sem lock e sem escrita durante o panic | ADR-009 §7 |
| V2-7 | Todos os requisitos F1–F9 da v1 em cada alvo, com o mesmo formato no fio | PRD v1 §3, wire-format |

## 4. Fora do escopo

- Outros fabricantes (STM32, RP2040, nRF…): v4.
- ESP32-S2 e modelos mais antigos que o ESP32.
- Telemetria por MQTT: v3, como mais um `Sink` sobre este componente.
- Host em Go: v2.x ([PRD v2.x](v2x-go-host.md)).

## 5. Restrições

- API pública e opções do Kconfig viram contrato com usuários externos: SemVer, e mudar uma
  delas é *breaking change*.
- O hook exige `CONFIG_LOG_VERSION=1`: restrição documentada, ou suporte ao log v2 do IDF.
- O custo do hook muda de chip para chip (atômicos emulados no C3, nativos no C6 e no S3) e é
  medido em cada alvo.
- `esp_cpu_get_cycle_count()` é por núcleo: a medição de ciclos trata a migração entre núcleos.
- No P4, sem rádio, a telemetria futura depende de um chip companheiro (`esp-hosted`).

## 6. Critérios de aceite (rascunho)

1. Os critérios 1–5 do PRD v1 §7 passam em cada alvo da V2-2 (o 3 medido em cada um).
2. Um projeto ESP-IDF novo adiciona o componente pelo Registry e vê o logcat sem mudar o código
   da aplicação (F1).
3. O mesmo `esp-logcat` lê todos os alvos sem saber qual é o chip.

## 7. Questões abertas (decididas no começo da v2)

- Estrutura de diretórios do componente (ADR-009 §6).
- Build do alvo no CI: matriz `idf.py set-target <chip>` ou PlatformIO (ADR próprio).
- Quando e como migrar `docs/` para inglês (CONVENTIONS §1).
