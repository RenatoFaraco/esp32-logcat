# ADR-012: Moldura das respostas do console (id, fim de resposta, erro, versão)
- Status: aceita
- Data: 2026-11-09
- Issue: #69 · Afeta: #27, #28, #29, #30 · Complementa: ADR-002, ADR-004, ADR-006, ADR-007

## Contexto
A ARCHITECTURE §6 definia a resposta de comando como `#> <texto>`. Isso basta para uma pessoa
digitando um comando de cada vez, mas não para uma ferramenta: as respostas dividem a porta USB
nativa com até ~1000 linhas de log por segundo, e o host precisa saber:
- **a que pedido** pertence uma linha de resposta (um `shell` e um script podem falar com a
  placa ao mesmo tempo);
- **onde a resposta termina**: `stats` e `log tail` têm várias linhas, e uma placa calada
  precisa ser distinguível de uma resposta completa;
- **que o comando falhou, e por quê**, numa forma única (ex.: `log level` com a tabela de 16
  tags cheia, #27);
- **que protocolo o firmware fala**, para um host mais novo não interpretar errado um firmware
  diferente.

O console ainda não existe (M5), então dá para decidir isto antes de haver compatibilidade a
manter.

## Decisão

### 1. Pedido (host → dispositivo)
```
<id> <comando> [argumentos]\n
3 log level PRESSURE D
- info
```
- `id`: decimal `1`–`9999`, sem zero à esquerda, escolhido pelo host; ou `-` quando uma pessoa
  digita no `shell` sem id.
- O firmware ecoa o id em todas as linhas da resposta. Ele não guarda ids nem recusa
  repetidos: casar pedido e resposta é trabalho do host.
- O host gira os ids e não reusa um id antes de passar o timeout da resposta anterior (§6). Uma
  resposta atrasada nunca é tomada como resposta de um pedido novo.
- `-` vale para uma sessão interativa: o host aceita no máximo um pedido `-` pendente por vez.
- A sintaxe dos comandos e dos argumentos é da #27 (`CommandParser`).

### 2. Resposta (dispositivo → host)
Zero ou mais linhas de dados e **exatamente um** terminador:

| Linha | Forma | Exemplo |
|-------|-------|---------|
| Dados | `#> <id> <chave=valor> [chave=valor ...]` | `#> 7 fw=0.3.0 proto=1 uptime_ms=5123` |
| Fim, ok | `#. <id> ok` | `#. 7 ok` |
| Fim, erro | `#! <id> <código> [texto]` | `#! 3 full tag table full (16 tags)` |

- **Chave:** `[a-z][a-z0-9_.]{0,31}`. O ponto agrupa: `not_sent.usb=0`.
- **Valor:** 1 ou mais bytes de `0x21`–`0x7E` (ASCII imprimível sem espaço, ADR-006). O
  primeiro `=` separa chave e valor.
- Uma linha de dados tem ao menos um campo, e cada chave aparece uma vez por linha. Separador:
  um espaço, nunca dois nem TAB. Entre linhas da mesma resposta uma chave pode se repetir (ex.:
  uma linha por sink); o significado é definido pelo comando.
- **Desconhecido não é zero:** um campo que o firmware não sabe medir é omitido. O host mostra
  "unknown", nunca `0`.
- **Códigos de erro da v1:**
  - `bad_command` (comando desconhecido);
  - `bad_args` (argumento faltando, sobrando ou inválido);
  - `full` (tabela ou recurso cheio);
  - `busy` (já há uma operação longa em curso, ex.: dump);
  - `timeout` (a resposta não conseguiu sair a tempo, §5);
  - `unsupported` (comando conhecido que este build não oferece);
  - `failed` (a operação falhou: flash, driver).

  O host aceita qualquer código `[a-z_]{1,32}` menos `ok`, para um firmware mais novo poder
  criar outros. Sucesso é só `#.`.
- **Texto do erro:** opcional, ASCII imprimível começando por um caractere que não é espaço
  (`#! 3 full ` com espaço sobrando é malformada), só para pessoas. Nenhuma ferramenta decide
  nada pelo texto.
- **Limite:** 170 bytes por linha com o `\n`, como uma linha de log. Uma resposta maior é
  dividida em várias linhas de dados.
- **Linha de protocolo malformada** (id inválido, campo sem `=`, chave repetida, mais de 170
  bytes, sem `\n` final): o host a trata como raw, mostrada e contada, nunca interpretada pela
  metade.
- **Comandos que reiniciam a placa:**
  - `reboot` responde `#. <id> ok`, espera a escritora esvaziar e só então reinicia;
  - `crash` não responde.

  Um reinício da placa (#56) com `reboot` ou `crash` pendente é o sucesso desses pedidos, não
  uma resposta perdida.

### 3. Bloco de dump: respostas que carregam linhas de log
`log tail` e `log dump` devolvem linhas de log inteiras. Elas não cabem em `#>`, que passaria
de 170 bytes com o prefixo, e vêm da flash, onde pode haver lixo. Por isso vão num **bloco
contado em bytes**:
```
#BEGIN <id> <nome> <bytes>
<exatamente <bytes> bytes, copiados do arquivo sem mudar nada>
#END <id> <crc32>
#. <id> ok
```
- `id`: o do pedido, inclusive `-`, nos dois marcadores.
- `nome`: `[A-Za-z0-9_][A-Za-z0-9._-]{0,63}` (arquivo da flash, ou `ram` para o `log tail`). É
  um rótulo, não um caminho: o `dump` (#29) escolhe o nome do arquivo local, porque o Windows
  reserva nomes como `CON`, `NUL` e `COM1`.
- `bytes`: decimal sem zero à esquerda, até 10 dígitos.
- **O conteúdo é o do arquivo, byte a byte:**
  - dentro do bloco não valem as regras de ASCII nem de 170 bytes;
  - o `DEL`+`\n` de uma linha rasgada (PRD §3), um `0xFF` de setor apagado ou uma sequência
    sem `\n` chegam como estão. O dump não sanitiza, porque trocar o `DEL` por `?`
    transformaria uma linha rasgada em registro válido;
  - o host passa as linhas do bloco pelo parser de log, que já classifica lixo, linha rasgada
    e linha longa (ADR-007).
- **O host lê exatamente `<bytes>` bytes** e então espera o `#END`. Ele nunca procura `#END`
  dentro do conteúdo.
- O valor de `bytes` é o tamanho do arquivo **no início** do dump. O arquivo ativo pode crescer durante a
  leitura (#23), e o que passar desse tamanho fica para o próximo dump.
- `crc32`: CRC-32 IEEE (polinômio refletido `0xEDB88320`, valor inicial e XOR final
  `0xFFFFFFFF`, o mesmo de `zlib.crc32`) sobre exatamente os `<bytes>` bytes. O firmware manda
  8 dígitos hex minúsculos; o host aceita maiúsculos.
- **Enquanto um bloco está aberto, a escritora não manda linhas de log para a USB.** Os sinks de
  flash e de RTC continuam recebendo normalmente. As linhas puladas são contadas como
  `not_sent.usb` e reportadas depois do `#END`, como qualquer perda de sink. Sem isso, uma
  linha ao vivo, igual em forma às do arquivo, entraria no bloco e quebraria a contagem e o
  CRC.
- O terminador `#.`/`#!` vem depois do `#END`. Um erro no meio do bloco termina com `#!` sem
  `#END`, e o host descarta o bloco.

### 4. Versão do protocolo
- O `info` responde `proto=1` (este ADR).
- **Compatível**, sem mudar o `proto`: chave nova, código de erro novo, comando novo. O host
  ignora chaves desconhecidas e mostra códigos desconhecidos.
- **Incompatível**, `proto` sobe: mudança na forma das linhas, no significado de uma chave
  existente, ou no formato da linha de log (ADR-002/ADR-006).
- Até saber o `proto` (o primeiro `info` da sessão), o host só mostra as linhas `#`, sem
  interpretá-las.
- Firmware com `proto` maior que o do host: o host avisa e só mostra as linhas. Firmware sem
  `info` (sem console): o host diz que o console não está disponível.

### 5. Quem escreve as respostas no firmware
- A **tarefa escritora continua a única dona do sink USB**. Resposta e log nunca intercalam
  bytes no meio de uma linha.
- As respostas vão por uma **fila própria, pequena** (da ordem de 4 entradas), que a escritora
  esvazia **antes** da fila de log a cada volta.
- Quem responde é a tarefa do console, nunca uma tarefa crítica, então ela **pode esperar** por
  vaga, com timeout:
  - resposta não é descartada em silêncio;
  - se o timeout estourar, o console tenta mandar um `#! <id> timeout`, e se nem isso couber
    o host sai pelo próprio timeout.
- **Escrita na USB:**
  - log continua com timeout zero (linha inteira ou nada, ADR-004);
  - resposta usa um timeout **curto e limitado** (da ordem de dezenas de ms por linha);
  - se o host parou de ler e o timeout estoura, a resposta é abortada (com `#! <id> timeout`,
    se couber), e a falha é contada.
- **Sem inanição:** depois de cada entrada da fila de respostas, a escritora atende ao menos
  uma entrada da fila de log. Num dump longo, flash e RTC continuam recebendo o log (só a USB
  fica de fora, §3). Uma resposta nunca segura os outros sinks além do timeout de uma linha.
- **Ordem:** a resposta pode sair antes de linhas de log produzidas antes do pedido e ainda
  na fila. O host não deduz ordem entre resposta e log; quem precisa de ordem usa `seq` e
  `uptime`.
- A regra do ADR-004 (quem loga nunca espera) continua valendo para o log. A fila de respostas
  é outro caminho, com outra política.
- Montar e validar linhas de resposta são funções puras no **núcleo** (ADR-003), testadas no
  PC; o adaptador do console é da #28.

### 6. Host
- **Timeouts:**
  - espera o terminador por **2 s** depois do pedido;
  - dentro de um bloco de dump, espera no máximo **2 s sem receber bytes**.

  Estourou: erro "no reply", com o que chegou descartado.
- **Bloco incompleto** (timeout, porta caiu, `#!` no meio, ou CRC que não confere):
  - o bloco inteiro é descartado, e o host avisa;
  - nada vai para o disco antes do `#END` com o CRC conferido;
  - um reinício da placa durante o bloco (#56) é reportado como provável causa.
- **Linhas de resposta fora de contexto** (`#>`, `#.`, `#!`, `#BEGIN` e `#END` sem pedido
  pendente com aquele id, ou na primeira linha depois de conectar, que pode ser o fim cortado
  de outra linha): o `shell` e o `dump` as mostram como raw e não agem sobre elas. O `stream`,
  que não faz pedidos, só as mostra.
- As linhas de log que chegam entre linhas de resposta (fora de um bloco) seguem para o stream
  normalmente; o id é o que separa.
- **JSON** (`--json`) é gerado pelo host a partir dos campos. No fio fica só o texto compacto.

## Alternativas consideradas
- **JSON no fio:** legível e auto-descritivo, mas exige escapar texto e montar objetos no
  firmware, sem alocação e com tamanho variável. O host gera o JSON sem perder nada, porque o
  fio já é chave=valor.
- **Binário com CRC por quadro (estilo SLIP/COBS):** robusto e compacto, mas ilegível num
  terminal comum, e o projeto inteiro é texto (ADR-002). O CRC fica para o que é grande e
  precisa de integridade, o bloco de dump.
- **Sem id, uma resposta por vez:** funciona com uma pessoa só, mas não com `shell` e script
  juntos, nem para descartar uma resposta atrasada depois de um timeout.
- **Respostas pela fila de log:** um caminho só, mas uma fila cheia descartaria respostas
  (ADR-004), e o host esperaria até o timeout sem saber por quê.
- **Dump linha a linha com `#>`:** não cabe no limite de 170 bytes, e obrigaria a sanitizar o
  conteúdo, apagando a marca de linha rasgada.

## Consequências
- **Firmware:**
  - uma fila a mais (~4 × tamanho de linha);
  - uma checagem a mais por volta da escritora;
  - um estado "bloco aberto" que tira a USB do log durante o dump;
  - um timeout limitado só para as respostas.
- **Host:**
  - um parser de linhas estrito;
  - um leitor de bloco contado em bytes no `dump` (#29);
  - o pedido e a resposta casados por id no `shell` (#30).
- **Sinais:** `#>`, `#.`, `#!`, `#BEGIN` e `#END` passam a ter forma exata. Qualquer outra coisa
  começando com `#` continua raw. Uma linha de log completa nunca começa com `#`: o formato
  começa com 4 dígitos.
- **Casos compartilhados:** `conformance/reply_cases.tsv`, gerado por
  `conformance/make_reply_cases.py`. Ele cobre as linhas de resposta e os marcadores de bloco,
  mais um caso de CRC conhecido sobre um conteúdo com `DEL`, `0xFF`, `\0` e trecho sem `\n`.
  O host já valida; o lado C++ entra com o formatter de respostas (#27/#28).
- **Ferramentas externas** (dashboards, telemetria da v3) leem o `--json` do host, não o fio.
