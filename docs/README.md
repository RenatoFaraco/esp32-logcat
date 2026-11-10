# Documentação

Dois grupos, separados por pergunta:

- **`project/`: como trabalhamos.** Para quem usa a ferramenta ou contribui: manuais,
  convenções e o roadmap.
- **`context/`: o que o sistema é e por quê.** Para quem vai mudar o código, sejam pessoas ou
  agentes: requisitos, arquitetura, a placa e as decisões.

Regra para um documento novo: se ele responde "como faço?", vai em `project/`. Se responde
"o que é?" ou "por que é assim?", vai em `context/`.

## `project/`: como trabalhamos

| Documento | Para quê |
|-----------|----------|
| [MANUAL.md](project/MANUAL.md) / [MANUAL.en.md](project/MANUAL.en.md) | Manual de uso (pt-BR e inglês): do zero ao logcat rodando, leitura da saída, problemas comuns |
| [CONVENTIONS.md](project/CONVENTIONS.md) | Idioma, branches, commits, estilo de C++ e Python, modelo de ADR |
| [ROADMAP.md](project/ROADMAP.md) | Marcos M0…M7, o que falta em cada um e o que vem depois da v1 |
| `manual/sessions/`, `manual/img/` | Sessões gravadas na placa e as imagens do manual geradas a partir delas (`scripts/manual_screenshots.py`) |
| `img/` | Capa do README (`scripts/readme_cover.py`) |

## `context/`: o que o sistema é e por quê

| Documento | Para quê |
|-----------|----------|
| [prd/v1.md](context/prd/v1.md) | **PRD corrente.** Requisitos da v1: problema, escopo (F1–F9, H1–H7), persistência, critérios de aceite, marcos M0…M7 |
| [prd/v2-multichip.md](context/prd/v2-multichip.md) | Rascunho: componente ESP-IDF multichip (ADR-009) |
| [prd/v2x-go-host.md](context/prd/v2x-go-host.md) | Rascunho: host em Go, binário único (ADR-010) |
| [prd/v3-rust-telemetry.md](context/prd/v3-rust-telemetry.md) | Rascunho: crate Rust (ADR-011) e telemetria por MQTT |
| [spec/wire-format.md](context/spec/wire-format.md) | O contrato no fio, para todas as implementações: linha de log (§1), respostas do console (§2), bloco de dump (§3), arquivo de sessão (§4) |
| [ARCHITECTURE.md](context/ARCHITECTURE.md) | Módulos, fluxo de dados, concorrência, orçamento de memória, protocolo do console |
| [hardware.md](context/hardware.md) | A placa real: chip, flash, portas e armadilhas (leia antes de mexer em gravação ou serial) |

Um PRD por versão: o da versão em andamento é o corrente; os seguintes ficam em rascunho, só com
o que os ADRs e o ROADMAP já decidiram, e viram "aprovado" quando a versão começar. "PRD" sem
versão, no código e nos documentos, quer dizer `prd/v1.md`. O formato no fio não é de uma
versão: mora na spec, que todos os PRDs citam.

### Decisões (`context/decisions/`)

| ADR | Decisão |
|-----|---------|
| [001](context/decisions/001-esp-idf-via-platformio.md) | ESP-IDF via PlatformIO em vez do framework Arduino |
| [002](context/decisions/002-text-line-format.md) | Formato de linha em texto legível na v1 |
| [003](context/decisions/003-platform-independent-core.md) | Núcleo independente de plataforma (ports & adapters) testado em `native` |
| [004](context/decisions/004-drop-logs-when-queue-full.md) | Descartar logs com fila cheia em vez de bloquear |
| [005](context/decisions/005-strict-cpp17-on-core-only.md) | C++17 estrito só no núcleo, via `env:native` |
| [006](context/decisions/006-printable-ascii-lines.md) | Linhas de log só com ASCII imprimível |
| [007](context/decisions/007-no-per-line-checksum-v1.md) | Sem checksum por linha na v1 |
| [008](context/decisions/008-host-frontends-tui.md) | Host em camadas, com TUI (Textual) como segundo frontend |
| [009](context/decisions/009-v2-espressif-multichip-component.md) | v2 como componente ESP-IDF multichip, só na família Espressif |
| [010](context/decisions/010-go-host-with-conformance-suite.md) | Host em Go na v2.x, validado por uma suíte de conformidade |
| [011](context/decisions/011-rust-firmware-crate.md) | Firmware em Rust na v3, como crate `no_std` paralelo |
| [012](context/decisions/012-console-reply-framing.md) | Moldura das respostas do console (id, fim de resposta, erro, versão) |

Um ADR novo segue o modelo em [CONVENTIONS.md](project/CONVENTIONS.md) e entra nesta tabela no
mesmo commit.
