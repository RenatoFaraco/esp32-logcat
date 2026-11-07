# ADR-011: Firmware em Rust na v3, como crate `no_std` paralelo
- Status: proposta
- Data: 2026-11-07

## Contexto
O ecossistema Rust para a família Espressif (`esp-hal`) já é multichip: um crate, uma *feature*
por chip, cobrindo os mesmos alvos da v2 (ADR-009). Quem escreve firmware em Rust hoje não tem
um logcat persistente com ferramenta de PC. O projeto também é um veículo de aprendizado do
autor, e Rust está entre as linguagens que ele quer aprender.

Reescrever o componente C++ em Rust jogaria fora a v2 e deixaria sem logcat quem usa ESP-IDF
em C/C++. Uma segunda implementação que fala **o mesmo formato** prova que o contrato (PRD §3)
é independente de linguagem: a mesma ferramenta de PC lê firmware C++ e firmware Rust.

## Decisão
1. A v3 inclui um crate Rust `no_std`, sobre `esp-hal`, que implementa o formato de linha do
   PRD §3 e a mesma separação entre núcleo e adaptadores (ADR-003). É uma implementação
   paralela: o componente C++ continua.
2. Escopo inicial mínimo: formatter, ring buffer, sink USB/UART e integração com a fachada
   `log` do Rust. Persistência, registro de boot e último suspiro vêm depois, nessa ordem.
3. O formatter Rust passa na mesma suíte de conformidade do host (ADR-010).

## Consequências
- Duas implementações de firmware: uma mudança de formato atualiza o formatter C++, o
  formatter Rust, os parsers e a suíte no mesmo commit (ADR-002).
- O F1 do PRD (capturar todo `ESP_LOGx` do IDF) não se aplica ao `esp-hal` bare metal: no Rust,
  a captura é pela fachada `log`. Os requisitos do crate precisam de uma versão própria.
- O núcleo Rust é testado com `cargo test` no PC, e o CI ganha um job Rust.
- O crate é publicado no crates.io, ao lado do componente no IDF Component Registry.
