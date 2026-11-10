# ADR-005: C++17 estrito só no núcleo, via `env:native`
- Status: aceita
- Data: 2026-10-20

## Contexto
O projeto adota C++17 (CLAUDE.md). O ESP-IDF compila C++ com um padrão mais novo por padrão
(`-std=gnu++26` no IDF 6.x usado aqui) e seus próprios headers e componentes são testados nesse modo. Forçar `-std=c++17` no
alvo mexeria no build do IDF inteiro, sem ganho real para os adaptadores, que só existem nele.

## Decisão
- `env:native` compila o núcleo e os testes com `-std=c++17 -fno-exceptions -fno-rtti
  -Wall -Wextra -Werror`. As flags exclusivas de C++ entram por `CXXFLAGS`
  (`firmware/scripts/native_cxx_flags.py`), porque `build_flags` também chega aos fontes C
  da Unity, onde `-fno-rtti` é inválido.
- Um teste (`test_toolchain`) verifica `__cplusplus == 201703L`.
- `env:esp32c3` usa o padrão do IDF; exceções e RTTI ficam desligadas em `sdkconfig.defaults`.

## Consequências
- O núcleo fica garantidamente em C++17: um recurso mais novo quebra o CI.
- Os adaptadores podem usar o que o IDF oferece; a regra "C++17" vale de fato só para o núcleo.
- O núcleo é compilado com dois padrões (17 no PC, 26 no alvo); diferenças são raras e
  aparecem no build do alvo, que também roda no CI.
