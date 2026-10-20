# ADR-003: Núcleo independente de plataforma (ports & adapters) testado em `native`
- Status: aceita
- Data: 2026-10-20

## Contexto
A maior parte da lógica (formatação, parser, ring buffer, filtros, rotação, comandos) não depende
do hardware. Testar tudo na placa é lento, exige o hardware (que só chega em novembro) e não roda
no CI. Além disso, o componente deve ser reaproveitável em outros produtos.

## Decisão
`firmware/lib/logcat_core` é C++ puro, sem incluir `esp_*`, `freertos/*`, `driver/*` ou LittleFS.
Dependências de plataforma entram por interfaces definidas no núcleo (`Clock`, `Sink`,
`FileSystem`, `BootInfo`) e implementadas por adaptadores em `firmware/src/adapters/`.
O núcleo é testado com Unity em `env:native` no PC e no CI.

## Consequências
- Testes rápidos e sem hardware; desenvolvimento do núcleo antes da placa chegar.
- Fronteira clara para reaproveitar o núcleo em outro MCU.
- Custo de chamadas virtuais nas interfaces (pequeno, fora do contexto de quem loga).
- O que é específico da plataforma (hook do `esp_log`, filas, flash) só é testado na placa.
