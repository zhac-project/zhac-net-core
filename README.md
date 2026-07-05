# zhac-net-core

ESP32-S3 firmware for [ZHAC]. Hosts the WiFi stack, HTTP/WebSocket
server, REST endpoints, MQTT gateway, and embeds the Web UI into
its SPIFFS partition. Communicates with the P4 main core over SPI
via the custom HAP binary protocol.

[ZHAC]: https://github.com/zhac-project/zhac-docs

## Responsibilities

- WiFi (STA / AP / APSTA) + mDNS
- HTTP / WebSocket server (port 80 + `/ws`)
- REST + transport-agnostic API handlers
- MQTT client (optional — toggleable at runtime)
- OTA updates
- NTP + time sync
- Web UI SPIFFS partition generation from `www-spa/dist/`

## Tree

```
zhac-net-core/
├── main/                (firmware entry, Kconfig, idf_component.yml)
├── components/
│   ├── hap_master/
│   ├── ws_server/
│   ├── mqtt_gw/
│   ├── mqtt/            (vendored esp-mqtt Apache-2.0)
│   ├── rule_store/
│   ├── simple_rules/
│   └── cron_parser/
├── www-spa/             (SUBMODULE — https://github.com/zhac-project/www-spa)
├── CMakeLists.txt
└── sdkconfig.defaults
```

Shared components are pulled from [zhac-components]; the ZHC library
from [embedded-zhc].

[zhac-components]: https://github.com/zhac-project/zhac-components
[embedded-zhc]:    https://github.com/zhac-project/embedded-zhc

## Building standalone

```bash
git clone --recursive https://github.com/zhac-project/zhac-net-core.git
cd zhac-net-core
source /path/to/esp-idf-v6.0/export.sh

# Build the SPA first — its dist/ becomes the SPIFFS image source.
(cd www-spa && npm ci && npm run build)

idf.py set-target esp32s3
idf.py build
```

For local development with live component overrides:

```bash
git clone https://github.com/zhac-project/zhac-components.git ../zhac-components
git clone https://github.com/zhac-project/embedded-zhc.git   ../embedded-zhc

export IDF_COMPONENT_OVERRIDE_PATH=$PWD/../zhac-components/components
export EMBEDDED_ZHC_PATH=$PWD/../embedded-zhc
idf.py build
```

With the sibling layout above, CMake also resolves
`../zhac-components/components` without `IDF_COMPONENT_OVERRIDE_PATH` — the
export just makes the override explicit.

## Flash

```bash
idf.py -p /dev/ttyUSB0 flash monitor
idf.py -p /dev/ttyUSB0 spiffs-flash   # SPIFFS only (after SPA rebuild)
```

## API authentication

> **TODO — revisit before production.** Auth currently defaults **OFF**
> (`auth_init()` in `main/main.cpp`, `auth_en = 0`). While off, **any client on
> the LAN/Wi-Fi can drive the controller unauthenticated** — REST, WebSocket,
> OTA, Zigbee reset, and arbitrary Lua via `script.run`. This is FINDINGS.md
> **A1 / F1**, deliberately left open for zero-config development. Re-enable
> (set `auth_en = 1`, or build a hardened image) before shipping.

The enforcement machinery is already in place and activates the moment auth is
enabled — only the default is off.

### How it works

- A random 128-bit token is generated on first boot and persisted in NVS
  (`zhac_auth/token`). Auth on/off lives in `zhac_auth/enabled`.
- **REST**: every mutating route is gated by `REQUIRE_AUTH` and checks the
  `X-Api-Key` header with a constant-time compare. A sliding-window lockout
  throttles failed attempts.
- **WebSocket**: the `/ws` handshake requires the token as a `?token=` query
  param (browsers can't set WS headers). With auth off, `/ws` is open.
- `GET /api/status` and the static SPA assets stay unauthenticated so the UI
  can always load.

### Enabling auth

1. **Get the token.** When auth is enabled the token is printed to the
   **serial console on every boot** (never to `/api/logs`):
   `*** ZHAC API auth ENABLED — token (serial-only): <hex> ***`
2. **Enable it** — either from the UI (**Settings → "Auth (bearer token)"**
   toggle, persists `enabled=1`) or by flipping the default in `auth_init()`
   for a hardened build.
3. **Give the browser the token** — **Settings → "This browser's token"**,
   paste, Save (writes `localStorage.zhac_token`, reconnects). For a browser
   without the UI field yet, DevTools console:
   `localStorage.setItem('zhac_token','<hex>'); location.reload()`.
4. Token rotation is available via `/api/system` (`system.token.rotate`).

### Known hardening gaps (tracked in FINDINGS.md)

- **A1 / F1** — auth defaults off (above).
- **F18 / A9** — the WS token travels in the URL query string (leaks to proxy
  / access logs); a `Sec-WebSocket-Protocol` token or short-lived ticket is the
  planned replacement.
- **F2** — the token (and all NVS secrets) sit in plaintext flash unless Secure
  Boot + Flash Encryption are enabled; see `sdkconfig.prod.defaults`.
- **Release log level** — release builds MUST keep `CONFIG_LOG_DEFAULT_LEVEL`
  ≤ INFO (value ≤ 3). `esp_http_client` logs the full request URL at DEBUG, and
  the `tg_gw` Telegram client carries the bot token in that URL — a DEBUG/VERBOSE
  image would leak the bot token to the serial console / `/api/logs`.
  `sdkconfig.prod.defaults` pins WARN (level 2); do not raise it for shipping
  images.

## License

GNU AGPL v3 or later. See `LICENSE`.

## Contributing

See `CONTRIBUTING.md`. All contributions require signing `CLA.md`.

## Versioning

Releases tagged `vYYYYMMDDVV` (UTC date + 2-digit revision). Each repo
tags its own version independently.
