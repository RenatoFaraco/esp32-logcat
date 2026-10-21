# ADR-007: Sem checksum por linha na v1
- Status: aceita
- Data: 2026-10-21

## Contexto
A revisão da issue #4 (persistence-auditor) apontou que, sem checksum, um bit invertido no
meio de uma linha completa na flash passa pelo parser como dado válido: uma letra trocada na
mensagem ou um dígito diferente no `uptime` não deixam rastro. Um CRC-32 por linha
(ex.: ` *A1B2C3D4` no fim) detectaria isso, ao custo de +10 bytes por linha (~6% da partição
de 512 KB) e de uma linha menos legível.

O que já existe sem checksum por linha:
- Linha rasgada por queda de energia: detectada pela falta do `\n` final (#6) e fechada ao
  reabrir o arquivo (#23).
- Lixo de flash (`0xFF`, bytes altos): rejeitado pelo filtro ASCII (ADR-006).
- Transporte para o PC: o dump vai em bloco `#BEGIN … #END <crc32>` (ARCHITECTURE §6).
- LittleFS protege seus metadados com CRC; a corrupção silenciosa de dados em NOR flash
  dentro da vida útil é rara.

## Decisão
O formato da v1 não tem checksum por linha. O risco residual é corrupção pontual dentro de
uma linha completa, que passa despercebida.

O que o formato **ainda pega** sem checksum: bit 7 ligado, bytes que viram controle,
separadores alterados, dígito que deixa de ser dígito, letra de nível fora de `EWIDV`.

O que **passa** (registrado pela auditoria da #5):
- Letras e dígitos de tag/mensagem que continuam imprimíveis; bits 0–3 de dígitos.
- Nível: `E` (0x45) ↔ `D` (0x44) e `W` (0x57) ↔ `V` (0x56) diferem em 1 bit — um ERROR
  pode ser relido como DEBUG sem rastro.
- `\n` (0x0A) ↔ `*` (0x2A) diferem no bit 5: duas linhas podem virar uma (com mensagem
  estranha) ou uma linha pode ser partida em duas, e o pedaço inicial parecer válido.

## Consequências
- Linha curta e legível em qualquer terminal; parser e formatter mais simples.
- Corrupção dentro de uma linha válida não é detectada.
- Reavaliar na v3 (assinatura/encadeamento por hash para valor probatório, PRD §4), quando
  integridade passa a ser requisito. Adicionar checksum é mudança de formato: formatter,
  parsers e testes dos dois lados no mesmo commit, e este ADR é substituído.
