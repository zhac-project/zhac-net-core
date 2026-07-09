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

Auth is **secure-by-default**: on a fresh unit (no stored preference) the REST +
WebSocket API require a token. The default is set by
`CONFIG_ZHAC_API_AUTH_DEFAULT_ENABLED` (Kconfig, default `y`); a choice the
operator later makes in the WebUI is stored in NVS (`zhac_auth/enabled`) and
always overrides the build default across reboots and updates.

### The token

- On first boot the unit gets an API token and persists it in NVS
  (`zhac_auth/token`). By default it is a **unique 32-hex-char (128-bit) random
  token per device**; a fleet image can instead seed a **known** bootstrap token
  at build time via `CONFIG_ZHAC_DEFAULT_API_TOKEN` (leave it empty in public
  builds — a shared, committed token is a foot-gun).
- The token is printed to the **serial console on boot** (never to `/api/logs`):
  `*** ZHAC API auth ENABLED — token (serial-only): <hex> ***`.

### How it works

- **REST**: every mutating route is gated by `REQUIRE_AUTH`, which checks the
  `X-Api-Key` header with a constant-time compare. A sliding-window lockout
  throttles failed attempts.
- **WebSocket**: the first frame on `/ws` must be an auth handshake —
  `{"cmd":"auth","args":{"token":"<hex>"}}` — before any other command runs (the
  token rides a WS frame, not the URL). This replaced the earlier `?token=`
  URL-query scheme (FINDINGS.md **F18**).
- `GET /api/status` and the static SPA assets stay unauthenticated so the UI can
  always load. Provisioning routes (`/api/wifi/*`) **are** gated — so a fresh
  unit needs its token before it can be onboarded (see below).

### Onboarding a fresh unit

Because provisioning is auth-gated, you need the token before WiFi setup:

- **Community / single unit** — read the random token from the serial console on
  first boot, then use it to provision.
- **Fleet image** — set a known `CONFIG_ZHAC_DEFAULT_API_TOKEN` so every unit
  comes up with the same label credential; provision with it, then rotate.

Give the browser the token in **Settings → "This browser's token"** (paste,
Save — writes `localStorage.zhac_token` and reconnects), or from DevTools:
`localStorage.setItem('zhac_token','<hex>'); location.reload()`.

### Changing / disabling auth

- **Rotate the token** — WebUI, or `POST /api/system/token/rotate` (generates a
  fresh token and de-authes live sockets).
- **Turn auth off at runtime — recommended for development.** WebUI
  **Settings → Auth**, or `POST /api/settings` with `{"auth_enabled": false}`.
  The choice persists in NVS and survives reboots and firmware updates, so you
  set it **once per dev unit** and it stays off across SPA/firmware rebuilds.
  On a fresh secure-by-default unit you need the token first to reach Settings:
  read the random token from the **serial console on first boot**, enter it in
  the SPA's sign-in gate, then toggle auth off. This touches nothing in the repo
  and leaves the shipped default secure.
- **Build a development image that boots with auth off (fresh NVS).** Disable the
  build default — but note `sdkconfig` is **checked in**, so do **not** flip it in
  place and commit it (that ships an insecure default to everyone). Instead:
    - `idf.py menuconfig` → *ZHAC* → turn off **“Require REST/WebSocket API auth
      by default”**, build, then `git checkout sdkconfig` so the change stays
      local; **or**
    - keep an un-committed `sdkconfig.dev.defaults` with
      `CONFIG_ZHAC_API_AUTH_DEFAULT_ENABLED=n` and build into a throwaway config
      so the tracked one is untouched:
      `idf.py -DSDKCONFIG=build/sdkconfig.dev -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.dev.defaults" build`.

  Either way the committed `sdkconfig.defaults` and `sdkconfig.prod.defaults`
  stay `=y`. The build default only applies to a **fresh NVS** — a unit that has
  ever had an auth preference set keeps it; `idf.py erase-flash` (or erase the
  `zhac_auth` namespace) to re-apply the default.

> **Warning — an auth-off image has no access control.** Any client on the LAN
> or within Zigbee RF range gets full unauthenticated control of the controller:
> firmware OTA, Zigbee network reset, and Lua script execution (arbitrary code).
> Use an open build only on a trusted development network, never on anything
> reachable from an untrusted host.

### Remaining hardening gaps (tracked in FINDINGS.md)

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
