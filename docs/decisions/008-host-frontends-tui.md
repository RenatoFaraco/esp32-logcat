# ADR-008: Host em camadas, com TUI (Textual) como segundo frontend
- Status: aceita
- Data: 2026-11-07

## Contexto
O PRD (H1–H6) define o host como CLI: `stream`, `replay`, `dump` e `shell`. Para acompanhar a
placa durante o desenvolvimento, uma interface interativa no terminal ajuda: rolar o histórico,
pausar, trocar o filtro sem reiniciar e ver contadores (lacunas de `seq`, linhas inválidas,
estado da conexão). Mas o valor do `adb logcat` está no stdout em pipe (`| grep`,
`> arquivo`, CI), que um TUI não oferece.

Opções de TUI avaliadas:
- `curses`: não existe nativo no Windows (plataforma principal do projeto).
- `urwid`: não roda nativo no Windows.
- `prompt_toolkit`: mais baixo nível, sem widgets de log prontos.
- **Textual**: da mesma equipe do `rich`, que já é dependência. Renderiza objetos
  `rich.text.Text`, é assíncrono, roda no Windows Terminal e no terminal do VS Code, e tem
  `App.run_test()` + Pilot para testar sem terminal.

## Decisão
1. O host é organizado em camadas, e cada frontend é só um consumidor:
   ```
   transport (bytes) → parser (Line) → stats/filters → render (rich Text) → { cli stream | tui }
   ```
   `parser`, `filters`, `transport`, `session` e `stats` não importam `rich` nem `textual`;
   `render` importa só `rich`; só o pacote `tui/` importa `textual`.
2. O renderer devolve `rich.text.Text`, montado com estilos explícitos: o texto da placa é
   dado, nunca markup, e caracteres de controle viram símbolos visíveis. O mesmo objeto serve
   ao `stream` (impresso num `Console`) e ao TUI (num `RichLog`).
3. O TUI é um comando a mais, `esp-logcat tui` (PRD H7), feito com Textual (`>=8.2,<9`, como
   dependência normal). `stream` e `replay` continuam texto puro em stdout.

## Consequências
- Filtros, estatísticas e reconexão são escritos uma vez e valem para os dois frontends.
- O TUI chega depois dos filtros (#17), que ele usa; `shell` (M5) e `dump` (M4/M5) entram
  depois como painéis.
- A leitura da serial no TUI fica numa thread worker, com atualização da UI em lotes e
  scrollback limitado: a placa pode mandar ~1 kHz de linhas.
- Mais uma dependência (Textual), que muda a API entre versões principais: faixa travada em
  `<9`.
- As regras de camada são verificadas pelo subagente `host-reviewer`.
