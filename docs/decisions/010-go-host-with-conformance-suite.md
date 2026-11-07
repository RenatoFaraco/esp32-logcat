# ADR-010: Host em Go na v2.x, validado por uma suíte de conformidade
- Status: proposta (quando aceita, substitui o ADR-008)
- Data: 2026-11-07

## Contexto
O host da v1 é Python 3.12 com `pyserial`, `rich` e Textual (ADR-008). Para um projeto aberto
ao público, a instalação pesa: quem usa ESP-IDF já tem Python, mas quem vem do PlatformIO, do
Arduino ou de outro fabricante (v4) muitas vezes não tem um Python configurado, nem quer criar
um `.venv` para ler logs. Um binário único por sistema operacional resolve isso.

Go gera binário estático para Windows, Linux e macOS, tem concorrência simples para serial +
UI + reconexão (goroutines e channels) e um TUI maduro (Bubbletea). O projeto também é um
veículo de aprendizado do autor, e Go está entre as linguagens que ele quer aprender.

Riscos de uma reescrita:
- Perder comportamento aprendido na placa: reset pela USB nativa no Windows (`hardware.md`,
  item 2), abrir a CH340K com DTR/RTS soltos (item 4), linhas grudadas na FIFO (item 7,
  `pipeline.split_glued`), reconexão quando a COM muda.
- Mais um parser do formato para manter: o custo que o ADR-002 já registra cresce.

## Decisão
1. A v1 e a v2 mantêm o host Python. Na v2.x, o host é reescrito em Go: CLI e TUI (Bubbletea),
   com os mesmos comandos e um binário por sistema operacional.
2. O contrato entre as implementações é o formato do PRD §3 mais uma **suíte de conformidade**
   independente de linguagem: entradas (bytes, como chegam da serial ou da flash) e saídas
   esperadas (linhas classificadas, lacunas de `seq`, avisos) em arquivos de dados, nascidos
   das fixtures de `host/tests/fixtures/`. Python e Go rodam a mesma suíte.
3. O host em Go vira o oficial quando passa na suíte e cobre os comandos do host Python. Até
   lá os dois coexistem; o destino do Python (aposentar ou manter como biblioteca) é decidido
   nesse momento.
4. As regras do ADR-008 que não dependem da linguagem continuam valendo: backend em camadas,
   frontends só consumidores, texto da placa tratado como dado e nunca como markup, `stream`
   em texto puro no stdout.

## Consequências
- Antes do Go, as fixtures precisam virar dados independentes de linguagem, e não só testes
  `pytest`. Esse trabalho também melhora a v1.
- Um período com dois hosts para manter, e uma mudança de formato passa a atualizar o
  formatter C++ e os dois parsers no mesmo commit.
- O `host-reviewer` precisará de uma versão para Go, e o CI de um job Go.
- A ponte direta com o ecossistema de dados do Python (MLOps, v3) deixa de ser automática:
  coletor e análise da telemetria escolhem a linguagem na v3.
- Distribuição pelos releases do GitHub (binários por SO), sem `.venv`.
