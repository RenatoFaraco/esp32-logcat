# PRD — esp32-logcat v2.x: host em Go

> Status: **rascunho**. Vira "aprovado" quando a v2.x começar, junto com o ADR-010 ("aceita").
> Fontes: ADR-010, ADR-008 e ROADMAP, "Depois da v1". Formato no fio e arquivo de sessão:
> [`spec/wire-format.md`](../spec/wire-format.md).

## 1. Problema

O host da v1 é Python 3.12 com `.venv`. Quem vem do PlatformIO, do Arduino ou de outro
fabricante muitas vezes não tem Python configurado, nem quer criar um ambiente para ler logs.

## 2. Objetivo

Um binário único por sistema operacional (Windows, Linux, macOS) com os mesmos comandos do host
Python, validado pelas mesmas entradas e saídas esperadas.

## 3. Escopo

| ID | Requisito | Origem |
|----|-----------|--------|
| GX-1 | CLI e TUI (Bubbletea) com os comandos do host Python: H1–H7 do PRD v1 | ADR-010 §1 |
| GX-2 | Lê e escreve tudo da `spec/wire-format.md`: linha de log (§1), respostas e dump (§2–§3), arquivo de sessão (§4) | ADR-010 §2 |
| GX-3 | Passa na suíte de conformidade independente de linguagem (`conformance/`), a mesma que o Python roda | ADR-010 §2 |
| GX-4 | Camadas do ADR-008: backend em camadas, frontends só consumidores, texto da placa como dado (nunca markup), `stream` em texto puro no stdout | ADR-010 §4 |
| GX-5 | O comportamento aprendido na placa: reset pela USB nativa no Windows, CH340K com DTR/RTS soltos, linhas grudadas na FIFO, reconexão quando a COM muda (`hardware.md`) | ADR-010, Contexto |
| GX-6 | Distribuição pelos releases do GitHub, um binário por sistema operacional | ADR-010, Consequências |

## 4. Fora do escopo

- Mudança de formato no fio: o Go segue a spec, não a redefine.
- Coletor e análise da telemetria: a linguagem é escolhida na v3.

## 5. Restrições

- Até o Go virar oficial, os dois hosts coexistem: uma mudança de formato atualiza o formatter
  C++ e os dois parsers no mesmo commit.
- Antes do Go, as fixtures de `host/tests/fixtures/` viram dados independentes de linguagem
  (entradas em bytes e saídas esperadas: linhas classificadas, lacunas de `seq`, avisos).
- O `host-reviewer` ganha uma versão para Go, e o CI um job Go.

## 6. Critérios de aceite (rascunho)

1. O host em Go passa em toda a suíte de `conformance/`.
2. Cobre todos os comandos do host Python (GX-1).
3. Uma sessão gravada por um host é reproduzida pelo outro com a mesma tela e o mesmo resumo.
4. Instala e roda num PC sem Python.

## 7. Questões abertas

- O destino do host Python quando o Go virar oficial: aposentar ou manter como biblioteca
  (ADR-010 §3).
