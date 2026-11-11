# esp32-logcat — contexto para o Claude

Sistema de logs persistente para firmware embarcado, estilo `adb logcat`.
Leia o PRD da versão corrente (`docs/context/prd/v1.md`) e, se a tarefa toca o que passa
pela serial ou pela flash, a spec do fio (`docs/context/spec/wire-format.md`) antes de
qualquer tarefa de implementação.

## Sobre o dono
Renato: engenheiro eletricista, forte em Python/data science, aprendendo embarcados
profissionais. Atue como mentor: explique o porquê das decisões de firmware
(concorrência, memória, flash, ISR). Português nas explicações; código, nomes e
commits em inglês.

## Ambiente
- Windows nativo (não WSL) para firmware — USB funciona direto.
- PlatformIO no VS Code. Python 3.12 com `.venv` na pasta `host/`.
- Placa alvo: ESP32-C3 rev v0.4 com 4 MB de flash embutida e duas USB-C: uma no
  USB-Serial-JTAG nativo, outra numa ponte CH340K na UART0. Hardware chegou em 06/nov/2026;
  fatos, portas e armadilhas em `docs/context/hardware.md` (leia antes de mexer em gravação/serial).
- Portas neste PC: COM5 = CH340K (UART0), COM6 = USB-Serial-JTAG nativo (via hub). O número da
  COM muda de máquina para máquina: nunca fixar no `platformio.ini`.
- Núcleo e host continuam testados no PC (`env:native`, pytest); a placa valida adaptadores.

## Estrutura
```
esp32-logcat/
├── CLAUDE.md
├── README.md
├── LICENSE                   MIT
├── .gitignore
├── .claude/
│   ├── agents/               subagentes do projeto (ver "Subagentes")
│   └── settings.json         permissões dos comandos de teste
├── .github/
│   ├── workflows/ci.yml      testes native + pytest + validação dos templates
│   └── ISSUE_TEMPLATE/       formulários de issue
├── conformance/              casos do fio lidos pelo C++ e pelo Python: linhas, pedidos, respostas
├── scripts/                  github_bootstrap.py (GitHub), serial_capture.py (boot nas 2 portas)
├── docs/                     índice em docs/README.md
│   ├── project/              como trabalhamos: MANUAL (pt/en), CONVENTIONS, ROADMAP, imagens
│   └── context/              o que o sistema é e por quê
│       ├── prd/          um PRD por versão: v1 (corrente), v2, v2.x, v3 (rascunhos)
│       ├── spec/         wire-format.md: linha de log, respostas, dump, sessão
│       ├── ARCHITECTURE.md
│       ├── hardware.md       placa real: chip, flash, portas, armadilhas
│       └── decisions/        ADRs curtos (uma decisão por arquivo)
├── firmware/                 projeto PlatformIO
│   ├── platformio.ini        envs: esp32c3 (alvo) e native (testes no PC)
│   ├── partitions.csv
│   ├── lib/logcat_core/      núcleo C++ SEM dependência do ESP-IDF
│   │   ├── include/logcat/
│   │   └── src/
│   ├── src/                  adaptadores IDF/FreeRTOS/LittleFS + app de demo
│   └── test/                 testes Unity (env:native)
└── host/                     ferramenta Python `esp-logcat`
    ├── pyproject.toml
    ├── src/esp_logcat/       backend (transport, parser, filtros) + render; tui/ (Textual)
    └── tests/
        └── fixtures/         logs de exemplo para testes e replay
```

## Regras de arquitetura
- Núcleo (`lib/logcat_core`) não inclui nada de `esp_*`, `freertos/*` ou LittleFS.
  Dependências de plataforma entram por interfaces (ports & adapters).
- Quem loga nunca bloqueia: só enfileira. Formatação e I/O ficam na tarefa escritora.
- Nada de alocação dinâmica no caminho de log depois da inicialização.
- C++17, sem exceções nem RTTI no firmware.
- Formato no fio definido em `docs/context/spec/wire-format.md` (§1 linha de log, §2–§3
  console e dump, §4 sessão) — mudança de formato exige atualizar a spec, o parser do host
  e os testes dos dois lados no mesmo commit, a começar pelos casos compartilhados
  (`conformance/make_line_cases.py` → `line_cases.tsv`, `make_request_cases.py` →
  `request_cases.tsv`, `make_reply_cases.py` → `reply_cases.tsv`).
- "PRD" sem versão quer dizer o PRD da v1 (`prd/v1.md`); os IDs F1–F9 e H1–H7 são dele.

## Fluxo de trabalho
1. Plano antes de editar (modo de planejamento) para qualquer coisa além de ajuste trivial.
2. Uma tarefa por vez, ligada a um marco do PRD (M0…M7).
3. Teste primeiro quando der (núcleo e host). Rodar:
   - `pio test -e native` (em `firmware/`)
   - `pytest` (em `host/`, com `.venv` ativo)
4. Commit pequeno por passo que funciona, mensagem convencional (`feat:`, `fix:`, `test:`…).
5. Nunca commitar segredos (Wi-Fi, chaves) — usar arquivo local ignorado pelo Git.

## Subagentes (`.claude/agents/`)
| Agente | Quando delegar | Pode |
|--------|----------------|------|
| `test-runner` | Depois de mudar `firmware/` ou `host/`; antes de commitar | Rodar native + pytest (alvo só se `src/`/build mudou) |
| `firmware-reviewer` | Depois de escrever/alterar C++ em `firmware/`, antes do PR | Só ler |
| `host-reviewer` | Depois de escrever/alterar Python em `host/` (backend ou TUI), antes do PR | Só ler |
| `persistence-auditor` | Mudou formato da linha, parser, buffer, flash, NVS, RTC ou partições | Só ler |
| `milestone-checker` | Antes de fechar marco ou abrir PR de release | Só ler |

Revisores e auditores não editam: os achados voltam para a conversa principal, que decide
e corrige. Mudou uma regra do projeto (CLAUDE.md, CONVENTIONS, PRD)? Revise os prompts dos
agentes no mesmo PR.
