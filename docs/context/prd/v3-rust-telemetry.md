# PRD — esp32-logcat v3: firmware em Rust e telemetria

> Status: **rascunho**. Vira "aprovado" quando a v3 começar. Fontes: ADR-011 (Rust, proposta) e a
> linha da v3 no ROADMAP, "Depois da v1" (telemetria, ainda sem ADR). Formato no fio:
> [`spec/wire-format.md`](../spec/wire-format.md).

A v3 tem duas frentes independentes. A parte de telemetria está menos decidida: os requisitos
dela são o que o ROADMAP já diz, e o resto está em §7.

## 1. Problema

- Quem escreve firmware em Rust não tem um logcat persistente com ferramenta de PC.
- Um dispositivo em campo só mostra os logs quando chega à bancada. Para monitorar drift de
  sensores (MLOps), os dados precisam sair do dispositivo enquanto ele trabalha.

## 2. Objetivo

- Um crate Rust `no_std` que fala o mesmo formato, lido pela mesma ferramenta de PC: prova que
  o contrato independe da linguagem.
- Um `Sink` de telemetria por MQTT sobre o componente da v2.

## 3. Escopo

### Firmware Rust (ADR-011)

| ID | Requisito | Origem |
|----|-----------|--------|
| R3-1 | Crate `no_std` sobre `esp-hal`, paralelo ao componente C++ (que continua) | ADR-011 §1 |
| R3-2 | Formato de linha da `spec/wire-format.md` §1 e a mesma separação núcleo/adaptadores (ADR-003) | ADR-011 §1 |
| R3-3 | Escopo inicial: formatter, ring buffer, sink USB/UART, integração com a fachada `log` do Rust | ADR-011 §2 |
| R3-4 | Depois, nessa ordem: persistência, registro de boot, último suspiro | ADR-011 §2 |
| R3-5 | O formatter Rust passa na suíte de `conformance/` | ADR-011 §3 |
| R3-6 | Publicado no crates.io; núcleo testado com `cargo test` no PC e no CI | ADR-011, Consequências |

### Telemetria (ROADMAP)

| ID | Requisito | Origem |
|----|-----------|--------|
| T3-1 | Envio por MQTT (Wi-Fi), como mais um `Sink` do componente da v2 | ROADMAP, ADR-009 |
| T3-2 | Métricas do dispositivo: heap, uptime, resets | ROADMAP |
| T3-3 | Formato binário/tokenizado para economizar banda | ROADMAP |

## 4. Fora do escopo

- Substituir o componente C++: o Rust é paralelo.
- F1 do PRD v1 (capturar todo `ESP_LOGx` do IDF) no Rust: com `esp-hal` bare metal a captura é
  pela fachada `log`.
- Criptografia/assinatura dos logs (backlog, sem versão).

## 5. Restrições

- Duas implementações de firmware: uma mudança de formato atualiza o formatter C++, o Rust, os
  parsers e a suíte no mesmo commit (ADR-002).
- Ferramentas externas (dashboards, telemetria) leem o `--json` do host, não o fio (ADR-012).
- No ESP32-P4, sem rádio, a telemetria depende de um chip companheiro (`esp-hosted`).

## 6. Critérios de aceite (rascunho)

1. O formatter Rust passa em toda a suíte de `conformance/`.
2. O mesmo `esp-logcat` lê um firmware C++ e um Rust sem saber qual é qual.
3. Telemetria: a definir com o ADR dela.

## 7. Questões abertas

- ADR da telemetria: broker, backend, segurança das credenciais (nunca no repositório).
- O formato binário: substitui o texto no MQTT ou vive ao lado dele, e como entra na spec.
- A linguagem do coletor e da análise (ADR-010, Consequências).
