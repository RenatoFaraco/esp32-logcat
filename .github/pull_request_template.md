## Summary

<!-- What changes and why. Keep the "why"; the diff already shows the "how". -->

Closes #

**Milestone:** M?

## Type

- [ ] feat
- [ ] fix
- [ ] test
- [ ] refactor / perf
- [ ] docs
- [ ] build / ci / chore

## How it was tested

- [ ] `pio test -e native`
- [ ] `pio run -e esp32c3`
- [ ] `pytest` and `ruff check . && ruff format --check .` (host)
- [ ] On hardware — board, commit and what was observed:

<!-- Logs, measurements or captures that support the change. -->

## Checklist

- [ ] Commits follow Conventional Commits and each one builds and passes tests
- [ ] C++ formatted with clang-format; Python with ruff
- [ ] Core stays platform-independent (no `esp_*`, `freertos/*`, `driver/*`, LittleFS includes)
- [ ] No dynamic allocation on the logging path; logging never blocks
- [ ] Line format unchanged — or host parser, C++ and Python tests updated in this PR
- [ ] RAM / flash / timing impact considered (ARCHITECTURE §8, PRD §7 criterion 3)
- [ ] Docs updated: ROADMAP items, ADR for non-obvious decisions, README if user-facing
- [ ] No secrets committed (Wi-Fi credentials, keys, tokens)
