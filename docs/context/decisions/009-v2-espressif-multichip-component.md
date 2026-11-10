# ADR-009: v2 como componente ESP-IDF multichip, só na família Espressif
- Status: proposta
- Data: 2026-11-07
- Requisitos: [PRD v2](../prd/v2-multichip.md) (rascunho)

## Contexto
A v1 roda só no ESP32-C3. Para quem adota o projeto, o chip muda de produto para produto, e um
logcat preso a um chip só serve a um produto. O projeto é open source e pensado para o público
em geral: quem adota adapta ao seu produto, e as decisões não seguem as necessidades de um
produto ou empresa específica.

Havia dois candidatos para a v2 (ROADMAP, "Depois da v1"):
- **Telemetria por MQTT** (ponte com MLOps). Exige Wi-Fi, um broker e um backend. Feita antes
  do multichip, ficaria presa ao C3 e teria de ser refeita para cada chip.
- **Componente multichip publicado.** Não depende da telemetria. Com ele pronto, o MQTT vira
  só mais um `Sink` e funciona em todo chip que tenha rádio.

Suportar outros fabricantes já na v2 (STM32, RP2040, nRF) multiplicaria o esforço: outro
sistema de build (CMake puro, Zephyr), outros SDKs e, talvez, outro nome para o projeto.

## Decisão
1. A v2 é um componente ESP-IDF publicado no IDF Component Registry, multichip **dentro da
   família Espressif**. Outros fabricantes ficam para a v4.
2. Alvos:

   | Chip | O que traz para o teste | Placa |
   |------|--------------------------|-------|
   | ESP32-C3 | RISC-V single-core sem atômicos nativos (o IDF emula), USB-Serial-JTAG | DevKitM-1, em uso (`hardware.md`) |
   | ESP32 | Dual-core Xtensa, **sem USB nativa** (só UART), memória RTC | já disponível |
   | ESP32-S3 | Dual-core Xtensa, USB-Serial-JTAG **e** USB-OTG, PSRAM | a comprar |
   | ESP32-C6 | RISC-V com atômicos nativos, LP RAM no lugar da RTC, Wi-Fi 6 | a comprar |
   | ESP32-P4 | Dual-core RISC-V, USB-OTG HS, **sem rádio** | a comprar (confirmar a revisão suportada pelo IDF) |

   Fora: ESP32-S2 (o S3 cobre o que ele teria de interessante) e modelos mais antigos. Chips
   futuros entram um de cada vez, cada um com seu bring-up.
3. Recursos do chip são detectados pelas *capabilities* do SoC (`SOC_*`, em `soc/soc_caps.h`),
   não por `CONFIG_IDF_TARGET_*`: o código pergunta "o chip tem X?", não "qual é o chip?".
4. As opções do componente vêm do Kconfig (núcleo da `WriterTask`, profundidade da fila, rótulo
   da partição de logs…) e são lidas como `constexpr` (CONVENTIONS: nada de `#define`).
5. Cada periférico de saída tem seu adaptador de `Sink`: `UsbJtagSink` (já existe), `UartSink`
   (já previsto na ARCHITECTURE §2.3) e um sink novo para USB-OTG CDC.
6. O componente junta núcleo e adaptadores sem misturar os dois: o núcleo continua sem IDF
   (ADR-003). A estrutura de diretórios é definida no começo da v2.
7. Concorrência em dual-core:
   - O núcleo não muda: ele já usa atômicos com ordem de memória explícita, e o TSan no CI
     verifica isso.
   - Nos adaptadores: afinidade da `WriterTask` configurável, com padrão `tskNO_AFFINITY`, e
     criação ainda estática (`xTaskCreateStaticPinnedToCore`). Quem deve ser fixada num núcleo
     é a tarefa crítica da aplicação, não o logger.
   - O último suspiro continua com um único escritor (a `WriterTask`), sem lock e sem nenhuma
     escrita durante o panic: um spinlock seguro pelo outro núcleo no momento do panic
     travaria o sistema.

## Consequências
- Uma placa e um bring-up por alvo, cada um registrado no `hardware.md`. As armadilhas do C3
  mostraram que cada chip tem as suas.
- O custo do hook muda de chip para chip (atômicos emulados no C3, nativos no C6 e no S3) e
  precisa ser medido em cada alvo.
- `esp_cpu_get_cycle_count()` é por núcleo: em dual-core, a medição de ciclos do hook precisa
  tratar a migração da tarefa entre núcleos.
- Build do alvo: provavelmente uma matriz `idf.py set-target <chip>` no CI. O PlatformIO tem
  histórico de demorar a suportar chips novos e fica para os testes `native`. Esse ponto ganha
  ADR próprio no começo da v2.
- O hook exige `CONFIG_LOG_VERSION=1` (`idf_log_hook.cpp`). Para quem usa o componente, isso
  vira uma restrição documentada ou passa a exigir suporte ao log v2 do IDF.
- API pública e opções do Kconfig viram contrato com usuários externos: SemVer de verdade, e
  uma mudança nelas é *breaking change*.
- Um público global pede documentação em inglês. Quando e como migrar `docs/` (CONVENTIONS §1)
  é decidido no começo da v2.
- A telemetria (v3) entra sobre essa base. No P4, sem rádio, ela depende de um chip
  companheiro (`esp-hosted`).
