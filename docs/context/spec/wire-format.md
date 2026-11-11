# Formato no fio — esp32-logcat

> O contrato entre o firmware e o host, em um lugar só. Vale para todas as implementações:
> firmware C++ (v1/v2) e Rust (v3, ADR-011), host Python (v1/v2) e Go (v2.x, ADR-010).
> Os **porquês** ficam nos ADRs citados; aqui fica só o **o quê**.
>
> Mudar qualquer seção exige, no mesmo commit: esta spec, os casos compartilhados em
> `conformance/` e todas as implementações que a leem passando de novo (CLAUDE.md).

| Seção | O quê | Casos compartilhados | Decisões |
|-------|-------|----------------------|----------|
| §1 | Linha de log | `conformance/line_cases.tsv` | ADR-002, ADR-006, ADR-007 |
| §2 | Pedidos e respostas do console | `conformance/request_cases.tsv`, `conformance/reply_cases.tsv` | ADR-012 |
| §3 | Bloco de dump | `conformance/reply_cases.tsv` (`block_crc`) | ADR-012 |
| §4 | Arquivo de sessão do host | testes do host (`test_session.py`) | PRD v1 H6 |

## 1. Linha de log

```
<boot>:<seq> <uptime_ms> <L> <TAG>: <mensagem>
0042:000137 0000123456 W PRESSURE: leitura fora da faixa (4.71 V)
```

- `boot`: contador de boots → correlaciona logs entre resets
- `seq`: sequência dentro do boot → detecta perda de linhas
- Só ASCII imprimível (`0x20`–`0x7E`), uma linha por registro terminada em `\n` (ADR-006)

Contrato exato (implementado em `logcat/line_formatter.hpp`, validado pelos parsers):

| Campo | Regra |
|-------|-------|
| `boot` | `boot % 10000`, 4 dígitos com zeros à esquerda |
| `seq` | `seq % 1000000`, 6 dígitos; volta a 0 e pode recuar um pouco (reordenação na fila) |
| `uptime_ms` | 10 dígitos, `uint32_t` (volta a 0 após ~49,7 dias) |
| `L` | `E` `W` `I` `D` `V`; nível inválido sai `?` e a linha é rejeitada pelo parser |
| `TAG` | 1–15 chars; vazia → `-`; espaço e `:` → `_` |
| separador | sempre `": "`, mesmo com mensagem vazia |
| mensagem | 0–127 chars; bytes fora de `0x20`–`0x7E` → `?` |
| truncada | os 124 primeiros chars + `...` (só quando a mensagem tem 127 chars; mensagem de 127 chars terminada em `...` ⇔ truncada, salvo a rara mensagem original idêntica) |
| tamanho máximo | 170 bytes com o `\n` |
| linha rasgada | ao reabrir um arquivo cuja última linha não termina em `\n`, o store (#23) acrescenta `DEL` (`0x7F`) + `\n`; o byte proibido faz o parser classificá-la como rasgada, nunca como registro |

Sem checksum por linha na v1 (ADR-007). Ordem cronológica = posição no arquivo, não
`boot`/`seq`/`uptime`.

## 2. Pedidos e respostas do console

Versão do protocolo: `proto=1`. Decisão e alternativas: ADR-012.

### 2.1 Pedido (host → dispositivo)

```
<id> <comando> [argumentos]\n
```

- `id`: decimal `1`–`9999` sem zero à esquerda, escolhido pelo host; ou `-` (pessoa digitando
  no `shell`). O dispositivo ecoa o id em todas as linhas da resposta e não guarda ids.
- O host gira os ids e não reusa um id antes de passar o timeout da resposta anterior
  (§2.3); aceita no máximo um pedido `-` pendente por vez.
- **Palavras:** separadas por um ou mais espaços; espaços em volta da linha são ignorados.
  Comandos em minúsculas, exatos (sem abreviação).
- **Fim de linha:** `\n`, `\r\n` ou `\r` (o que um terminal serial manda), ou nenhum. O
  host manda `\n`.
- **Tamanho:** até 64 bytes antes do fim de linha, só `0x20`–`0x7E`.
- Linha só com espaços: ignorada, sem resposta.

| Comando | Argumentos | O que pede |
|---------|------------|------------|
| `info` | — | identidade e estado do dispositivo |
| `stats` | — | contadores do motor de log, acumulados desde o boot |
| `log level <tag> <L>` | `tag`: 1 ou mais caracteres sem espaço, cortada em 15; `*` é o padrão. `L`: `E` `W` `I` `D` `V`, ou `N` (silencia a tag); só maiúsculas | muda o nível de uma tag em tempo de execução |
| `log tail` | — | as últimas linhas em RAM, num bloco (§3) |
| `log dump` | — | os logs gravados na flash, em blocos (§3) |
| `log clear` | — | apaga os logs gravados na flash |
| `reboot` | — | reinicia o dispositivo |
| `crash <panic\|wdt>` | `panic` ou `wdt` | provoca um crash, para testar a recuperação |

Pedido rejeitado (a resposta é só o terminador de erro, §2.2):

| Situação | Código | id da resposta |
|----------|--------|----------------|
| a primeira palavra não é um id válido | `bad_args` | `-` |
| byte fora de `0x20`–`0x7E`, ou mais de 64 bytes | `bad_args` | `-` |
| comando desconhecido, ou nenhum depois do id; subcomando de `log` desconhecido | `bad_command` | o do pedido |
| argumento faltando, sobrando ou inválido (inclusive `log` sem subcomando) | `bad_args` | o do pedido |

Casos compartilhados dos pedidos: `conformance/request_cases.tsv`.

### 2.2 Resposta (dispositivo → host)

Zero ou mais linhas de dados e **exatamente um** terminador:

| Linha | Forma |
|-------|-------|
| Dados | `#> <id> <chave=valor>[ <chave=valor> ...]` |
| Fim, ok | `#. <id> ok` |
| Fim, erro | `#! <id> <código>[ <texto>]` |

| Elemento | Regra |
|----------|-------|
| `id` | `-` ou `[1-9][0-9]{0,3}` |
| chave | `[a-z][a-z0-9_.]{0,31}`; uma vez por linha (pode repetir entre linhas) |
| valor | 1 ou mais bytes de `0x21`–`0x7E`; o primeiro `=` separa chave e valor |
| separador | um espaço, nunca dois nem TAB, entre os elementos acima e entre os campos; nada de espaço depois do último campo de uma linha de dados, nem depois do código quando não há texto |
| código | `[a-z_]{1,32}`, menos `ok`. Na v1: `bad_command`, `bad_args`, `full`, `busy`, `timeout`, `unsupported`, `failed` |
| texto | opcional, `0x20`–`0x7E`, começa por um caractere que não é espaço; pode ter espaços internos e finais; só para pessoas |
| fim de linha | `\n` puro: sem `\n` final, ou com `\r`, a linha é raw |
| tamanho máximo | 170 bytes com o `\n` (resposta maior: várias linhas de dados) |
| campo desconhecido | omitido, nunca `0` |

- Linha que começa com `#` e não casa exatamente com uma forma desta seção (ou da §3) é **raw**:
  mostrada e contada, nunca interpretada pela metade (ex.: chave repetida na mesma linha,
  campo sem `=`, id inválido). Uma linha de log nunca começa com `#`.
- `reboot` responde `#. <id> ok` antes de reiniciar; `crash` não responde. O reinício da placa
  é o sucesso desses pedidos.
- O `info` traz `proto=1`. Chave, código ou comando novo não mudam o `proto`; mudança na forma
  das linhas, no significado de uma chave existente ou na §1 sobe o `proto`.
- Até o primeiro `info` da sessão dar o `proto`, o host só mostra as linhas `#`, sem
  interpretá-las. `proto` maior que o do host: avisa e só mostra. Sem `info` (firmware sem
  console): o host diz que o console não está disponível.

### 2.3 Tempo e contexto (host)

- O terminador deve chegar em até **2 s** depois do pedido; senão, "no reply" e o que chegou é
  descartado.
- Linha de resposta sem pedido pendente com aquele id, ou a primeira linha depois de conectar:
  o `shell` e o `dump` a mostram como raw e não agem sobre ela; o `stream`, que não faz
  pedidos, só a mostra.
- Entre linhas de resposta podem chegar linhas de log (fora de um bloco); o id é o que separa.
  O host não deduz ordem entre resposta e log.

## 3. Bloco de dump

Respostas que carregam linhas de log inteiras (`log tail`, `log dump`):

```
#BEGIN <id> <nome> <bytes>
<exatamente <bytes> bytes, copiados do arquivo sem mudar nada>
#END <id> <crc32>
#. <id> ok
```

| Elemento | Regra |
|----------|-------|
| `id` | o do pedido (§2.2), inclusive `-`, nos dois marcadores |
| `nome` | `[A-Za-z0-9_][A-Za-z0-9._-]{0,63}`; rótulo (arquivo da flash, ou `ram`), não caminho |
| `bytes` | `0` ou `[1-9][0-9]{0,9}`; tamanho do arquivo **no início** do dump |
| conteúdo | os bytes do arquivo como estão: sem regra de ASCII nem de 170 bytes; `DEL`+`\n`, `0xFF`, `\0` e trechos sem `\n` chegam intactos |
| `crc32` | CRC-32 IEEE (polinômio refletido `0xEDB88320`, início e XOR final `0xFFFFFFFF`, o de `zlib.crc32`) sobre o conteúdo; 8 dígitos hex, minúsculos no envio, maiúsculos aceitos |

- O host lê exatamente `<bytes>` bytes e então espera o `#END`; nunca procura `#END` no
  conteúdo. As linhas do conteúdo passam pelo parser da §1.
- Com um bloco aberto, nenhuma linha de log vai para a USB (as puladas são contadas como
  `not_sent.usb`).
- O terminador vem depois do `#END`. Um `#!` no meio do bloco o encerra sem `#END`.
- Bloco incompleto (sem bytes por **2 s**, porta caiu, `#!` no meio, CRC que não confere): o
  host descarta o bloco inteiro; nada vai para o disco antes do `#END` conferido.

## 4. Arquivo de sessão do host

Gravado pelo `--save` e lido pelo `replay` (#21). Não passa pelo fio, mas é o formato que outro
host (Go, ADR-010) precisa ler e escrever igual.

```
# esp-logcat session v1
# 2026-11-08T10:15:02.120 notice connected connected to COM6
2026-11-08T10:15:03.410 0000:000107 0000001030 I HEARTBEAT: tick 1
# 2026-11-08T10:15:09.002 raw 303030303a3030
```

| Registro | Regra |
|----------|-------|
| cabeçalho | primeira linha, exatamente `# esp-logcat session v1`; sem ele o arquivo é um log comum (e suas linhas `#` são linhas do dispositivo) |
| hora do PC | `AAAA-MM-DDTHH:MM:SS.mmm` (hora local, 23 caracteres); `-` quando desconhecida |
| linha do dispositivo | `<hora> ` + os bytes exatos que o separador de linhas produziu, `\n` incluído: válidas, inválidas e raw, antes de qualquer filtro |
| `# <hora> notice <tipo> <texto>` | aviso do transporte; `<tipo>` é `connected`, `disconnected`, `waiting` ou `overflow`; a hora é sempre conhecida; no texto, `\r` e `\n` viram espaço e o resto vai em UTF-8 (pode não ser ASCII: vem do sistema, não da placa) |
| `# <hora> raw <hex>` | bytes que um registro comum não carrega intactos: sem `\n` final, com `\r`, ou sem hora conhecida; hex minúsculo, sem separadores |

- Lacunas, reinícios e afins **não** são gravados: o `replay` os calcula de novo das linhas.
- **Leitura:**
  - a primeira linha decide: só um arquivo cujo primeiro registro é exatamente o cabeçalho,
    `\n` incluído (um cabeçalho com `\r\n` não vale), é sessão; senão, todas as linhas são do
    dispositivo, inclusive as `#`;
  - registro `#` desconhecido ou danificado é ignorado;
  - numa sessão, uma linha sem `#` e sem a hora no formato acima seguida de espaço não é
    ignorada: segue como linha do dispositivo, com hora desconhecida;
  - hora com a forma certa mas data inválida: é retirada da linha, e a hora fica
    desconhecida.
