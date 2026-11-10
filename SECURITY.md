# Security Policy

## Supported versions

esp32-logcat is pre-1.0 and maintained by one person. Only the latest code receives fixes:

| Version | Supported |
| ------- | --------- |
| `main` | ✅ |
| latest `0.x` release | ✅ |
| older `0.x` releases | ❌ — update to the latest release |

## Reporting a vulnerability

**Please do not open a public issue for security problems.**

Report privately through GitHub: **Security → Report a vulnerability** on this repository
([direct link](https://github.com/RenatoFaraco/esp32-logcat/security/advisories/new)).
Include the affected component (firmware core, firmware adapters, host tool), the version or
commit, steps to reproduce and, if possible, a log or proof of concept.

What to expect:

- **Acknowledgement within 7 days.** This is a personal project; there is no fixed deadline for
  a fix, but you will get updates on the advisory as the work progresses.
- If the report is accepted, the fix is developed in a private fork when needed, released, and
  the advisory is published crediting you (unless you prefer otherwise).
- If it is declined (for example, it falls under a known limitation below), you will get an
  explanation.

## Scope and known limitations

These are documented design decisions, not vulnerabilities. Reports about them are welcome as
feature requests:

- **Logs are not encrypted or signed.** Anyone with physical access to the device or its flash
  can read and modify stored logs. Signing and hash chaining are planned for v3 (see
  [`docs/context/PRD.md`](docs/context/PRD.md) §4).
- **No per-line checksum** ([ADR-007](docs/context/decisions/007-no-per-line-checksum-v1.md)): a
  corrupted byte inside an otherwise valid line can go undetected.
- **The USB console has no authentication** (milestone M5): whoever is connected to the device
  can read logs and run console commands, including `reboot` and `crash`.
- **Do not log secrets** (Wi-Fi credentials, keys, tokens). The logging system stores whatever
  it is given in plain text; see [`docs/project/CONVENTIONS.md`](docs/project/CONVENTIONS.md).

In scope, among others: memory-safety bugs in the firmware (buffer overflows, out-of-bounds
reads when parsing data read back from flash or RTC memory), a log message able to forge log
lines or console replies, and crashes of the host tool on crafted input.
