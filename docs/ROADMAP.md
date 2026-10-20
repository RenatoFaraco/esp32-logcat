# Roadmap — esp32-logcat

> Escopo e critérios de aceite → `PRD.md` · Componentes → `ARCHITECTURE.md`
> 🖥️ = roda sem hardware (PC/`native`) · 🔌 = precisa da placa
> Hardware previsto para ~11/nov/2026.

Cada marco termina com: testes passando no CI, commit/tag e este arquivo atualizado.

---

## Fase 1 — antes do hardware (até ~11/nov)

### M0 · Fundação 🖥️
- [ ] Repositório: `README.md` (esqueleto), `LICENSE` (MIT), `.gitignore` (PlatformIO + Python)
- [ ] `firmware/platformio.ini` com `env:esp32c3` (framework espidf) e `env:native`
- [ ] `firmware/partitions.csv` (PRD §5)
- [ ] Firmware "hello" que compila para o C3 (sem gravar)
- [ ] `host/pyproject.toml`, `.venv`, pacote `esp_logcat` com CLI vazia
- [ ] `.clang-format`, `ruff` configurados
- [ ] CI (`.github/workflows/ci.yml`): `pio test -e native`, `pio run -e esp32c3`, `pytest`, `ruff`
- [ ] ADRs 001–004
- **Pronto quando:** CI verde num push.

### M1 · Núcleo 🖥️
- [ ] `LogRecord`, `Sequencer`
- [ ] `LineFormatter` e `LineParser` (ida e volta: format → parse = original)
- [ ] `RingBuffer<T, N>`
- [ ] `TagLevelFilter`
- [ ] Testes Unity para tudo, incluindo casos de borda (truncamento, tag longa, buffer cheio)
- **Pronto quando:** cobertura dos módulos do núcleo e CI verde.

### M3 · Ferramenta host 🖥️  *(antecipado — não depende do M2)*
- [ ] `parser.py` (4 tipos de linha, ARCHITECTURE §6)
- [ ] `filters.py` com sintaxe `TAG:L`
- [ ] `render.py` com cores
- [ ] `esp-logcat replay <arquivo>` + fixtures em `tests/fixtures/`
- [ ] Testes de compatibilidade: as mesmas linhas de exemplo validadas pelo parser C++ e pelo Python
- **Pronto quando:** `replay` de um log de exemplo filtra e colore corretamente.

### M4a · Persistência — lógica 🖥️
- [ ] Interface `FileSystem` + implementação fake em memória para testes
- [ ] `RotatingStore`: rotação, limite por arquivo, limite total, descarte
- **Pronto quando:** testes de rotação cobrindo encher, girar e descartar.

### M5a · Console — lógica 🖥️
- [ ] `CommandParser` para todos os comandos do PRD (F9)
- [ ] Testes de comandos válidos, inválidos e com argumentos errados

---

## Fase 2 — com hardware

### Chegada das placas 🔌
- [ ] Identificar qual USB-C é o USB-Serial-JTAG nativo e qual é a ponte UART (e o chip da ponte)
- [ ] Confirmar tamanho da flash (`esptool.py flash_id`)
- [ ] Gravar o "hello" e ver saída nas duas portas

### M2 · Captura e escrita 🔌
- [ ] `IdfLogHook`, `FreeRtosLogQueue`, `WriterTask`, `UsbJtagSink`, `EspClock`
- [ ] Proteção contra recursão e contador de descartes
- [ ] App de demo com tarefa crítica de 1 kHz alternando um GPIO
- [ ] **Medição:** jitter com analisador lógico (PulseView) + `esp_timer` interno; definir o X do critério 3
- [ ] `esp-logcat stream` funcionando com a placa

### M4b · Persistência — integração 🔌
- [ ] `LittleFsFileSystem`, `NvsBootInfo`
- [ ] Linha de boot com contador, motivo do reset e versão
- [ ] Política de flush (periódico + ERROR + comando)

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
