# ADR-002: Formato de linha em texto legível na v1
- Status: aceita
- Data: 2026-10-20

## Contexto
Os logs precisam ser lidos por uma pessoa num terminal serial qualquer, pelo `esp-logcat` e,
depois de baixados da flash, por ferramentas comuns (`grep`, editor). Um formato binário ou
tokenizado economiza banda e flash, mas exige o host para ler qualquer coisa.

## Decisão
Texto, uma linha por registro, no formato do PRD §3:
`<boot>:<seq> <uptime_ms> <L> <TAG>: <mensagem>`, com campos numéricos de largura fixa.
O mesmo texto vai para USB, RAM e flash.

## Consequências
- Legível sem ferramenta; um único parser (C++ e Python) para todas as fontes.
- Campos de largura fixa simplificam o parser e a ordenação.
- Mais bytes por linha que um formato binário → menos histórico na mesma partição.
- Mudar o formato exige atualizar formatter, parsers e testes dos dois lados no mesmo commit.
- Formato binário fica para a v2.
