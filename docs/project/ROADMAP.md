# Roadmap — esp32-logcat

> Escopo e critérios de aceite → `docs/context/prd/` (v1 corrente) · Formato no fio →
> `docs/context/spec/wire-format.md` · Componentes → `ARCHITECTURE.md` · Placa → `hardware.md`
> 🖥️ = roda sem hardware (PC/`native`) · 🔌 = precisa da placa
> Hardware chegou em 06/nov/2026 (previsão era ~11/nov).

Cada marco termina com: testes passando no CI, commit/tag e este arquivo atualizado.

---

## Fase 1 — antes do hardware (encerrada em 06/nov)

### M0 · Fundação 🖥️ — `v0.1.0`
- [x] Repositório: `README.md` (esqueleto), `LICENSE` (MIT), `.gitignore` (PlatformIO + Python)
- [x] `firmware/platformio.ini` com `env:esp32c3` (framework espidf) e `env:native`
- [x] `firmware/partitions.csv` (PRD §5)
- [x] Firmware "hello" que compila para o C3 (sem gravar)
- [x] `host/pyproject.toml`, `.venv`, pacote `esp_logcat` com CLI vazia
- [x] `.clang-format`, `ruff` configurados
- [x] CI (`.github/workflows/ci.yml`): `pio test -e native`, `pio run -e esp32c3`, `pytest`, `ruff`
- [x] ADRs 001–005
- **Pronto quando:** CI verde num push.

### M1 · Núcleo 🖥️ — `v0.2.0`
- [x] `LogRecord`, `Sequencer`
- [x] `LineFormatter` e `LineParser` (ida e volta: format → parse = original)
- [x] `RingBuffer<T, N>`
- [x] `TagLevelFilter`
- [x] Testes Unity para tudo, incluindo casos de borda (truncamento, tag longa, buffer cheio)
- [x] CI com sanitizers (ASan/UBSan/TSan) no núcleo (#42)
- **Pronto quando:** cobertura dos módulos do núcleo e CI verde → **release `v0.2.0`**.

---

## Fase 2 — com hardware (desde 06/nov)

**Ordem de trabalho combinada** (firmware primeiro: a placa é a maior incerteza; o host entra
quando o firmware passa a precisar dele):

1. ~~Chegada das placas (#9)~~ — feito em 06/nov
2. ~~Fechar o M1: sanitizers (#42) e release `v0.2.0`~~ — feito em 06/nov
3. ~~**Esqueleto do host** ("walking skeleton"): parser (#16), cores (#18) e `stream` mínimo
   (#15)~~
4. ~~M2 · parte 1 — captura e saída USB (#10, #11, #12), com uma tarefa de heartbeat~~
5. ~~Resto do M3 — filtros (#17), replay (#19), fixtures C++/Python (#20), sessão (#21)~~
6. ~~M2 · parte 2 — `stream` completo com filtros, app de demo e medição interna de jitter
   (#15, #13)~~; a medição com o analisador (#14) foi para o M6
7. ~~M5 no firmware: `CommandParser` (#27) e console (#28)~~ e release `v0.3.0` — 11/nov
8. `esp-logcat shell` (#30), M4a (#22, #23), M4b, `esp-logcat dump` (#29), M6, M7 (stretch)

Os blocos 🖥️ (M3, M4a, M5a) podem ser adiantados sempre que o trabalho com a placa travar.

### Chegada das placas 🔌 (#9)
- [x] Identificar a ponte UART e o chip da ponte (CH340K na UART0)
- [x] Identificar a porta USB-Serial-JTAG nativa (`VID_303A&PID_1001`)
- [x] Confirmar tamanho da flash (`esptool.py flash_id`): 4 MB embutida, XMC
- [x] Gravar o "hello" e ver saída na ponte UART
- [x] Ver saída na porta nativa (mesma saída nas duas portas)

### M2 · Captura e escrita 🔌 — parte 1 — `v0.3.0`
- [x] `IdfLogHook`, `FreeRtosLogQueue`, `WriterTask`, `UsbJtagSink`, `EspClock` (#10, #11, #12)
- [x] Proteção contra recursão e contador de descartes (#11, #12)
- [x] Interação com o filtro por tag do próprio IDF (`esp_log_level_set`) — ver #10. O comando
      `log level` (#28) mexe nos dois portões. Limitação: um `esp_log_level_set` chamado direto
      pela aplicação só mexe no do IDF, e o `TagLevelFilter` não fica sabendo

### M3 · Ferramenta host 🖥️ — `v0.3.0`
- [x] `parser.py`: formato do PRD (espelho do parser C++), formato nativo do ESP-IDF, resposta de comando, marcadores de dump e raw (#16)
- [x] `esp-logcat stream` mínimo (sem filtros): transporte serial com reconexão, estatísticas de `seq`, `ports` (#15, parte 1)
- [x] `filters.py` com sintaxe `TAG:L` (#17)
- [x] `render.py` com cores, devolvendo `rich.text.Text` (#18)
- [x] `esp-logcat tui` com Textual, depois dos filtros (H7, ADR-008, #56)
- [x] `esp-logcat replay <arquivo>` + fixtures em `tests/fixtures/` (#19)
- [x] Testes de compatibilidade: as mesmas linhas de exemplo validadas pelo parser C++ e pelo Python (#20, `conformance/`)
- [x] Sessão salva com a hora do PC (`--save`), reproduzida igual pelo `replay` (H6, #21)
- **Pronto quando:** `replay` de um log de exemplo filtra e colore corretamente.

### M2 · Captura e escrita 🔌 — parte 2 — `v0.3.0`
- [x] `esp-logcat stream` funcionando com a placa (as imagens do manual são sessões reais)
- [x] App de demo com tarefa crítica de 1 kHz alternando um GPIO, e varredura de carga de log em fases (#13)
- [x] Medição interna: a tarefa de 1 kHz mede o próprio atraso e a varredura reporta
      p50/p99/máx por fase (`TIMING`, #13). A medição externa e o X foram para o M6 (#14)

### M4a · Persistência — lógica 🖥️
- [x] Interface `FileSystem` + implementação fake em memória para testes (#22): o fake
      também simula partição cheia, falhas e queda de energia em qualquer ponto
- [ ] `RotatingStore`: rotação, limite por arquivo, limite total, descarte
- **Pronto quando:** testes de rotação cobrindo encher, girar e descartar.

### M4b · Persistência — integração 🔌
- [ ] `LittleFsFileSystem`, `NvsBootInfo`
- [ ] Linha de boot com contador, motivo do reset e versão (reset pelo pino EN aparece como `POWERON` no C3 — ver `hardware.md`)
- [ ] Política de flush (periódico + ERROR + comando)

### M5a · Console — lógica 🖥️
- [x] `CommandParser` para todos os comandos do PRD (F9), mais `info` e `stats` (#27)
- [x] Testes de comandos válidos, inválidos e com argumentos errados (casos compartilhados
      em `conformance/request_cases.tsv`)
- [x] `ReplyFormatter`: linhas de resposta e CRC-32 do bloco, conferidos contra
      `conformance/reply_cases.tsv` (ADR-012)

### M5b · Console — integração 🔌
- [x] Console na porta nativa, com 7 dos 9 comandos do F9 funcionando na placa (#28)
- [ ] `log dump` e `log clear` de verdade: dependem da flash (M4); até lá, `unsupported`
- [x] `esp-logcat shell`: um comando (com `--json`), prompt interativo com logs filtrados, ou
      pipe (#30)
- [ ] `esp-logcat dump` (bloco `#BEGIN/#END` com CRC; o `Framer` da #30 já lê o bloco)

### M6 · Robustez 🔌
- [ ] `RtcLastWords` com mágico + CRC
- [ ] Teste: `crash panic` e `crash wdt` → próximo boot mostra motivo e últimas linhas
- [ ] Teste de corte de energia (desligar a USB durante logs intensos, repetir N vezes)
- [ ] **Medição:** jitter com analisador lógico (PulseView), conferindo a medição interna da
      #13; definir o X do critério 3 (#14)
- [ ] Validar todos os critérios de aceite do PRD
- **Release v1.0.0** com GIF no README

### M7 · Core dump (stretch) 🔌
- [ ] Core dump em flash e instruções de análise no PC

---

## Depois da v1

Combinado em 07/nov/2026. O projeto é open source e pensado para o público em geral: quem
adota adapta ao seu produto. A ordem segue as dependências: multichip primeiro, porque a
telemetria vira só mais um `Sink` sobre ele. Os ADRs 009–011 estão como **proposta** e viram
"aceita" quando a versão começar.

| Versão | Ideia | Por quê | Decisão |
|--------|-------|---------|---------|
| v1.1 | Core dump, comando `log grep` no dispositivo | — | — |
| v2 | Componente ESP-IDF multichip (família Espressif) publicado no IDF Component Registry | O chip muda de produto para produto | ADR-009 · [PRD v2](../context/prd/v2-multichip.md) |
| v2.x | Host em Go (CLI + TUI), binário único por SO, validado por suíte de conformidade | Instalar sem Python nem `.venv` | ADR-010 · [PRD v2.x](../context/prd/v2x-go-host.md) |
| v3 | Crate Rust `no_std` (`esp-hal`) com o mesmo formato de linha | Logcat para firmware em Rust; prova que o formato independe da linguagem | ADR-011 · [PRD v3](../context/prd/v3-rust-telemetry.md) |
| v3 | Envio via MQTT (Wi-Fi), telemetria e métricas (heap, uptime, resets) + formato binário/tokenizado | **MLOps**: monitorar drift de sensores em campo; o binário economiza banda | [PRD v3](../context/prd/v3-rust-telemetry.md) (rascunho, sem ADR) |
| v4 | Outros fabricantes (STM32, RP2040, nRF…) e versão mínima para CH32V003 (só ring buffer + UART) | Multichip além da Espressif; pode exigir renomear o projeto | — |
| backlog | Assinatura/encadeamento por hash dos logs | Valor probatório em perícia (ADR-007) | — |

### Placas da v2 (ADR-009)

| Chip | O que traz para o teste | Situação |
|------|--------------------------|----------|
| ESP32-C3 | RISC-V single-core sem atômicos nativos, USB-Serial-JTAG | DevKitM-1 em uso (`hardware.md`) |
| ESP32 | Dual-core Xtensa, só UART (sem USB nativa), memória RTC | disponível, sem bring-up |
| ESP32-S3 | Dual-core Xtensa, USB-Serial-JTAG + USB-OTG, PSRAM | a comprar |
| ESP32-C6 | RISC-V com atômicos nativos, LP RAM, Wi-Fi 6 | a comprar |
| ESP32-P4 | Dual-core RISC-V, USB-OTG HS, sem rádio | a comprar (confirmar a revisão suportada pelo IDF) |

Fora: ESP32-S2 e modelos mais antigos. Chips novos entram um de cada vez, cada um com seu
bring-up.
