# ADR-004: Descartar logs com fila cheia em vez de bloquear
- Status: aceita
- Data: 2026-10-20

## Contexto
Quem loga pode ser uma tarefa crítica (aquisição de sinal a 1 kHz). Se a fila de logs encher
(rajada de logs, flash lenta), há duas opções: esperar espaço ou descartar a linha.
Esperar altera o timing da tarefa crítica — exatamente o que o PRD §5 proíbe.

## Decisão
O envio para a fila usa timeout zero. Fila cheia → a linha é descartada e um contador `dropped`
é incrementado. Quando a próxima linha passar, a `WriterTask` emite
`W LOGCAT: dropped N lines`. O campo `seq` também expõe lacunas no host.

## Consequências
- O custo de logar é limitado e previsível; logar nunca bloqueia.
- Linhas podem se perder sob carga, mas a perda é sempre visível (aviso + saltos de `seq`).
- O tamanho da fila passa a ser um parâmetro de projeto, dimensionado no M2 com medição.
