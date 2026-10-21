# Changelog

All notable changes to this project are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Core `LogRecord` (160 B, trivially copyable for the FreeRTOS queue) with bounded tag/message
  copies and truncation flag; `LogLevel` numbered like ESP-IDF's levels, with letter
  conversions for the line format.
- Core `Sequencer`: per-boot `seq` wrapping at 1,000,000 (6-digit field) and a dropped-line
  counter, safe for concurrent loggers.
- The target build now compiles the core library.
- ADR-006: log lines are printable ASCII only (`0x20`–`0x7E`), single-line, enforced by the
  formatter and checked by the parsers.
- Core `format_line`: `LogRecord` to the PRD §3 line (max 170 bytes) in a fixed-size buffer,
  no printf. Boot printed modulo 10000, seq modulo 1e6, non-ASCII and control bytes mapped to
  `?`, empty tag `-`, truncated messages shown as 124 chars + `...`.
- ADR-007: no per-line checksum in v1.
- Core `parse_line`: strict parser for the PRD §3 line, returning a categorized status
  (`ok`, `torn`, `garbage`, `bad_header`, `bad_level`, `overlong`). Torn lines (no final `\n`
  or closed with DEL) are never taken as records; `format_line(parse_line(L)) == L` for every
  valid line.

## [0.1.0] - 2026-10-21

Milestone **M0 · Foundation**: project scaffolding, build environments and CI.
No logging functionality yet.

### Added

- **Firmware project** (PlatformIO, ESP-IDF 6.x via `espressif32` 7.1.3, pinned):
  - `env:esp32c3` for the target (generic ESP32-C3, DIO flash) with a "hello" app.
  - `env:native` for host unit tests with Unity: strict C++17, `-Wall -Wextra -Werror`,
    no exceptions, no RTTI, plus a toolchain test asserting the C++ standard.
  - Custom partition table: NVS, 1.5 MB factory app, 512 KB `logs` (LittleFS), 64 KB core dump.
  - `sdkconfig.defaults` with flash size, custom partitions and C++ exceptions/RTTI disabled.
- **Host tool** `esp-logcat` (Python 3.12): package skeleton with the `stream`, `dump`, `shell`
  and `replay` subcommands registered as placeholders, ruff and pytest configured.
- **CI** (GitHub Actions): native tests, target build, C++ formatting check (clang-format),
  ruff, pytest, and schema validation of issue forms and workflows.
- **Docs**: PRD, architecture, roadmap, conventions and ADRs 001–005 (ESP-IDF via PlatformIO,
  text line format, platform-independent core, drop on full queue, strict C++17 on the core).
- **Contribution tooling**: issue forms (bug, crash/unexpected reset, feature, performance,
  documentation and planned-work tasks), pull request template, and
  `scripts/github_bootstrap.py` recording the M0–M7 plan as labels, milestones and issues.
- **Claude Code project setup**: subagents for firmware review, persistence audit, test running
  and milestone checks, with pre-approved test commands.
- Repository basics: MIT license, `.gitignore`, `.gitattributes` (LF everywhere),
  `.clang-format`.

[Unreleased]: https://github.com/RenatoFaraco/esp32-logcat/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/RenatoFaraco/esp32-logcat/releases/tag/v0.1.0
