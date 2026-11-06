# Roadmap — esp32-logcat

> Escopo e critérios de aceite → `PRD.md` · Componentes → `ARCHITECTURE.md` · Placa → `hardware.md`
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

1. Chegada das placas (#9)
2. Fechar o M1: sanitizers (#42) e release `v0.2.0`
3. M2 · parte 1 — captura e saída USB (#10, #11, #12)
4. M3 · ferramenta host (#16–#21)
5. M2 · parte 2 — `stream`, app de demo e medição de jitter (#15, #13, #14)
6. M4 (a e b), M5 (a e b), M6, M7 (stretch)

Os blocos 🖥️ (M3, M4a, M5a) podem ser adiantados sempre que o trabalho com a placa travar.

### Chegada das placas 🔌 (#9)
- [x] Identificar a ponte UART e o chip da ponte (CH340K na UART0)
- [x] Identificar a porta USB-Serial-JTAG nativa (`VID_303A&PID_1001`)
- [x] Confirmar tamanho da flash (`esptool.py flash_id`): 4 MB embutida, XMC
- [x] Gravar o "hello" e ver saída na ponte UART
- [x] Ver saída na porta nativa (mesma saída nas duas portas)

### M2 · Captura e escrita 🔌 — parte 1
- [ ] `IdfLogHook`, `FreeRtosLogQueue`, `WriterTask`, `UsbJtagSink`, `EspClock`
- [ ] Proteção contra recursão e contador de descartes
- [ ] Interação com o filtro por tag do próprio IDF (`esp_log_level_set`) — ver #10

### M3 · Ferramenta host 🖥️
- [ ] `parser.py` (4 tipos de linha, ARCHITECTURE §6)
- [ ] `filters.py` com sintaxe `TAG:L`
- [ ] `render.py` com cores
- [ ] `esp-logcat replay <arquivo>` + fixtures em `tests/fixtures/`
- [ ] Testes de compatibilidade: as mesmas linhas de exemplo validadas pelo parser C++ e pelo Python
- **Pronto quando:** `replay` de um log de exemplo filtra e colore corretamente.

### M2 · Captura e escrita 🔌 — parte 2
- [ ] `esp-logcat stream` funcionando com a placa
- [ ] App de demo com tarefa crítica de 1 kHz alternando um GPIO
- [ ] **Medição:** jitter com analisador lógico (PulseView) + `esp_timer` interno; definir o X do critério 3

### M4a · Persistência — lógica 🖥️
- [ ] Interface `FileSystem` + implementação fake em memória para testes
- [ ] `RotatingStore`: rotação, limite por arquivo, limite total, descarte
- **Pronto quando:** testes de rotação cobrindo encher, girar e descartar.

### M4b · Persistência — integração 🔌
- [ ] `LittleFsFileSystem`, `NvsBootInfo`
- [ ] Linha de boot com contador, motivo do reset e versão (reset pelo pino EN aparece como `POWERON` no C3 — ver `hardware.md`)
- [ ] Política de flush (periódico + ERROR + comando)

### M5a · Console — lógica 🖥️
- [ ] `CommandParser` para todos os comandos do PRD (F9)
- [ ] Testes de comandos válidos, inválidos e com argumentos errados

### M5b · Console — integração 🔌
- [ ] Console na porta nativa; comandos executando de verdade
- [ ] `esp-logcat dump` (bloco `#BEGIN/#END` com CRC) e `esp-logcat shell`

### M6 · Robustez 🔌
- [ ] `RtcLastWords` com mágico + CRC
- [ ] Teste: `crash panic` e `crash wdt` → próximo boot mostra motivo e últimas linhas
- [ ] Teste de corte de energia (desligar a USB durante logs intensos, repetir N vezes)
- [ ] Validar todos os critérios de aceite do PRD
- **Release v1.0.0** com GIF no README

### M7 · Core dump (stretch) 🔌
- [ ] Core dump em flash e instruções de análise no PC

---

## Depois da v1

| Versão | Ideia | Ponte com |
|--------|-------|-----------|
| v1.1 | Core dump, comando `log grep` no dispositivo | — |
| v2 | Envio via MQTT (Wi-Fi), telemetria e métricas (heap, uptime, resets) | **MLOps**: base para monitorar drift de sensores em campo |
| v2 | Formato binário/tokenizado | Banda e flash |
| v2 | Componente reutilizável publicado (PlatformIO Registry / IDF Component Registry) | Portfólio |
| v3 | Assinatura/encadeamento por hash dos logs | Valor probatório em perícia |
| — | Versão mínima para CH32V003 (só ring buffer + UART) | Estudo de RISC-V bare metal |
