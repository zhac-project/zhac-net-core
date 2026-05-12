# zhac-net-core

ESP32-S3 firmware for [ZHAC]. Hosts the WiFi stack, HTTP/WebSocket
server, REST endpoints, MQTT gateway, and embeds the Web UI into
its SPIFFS partition. Communicates with the P4 main core over SPI
via the custom HAP binary protocol.

[ZHAC]: https://github.com/zhac-project/zhac-platform

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

When building from `zhac-platform`, all overrides are exported
automatically.

## Flash

```bash
idf.py -p /dev/ttyUSB0 flash monitor
idf.py -p /dev/ttyUSB0 spiffs-flash   # SPIFFS only (after SPA rebuild)
```

## License

GNU AGPL v3 or later. See `LICENSE`.

## Contributing

See `CONTRIBUTING.md`. All contributions require signing `CLA.md`.

## Versioning

Releases tagged `vYYYYMMDDVV`. See `zhac-platform` README for the
scheme.
