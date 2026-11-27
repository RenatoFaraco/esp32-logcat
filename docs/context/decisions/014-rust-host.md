# ADR-014: Host em Rust, antecipado para o M5.5
- Status: aceita
- Data: 2026-11-26
- Substitui: ADR-010 (host em Go na v2.x). Quando o host Rust virar o oficial, substitui também o ADR-008 na parte do Textual
- Requisitos: [PRD do host em Rust](../prd/rust-host.md)

## Contexto
O ADR-010 planejava reescrever o host em Go na v2.x, com dois motivos: um binário único por
sistema operacional, sem `.venv`, e uma suíte de conformidade como contrato entre as
implementações. Os dois motivos continuam valendo. Três coisas mudaram:

- **A linguagem.** Rust também gera binário único, tem TUI madura (`ratatui`) e acesso à serial
  (`serialport`). Ao contrário do Go, ele aparece também no firmware: o crate do ADR-011 e o
  esp32-devkit-tester podem dividir código com o host. Rust já está nos planos do autor; Go
  sairia como mais uma linguagem só para o host.
- **O momento.** O custo de portar cresce a cada funcionalidade nova escrita em Python. O
  host Python tem cerca de 3.400 linhas na v0.4.0, e o M5 (`esp-logcat dump`) e o M6 ainda
  vão crescer o host. Portar agora é portar o menor host que vai existir.
- **O uso.** Ter que ativar o `.venv` antes de cada sessão já atrapalha o uso diário, e não só
  o público externo.

A suíte de conformidade que o ADR-010 pedia já existe (`conformance/*.tsv`, lida pelo C++ e
pelo Python). O porte tem um gabarito desde o primeiro dia.

## Decisão
1. O host passa a ser em Rust, num workspace Cargo em `host-rs/` enquanto coexistir com o
   Python:

   | Crate | O que tem | Depende de I/O? |
   |-------|-----------|-----------------|
   | `logcat-wire` | Parser e classificação de linhas, framer de respostas e dump, lacunas de `seq`, arquivo de sessão (wire-format §1–§4) | Não: testado só com bytes e `conformance/` |
   | `logcat-serial` | Descoberta de portas por VID:PID, abrir a CH340K com DTR/RTS soltos, reconexão quando a COM some ou muda | Sim. Pensado para ser dividido com o esp32-devkit-tester |
   | `esp-logcat` | Binário: CLI (`clap`) e TUI (`ratatui` + `crossterm`), os mesmos comandos do host Python | Sim |

2. O contrato continua sendo a spec (`wire-format.md`) e a suíte `conformance/`: o
   `logcat-wire` roda os mesmos `.tsv` que o C++ e o Python. Uma fixture nova do Python vira
   caso de conformidade antes de virar teste Rust.
3. **Congelamento do Python.** A partir deste ADR, o host Python só recebe correções. Nenhuma
   funcionalidade nova entra nele: o `esp-logcat dump` (M5) e a detecção de reinício pela
   linha `rst:` da ROM e pela regressão do timestamp nascem em Rust.
4. **Concorrência sem async na primeira versão.** Uma thread por fonte (serial), canais para o
   estado e a TUI. A leitura da serial é bloqueante e com poucas fontes, e threads são mais
   simples de entender e testar que um runtime async. O async é reavaliado quando entrar
   rede (#85) ou muitas fontes simultâneas.
5. As regras do ADR-008 que não dependem da linguagem continuam: backend em camadas,
   frontends só consumidores, texto da placa tratado como dado (nunca como markup), `stream`
   em texto puro no stdout.
6. O host Rust vira o oficial quando passa em toda a `conformance/` e cobre os comandos do
   Python (`ports`, `stream`, `tui`, `replay`, `shell`, `dump`). Nesse momento o Python é
   aposentado, e `host-rs/` passa a se chamar `host/`.
7. O `logcat-wire` evita depender de `std` onde não precisa: se custar pouco, ele fica
   compilável em `no_std` + `alloc`, e o crate de firmware do ADR-011 pode reaproveitar o
   formato.

## Consequências
- Um período com dois hosts. Como o Python fica congelado, a manutenção dupla se limita a
  mudanças de formato, que já atualizam os dois lados no mesmo commit (ADR-002).
- O CI ganha um job Rust: `cargo fmt --check`, `cargo clippy -D warnings` e `cargo test` em
  Linux e Windows (o Windows é onde mora o comportamento da serial, `hardware.md`).
- O `host-reviewer` ganha uma versão para Rust, e o `test-runner` passa a rodar `cargo test`.
- Distribuição pelos releases do GitHub, um binário por sistema operacional.
- Vários dispositivos ao mesmo tempo e transporte pela rede (#85) entram depois da paridade,
  já sobre o host Rust. A identidade de um dispositivo passa a ser o MAC, não a COM.
- O ADR-011 passa a citar este ADR, e não o ADR-010, como dono da suíte de conformidade.
