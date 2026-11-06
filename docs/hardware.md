# Hardware — placa de desenvolvimento

> Levantado no bring-up (issue #9), 06/nov/2026. Lista de compras e uso: `PRD.md` §6.

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

Saída do console: o IDF manda o console para a UART0 e, como secundário, para a USB-Serial-JTAG
(padrão do C3). As duas portas recebem **a mesma saída**, inclusive o bootloader da ROM.

## Gravar e monitorar

```sh
cd firmware
pio run -e esp32c3 -t upload --upload-port COM5   # ou a porta nativa (COM6): as duas gravam
pio device monitor --port COM5 -b 115200
```

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

Ao abrir a COM da USB-Serial-JTAG, os primeiros bytes podem ser **restos do boot anterior**
(um pedaço de linha). O host deve tolerar isso: o parser classifica como linha "raw".

**4. Ponte CH340K: cuidado ao abrir a porta**

Na ponte, `RTS` controla o EN (reset) e `DTR` o GPIO9 (modo de gravação). Abrir a porta com
essas linhas ativas segura o chip em reset ou o joga no modo de download. Ferramentas devem
abrir com `DTR` e `RTS` soltos.

**5. Memória RTC usada pelo heap**

O boot mostra `RTCRAM` (~7 KiB em `0x50000024`) entregue ao heap. O buffer de último suspiro
(M6, `RTC_NOINIT`) ocupa parte dessa memória e o heap cede o espaço: orçamento de ~2 KB (#31).

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
`LineFormatter` no hardware real, no formato do PRD §3.

## Pendências

- [ ] Testar QIO (a flash XMC embutida provavelmente suporta): ganho pequeno de velocidade de
  leitura; só vale com medição.
- [ ] Mapear quais GPIOs estão livres nesta placa para o pino de medição de jitter (#13/#14).
