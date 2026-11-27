# Contributing to esp32-logcat

First of all, thank you for your interest in contributing to **esp32-logcat**!

Contributions of all kinds are welcome, including bug reports, feature requests, code improvements, tests, and documentation.

This project aims to provide reliable, persistent logging for ESP32-based firmware. Contributions should preserve its core principles: non-blocking logging, predictable resource usage, portability of the core, and reliable recovery of diagnostic information.

## Table of Contents

- [Before You Start](#before-you-start)
- [Ways to Contribute](#ways-to-contribute)
- [Reporting Bugs](#reporting-bugs)
- [Suggesting Features](#suggesting-features)
- [Development Setup](#development-setup)
- [Development Guidelines](#development-guidelines)
- [Testing](#testing)
- [Protocol and Conformance](#protocol-and-conformance)
- [Documentation](#documentation)
- [Git Workflow](#git-workflow)
- [Pull Requests](#pull-requests)
- [Code of Conduct](#code-of-conduct)

## Before You Start

Before opening an issue or submitting a pull request:

1. Check existing issues and pull requests to avoid duplicated work.
2. Read the project documentation relevant to your contribution.
3. For implementation work, review the current Product Requirements Document (PRD) and architecture.
4. For changes involving serial communication, parsing, or persistent storage, read the wire-format specification.

Useful references:

- [README](README.md)
- [Documentation index](docs/README.md)
- [Architecture](docs/context/ARCHITECTURE.md)
- [Current PRD](docs/context/prd/v1.md)
- [Wire format specification](docs/context/spec/wire-format.md)
- [Development conventions](docs/project/CONVENTIONS.md)
- [Roadmap](docs/project/ROADMAP.md)

When in doubt, open an issue before implementing a substantial change.

## Ways to Contribute

You can contribute in several ways:

- Report bugs and unexpected behavior.
- Suggest improvements to the firmware or host application.
- Improve the Python CLI and terminal interface.
- Add unit tests and conformance cases.
- Improve firmware reliability and resource efficiency.
- Improve documentation and examples.
- Review existing issues and pull requests.

Small, focused contributions are especially welcome.

## Reporting Bugs

A useful bug report should provide enough information to reproduce and investigate the problem.

Please include:

- A clear description of the observed behavior.
- The expected behavior.
- Steps to reproduce the issue.
- Relevant terminal output, logs, or error messages.
- Hardware model and ESP32 revision.
- ESP-IDF version, when relevant.
- Python and operating system versions, when relevant.
- Whether the issue occurs consistently or intermittently.

For firmware-related problems, also mention:

- Which USB connection was used (native USB-Serial-JTAG or USB-to-UART bridge).
- Whether the device was reset, power-cycled, or disconnected.
- Whether the problem survives a reboot.
- Whether persistent logs were available after the failure.

For parser or transport issues, provide the raw input whenever possible.

**Important:** Remove passwords, Wi-Fi credentials, access tokens, and other sensitive information before publishing logs.

## Suggesting Features

Feature requests should explain the problem before proposing a solution.

Please describe:

1. The use case.
2. The limitation or problem being addressed.
3. The expected behavior.
4. Any relevant constraints, such as RAM, flash, latency, or compatibility.

Features that affect the architecture, wire protocol, persistence model, or public CLI interface should be discussed before implementation.

## Development Setup

### Prerequisites

The current development environment uses:

- Python 3.12 or newer.
- ESP-IDF 6.1.1 (on Windows, installed with the EIM installer).
- CMake 3.25+, Ninja and a C++17 GCC for the firmware core tests on the PC.
- Git.
- An ESP32-C3 development board for hardware validation.

The host application can be developed and tested without an ESP32 board.

### Clone the repository

```bash
git clone https://github.com/RenatoFaraco/esp32-logcat.git
cd esp32-logcat
```

### Firmware environment

Install ESP-IDF 6.1.1 and open its terminal (on Windows, the ESP-IDF PowerShell that the EIM
installer creates; it also brings CMake and Ninja).

Run the core unit tests on the PC:

```bash
cd firmware/test/host
cmake --preset native && cmake --build --preset native && ctest --preset native
```

Build the ESP32-C3 firmware:

```bash
cd firmware/examples/demo
idf.py build
```

On Linux or WSL, the sanitizer presets can also be executed locally:

```bash
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
```

AddressSanitizer, UndefinedBehaviorSanitizer, and ThreadSanitizer help detect memory errors, undefined behavior, and data races.

These environments are also executed by the project's CI pipeline.

### Host environment

From the repository root:

```bash
cd host
python -m venv .venv
```

Activate the environment.

Windows PowerShell:

```powershell
.venv\Scripts\Activate.ps1
```

Linux/macOS:

```bash
source .venv/bin/activate
```

Install development dependencies:

```bash
pip install -e ".[dev]"
```

Run tests:

```bash
pytest
```

Run linting and formatting checks:

```bash
ruff check .
ruff format --check .
```

## Development Guidelines

### General principles

Contributions should preserve the following architectural principles:

- Logging must not block the application task.
- The logging hook should perform minimal work.
- Formatting and persistent storage should remain outside the caller's execution path.
- Memory usage should be predictable.
- The platform-independent core must remain independent of ESP-IDF and FreeRTOS.
- Hardware-specific behavior belongs in platform adapters.
- Failures in the logging subsystem should not unnecessarily compromise the application.

### Firmware (C++)

The firmware follows C++17 conventions.

Important requirements:

- Avoid dynamic memory allocation in the logging path after initialization.
- Prefer fixed-size buffers and bounded data structures.
- Use fixed-width integer types where appropriate.
- Avoid exceptions and RTTI in the target firmware.
- Keep the core independent of ESP-IDF, FreeRTOS, drivers, and LittleFS.
- Check return values from platform APIs.
- Avoid blocking operations in logging hooks and interrupt-sensitive paths.
- Consider stack usage, task priorities, queue capacity, and watchdog behavior.

Concurrency-sensitive changes require particular care.

When modifying queues, callbacks, hooks, shared buffers, or persistent storage, consider:

- Task and interrupt execution contexts.
- Thread safety and synchronization.
- Ownership and lifetime of shared data.
- Queue overflow behavior.
- Memory visibility and atomicity.
- Failure handling during reset or panic.

Do not introduce blocking synchronization into the logging path without an explicit architectural justification.

### Python (host)

The host application uses Python 3.12 and a `src` package layout.

Contributions should:

- Follow PEP 8 naming conventions.
- Include type hints for public functions.
- Keep parsing and filtering logic independent of serial I/O.
- Prefer small, testable functions.
- Handle malformed or incomplete input safely.
- Avoid unbounded buffering.
- Preserve compatibility between the firmware output and the host parser.

Use Ruff for linting and formatting.

### Wire protocol

The wire format is a shared contract between firmware and host software.

Any change affecting log lines, console requests, replies, dump framing, or session data must update:

1. The wire-format specification.
2. The firmware implementation.
3. The host implementation.
4. Relevant conformance cases.
5. Tests on both sides.

Protocol changes should be backward-compatible whenever practical. Breaking changes must be explicitly documented and discussed.

## Testing

Testing is an essential part of every contribution.

Choose the tests relevant to the modified components.

| Component | Command |
|---|---|
| Firmware core | `cd firmware/test/host && cmake --preset native && cmake --build --preset native && ctest --preset native` |
| Firmware ASan/UBSan (Linux) | same, with the `asan` preset |
| Firmware TSan (Linux) | same, with the `tsan` preset |
| ESP32-C3 build | `cd firmware/examples/demo && idf.py build` |
| Host application | `cd host && pytest` |
| Host lint | `cd host && ruff check .` |
| Host formatting | `cd host && ruff format --check .` |

### Testing expectations

- Every bug fix should include a regression test whenever practical.
- New functionality should include tests for normal and failure conditions.
- Boundary conditions should be tested explicitly.
- Parser changes should include malformed and incomplete input cases.
- Concurrency-sensitive core changes should be covered by ThreadSanitizer where applicable.
- Memory-sensitive changes should be covered by AddressSanitizer and UndefinedBehaviorSanitizer where applicable.

Hardware-dependent behavior must be identified separately from host-testable behavior.

If you cannot test on physical hardware, state that clearly in your pull request.

**Never claim that a hardware test was performed when only a host build or unit test was executed.**

### Continuous Integration

Pull requests are automatically checked by GitHub Actions.

The current CI pipeline covers:

- C++ formatting.
- Native firmware unit tests.
- ESP32-C3 firmware build.
- AddressSanitizer and UndefinedBehaviorSanitizer.
- ThreadSanitizer.
- Python tests.
- Python linting and formatting.
- GitHub issue form and workflow validation.

All applicable CI checks should pass before a pull request is merged.

## Protocol and Conformance

The `conformance/` directory contains shared test cases used to verify that firmware and host implementations agree on the wire format.

If a protocol change is necessary:

1. Update the relevant case generator.
2. Regenerate the corresponding TSV file.
3. Update firmware tests.
4. Update host tests.
5. Update the wire-format specification.
6. Ensure all affected tests pass.

The expected behavior must be defined independently of either parser implementation. Neither the C++ formatter nor the Python parser should be treated as the sole source of truth.

## Documentation

Documentation is part of the implementation.

When changing behavior, update the corresponding documentation in the same pull request.

Use the following organization:

- `docs/project/`: user-facing documentation, manuals, conventions, and roadmap.
- `docs/context/`: requirements, architecture, hardware information, specifications, and architectural decisions.

Architectural decisions that are not obvious should be documented as an ADR.

A new ADR should follow the template in `docs/project/CONVENTIONS.md` and be added to the documentation index.

If a milestone is completed, update the relevant entries in `docs/project/ROADMAP.md`.

## Git Workflow

### Branches

The `main` branch should remain buildable and pass CI.

Create a dedicated branch for each contribution:

```bash
git switch main
git pull origin main
git switch -c feat/short-description
```

Use one of the following prefixes:

| Prefix | Purpose |
|---|---|
| `feat/` | New functionality |
| `fix/` | Bug fixes |
| `docs/` | Documentation changes |
| `chore/` | Maintenance and tooling |

Examples:

```text
feat/console-history
fix/serial-reconnect
docs/contributing-guide
chore/ci-cleanup
```

### Commit messages

Follow Conventional Commits:

```text
<type>(<optional-scope>): <imperative summary>
```

Use a concise summary of no more than 72 characters.

Supported types include:

- `feat`
- `fix`
- `test`
- `refactor`
- `docs`
- `build`
- `ci`
- `chore`
- `perf`

Examples:

```text
feat(host): add session replay command
fix(fw): prevent queue access during shutdown
test(core): cover ring buffer overflow
docs: add contribution guidelines
```

Keep commits focused. One commit should represent one coherent change.

## Pull Requests

Pull requests are the preferred way to submit contributions, including contributions from the repository owner.

Before opening a pull request:

- Rebase or update your branch against the latest `main`.
- Run the relevant tests.
- Run formatting and linting checks.
- Review your own diff.
- Update documentation where necessary.
- Ensure no generated build artifacts, local configuration, or secrets are included.

### Pull request description

A good pull request should include:

- A concise explanation of the problem.
- What was changed and why.
- The related issue, if applicable.
- Tests performed and their results.
- Hardware validation performed, if applicable.
- Known limitations or tests that could not be performed.

For example:

```markdown
## Summary

Describe the problem and the proposed solution.

## Changes

- Describe the main implementation changes.
- Mention relevant documentation updates.

## Validation

- [ ] Firmware native tests
- [ ] Firmware sanitizers (if applicable)
- [ ] ESP32-C3 build (if applicable)
- [ ] Host tests (if applicable)
- [ ] Hardware validation (if applicable)
- [ ] Documentation updated

## Limitations

Describe anything that could not be verified.
```

Keep pull requests focused on a single logical change whenever possible.

A pull request may be requested to undergo revisions before merging. This is part of the review process and helps maintain the reliability and consistency of the project.

## Code of Conduct

All contributors are expected to follow the project's [Code of Conduct](CODE_OF_CONDUCT.md).

Be respectful, constructive, and open to technical discussion.

## License

By contributing to this repository, you agree that your contributions will be distributed under the project's [MIT License](LICENSE).

Thank you for helping make esp32-logcat more reliable!
