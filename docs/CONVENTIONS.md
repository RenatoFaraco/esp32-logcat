# Convenções — esp32-logcat

> Regras de como o código, os commits e os documentos são escritos.
> Na dúvida: consistência com o que já existe no repositório vence preferência pessoal.

## 1. Idioma

- **Código, nomes, comentários no código, mensagens de commit, issues:** inglês.
- **Documentos em `docs/`:** português na fase de desenvolvimento; `README.md` em inglês (portfólio/SEO).

## 2. Git

### Branches
- `main` sempre compilando e com CI verde.
- Trabalho em `feat/<assunto>`, `fix/<assunto>`, `docs/<assunto>`, `chore/<assunto>`.
- Merge via Pull Request, mesmo trabalhando sozinho (histórico limpo + CI antes de entrar).

### Commits — Conventional Commits
```
<tipo>(<escopo opcional>): <resumo no imperativo, ≤ 72 caracteres>

<corpo opcional: o porquê, não o como>
```
Tipos: `feat`, `fix`, `test`, `refactor`, `docs`, `build`, `ci`, `chore`, `perf`.
Escopos sugeridos: `core`, `fw`, `host`, `ci`, `docs`.

Exemplos:
```
feat(core): add LineParser with round-trip tests
fix(fw): avoid recursion when LittleFS logs inside writer task
perf(fw): reduce LogRecord to 160 bytes
```

Um commit = uma mudança coerente que compila e passa nos testes.

### Versões
- SemVer (`v0.x.y` até a v1.0.0). Tag ao fim de cada marco relevante.
- `CHANGELOG.md` no formato *Keep a Changelog*, atualizado a cada release.

## 3. C++ (firmware)

### Base
- C++17, `-Wall -Wextra -Werror` no `native`; `-fno-exceptions -fno-rtti` no alvo.
- **Sem alocação dinâmica** (`new`, `malloc`, `std::string`, `std::vector`) no caminho de log
  após a inicialização. Preferir arrays de tamanho fixo e templates com capacidade em parâmetro.
- Tipos de largura fixa (`uint32_t`, `size_t`), `enum class`, `constexpr` em vez de `#define`.
- Headers com `#pragma once`. Tudo no namespace `logcat`.
- Formatação automática com `.clang-format` (baseado em Google, indentação 4, coluna 100).

### Nomes
| Elemento | Estilo | Exemplo |
|----------|--------|---------|
| Tipos, classes, structs, enums | `PascalCase` | `RingBuffer`, `LogLevel` |
| Funções, métodos, variáveis | `snake_case` | `write_line()`, `boot_count` |
| Membros privados | `snake_case_` | `head_`, `capacity_` |
| Constantes `constexpr` | `kPascalCase` | `kMaxTagLen` |
| Valores de `enum class` | `PascalCase` | `LogLevel::Warn` |
| Arquivos | `snake_case` | `ring_buffer.hpp`, `ring_buffer.cpp` |

### Organização
- Núcleo: `lib/logcat_core/include/logcat/<modulo>.hpp` + `src/<modulo>.cpp`.
- Nenhum `#include` de `esp_*`, `freertos/*`, `driver/*` ou LittleFS no núcleo.
- Adaptadores em `src/adapters/`, um par `.hpp/.cpp` por adaptador.

### Tags de log
- Maiúsculas, curtas (≤ 15 caracteres), uma por módulo: `static constexpr const char* TAG = "PRESSURE";`
- Tag reservada do sistema: `LOGCAT`. Último suspiro: `LASTWORDS`.

### Níveis — quando usar
| Nível | Uso |
|-------|-----|
| E | Falha que impede a função de continuar; força flush |
| W | Situação anormal recuperada (retry, valor fora da faixa) |
| I | Eventos de ciclo de vida (boot, conexão, mudança de estado) — pouco frequentes |
| D | Detalhe para depuração; desligado por padrão em produção |
| V | Rastreamento fino (cada amostra, cada byte) |

Nunca logar segredos (senha de Wi-Fi, chaves, tokens).

## 4. Python (host)

- Python ≥ 3.12, *src layout* (`host/src/esp_logcat/`), `pyproject.toml`.
- Dependências mínimas: `pyserial`, `rich`. Dev: `pytest`, `ruff`.
- `ruff check` e `ruff format` (linha 100). Type hints em toda função pública.
- Nomes PEP 8: `snake_case` para funções/módulos, `PascalCase` para classes.
- Lógica (parser, filtros) em funções puras, sem I/O → testáveis sem placa.
- Ambiente: `.venv` dentro de `host/`, nunca commitado.

## 5. Testes

| Onde | Ferramenta | Comando |
|------|-----------|---------|
| Núcleo C++ | Unity (`pio test`) | `cd firmware && pio test -e native` |
| Host | pytest | `cd host && pytest` |

- Nome do teste descreve o comportamento: `test_ring_buffer_overwrites_oldest_when_full`.
- Bug corrigido → teste que reproduz o bug entra no mesmo commit.
- Fixtures de log compartilhadas em `host/tests/fixtures/` e usadas pelos dois lados para
  garantir que formatter (C++) e parser (Python) concordam.

## 6. Documentação

- Mudou o comportamento → atualizar o documento correspondente no mesmo PR.
- `ROADMAP.md`: marcar itens concluídos ao fechar cada PR.
- Decisões não óbvias → ADR em `docs/decisions/NNN-titulo-curto.md`:

```markdown
# ADR-NNN: <título>
- Status: proposta | aceita | substituída por ADR-XXX
- Data: AAAA-MM-DD

## Contexto
<qual problema/força motivou a decisão>

## Decisão
<o que foi decidido>

## Consequências
<ganhos, custos, o que fica mais difícil>
```
