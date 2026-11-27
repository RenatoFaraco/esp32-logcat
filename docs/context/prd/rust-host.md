# PRD — esp32-logcat: host em Rust

> Status: **aprovado** (marco M5.5, antes da v1.0). Substitui o rascunho do host em Go
> (v2.x). Fontes: ADR-014, ADR-008 e ROADMAP. Formato no fio e arquivo de sessão:
> [`spec/wire-format.md`](../spec/wire-format.md).

## 1. Problema

O host da v1 é Python 3.12 com `.venv`. Quem vem do PlatformIO, do Arduino ou de outro
fabricante muitas vezes não tem Python configurado, nem quer criar um ambiente para ler logs. O
próprio uso diário esbarra nisso: o `esp-logcat` só existe com o `.venv` ativo.

## 2. Objetivo

Um binário único por sistema operacional (Windows, Linux, macOS) com os mesmos comandos do host
Python, validado pelas mesmas entradas e saídas esperadas, e com a base (vários dispositivos,
rede) que o Python não teria.

## 3. Escopo

| ID | Requisito | Origem |
|----|-----------|--------|
| RH-1 | CLI (`clap`) e TUI (`ratatui`) com os comandos do host Python: H1–H7 do PRD v1, mais `shell` | ADR-014 §1, §6 |
| RH-2 | Lê e escreve tudo da `spec/wire-format.md`: linha de log (§1), respostas e dump (§2–§3), arquivo de sessão (§4) | ADR-014 §2 |
| RH-3 | Passa na suíte de conformidade (`conformance/`), a mesma que o C++ e o Python rodam | ADR-014 §2 |
| RH-4 | Camadas do ADR-008: backend em camadas, frontends só consumidores, texto da placa como dado (nunca markup), `stream` em texto puro no stdout | ADR-014 §5 |
| RH-5 | O comportamento aprendido na placa: reset pela USB nativa no Windows, CH340K com DTR/RTS soltos, linhas grudadas na FIFO, reconexão quando a COM muda (`hardware.md`) | ADR-014, Contexto |
| RH-6 | `esp-logcat dump`: lê o bloco `#BEGIN/#END` e confere o CRC (wire-format §3) | ROADMAP, M5b |
| RH-7 | Detecção de reinício: pela linha `rst:` da ROM na UART e, onde ela não chega (USB-Serial-JTAG), pela queda da porta mais a regressão do timestamp; separador visível e contador `restarts` | ADR-014 §3 |
| RH-8 | Distribuição pelos releases do GitHub, um binário por sistema operacional | ADR-014, Consequências |

## 4. Fora do escopo

- Mudança de formato no fio: o Rust segue a spec, não a redefine.
- Vários dispositivos ao mesmo tempo e transporte pela rede (#85): depois da paridade, sobre
  este host.
- Coletor e análise da telemetria: a linguagem é escolhida na v3.

## 5. Restrições

- Até o Rust virar oficial, os dois hosts coexistem, com o Python congelado (só correções):
  uma mudança de formato atualiza o formatter C++ e os dois parsers no mesmo commit.
- Uma fixture de `host/tests/fixtures/` usada no porte vira caso de `conformance/` antes de
  virar teste Rust.
- O `host-reviewer` ganha uma versão para Rust, e o CI um job Rust (Linux e Windows).

## 6. Critérios de aceite

1. O host em Rust passa em toda a suíte de `conformance/`.
2. Cobre todos os comandos do host Python (RH-1) e o `dump` (RH-6).
3. Uma sessão gravada por um host é reproduzida pelo outro com a mesma tela e o mesmo resumo.
4. Instala e roda num PC sem Python.
5. Um reset pelo botão é contado em `restarts` tanto pela UART quanto pela USB-Serial-JTAG (RH-7).

## 7. Questões abertas

- Se o `logcat-wire` fica compilável em `no_std` + `alloc` para servir ao crate do ADR-011
  (ADR-014 §7).
