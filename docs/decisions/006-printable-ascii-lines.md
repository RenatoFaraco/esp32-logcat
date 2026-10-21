# ADR-006: Linhas de log só com ASCII imprimível
- Status: aceita
- Data: 2026-10-21

## Contexto
A revisão da issue #4 (persistence-auditor e firmware-reviewer) apontou três problemas que
dependem de quais bytes podem aparecer numa linha:
- Uma mensagem com `\n` ou `\r` vira duas linhas, e a segunda pode imitar um registro válido
  (`LOGE(TAG, "a\n0042:000001 0000000001 E FAKE: boom")`).
- O corte em 127 bytes pode partir um caractere UTF-8 multibyte, e o host quebra ou descarta a
  linha ao decodificar.
- Ao reler a flash depois de um corte de energia, o parser precisa distinguir texto de lixo
  (setor apagado `0xFF`, bytes corrompidos). Com UTF-8 liberado, quase qualquer sequência de
  bytes altos é "texto possível".

Os logs do ESP-IDF já são ASCII, e o CONVENTIONS pede textos do código em inglês.

## Decisão
- Toda linha gravada ou enviada contém só ASCII imprimível (`0x20`–`0x7E`) e termina com um
  único `\n`.
- Quem garante é o **formatter** (#5), na `WriterTask`: troca qualquer byte fora da faixa por
  `?`. Quem loga não paga esse custo; o `LogRecord` guarda o texto como recebido.
- O **parser** (#6, C++ e Python) rejeita linhas com bytes fora da faixa como corrompidas.

## Consequências
- Mensagem com `\n` não forja linhas; o corte em 127 bytes nunca deixa UTF-8 quebrado.
- A detecção de lixo de flash fica simples e estrita.
- Acentos e outros caracteres não ASCII aparecem como `?`. Logs devem ser escritos em inglês.
- Se um dia for preciso UTF-8, a mudança é de formato: formatter, parsers e testes dos dois
  lados no mesmo commit, e este ADR é substituído.
