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

## Consequências
- Linha curta e legível em qualquer terminal; parser e formatter mais simples.
- Corrupção dentro de uma linha válida não é detectada.
- Reavaliar na v3 (assinatura/encadeamento por hash para valor probatório, PRD §4), quando
  integridade passa a ser requisito. Adicionar checksum é mudança de formato: formatter,
  parsers e testes dos dois lados no mesmo commit, e este ADR é substituído.
