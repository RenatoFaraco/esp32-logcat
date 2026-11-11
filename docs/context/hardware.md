# Hardware — placa de desenvolvimento

> Levantado no bring-up (issue #9), 06/nov/2026. Lista de compras e uso: `prd/v1.md` §6.

## Placa

| Item | Valor | Como foi verificado |
|------|-------|---------------------|
| Chip | ESP32-C3 (QFN32), **revisão v0.4** | `esptool.py chip_id` |
| Cristal | 40 MHz | `esptool.py chip_id` |
| Flash | **4 MB embutida no encapsulamento**, fabricante XMC (ID `0x46`, dispositivo `0x4016`) | `esptool.py flash_id` |
| Modo de flash | DIO (configurado; QIO não testado) | log do bootloader: `SPI Mode: DIO` |
| MAC | `80:53:e0:0a:5d:ac` (só desta unidade) | `esptool.py chip_id` |
| Estado de fábrica | flash **totalmente apagada** (`0xFF` a partir de `0x0`) — sem bootloader nem firmware | `esptool.py read_flash 0x0 0x20` |
| ESP-IDF | 6.1.0 (via `espressif32 @ 7.1.3`) | banner do bootloader |

A tabela de partições (`firmware/partitions.csv`, 4 MB) cabe e foi confirmada no boot:
`logs` 512 KB em `0x190000`, `coredump` 64 KB em `0x210000`.

## As duas USB-C

| Porta | Ligação no chip | Identificação no Windows | COM neste PC |
|-------|-----------------|--------------------------|--------------|
| Ponte UART | **CH340K** → UART0 (GPIO 20/21) | "USB-SERIAL CH340K", `VID_1A86&PID_7522` | COM5 |
| Nativa | **USB-Serial-JTAG** do próprio chip | "Dispositivo Serial USB", `VID_303A&PID_1001` (driver genérico) | COM6 (via hub) |

O número da COM muda de PC para PC e de porta para porta: por isso não está no
`platformio.ini`. Para achar as portas:

```powershell
Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match '\(COM\d+\)' } |
  ForEach-Object { '{0} | {1}' -f $_.Name, $_.DeviceID }
```

O que sai em cada porta (desde #11/#12, firmware com `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y`):

| | Nativa (COM6) — o stream do logcat | Ponte UART (COM5) — o console |
|---|---|---|
| ROM, bootloader e logs do início do app (formato do IDF) | sim (a ROM imprime nas duas) | sim |
| Linhas do logcat (formato da spec, wire-format §1) | sim, pelo driver, só `\n` | sim, espelho, com `\r\n` |
| `printf` e texto que não é log | não | sim |
| Avisos `W LOGCAT: ...` (descartes) | sim | sim |

Antes, o IDF mandava o console também para a USB-Serial-JTAG como secundário (padrão do C3), e
as duas portas recebiam a mesma saída. O console secundário bloqueava ~50 ms por linha quando
ninguém lia a porta nativa (#12) e disputaria a FIFO com o driver: foi desligado.

## Gravar e monitorar

```sh
cd firmware
pio run -e esp32c3 -t upload --upload-port COM5   # ou a porta nativa (COM6): as duas gravam
```

Para acompanhar a placa, use a ferramenta do projeto, que já trata as armadilhas abaixo
(abre com DTR/RTS soltos, normaliza CRLF, reconecta e acha a placa se a COM mudar):

```sh
host/.venv/Scripts/esp-logcat ports                    # lista as portas e marca a nativa
host/.venv/Scripts/esp-logcat stream                   # nativa, se houver só uma
host/.venv/Scripts/esp-logcat stream --port COM5 --reset --time
```

O stdout recebe só as linhas da placa; avisos (conexão, lacunas, reinício) e o resumo vão
para o stderr. `pio device monitor --port COM5 -b 115200` continua funcionando.

`scripts/serial_capture.py` reinicia a placa e grava o boot das duas portas ao mesmo tempo,
sem interação (útil para registrar evidências):

```sh
host/.venv/Scripts/python scripts/serial_capture.py COM5 COM6 6 boot
```

## Comportamentos que importam para o projeto

**1. Motivo de reset depende de como o reset acontece**

| Como reiniciou | Motivo no boot (`rst:`) |
|----------------|-------------------------|
| Pino EN (botão RESET, ou `RTS` da ponte CH340K) | `0x1 (POWERON)` |
| Via USB nativa (`RTS` na porta USB-Serial-JTAG) | `0x15 (USB_UART_CHIP_RESET)` |

Consequência para o registro de boot (F7, #25): **apertar o botão de reset e cortar a energia
geram o mesmo motivo** — o hardware não distingue. O reset pela USB nativa é distinguível.

**2. Reset pela USB nativa no Windows exige um truque**

O driver `usbser.sys` só envia ao dispositivo o estado das linhas de controle quando o `DTR`
muda. Mudar só o `RTS` não chega à placa. Depois de cada mudança de `RTS`, reescreva o `DTR`
com o mesmo valor (é o que o `esptool` faz). Relevante para o `transport.py` do host (#16).
Sequência de reset normal: `DTR` solto, `RTS` ligado por 200 ms, `RTS` solto.

**3. A porta nativa guarda saída enquanto ninguém lê**

Ao abrir a COM da USB-Serial-JTAG, os primeiros bytes podem ser **restos de antes** (um
pedaço de linha). Como eles chegam, e como o host trata isso: item 7.

**4. Ponte CH340K: cuidado ao abrir a porta**

Na ponte, `RTS` controla o EN (reset) e `DTR` o GPIO9 (modo de gravação). Abrir a porta com
essas linhas ativas segura o chip em reset ou o joga no modo de download. Ferramentas devem
abrir com `DTR` e `RTS` soltos.

**5. Reset pelo EN pode derrubar a USB nativa de vez (com hub)**

Gravar ou resetar pela ponte CH340K reinicia o chip pelo pino EN, o que derruba a USB nativa
de forma brusca. Com a placa no hub, o Windows às vezes falha ao reconectar: aparece
"Dispositivo USB Desconhecido (Falha na Solicitação de Descritor de Dispositivo)"
(`VID_0000&PID_0002`) e a COM da porta nativa some — um novo reset não resolve; só
desconectar e reconectar o cabo (visto em 06/nov). Para evitar:
- monitorando a porta nativa, grave **pela porta nativa** (o reset via USB-Serial-JTAG não
  derruba o link); ou monitore pela CH340K, que não cai no reset;
- prefira a porta nativa ligada direto no PC, sem hub.
A porta pode voltar com outro número de COM.

**6. Memória RTC usada pelo heap**

O boot mostra `RTCRAM` (~7 KiB em `0x50000024`) entregue ao heap. O buffer de último suspiro
(M6, `RTC_NOINIT`) ocupa parte dessa memória e o heap cede o espaço: orçamento de ~2 KB (#31).

**7. A porta nativa guarda o começo de uma linha e o gruda na próxima**

Medido em 07/nov: com ninguém lendo a USB nativa, os primeiros **64 bytes** da linha corrente
ficam presos na FIFO de hardware da USB-Serial-JTAG, e o resto da linha (e as seguintes) é
descartado pelo driver. Esses 64 bytes **não chegam ao abrir a porta**: só saem junto com a
próxima escrita da placa, colados nela, sem `\n` entre os dois. Exemplo real:

```
0000:000034 0000029110 I HEARTBEAT: tick 29, capture 12566 cycle0000:000051 0000046160 I HEARTB...
```

O resultado cabe em 170 bytes e é imprimível: passaria como uma `LogLine` válida, com
mensagem falsa e uma "lacuna" de 17 linhas. Depois de um reset, o pedaço gruda no `ESP-ROM:`.
A FIFO guarda os primeiros 64 bytes escritos: com linhas curtas, pode ser uma linha inteira e
o começo da seguinte. O host trata isso (`pipeline.split_glued`): nas linhas que começam nos
primeiros 64 bytes depois de conectar, corta onde começa outro cabeçalho ou o `ESP-ROM:`, e o
pedaço vira linha rasgada; uma lacuna logo depois de uma dessas linhas conta como "não
recebida antes da conexão", não como perda.

Desde #12 o firmware evita isso na origem: o `UsbJtagSink` escreve pelo driver, que copia a
linha inteira (ou nada) para o seu ring buffer, e o console secundário não escreve mais na FIFO.
Medido em 07/nov: depois de 40 s com a porta fechada, a primeira linha ao abrir já é a ao vivo,
seguida de `W LOGCAT: 39 lines not sent to usb: no reader` (o que estava no ring é descartado
pelo `pyserial`, que limpa o buffer de entrada ao abrir). O tratamento do host continua valendo
para firmwares antigos e para a FIFO da ROM.

**8. O driver da UART espera sem dormir, e `stdout` manda um byte por vez**

Visto em 10/nov (#28): sob carga, a tarefa escritora ficou presa para sempre em `uart_tx_all`,
com o IDLE sem CPU e o watchdog reclamando a cada 5 s. Duas coisas se somam no ESP-IDF 6.1:
- `uart_write_bytes` com o ring cheio fica num laço que **não dorme** esperando vaga;
- `stdout` (VFS) chama o driver **byte a byte**, e cada byte vira dois itens no ring.

O `ConsoleSink` agora escreve uma vez por linha, direto no driver, e só se couber; senão recusa,
e o `Writer` conta (`not_sent.console`). A consulta `uart_get_tx_buffer_free_size` soma os dois
trechos livres do ring, e uma escrita precisa de um só: por isso a checagem pede o dobro da linha.

Efeito medido na fase com espelho UART da #13: o atraso máximo do ISR do timer caiu de
**99 312 µs** (198 períodos perdidos) para **até 119 µs** (nenhum perdido). O caminho exato
que segurava as interrupções não foi isolado; a medição externa é da #14. Com esse caminho,
também apareceu 1 bloco de dump com bytes a mais em 200; depois da correção, 0 em 3491.

## Boot capturado (06/nov, reset pela USB nativa, trecho)

```
ESP-ROM:esp32c3-api1-20210207
rst:0x15 (USB_UART_CHIP_RESET),boot:0xc (SPI_FAST_FLASH_BOOT)
I (24) boot: ESP-IDF 6.1.0 2nd stage bootloader
I (41) boot.esp32c3: SPI Flash Size : 4MB
I (78) boot:  3 logs             Unknown data     01 83 00190000 00080000
I (85) boot:  4 coredump         Unknown data     01 03 00210000 00010000
I (169) cpu_start: GPIO 20 and 21 are used as console UART I/O pins
I (253) APP: formatted 49 bytes, LogRecord=160 bytes
0000:000000 0000000000 I APP: esp32-logcat hello
I (263) APP: parsed back: ok (tag=APP)
I (263) APP: ring buffer: 1/4, consistent=1
```

A linha `0000:000000 0000000000 I APP: esp32-logcat hello` é a primeira linha gerada pelo
`LineFormatter` no hardware real, no formato da spec (wire-format §1).

## Pendências

- [ ] Testar QIO (a flash XMC embutida provavelmente suporta): ganho pequeno de velocidade de
  leitura; só vale com medição.
- [x] Pinos de medição de tempo (#13/#14): **GPIO 4** (inverte no ISR do timer), **GPIO 5**
  (inverte quando a tarefa crítica roda) e **GPIO 6** (N+1 pulsos no início da fase N), em
  `firmware/src/app/pins.hpp`. Ficam fora dos strapping (2, 8, 9), da USB (18, 19), da UART0
  (20, 21) e da flash embutida (11–17). Os mesmos números da serigrafia: conferir antes de
  ligar o analisador.
