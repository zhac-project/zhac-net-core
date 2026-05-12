# ONBOARDING — zhac-net-core (ESP32-S3 firmware)

You are an AI agent arriving on **zhac-net-core**, the S3-side
firmware for ZHAC. This repo owns the WiFi gateway (REST, WS, MQTT)
and the S3 half of the HAP protocol. It also nests `www-spa` as a
submodule so standalone clones carry the UI. Read top-to-bottom
before coding.

---

## 1. Platform context

**ZHAC** = dual-chip ESP32 Zigbee Home Automation Controller.

- **ESP32-S3** runs this firmware — WiFi, REST, WebSocket, MQTT.
- **ESP32-P4** runs `zhac-main-core` — Zigbee coordinator, Lua.
- They talk over SPI using the custom **HAP** binary protocol. S3 is
  the host; P4 is the slave.
- `www-spa` (Preact 10 + Vite 5) is bundled into S3 SPIFFS.

Data flow (S3 perspective):

```
Web UI (Preact) ──WS── ws_server ──► api_handlers (transport-agnostic)
                                          │
                                          ▼
                                      hap_master ──► SPI ──► P4
                                          ▲                  │
                                          │           (decoded attributes)
                                          └──────────────────┘
                                          │
                                          ├──► ws_event_broadcast ──► UI
                                          └──► mqtt_gw_publish ──► broker
```

### Repo split

Tag `v2026042301` (2026-04-23) baseline. 7 repos:
`zhac-platform`, `embedded-zhc`, `zhac-components`, `zhac-main-core`,
**`zhac-net-core`** *(this)*, `www-spa` (nested submodule),
`zhac-docs`.

---

## 2. What this repo owns

- `main/` — entry, REST + WS bridges, API handlers, hap_bridge, logs,
  metrics, WiFi manager.
- `components/` — S3-only (rule store + engine, MQTT, ws_server, cron,
  HAP master).
- `www-spa/` — **nested submodule** so the S3 SPIFFS partition can be
  built without a separate checkout.

### Layout

```
zhac-net-core/
├── main/
│   ├── main.cpp
│   ├── api_handlers.{h,cpp}     (33 transport-agnostic C handlers, ~1 547 LOC)
│   ├── api_system.cpp           (WiFi, permit-join, reboot, system info)
│   ├── api_devices.cpp · api_groups.cpp · api_rules.cpp …
│   ├── rest_*.cpp               (HTTP wrappers around api_*)
│   ├── ws_bridge.cpp            (35-entry dispatch table)
│   ├── hap_bridge.cpp           (SPI master glue to P4)
│   ├── log_ring.cpp             (vprintf hook → ring buffer → MQTT / WS)
│   ├── metrics_mqtt.cpp · wifi_mgr.{h,cpp} · groups_store.{h,cpp}
│   ├── idf_component.yml        (10 shared component deps)
│   └── CMakeLists.txt
├── components/
│   ├── ws_server/               (WebSocket server, envelope parser)
│   ├── hap_master/              (S3 side of HAP — SPI master)
│   ├── mqtt_gw/                 (MQTT gateway with worker-task publish)
│   ├── mqtt/                    (low-level MQTT client glue)
│   ├── rule_store/              (NVS-backed rule DSL store)
│   ├── simple_rules/            (rule engine — trigger/action dispatch)
│   └── cron_parser/             (cron expression parser for rules)
├── www-spa/                     (nested submodule → https://github.com/zhac-project/www-spa.git)
├── partitions.csv · sdkconfig.defaults
├── CMakeLists.txt
└── LICENSE · NOTICE · CLA.md · CONTRIBUTORS.md · CONTRIBUTING.md
```

### Shared deps (from `zhac-components`)

Pulled via `main/idf_component.yml`:
`zap_common`, `zap_store`, `event_bus`, `device_shadow`,
`device_backend`, `zhc_adapter`, `hap_protocol`, `hap_session`,
`hap_json`, `metrics`. Plus `embedded-zhc` via CMake `FetchContent`.

---

## 3. Building

The web UI builds **first** — its `dist/` feeds the SPIFFS partition
image.

```bash
# 1. Build the Web UI (nested submodule)
cd www-spa
npm install
npm run build          # produces www-spa/dist/
cd ..

# 2. Build S3 firmware
idf.py set-target esp32s3
idf.py build
```

With local sibling checkouts:

```bash
export IDF_COMPONENT_OVERRIDE_PATH=$HOME/webapp/zhac/zhac-components/components
export EMBEDDED_ZHC_PATH=$HOME/webapp/zhac/embedded-zhc
idf.py build
```

**Do not run `idf.py build` for the user.** They build firmware
themselves. Job stops at code changes.

If you edited anything under `www-spa/`, rebuild the SPA **before**
telling the user to reflash — the SPIFFS partition image is
regenerated from `www-spa/dist/`.

---

## 4. Key surfaces

### `main/api_handlers.{h,cpp}` — the 33 transport-agnostic handlers

Every REST and WS endpoint reduces to one of these `api_*` functions.
Both `rest_*.cpp` wrappers and the WS dispatch table (`ws_bridge.cpp`,
35 entries) call the same C handler. **New endpoints land here
first.**

Signature pattern:

```cpp
esp_err_t api_<name>(const cJSON* args, cJSON* out, char* err, size_t err_len);
```

Wire it into REST (`rest_*.cpp`) and WS (`ws_bridge.cpp`) as matching
pairs.

### `main/ws_bridge.cpp` — WS dispatch

35 entries. Maps WS `cmd` strings to `api_*` handlers. Envelope
shape:

```
client → server:  {id, cmd, args}
server → client:  {id, ok, data|err}
```

Push events (emitted without a client `id`):
`device.added`, `device.updated`, `device.removed`, `attr.bulk`,
`alert.*`. Broadcast via `ws_event_broadcast(event_name, payload)`.

### `main/hap_bridge.cpp` — SPI master glue

Everything traveling over HAP to P4 goes through this file. Four call
sites into `mqtt_gw_publish` for diagnostics — all updated to the
new `size_t payload_len` API.

### `components/mqtt_gw/`

**Publish is non-blocking** via a worker task + bounded queue. Do
**not** revert this — the S3 freeze tracked down in 2026-04 was
`mqtt_gw_publish` blocking the `ESP_LOG` vprintf hook via
`log_ring.cpp`.

```cpp
struct PubItem { char* topic; char* payload; size_t payload_len; uint8_t qos; bool retain; };
static QueueHandle_t s_pubq = nullptr;
static void mqtt_pub_worker(void*) { /* xQueueReceive → esp_mqtt_client_publish */ }
void mqtt_gw_publish(...) {
    // heap-alloc PubItem
    // xQueueSend(timeout=0) — drop on full
    // NO LOGGING
}
```

API takes explicit `size_t payload_len` now; every call site
(`hap_bridge.cpp` x4, `metrics_mqtt.cpp`, `log_ring.cpp`,
`zhac_lua_module.cpp` in main-core, `simple_rules.cpp`,
`mqtt_gw_p4.cpp`) was updated.

### `components/simple_rules/`

Rule engine — parses DSL, matches triggers, dispatches actions. DSL
documented in `zhac-docs/RULES_DSL.md`. Rules run on S3; Lua runs on
P4. Two different engines on purpose.

### `main/api_system.cpp` highlights

- `api_zigbee_permit_join` / `api_zigbee_permit_join_status` — the
  latter is a server-side status endpoint with a countdown timer
  driven by `s_permit_join_deadline_us` (`esp_timer_get_time()`).
- `api_wifi_connect` uses a one-shot `esp_timer_start_once(_,
  1000000)` to reboot **after** the WS reply has flushed. Do not
  call `esp_restart()` inline.

---

## 5. Cross-repo contracts

### WS envelope (SPA↔S3)

```
client → server:  {id, cmd, args}
server → client:  {id, ok, data|err}
```

Push events: no `id`. Fields documented in `zhac-docs/WS_API.md`.

### HAP (S3↔P4)

Defined in `zhac-components/components/hap_protocol/`. This repo
implements the host (S3) side; `zhac-main-core` implements the slave
(P4) side.

### `ZclAttribute` (52 B)

Canonical in `zap_common`. Shared with P4 and Web UI wire format.

### Sizes

`ZclAttribute` = 52 B, `ZclAttrEvent` = 96 B, `ShadowAttr` = 52 B,
`ZapDevice` = 522 B. Changes → bump `NVS_SHADOW_VERSION` (v4).

---

## 6. Conventions

- **C++17** (`-fno-exceptions -fno-rtti`).
- **Transport-agnostic handlers first.** New endpoint? Write
  `api_<name>` in `api_handlers.{h,cpp}` first, then wire REST and WS.
- **Never log from `mqtt_gw_publish` path.** Permanent rule.
- **Bounded queues** everywhere the log pipeline or an ISR can reach.
  Drop-on-full is the default.
- **String keys ≤ 20 chars, string values ≤ 24 chars.**
- **Use `zhc_adapter` exclusively** for attribute operations. No
  direct `zhc::` includes in firmware.
- **WS pushes are how the UI stays in sync.** Emit
  `device.updated` / `attr.bulk` promptly after any shadow change
  that must reach the SPA.

---

## 7. User preferences (persistent)

- **User builds firmware themselves.** Don't run `idf.py build`.
- **Early-dev stance.** Breaking changes OK. No migration shims, no
  dual code paths.
- **Prefer hook/callback registration** when two components would
  need each other (see `zhac_adapter_register_shadow_hook`).

---

## 8. Gotchas

- **Don't reintroduce inline `esp_restart()`**. `api_wifi_connect`
  uses a deferred timer for a reason.
- **Don't block the log pipeline.** If in doubt, `ESP_LOGD` is fine;
  anything that might publish MQTT / emit WS / hit the network from
  within a log handler is not.
- **Permit-join status endpoint is server-side.** Don't poll from the
  UI without calling `api_zigbee_permit_join_status` — the deadline
  lives on S3, not in the browser.
- **WS reply before restart.** Any handler that triggers reboot must
  return a response first, then defer the restart.
- **SPA ↔ firmware mismatch.** Build `www-spa` before flashing S3
  after UI changes. The nested submodule is a convenience, not a
  guarantee the build system rebuilds it automatically on `npm
  install`-less trees.
- **Devices.jsx moved the "hard remove" checkbox to DeviceDetail.**
  The server still accepts the flag; don't reinstate the list-level
  checkbox.
- **Optimistic UI belongs on both ends.** AttrBoolRow / AttrEnumRow
  in the SPA use `localV` overrides; P4's `handle_set_attribute`
  writes an optimistic shadow. S3 relays — no extra optimism needed
  here.

---

## 9. Licensing

- **AGPL-3.0-or-later** for this repo.
- Every file starts with:
  ```
  // SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
  // SPDX-License-Identifier: AGPL-3.0-or-later
  ```
- `LICENSE` points to `LICENSES/AGPL-3.0-or-later.txt`.
- CLA via `CLA.md` — Apache ICLA v2.2 + §4 relicensing grant. Sign
  by adding yourself to `CONTRIBUTORS.md` in your first PR (covers
  all 7 ZHAC repos).

Nested `www-spa` is also AGPL. The SPIFFS image is a build artefact
of both.

---

## 10. Where to go next

- **REST API**: `zhac-docs/REST_API.md`, `openapi.yaml`.
- **WS API**: `zhac-docs/WS_API.md`.
- **Rules DSL** (S3-side engine): `zhac-docs/RULES_DSL.md`.
- **HAP protocol**:
  `zhac-components/components/hap_protocol/README.md`.
- **ZCL attribute flow**:
  `zhac-components/components/device_shadow/README.md`.
- **Knowledge graph** (legacy monorepo): `graphify-out/graph.json`
  (old monorepo, internal) — query with `/graphify query "..."` if available.

---

*Tag on first split: `v2026042301` · 2026-04-23.*
*License: AGPL-3.0-or-later · Maintainer: Evgenij Cjura.*
