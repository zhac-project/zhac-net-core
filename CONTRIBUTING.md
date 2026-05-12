# Contributing to zhac-net-core

ESP32-S3 firmware — WiFi, REST, WebSocket, MQTT, Web UI host.

## License and CLA

Licensed under **AGPL-3.0-or-later**. All contributions require signing
`CLA.md`. See `CONTRIBUTORS.md` for signup.

## Prerequisites

- ESP-IDF v6.0 (`xtensa-esp-elf` for ESP32-S3)
- Node.js ≥18 (for the Web UI submodule)
- Python ≥3.10 (pytest, optional)

## Build

The SPA must be built first — its `dist/` becomes the SPIFFS partition
source.

```bash
git submodule update --init --recursive    # pulls www-spa
(cd www-spa && npm ci && npm run build)
idf.py set-target esp32s3
idf.py build
```

When building from `zhac-platform`, all this runs automatically via
`just build`.

## Flash + monitor

```bash
idf.py -p /dev/ttyUSB0 flash monitor
idf.py -p /dev/ttyUSB0 spiffs-flash    # just the SPA partition
```

## SPDX headers for new files

```c
// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
```

## Adding a new API endpoint

API handlers are **transport-agnostic** — one function serves both
REST and WS via an `ApiHandlerFn` typedef.

1. Implement the handler in `main/api_*.cpp` (`api_*` naming).
2. Register the WS command in `main/ws_bridge.cpp` dispatch table.
3. If a REST wrapper is needed, add it in `main/rest_*.cpp`.
4. Document in the platform repo's `docs/WS_API.md`.
5. Add a pytest test under `tests/` (or in the platform repo's
   integration suite for hardware-dependent paths).

## Socket pool warning

The embedded httpd has a bounded socket pool (`CONFIG_LWIP_MAX_SOCKETS
= 16`, `httpd_config.max_open_sockets = 9`). Mobile browsers open up
to 6 parallel TCP connections for speculative fetches. Before adding
a feature that opens new long-lived sockets (a second WebSocket, a
streaming endpoint, etc.), audit peak concurrent socket usage.

## Style

- C++17 (no exceptions, no RTTI).
- 4-space indent, `snake_case` vars/funcs, `UPPER_CASE` macros.
- No `std::string`, `std::vector` in hot paths.
- Prefer `snprintf` over concat.

## Running tests

```bash
pytest tests/ -v
```

## Reporting bugs

Open an issue with:
- Firmware version (`zhac status` → `s3.fw`)
- Reset reason from boot log
- Minimal reproduction — curl / wscat trace ideal
- Browser + version if the UI is involved
