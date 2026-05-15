# ws_server — WebSocket server (S3 only)

Thin wrapper around ESP-IDF's `esp_http_server` that owns the boot of
the on-device HTTP daemon, registers the `/ws` endpoint, and exposes a
small fan-out / point-reply API. The same `httpd_handle_t` is reused by
the REST handlers in `firmware/zhac-net-core/main/rest_*.cpp`, so the SPA, the
REST API and the WS channel all share one socket pool.

## Purpose

The SPA in `www-spa/` is WebSocket-first: every command/response
envelope and every push event traverses `/ws`. This component is the
device-side half of that channel — it accepts the upgrade, optionally
authenticates the handshake, tracks live sockets and provides
`broadcast`/`reply` primitives that callers (`ws_bridge.cpp`,
`hap_bridge.cpp`, `log_ring.cpp`) drive.

It does **not** know anything about the message envelope or the API. It
just hands raw text frames to a registered RX callback and ships
caller-formatted JSON back out.

## Where it sits

- **Chip:** ESP32-S3 only (P4 has no IP stack).
- **Owns:** the singleton `httpd_handle_t` (`server_port=80`,
  `max_open_sockets=9`, `max_uri_handlers=48`, `stack_size=8192`,
  `lru_purge_enable=true`).
- **Called by:**
  - `firmware/zhac-net-core/main/main.cpp:589` — `ws_server_init()` boots the
    daemon during S3 startup.
  - `firmware/zhac-net-core/main/main.cpp:649` — installs `on_ws_rx` as the
    RX callback that decodes `{id,cmd,args}` envelopes.
  - `firmware/zhac-net-core/main/main.cpp:121` /
    `auth_rotate_token()` (`main.cpp:131`) — pushes the API token after
    NVS load and on rotation.
  - `firmware/zhac-net-core/main/ws_bridge.cpp:236` — `ws_event_broadcast()`
    formats the `{event,data}` envelope, then calls
    `ws_server_broadcast()`.
  - `firmware/zhac-net-core/main/ws_bridge.cpp` `on_ws_rx` /
    `ws_dispatch_send_reply` — calls `ws_server_reply(fd, …)` for
    correlated command responses.
  - `firmware/zhac-net-core/main/rest_ops.cpp:527` —
    `ws_server_get_handle()` to register the REST URI handlers on the
    same daemon.
- **Dependencies (`REQUIRES`):** `esp_http_server`, `freertos`.

## Public API

All declarations live in `include/ws_server.h`.

| Symbol | Purpose | Notes / called from |
|--------|---------|---------------------|
| `using WsRxCallback = void(*)(int fd, const char* data, size_t len);` | Per-frame callback for inbound text frames. `fd` identifies the socket so the handler can reply with `ws_server_reply()`. | Single global slot; latest registration wins. |
| `void ws_server_init();` | Creates the fd-table mutex, starts the httpd daemon on port 80, registers the `/ws` URI handler. Logs `ws_server started on :80/ws`. Idempotent guards are absent — call exactly once. | `main.cpp:app_main` |
| `void ws_server_broadcast(const char* json, size_t len);` | Sends `json` as `HTTPD_WS_TYPE_TEXT` to every fd in the table. Snapshots the fd list under `s_mutex`, releases it, then loops `httpd_ws_send_frame_async`. Failed sends drop the fd via `remove_fd()` and log `ws_broadcast: send failed fd=%d err=%d`. | `ws_bridge.cpp:ws_event_broadcast` |
| `void ws_server_reply(int fd, const char* json, size_t len);` | One-shot point reply for command responses correlated by envelope `id`. No-op if `fd < 0` or daemon is down. Logs `ws_reply: send failed …` on failure but does **not** evict the fd (caller may retry). | `ws_bridge.cpp` dispatch path |
| `httpd_handle_t ws_server_get_handle();` | Returns the underlying daemon handle so other components can register additional URIs on the same daemon. Returns `nullptr` before `ws_server_init()` succeeds. | `rest_ops.cpp:527` |
| `void ws_server_set_rx_callback(WsRxCallback cb);` | Installs the inbound-frame callback. Pass `nullptr` to detach. | `main.cpp:649` |
| `void ws_server_set_api_token(const char* token);` | Sets the 32-byte hex API token (max 32 chars + NUL). Pass `nullptr` or `""` to disable handshake auth. | `main.cpp:121,149` and `auth_rotate_token` |
| `int ws_server_client_count();` | Snapshot of the live fd count under `s_mutex`. Mostly for logging / metrics gating. | available to callers |

## Important constants

| Symbol | Value | Where | Why |
|--------|-------|-------|-----|
| `MAX_WS_CLIENTS` | `3` | `ws_server.cpp:15` | Hard cap on concurrent WS sockets. Excess accepts are logged as `WS client limit reached, rejecting fd=%d`. The fd is added to the table only if there's room; httpd keeps the underlying socket but the server will never broadcast to it. |
| `s_api_token` | `char[33]` | `ws_server.cpp:13` | Fixed-size token buffer; tokens longer than 32 chars are truncated. |
| Auth token length check | `strlen(key) != 32` | `ws_server.cpp` `ws_handler` | Handshake auth requires *exactly* 32 chars. |
| `cfg.max_open_sockets` | `9` | `ws_server.cpp:ws_server_init` | LWIP cap is 10; httpd reserves one for the listen socket. |
| `cfg.max_uri_handlers` | `48` | `ws_server.cpp:ws_server_init` | Headroom for REST endpoints registered via `ws_server_get_handle()`. |
| `cfg.stack_size` | `8192` | `ws_server.cpp:ws_server_init` | Default 4096 was too small for the static-file handler's path buffers. |

## Wire format / handshake

The component is envelope-agnostic — JSON shape is decided by callers
(`ws_bridge.cpp` and the SPA per `docs/WS_API.md`). For reference:

- **Command request:**  `{"id":<u32>, "cmd":"<name>", "args":{…}}`
- **Command response:** `{"id":<u32>, "ok":true|false, "data":…|"err":"…"}`
- **Push event:**       `{"event":"<name>", "data":{…}}`

Handshake (HTTP `GET /ws`, `Upgrade: websocket`):

1. If no API token is configured, the upgrade is accepted unconditionally.
2. If a token is configured, the handler reads either:
   - the `X-Api-Key` header (preferred for non-browser clients), or
   - the `?token=<…>` query parameter (required for browser
     `WebSocket()` constructor — it cannot set custom headers).
3. The token is required to be exactly 32 chars and is compared
   constant-time (XOR-accumulate over all 32 bytes — no short-circuit).
4. On mismatch the handler returns `401 Unauthorized` and the fd is
   not added to the table.

There is no in-band `{"auth": "..."}` re-handshake; auth is purely a
handshake-time decision.

## Threading & concurrency

- `s_mutex` (FreeRTOS mutex, created in `ws_server_init`) protects only
  the fd table (`s_fds`, `s_fd_count`). Never held across an httpd
  send.
- `add_fd` / `remove_fd` take `s_mutex` for the duration of the
  table edit.
- `ws_server_broadcast` takes `s_mutex` only long enough to
  `memcpy` the fd list onto the stack, then releases it before the
  `httpd_ws_send_frame_async` loop. This is the load-bearing pattern
  that prevents a slow client from blocking the next broadcast.
- `ws_server_reply` does not take `s_mutex` — it operates on a single
  caller-supplied fd.
- Callers **must not** hold any of their own mutexes across
  `ws_server_broadcast` / `ws_server_reply` (see WEB-F2 below). The
  canonical caller-side pattern is in `ws_bridge.cpp:ws_event_broadcast`:
  format under the local scratch mutex, snapshot the bytes, release
  the local mutex, then call into `ws_server`.
- `httpd_ws_send_frame_async` itself enqueues work on the httpd
  control socket; concurrent broadcasts from multiple producer tasks
  serialise inside httpd, not inside this component.

## Error / failure modes

| Condition | Behaviour |
|-----------|-----------|
| fd table full | `add_fd` logs `WS client limit reached, rejecting fd=%d`, leaves the socket alive but unsubscribed. |
| Send failure during broadcast | `ws_broadcast: send failed fd=%d err=%d`; the fd is removed from the table immediately. |
| Send failure during reply | `ws_reply: send failed fd=%d err=%d`; the fd is **not** removed — the caller may retry or wait for the next broadcast to evict it. |
| Auth missing / wrong length / mismatch | `401 Unauthorized` body `unauthorized`, fd not added. |
| `httpd_start` failure | `ws_server_init` logs `httpd_start failed` and returns; subsequent calls are no-ops because `s_server` stays `nullptr`. |
| Inbound `HTTPD_WS_TYPE_CLOSE` or `httpd_ws_recv_frame` error | fd removed from the table, error returned to httpd. |
| Inbound payload >256 B | Frame is truncated to the 256-byte stack buffer in `ws_handler`. Callers must keep WS commands small (the SPA's largest commands fit). |

## Integration example

```c
#include "ws_server.h"

static void on_ws_rx(int fd, const char* data, size_t len) {
    // Parse {id, cmd, args} envelope, dispatch, reply with
    // ws_server_reply(fd, response_json, response_len).
}

void s3_init(const char* api_token) {
    ws_server_init();
    ws_server_set_rx_callback(on_ws_rx);
    if (api_token && api_token[0]) {
        ws_server_set_api_token(api_token);   // 32-char hex token
    }
}

void push_event(const char* envelope_json, size_t n) {
    if (ws_server_client_count() == 0) return;
    ws_server_broadcast(envelope_json, n);
}
```

## Recent changes

- **2026-04-25 — WEB-F2 (CRITICAL → fixed).**
  `ws_server_broadcast` now snapshots the fd table under `s_mutex` and
  releases the lock before the per-fd `httpd_ws_send_frame_async`
  loop. The matching caller-side fix lives in
  `firmware/zhac-net-core/main/ws_bridge.cpp:ws_event_broadcast`: format the
  `{event,data}` envelope into the static 2 KB scratch under
  `s_evt_mutex`, copy onto the stack, release `s_evt_mutex`, then call
  `ws_server_broadcast`. A slow WS client can no longer stall TaskHAP,
  the log task or the alert task. See `docs/FINDINGS.md#web-f2`.
- **2026-04-25 — CC-F6 (related).** The dead raw-broadcast path in
  `firmware/p4_core/main/hap_dispatch.cpp` (`bulk_push` /
  `flush_bulk` / `encode_bulk`) is being retired alongside the type-tag
  split (HAP-F3); the S3 side already routes everything through
  `ws_event_broadcast`.

## Cross-references

- `docs/WS_API.md` — command list and envelope schema (SPA contract).
- `docs/REST_API.md` — REST surface that shares the same `httpd_handle_t`.
- `docs/FINDINGS.md#web-f2`, `docs/FINDINGS.md#cc-f8` — the audit
  entries that drove today's broadcast fix and the API-token rate-limit
  follow-up.
- `firmware/zhac-net-core/main/ws_bridge.cpp` — envelope formatting,
  RX dispatch table (~35 entries), `ws_event_broadcast`.
- `firmware/zhac-net-core/main/api_handlers.{h,cpp}` — the transport-agnostic
  command handlers reached from both REST and WS.
- `components/metrics/README.md` — sibling shared-infrastructure
  component.
