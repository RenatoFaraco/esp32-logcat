# Arquitetura — esp32-logcat

> **O quê / por quê** → `PRD.md` · **Quando** → `ROADMAP.md` · **Como escrever** → `CONVENTIONS.md`
> Este documento descreve **como** o sistema funciona por dentro.

## 1. Visão geral

```
                         ┌──────────────────── ESP32-C3 ─────────────────────┐
  código da aplicação    │                                                   │
  ESP_LOGI(TAG, ...)  ───┼─► [IdfLogHook] ──vsnprintf──► LogRecord           │
  logs do próprio IDF ───┤      (contexto de quem loga)      │               │
                         │                                   ▼ xQueueSend(0) │
                         │                         ┌──── LogQueue ────┐      │
                         │                         └────────┬─────────┘      │
                         │                                  ▼                │
                         │                          [WriterTask] prio baixa  │
                         │              ┌───────────┬───────┴──────┬─────────┤
                         │              ▼           ▼              ▼         │
                         │        UsbJtagSink   RingBuffer   RotatingStore   │
                         │        (console)      (RAM)      (LittleFS/flash) │
                         │              │        LastWords (RTC_NOINIT)      │
                         │        [Console] ◄── comandos: info, log dump ... │
                         └──────────────┼────────────────────────────────────┘
                                        │ USB (texto)
                         ┌──────────────▼──────── PC ────────────────────────┐
                         │ esp-logcat: Transport → Parser → Filter → Render  │
                         │             (serial | arquivo)        (terminal,  │
                         │                                       arquivo)    │
                         └───────────────────────────────────────────────────┘
```

Princípio central: **quem loga paga o mínimo e nunca espera.** Todo I/O lento
(USB, flash) acontece na `WriterTask`.

## 2. Camadas (ports & adapters)

```
firmware/
  lib/logcat_core/   NÚCLEO — C++17 puro, zero dependência de plataforma
  src/adapters/      ADAPTADORES — implementam as interfaces com IDF/FreeRTOS/LittleFS
  src/app/           APP DE DEMO — gera logs, crash de teste, tarefa crítica de 1 kHz
```

O núcleo compila e é testado no PC (`env:native`). Os adaptadores só existem no alvo.
Regra: **dependência aponta para dentro** — adaptador conhece o núcleo, núcleo não
conhece adaptador.

### 2.1 Núcleo (`lib/logcat_core`)

| Módulo | Responsabilidade |
|--------|------------------|
| `LogRecord` | Struct de tamanho fixo: `boot`, `seq`, `uptime_ms`, `level`, `tag[16]`, `msg[N]`, `truncated` |
| `LineFormatter` | `LogRecord` → linha de texto no formato do PRD §3 |
| `LineParser` | Linha → `LogRecord` (usado no firmware para ler a flash e espelhado no host) |
| `RingBuffer<T, N>` | Buffer circular estático, sobrescreve o mais antigo. Sem construtor (pode viver em `RTC_NOINIT`), `is_consistent()` para validar memória que sobreviveu a reset; **não** é thread-safe |
| `TagLevelFilter` | Nível por tag + nível padrão, tabela estática (ex.: 16 tags) |
| `RotatingStore` | Lógica de rotação: arquivo corrente, limite por arquivo e total, descarte do mais antigo — sobre a interface `FileSystem` |
| `CommandParser` | Linha de comando do console → `Command` (enum + argumentos) |
| `Sequencer` | Contador `seq` por boot e contador de linhas descartadas |

### 2.2 Interfaces (ports) definidas no núcleo

```cpp
struct Clock      { virtual uint32_t uptime_ms() = 0; };
struct Sink       { virtual void write(const char* line, size_t len) = 0;
                    virtual void flush() = 0; };
struct FileSystem { virtual bool open_append(const char* path) = 0; /* ...
                    write, size, remove, rename, list */ };
struct BootInfo   { virtual uint32_t boot_count() = 0;
                    virtual ResetReason reset_reason() = 0; };
```

### 2.3 Adaptadores (`src/adapters`)

| Adaptador | Implementa | Base |
|-----------|-----------|------|
| `IdfLogHook` | entrada de logs | `esp_log_set_vprintf` |
| `FreeRtosLogQueue` | fila não bloqueante | `xQueueSend(..., 0)` |
| `UsbJtagSink` | `Sink` | `usb_serial_jtag` driver |
| `UartSink` | `Sink` | `uart` driver (porta da ponte USB-UART) |
| `LittleFsFileSystem` | `FileSystem` | `esp_littlefs` na partição `logs` |
| `NvsBootInfo` | `BootInfo` | NVS + `esp_reset_reason()` |
| `RtcLastWords` | último suspiro | buffer em `RTC_NOINIT_ATTR` |
| `EspClock` | `Clock` | `esp_timer_get_time()` |

## 3. Caminho de um log (detalhes que importam)

1. **Captura.** `IdfLogHook` recebe `(fmt, va_list)`. O `va_list` **não pode atravessar
   tarefas**, então a formatação da *mensagem* (`vsnprintf`) acontece no contexto de
   quem logou, num buffer local de tamanho fixo. É o único custo pago pelo chamador —
   documentado e medido no M2.
2. **Enfileiramento.** `LogRecord` montado e enviado com timeout **zero**.
   Fila cheia → descarta e incrementa `dropped`. Na próxima linha que passar, a
   `WriterTask` emite `W LOGCAT: dropped N lines`. Nunca bloquear.
3. **Escrita.** `WriterTask` (prioridade baixa) consome a fila e envia a cada sink.
4. **Flush para flash.** Periódico (5 s) **ou** imediato se nível ≥ ERROR **ou** por comando.
5. **Último suspiro.** Toda linha também vai para um mini ring em `RTC_NOINIT`
   (sobrevive a panic/WDT/reset por software, **não** a falta de energia).
   Protegido por número mágico + CRC; validado no boot e despejado no log como
   `I LASTWORDS: ...`.

### Armadilhas tratadas

| Problema | Solução |
|----------|---------|
| Recursão: LittleFS ou driver USB logam dentro da `WriterTask` | Flag de reentrada: logs gerados pela própria `WriterTask` vão direto ao sink USB, sem fila |
| Log em ISR | `ESP_LOGx` é proibido em ISR; usar `ESP_DRAM_LOGx` (não passa pelo hook) ou sinalizar uma tarefa |
| Logs antes do hook | Logs do bootloader e do início do app saem pelo console padrão; o hook é instalado o mais cedo possível em `app_main` |
| Mensagem maior que `msg[N]` | Trunca e marca `truncated`; a linha mostra até 124 chars + `...` (ADR-006) |
| `\n` ou bytes não ASCII na mensagem | O formatter troca por `?`: uma mensagem nunca vira duas linhas (ADR-006) |
| Desgaste da flash | Escrita em lote no flush; LittleFS faz *wear leveling* |
| Corte de energia no meio da escrita | LittleFS é resiliente a falha de energia; perde-se no máximo a janela de flush |

## 4. Boot

```
app_main()
 ├─ NvsBootInfo: boot_count++ , reset_reason
 ├─ instala IdfLogHook + cria fila + WriterTask
 ├─ RtcLastWords: valida → despeja linhas do boot anterior → reinicia buffer
 ├─ LittleFsFileSystem: monta partição "logs" (formata se corrompida)
 ├─ log: I LOGCAT: boot=<n> reason=<PANIC|WDT|POWERON|SW|...> fw=<versão>
 └─ Console + app de demo
```

## 5. Persistência em flash

- Partição `logs` (512 KB, LittleFS).
- Arquivos `log_000.txt` … `log_NNN.txt`; corrente = maior índice.
- Limite por arquivo: 64 KB. Limite total: 448 KB (folga para metadados do FS).
- Ao exceder o total: remove o arquivo de menor índice.
- Conteúdo: as mesmas linhas de texto do console → o host lê com o mesmo parser.

## 6. Protocolo no fio (USB)

Tudo é texto, linha a linha (`\n`). Três tipos de linha, distinguíveis pelo início:

| Tipo | Formato | Exemplo |
|------|---------|---------|
| Log | `<boot>:<seq> <uptime> <L> <TAG>: <msg>` | `0042:000137 0000123456 W PRESSURE: fora da faixa` |
| Resposta de comando | `#> <texto>` | `#> boot=42 reason=PANIC fw=0.3.0` |
| Bloco de dump | `#BEGIN <arquivo> <bytes>` … linhas … `#END <crc32>` | |

O host separa os três tipos; linhas que não casam com nenhum são exibidas como "raw"
(ex.: saída do bootloader).

## 7. Ferramenta host (`host/`)

```
src/esp_logcat/
  cli.py          argparse: stream | dump | shell | replay
  transport.py    SerialTransport (pyserial) | FileTransport (replay)
  parser.py       linha → LogLine | CommandReply | DumpBlock | Raw
  filters.py      sintaxe estilo logcat: "*:W" "PRESSURE:D"
  render.py       cores por nível (rich), timestamp do PC ao lado do uptime
  session.py      grava a sessão em arquivo
```

Dependências: `pyserial`, `rich`. Parser e filtros são funções puras → fáceis de testar.

## 8. Orçamento de memória (estimativa inicial, validar no M2)

| Item | RAM |
|------|-----|
| Fila: 32 × `LogRecord` (~160 B) | ~5 KB |
| RingBuffer: 64 linhas | ~10 KB |
| Stack da `WriterTask` | 4 KB |
| LastWords (RTC) | 2 KB |
| **Total** | **~21 KB** de ~400 KB do C3 |

## 9. Decisões registradas

Decisões relevantes viram ADRs em `docs/decisions/` (modelo em `CONVENTIONS.md`).
Iniciais:

- ADR-001: ESP-IDF via PlatformIO (C++17) em vez de framework Arduino
- ADR-002: Formato de linha em texto legível na v1 (binário fica para a v2)
- ADR-003: Núcleo independente de plataforma (ports & adapters) testado em `native`
- ADR-004: Descartar logs com fila cheia em vez de bloquear
