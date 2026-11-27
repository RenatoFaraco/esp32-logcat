# ADR-013: Build do firmware em ESP-IDF puro (`idf.py`), sem PlatformIO
- Status: aceita
- Data: 2026-11-26
- Substitui: ADR-001 (só a parte "via PlatformIO"; a escolha do ESP-IDF em vez do Arduino continua)

## Contexto
O ADR-001 escolheu o ESP-IDF com o PlatformIO como sistema de build. Na prática, o PlatformIO
virou uma camada a mais entre o projeto e o IDF:
- O destino do projeto é um componente no IDF Component Registry (ADR-009), e quem adota um
  componente usa `idf.py`, não o PlatformIO. Hoje o projeto não é construído do jeito que o
  público vai consumi-lo.
- O `platformio.ini` mistura conceitos do PlatformIO (`env:native`, `test_filter`,
  `extra_scripts`) com o build CMake do IDF, que já existe por baixo (`firmware/CMakeLists.txt`).
- A versão do IDF vem amarrada à versão da plataforma `espressif32`, e não é escolhida
  diretamente.
- O esp32-devkit-tester, feito depois, mostrou que `idf.py` mais CMake puro resolvem o mesmo
  problema com menos peças.

O custo da migração é quase todo de arquivos de build e CI, não de código: o núcleo e os
adaptadores não mudam. Por isso a migração é feita agora (marco M5.5), antes do M6 e da v2.
Esperar não deixa a migração mais cara, mas cada marco feito no PlatformIO é mais um conjunto
de envs e scripts para portar.

**A linguagem não muda.** O firmware continua em C++17, sem exceções nem RTTI (ADR-005). O
ESP-IDF aceita componentes em C++. O que o público de C precisa é de uma API C na fronteira do
componente, não de um núcleo reescrito em C.

## Decisão
1. O firmware passa a ser construído só com o ESP-IDF (`idf.py`), fixado na versão que o build
   atual já usa (IDF 6.1, `dependencies.lock`). O PlatformIO sai do projeto.
   Na #91 a versão exata ficou **6.1.1**: o instalador da Espressif (EIM) oferece só a
   última correção de cada versão, e o CI usa a mesma (`espressif/idf:v6.1.1`). Uma
   versão de correção não muda APIs nem Kconfig; o comportamento foi conferido na placa.
2. A estrutura de diretórios do componente, que o ADR-009 §6 deixava para a v2, vem para agora:

   ```
   firmware/
   ├── components/esp32_logcat/     o componente publicável
   │   ├── CMakeLists.txt           idf_component_register
   │   ├── idf_component.yml        dependências (littlefs)
   │   ├── include/esp32_logcat.h   API pública em C (extern "C")
   │   ├── core/                    o núcleo de hoje (lib/logcat_core): sem IDF (ADR-003)
   │   └── port/                    os adaptadores de hoje (src/adapters)
   ├── examples/demo/               projeto IDF: app de demo, sdkconfig.defaults, partitions.csv
   └── test/
       ├── host/                    testes do núcleo no PC: CMake + Unity + ctest
       └── target/                  app de teste na placa (hoje test_target_littlefs/nvs)
   ```

   O multichip continua no ADR-009 e na v2: aqui muda só onde os arquivos moram.
3. **API C.** `esp32_logcat.h` expõe a inicialização e os comandos usados pela aplicação
   (`esp32_logcat_init`, `esp32_logcat_sync`…), com structs de configuração em C. Por dentro
   continua C++.
4. **Testes do núcleo no PC** (o antigo `env:native`): projeto CMake próprio em `test/host/`,
   com Unity fixado por versão e os mesmos flags estritos do ADR-005 (`-std=c++17
   -fno-exceptions -fno-rtti -Wall -Wextra -Werror`). Presets do CMake (`CMakePresets.json`):
   `native`, `asan` (ASan + UBSan) e `tsan`, com as mesmas opções dos sanitizers do CI atual.
5. **Testes na placa**: um app de teste IDF com Unity, rodado com `idf.py -p COMx flash monitor`.
   A automação com `pytest-embedded` fica para quando houver mais de uma placa (v2).
6. **Variantes do build** (o antigo `env:esp32c3_timing`): uma opção no Kconfig do exemplo,
   ligada por um segundo arquivo de defaults (`sdkconfig.timing`), em vez de `-D` solto.
7. **CI**: o job do firmware roda na imagem oficial `espressif/idf` com a versão fixada. Os jobs
   do núcleo usam CMake e o compilador do runner, sem o IDF.

## Consequências
- O projeto passa a ser construído como o público vai consumi-lo, e a publicação no Registry
  (v2) não precisa de outra mudança de estrutura.
- No Windows, o IDF é instalado pelo instalador da Espressif (ou pela extensão ESP-IDF do
  VS Code), e não mais pela extensão do PlatformIO.
- `idf.py monitor` mexe em DTR/RTS ao abrir a porta. Na CH340K, RTS é o EN e DTR é o GPIO9
  (`hardware.md`, item 4): o `esp-logcat` continua sendo a ferramenta de leitura, e o monitor
  do IDF só é usado com cuidado.
- `CLAUDE.md`, `CONVENTIONS.md`, `CONTRIBUTING.md`, `README.md`, o MANUAL, os subagentes
  (`test-runner`, `firmware-reviewer`, `milestone-checker`), o `.claude/settings.json`, os
  templates de issue e de PR e o ADR-005 deixam de citar `pio`/`env:native`; tudo isso é
  atualizado no PR da migração (conferência: `git grep -nE 'pio (run|test)|env:native'` vazio fora dos ADRs e do
  CHANGELOG).
- O ADR-009 continua "proposta": este ADR antecipa a estrutura, não o multichip.
