# Arquitetura — esp32-logcat

> **O quê / por quê** → `prd/v1.md` · **No fio** → `spec/wire-format.md` · **Quando** → `ROADMAP.md` · **Como escrever** → `CONVENTIONS.md`
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
| `LineFormatter` | `LogRecord` → linha de texto no formato da spec (wire-format §1) |
| `LineParser` | Linha → `LogRecord` (usado no firmware para ler a flash e espelhado no host) |
| `RingBuffer<T, N>` | Buffer circular estático, sobrescreve o mais antigo. Sem construtor (pode viver em `RTC_NOINIT`), `is_consistent()` para validar memória que sobreviveu a reset; **não** é thread-safe |
| `TagLevelFilter` | Nível por tag + nível padrão, tabela estática de 16 tags; `None` silencia. Um escritor (console), leitores sem lock em todo log; só load/store atômicos |
| `RotatingStore` | Lógica de rotação: arquivo corrente, limite por arquivo e total, descarte do mais antigo — sobre a interface `FileSystem` |
| `CommandParser` | Linha de pedido do console (`<id> <comando> [args]`) → `Command` (tipo, id a ecoar, tag e nível). Entende o pedido inteiro ou o rejeita (`bad_command`/`bad_args`); função pura |
| `ReplyFormatter` | Monta as linhas de resposta (`#>`, `#.`, `#!`, `#BEGIN`, `#END`) num buffer fixo e recusa montar uma linha malformada; CRC-32 do bloco de dump (tabela de 16 entradas) |
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
| Recursão: LittleFS ou driver USB logam dentro da `WriterTask` | O hook reconhece a própria `WriterTask` e manda o texto cru para o console (UART), sem fila: o sink USB pode ser justamente quem está falhando, e a fila formaria o laço escrever → logar → enfileirar → escrever |
| Host ausente na USB nativa | `UsbJtagSink` escreve com timeout 0 no ring buffer do driver (linha inteira ou nada); linhas recusadas são contadas e reportadas (`W LOGCAT: N lines not sent to usb`) quando o host volta |
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

Resumo. O contrato exato está em `spec/wire-format.md` (§1 linha de log, §2 respostas,
§3 bloco de dump); em caso de diferença, vale a spec.

Tudo é texto, linha a linha (`\n`), ASCII imprimível, no máximo 170 bytes por linha. O host
manda pedidos `<id> <comando> [args]` (id `1`–`9999`, ou `-` digitado à mão), e o
dispositivo responde ecoando o id (ADR-012):

| Tipo | Formato | Exemplo |
|------|---------|---------|
| Log | `<boot>:<seq> <uptime> <L> <TAG>: <msg>` | `0042:000137 0000123456 W PRESSURE: fora da faixa` |
| Resposta, dados | `#> <id> <chave=valor> ...` | `#> 7 fw=0.3.0 proto=1 uptime_ms=5123` |
| Resposta, fim ok | `#. <id> ok` | `#. 7 ok` |
| Resposta, fim com erro | `#! <id> <código> [texto]` | `#! 3 full tag table full (16 tags)` |
| Bloco de dump | `#BEGIN <id> <nome> <bytes>` … `<bytes>` bytes do arquivo … `#END <id> <crc32>` | `#BEGIN 4 log_003.txt 4096` |

Toda resposta termina com exatamente um `#.` ou `#!`, e um bloco de dump vem antes dele. O
conteúdo do bloco é o do arquivo byte a byte, inclusive lixo de flash e linhas rasgadas: o
host o lê pela contagem de bytes, confere o CRC-32 e passa as linhas pelo parser de log. Com um
bloco aberto, o log não vai para a USB (flash e RTC seguem). As respostas passam por uma fila
própria, que a tarefa escritora atende antes da fila de log (alternando com ela), e nunca
intercalam bytes com o log. Campo desconhecido é omitido, nunca zero. O host separa os
tipos; linha de protocolo malformada, ou que não casa com nenhum tipo, é exibida como "raw"
(ex.: saída do bootloader). Casos compartilhados: `conformance/reply_cases.tsv`.

## 7. Ferramenta host (`host/`)

```
transport (bytes) → parser (Line) → stats/filters → render (rich Text) → { cli stream | tui }
```

```
src/esp_logcat/
  cli.py          argparse: stream | replay | dump | shell | tui
  transport.py    SerialTransport (pyserial) | FileTransport (replay)
  parser.py       bytes → LogLine | InvalidLine | IdfLine | CommandReply | DumpBegin/End | RawLine
  stats.py        lacunas e reordenação de seq, uptime que regride, mudança de boot
  filters.py      sintaxe estilo logcat: "*:W" "PRESSURE:D"
  render.py       Line → rich.text.Text: cores por nível, timestamp do PC ao lado do uptime
  session.py      sessão salva (--save): cada linha com a data/hora do PC, avisos do
                  transporte; o replay a lê de volta e mostra o mesmo
  tui/            frontend interativo (Textual): log, filtro, contadores, estado da porta
```

Camadas (ADR-008): só `render` importa `rich` e só `tui/` importa `textual`; o resto é
Python puro, testado sem terminal. `stream` e `replay` continuam texto em stdout (pipe,
`> arquivo`); o TUI é um consumidor a mais do mesmo backend. O texto da placa é dado, nunca
markup: o renderer monta `Text` com estilos explícitos e troca caracteres de controle e de
formatação por escapes ASCII (`\x1b`, `\u202E`). O texto do `Text` é a linha original sem o
`\n`; quem imprime usa `soft_wrap=True` para o `Console` não quebrar a linha em 80 colunas
fora do terminal.

Dependências: `pyserial`, `rich`, `textual` (quando o TUI entrar). Parser, filtros e
estatísticas são funções puras → fáceis de testar.

## 8. Orçamento de memória (estimativa inicial, validar no M2)

| Item | RAM |
|------|-----|
| Fila: 32 × `LogRecord` (~160 B) | ~5 KB |
| RingBuffer: 64 linhas | ~10 KB |
| Stack da `WriterTask` | 4 KB |
| LastWords (RTC) | 2 KB |
| **Total** | **~21 KB** de ~400 KB do C3 |

## 9. Decisões registradas

Decisões relevantes viram ADRs em `docs/context/decisions/` (modelo em `CONVENTIONS.md`).
Iniciais:

- ADR-001: ESP-IDF via PlatformIO (C++17) em vez de framework Arduino
- ADR-002: Formato de linha em texto legível na v1 (binário fica para a v3)
- ADR-003: Núcleo independente de plataforma (ports & adapters) testado em `native`
- ADR-004: Descartar logs com fila cheia em vez de bloquear
- ADR-005: C++17 estrito só no núcleo, via `env:native`
- ADR-006: Linhas de log só com ASCII imprimível
- ADR-007: Sem checksum por linha na v1
- ADR-008: Host em camadas, com TUI (Textual) como segundo frontend

Propostas para depois da v1 (ROADMAP, "Depois da v1"):

- ADR-009: v2 como componente ESP-IDF multichip, só na família Espressif
- ADR-010: Host em Go na v2.x, validado por uma suíte de conformidade (substituirá o ADR-008)
- ADR-011: Firmware em Rust na v3, como crate `no_std` paralelo
