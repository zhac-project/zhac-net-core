# ws_server — WebSocket server (S3 only)

Thin wrapper around ESP-IDF's `esp_http_server` that owns the boot of the
on-device HTTP daemon, registers the `/ws` endpoint, and exposes a small
fan-out / point-reply API. The same `httpd_handle_t` is reused by the REST
handlers in `main/rest_*.cpp` (fetched via `ws_server_get_handle()`), so the
SPA, the REST API and the WS channel all share one socket pool.

The component is envelope-agnostic: it hands raw text frames to a registered
RX callback and ships caller-formatted JSON back out. Envelope shape
(`{id,cmd,args}` / `{event,data}`) is owned by `main/ws_bridge.cpp` and
documented in `zhac-docs/WS_API.md` (sibling repo).

## Where it sits

- **Chip:** ESP32-S3 only (P4 has no IP stack).
- **Owns:** the singleton `httpd_handle_t` — `server_port=80`,
  `max_open_sockets=9` (LWIP cap 10, one reserved for listen),
  `max_uri_handlers=48`, `stack_size=12288` (the worst observed chain —
  `api_rule_list → hap_roundtrip → SPI exchange → BULK callback →
  ws_event_broadcast → lwip_send` — tripped the canary at 8 K),
  `lru_purge_enable=true`, TCP keepalive `10/5/3`,
  `recv/send_wait_timeout=10` (F33 slowloris bound), and a custom
  `close_fn` (see Lifecycle).
- **Called by:**
  - `main/main.cpp` — `ws_server_init()` at boot; pushes the API token after
    NVS load and on rotation (`ws_server_set_api_token`,
    `ws_server_fd_deauth_all`).
  - `main/ws_bridge.cpp` — installs `on_ws_rx`; its TX worker is the **only**
    caller of `ws_server_broadcast()`; dispatch replies via
    `ws_server_reply(fd, …)`.
  - `main/log_ring.cpp` — WS log sink; checks `ws_server_in_broadcast()`
    before fanning a log line out to WS.
  - `main/rest_ops.cpp` — `ws_server_get_handle()` to register REST URIs on
    the same daemon.
  - `components/remote_client` — `ws_server_register_reply_hook()` to route
    sentinel-fd replies over the cloud link.
- **Dependencies (`REQUIRES`):** `esp_http_server`; uses FreeRTOS mutex +
  `heap_caps_malloc` (PSRAM).

## Public API (`include/ws_server.h`)

| Symbol | Contract |
|--------|----------|
| `ws_server_init()` | Creates the fd-table mutex, starts httpd on :80, registers `/ws`, installs `close_fn`. Call exactly once. On `httpd_start` failure logs and returns; all other calls are then no-ops (`s_server == nullptr`). |
| `ws_server_broadcast(json, len)` | Fan-out of one TEXT frame to every **authed** fd. **Single-broadcaster contract: only the ws_bridge TX worker may call this** (enforced by a `configASSERT` on the `s_broadcast_task` guard). Producers must use `ws_bridge_broadcast_enqueue()` / `ws_event_broadcast()` (`main/s3_internal.h`) instead. Snapshots the fd list under `s_mutex`, releases it, then loops `httpd_ws_send_frame_async`. **Never logs inside the send loop** — failures are collected, dead fds removed first, logged after (log-sink recursion guard). |
| `ws_server_reply(fd, json, len)` | Point reply for command responses correlated by envelope `id`. Sentinel-fd fast path: a registered reply hook is invoked instead of httpd send. On send failure the fd **is evicted** (`remove_fd` before logging — same ordering rule as broadcast). |
| `ws_server_get_handle()` | Underlying daemon handle for registering additional URIs. `nullptr` before init. |
| `ws_server_set_rx_callback(cb)` | Installs the inbound text-frame callback (single slot, latest wins, `nullptr` detaches). |
| `ws_server_set_api_token(token)` | Sets the 32-char token (`char[33]`, truncated). `nullptr`/`""` disables auth entirely (token set ⇔ auth on). |
| `ws_server_client_count()` | Live fd count under `s_mutex`. |
| `ws_server_in_broadcast()` | True when the *calling* task is inside the broadcast send loop. Used by the log pipeline to drop WS fan-out of lines logged from within the TX path. |
| `ws_server_fd_is_authed(fd)` / `ws_server_fd_set_authed(fd)` | F18 per-fd auth state; the dispatch layer permits only `auth` until set. |
| `ws_server_fd_deauth_all()` | Q48: clears every authed flag on token rotation — stale-token sockets must re-auth. |
| `ws_server_register_reply_hook(sentinel_fd, hook)` | Routes `ws_server_reply` for one reserved sentinel fd (outside legal httpd range) to a hook (e.g. remote_client). Single slot; register before traffic; `nullptr` unregisters. |

## Constants

| Symbol | Value | Why |
|--------|-------|-----|
| `MAX_WS_CLIENTS` | 3 | Hard cap on concurrent WS sockets. **Table full → `httpd_sess_trigger_close(fd)`** — a slot-less socket would be a half-functional zombie (accepts commands, never sees a broadcast). |
| `WS_RX_MAX` | 8 KB | DS10: cap on an inbound text frame (covers rule DSL + `script.check` Lua source). Larger frames are logged and dropped — no huge forced allocation. |
| Token length | exactly 32 chars | Constant-time XOR-accumulate compare, no short-circuit. |

## Auth (F18 — first-message auth)

When a token is configured the socket **may open unauthenticated**; there is
no 401 at the upgrade. Per-fd state drives two gates:

1. **Handshake fast-path:** a valid token in the `X-Api-Key` header (or the
   legacy `?token=` query) marks the fd authed at upgrade time — back-compat
   for non-browser clients. Browsers connect bare and send a first
   `{"cmd":"auth"}` message instead, keeping the long-lived token out of
   proxy/access logs. The dispatch layer (`ws_bridge.cpp`) rejects every
   other command on an unauthed fd.
2. **Broadcast gate:** `ws_server_broadcast` skips unauthed fds when auth is
   on — an unauthenticated socket passively receives **nothing** (no device
   states, alerts, log lines); it only gets targeted `ws_server_reply`
   frames (e.g. the auth handshake response itself, which is never a
   broadcast).

Token rotation calls `ws_server_fd_deauth_all()` so live sessions on the old
token drop back to the auth-only command set.

## Broadcast TX contract (single broadcaster)

`ws_server_broadcast` is **not** a general-purpose producer API:

- The ONLY sanctioned caller is the ws_bridge TX worker
  (`ws_bridge_tx_init`). Arbitrary tasks enqueue via
  `ws_bridge_broadcast_enqueue(json, len)` — copies the frame, never blocks,
  **never logs** (log-pipeline-safe; queue-full/OOM drops are counted in
  `ws_bridge_tx_drops()` and surfaced in `status.get`). See the contract
  block in `main/s3_internal.h`.
- A `configASSERT` before the guard store fires if a second task enters the
  send loop concurrently.
- Inside the send loop: no `ESP_LOG*` and no early returns between the
  `s_broadcast_task` guard stores — with the WS log sink enabled a log line
  re-enters the WS TX path, and a stuck guard would permanently mute the
  sink. `ws_server_in_broadcast()` is the sink's re-entry check.
- Failure handling: collect failed fds, `remove_fd` them all **first**, log
  after — the fd is already gone, so a re-broadcast of the log line can't
  re-fail (bounded, no recursion).

## Lifecycle: close_fn + control frames

- `cfg.close_fn = ws_httpd_close_fn` fires on **every** terminated session —
  plain HTTP and WS alike; graceful CLOSE, TCP RST, keepalive death, LRU
  purge. It runs `remove_fd(sockfd)` (no-op for non-WS fds) **and then
  `close(sockfd)`** — under IDF v6.0 a custom `close_fn` replaces the
  server's own close, so skipping the `close()` would leak the socket.
  Without this hook, abnormal disconnects only freed a table slot when a
  later send happened to fail.
- `handle_ws_control_frames = false`: httpd itself answers PING with PONG
  and replies to CLOSE (RFC 6455), then tears the session down through
  `close_fn`. (The old `true` setting swallowed PING without a PONG, so
  keepalive-probing clients timed out.) Control frames never reach
  `ws_handler`.

## Inbound path

Text frames only. The handler probes the frame length (`max_len=0`),
enforces `WS_RX_MAX`, then reads into a right-sized **PSRAM** buffer
(`MALLOC_CAP_SPIRAM`, internal-heap fallback), NUL-terminates and invokes
the RX callback. The old fixed 256-byte stack buffer (truncated rule DSL /
Lua uploads) is gone.

## Threading

- `s_mutex` protects only the fd table (`s_fds`, `s_fd_authed`,
  `s_fd_count`); never held across an httpd send.
- `httpd_ws_send_frame_async` enqueues on the httpd control socket; the
  single-broadcaster rule means fan-out ordering is decided in the TX
  worker's queue, not by racing producers.
- Callers must not hold their own mutexes across `ws_server_broadcast` /
  `ws_server_reply` (WEB-F2): format under a local lock, snapshot, release,
  then send.

## Failure modes

| Condition | Behaviour |
|-----------|-----------|
| fd table full | `WS client limit (3) reached, closing fd=%d` + `httpd_sess_trigger_close`. |
| Send failure (broadcast) | fd removed first, then `ws_broadcast: send failed fd=%d err=%d (removed)`. |
| Send failure (reply) | fd removed first, then `ws_reply: send failed fd=%d err=%d (removed)`. |
| Inbound frame > 8 KB | `ws frame %u B exceeds cap %u — dropped`; connection stays up. |
| Frame buffer OOM | `ESP_ERR_NO_MEM` returned to httpd. |
| `httpd_ws_recv_frame` error | fd removed, error returned to httpd (session torn down → `close_fn`). |
| `httpd_start` failure | logged; component stays inert. |

## Recent changes

- **2026-06 — d4a4626 (WS hardening cluster).** Broadcasts are auth-gated
  (unauthed fds receive nothing but targeted replies); log-sink recursion on
  dead-fd sends killed via the never-log-in-send-loop rule +
  `ws_server_in_broadcast()` guard; `close_fn` slot cleanup on every session
  end (incl. RST/LRU) with explicit `close(sockfd)`; `handle_ws_control_frames
  = false` so httpd answers PING/PONG and CLOSE itself; 8 KB PSRAM inbound
  cap; table-full now closes the socket; reply-path eviction; single-
  broadcaster `configASSERT`. **Hardware gate:** PING→PONG keepalive and
  client-table reclaim after a hard TCP RST need manual on-device
  verification.

## Cross-references

- `zhac-docs/WS_API.md` — command list + envelope schema (SPA contract).
- `zhac-docs/REST_API.md` — REST surface sharing the same `httpd_handle_t`.
- `main/ws_bridge.cpp` — envelope formatting, RX dispatch, TX worker,
  `ws_event_broadcast` / `ws_bridge_broadcast_enqueue`.
- `main/s3_internal.h` — the producer-side broadcast contract block.
- `main/log_ring.cpp` — WS log sink (consumer of `ws_server_in_broadcast`).
- `components/remote_client` — sentinel-fd reply hook user.
