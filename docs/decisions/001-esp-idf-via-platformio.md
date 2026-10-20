# ADR-001: ESP-IDF via PlatformIO em vez do framework Arduino
- Status: aceita
- Data: 2026-10-20

## Contexto
O sistema precisa interceptar todo log do sistema (`esp_log_set_vprintf`), ler o motivo de
reset, usar NVS, memória `RTC_NOINIT`, LittleFS numa partição própria e o driver do
USB-Serial-JTAG. O framework Arduino esconde parte disso atrás de camadas próprias e fixa uma
versão do IDF com `sdkconfig` pré-compilado, sem permitir ajustar opções do sistema.

## Decisão
Usar o framework ESP-IDF diretamente, com o PlatformIO como sistema de build (VS Code, `env:native`
para testes no PC, CI simples). Versão da plataforma fixada em `platformio.ini` e opções do IDF em
`firmware/sdkconfig.defaults`.

## Consequências
- Acesso total às APIs do IDF e a `menuconfig`; o mesmo código serve a produtos da FSI-TECH.
- Build do IDF via CMake: `firmware/CMakeLists.txt` e `src/CMakeLists.txt` precisam ser mantidos.
- Menos exemplos prontos que no Arduino; curva de aprendizado maior (assumida de propósito).
- Atualizar a plataforma é uma decisão explícita (troca da versão fixada), não um efeito colateral.
