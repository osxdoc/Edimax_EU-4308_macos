# Repository Guidelines

## Project Structure & Module Organization

This repository implements an experimental macOS userspace driver for Edimax EU-4308 / ASIX AX88179 Ethernet adapters.

- `edimax_driver.c`: USB access through IOKit, packet forwarding through BPF and paired `feth` interfaces, link management, and cleanup.
- `tools/dump_mii.c`: standalone USB MII/PHY register diagnostic.
- `services/com.edimax.ax88179.driver.plist`: launchd service definition.
- `Install.command` and `Uninstall.command`: Finder-friendly Bash installers.
- `Makefile`: build and service lifecycle commands; `README.md`: user setup instructions.

`tests/test_driver.c` checks packet handling; `tests/test_service.sh` checks installers with isolated system-command mocks. There is no asset directory.

## Build, Test, and Development Commands

Use macOS with Xcode Command Line Tools (`clang`, `make`). Builds use IOKit, CoreFoundation, SystemConfiguration, and pthreads, with no external dependencies.

- `make`: build the universal `edimax_driver` binary for arm64 and x86_64.
- `make tools`: build `tools/dump_mii`.
- `make test`: run sanitizer-backed regression tests and configuration syntax checks.
- `make run` or `sudo ./edimax_driver`: run interactively; stop with Ctrl+C.
- `make install` / `make uninstall`: build the helper and install or remove the launchd service; requires administrator access.
- `make clean`: remove generated binaries.

## Coding Style & Naming Conventions

Follow existing C style: four-space indentation, opening braces on the declaration line, `snake_case` functions and variables, uppercase register constants, and `g_` prefixes for global state. Keep internal helpers `static`. Use tabs for Makefile recipes and quote shell paths. No formatter or linter configuration is tracked; compile with the existing `-Wall -Wextra` flags and avoid introducing warnings.

## Testing Guidelines

Tests use C assertions with AddressSanitizer and UndefinedBehaviorSanitizer; no coverage threshold is configured. Run `make clean && make && make tools && make test` for code changes. Hardware validation requires an AX88179 adapter: check link negotiation, DHCP, IPv4/IPv6 traffic, unplug/replug recovery, Wi-Fi failover, and interface cleanup on termination. Record macOS version, CPU architecture, hardware, commands, and results; identify untested cases explicitly.

## Commit & Pull Request Guidelines

History mixes imperative subjects with recent `fix:`, `feat:`, and `docs:` prefixes; prefer those prefixes and concise, specific subjects. Keep changes focused. PRs should describe the problem, resulting behavior, related issues, and validation performed. Include logs for driver changes and screenshots when Network settings behavior changes. Keep installer and Makefile behavior aligned through `tools/service.sh`, update README instructions when needed, and exclude generated binaries. Runtime and installation checks change system networking; use a machine where interruption is acceptable.
