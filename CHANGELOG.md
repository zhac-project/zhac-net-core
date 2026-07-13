# Changelog

All notable changes to `zhac-net-core` (ESP32-S3 firmware) are documented
in this file. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
the platform-wide `vYYYYMMDDVV` scheme tagged from `zhac-platform`.

## [Unreleased]

### Fixed

- **HAP roundtrip requests now retransmit on loss (`NEEDS_ACK`); device.list
  pages retry once.** Roundtrip request frames were sent `NO_ACK` — a request
  lost on the SPI leg was completely silent (no retransmit, no log on either
  side; the P4 never saw it) and only surfaced as the waiter's 5 s timeout,
  which `api_devices` escalated to a 500 that blocked the SPA login gate.
  Requests are now `NEEDS_ACK`: the session retransmits at 1 s (up to 5×) and
  the peer's seen-ring dedups duplicates, so a single lost frame costs ~1 s
  instead of a failed roundtrip. `hap_roundtrip_v2` also fails fast when the
  retransmit window is full instead of burning the full timeout, and
  `devlist_fetch_all` retries a failed page once (deadline-gated — the
  documented worst-case stall bound is unchanged) so one bad page no longer
  fails the whole device list. Symptom: intermittent "hap_roundtrip_v2
  timeout … device.list page N roundtrip failed → 500" with a silent P4,
  more frequent once SHADOW_OPTIMISTIC forwarding raised ambient SPI traffic.

- **sdkconfig — correct the `CONFIG_` prefix on the remote-client default
  (CODEX M-04).** `sdkconfig.defaults` had `ZHAC_REMOTE_CLIENT_ENABLE=y` without
  the `CONFIG_` prefix, so Kconfig ignored it and a clean build fell back to
  `default n` — the tracked/generated `sdkconfig` masked the mismatch in
  incremental builds. Now `CONFIG_ZHAC_REMOTE_CLIENT_ENABLE=y`, so clean and
  incremental builds agree. `CONFIG_ZHAC_DEFAULT_API_TOKEN` stays empty in the
  committed defaults — a unique random token is generated per device on first
  boot; a shared committed token would bake one credential into every public
  build (set a fleet bootstrap token only in a private overlay, outside git).
- **API JSON serialization — bounded, fail-closed writer (CODEX H-01/H-02).**
  `grp_to_json()`, `api_group_list()` and `api_alerts_get()` used the unchecked
  `pos += snprintf(buf+pos, cap-pos, ...)` idiom: on truncation `pos` can exceed
  `cap`, so `buf+pos` walks past the buffer and `cap-pos` underflows to a huge
  size_t — a maximum-size group (16 members) into the list handler's `tmp[256]`
  reproduced a **stack-buffer-overflow** under AddressSanitizer, and
  `grp_to_json` also always emitted `"name":""` (a discarded escaped-length).
  All three now serialise through a new checked `JsonWriter` (`main/json_buf.h`)
  that keeps `pos <= cap`, escapes strings inline, and returns 0 on overflow —
  callers fail closed with valid JSON (dropping entries that don't fit rather
  than corrupting the buffer or returning an oversized `rsp_len`). `grp_to_json`
  clamps `member_count` and bounds the name read. New ASan+UBSan host suite
  `test/host/test_json_safety.cpp` (16 checks).
- **Groups store — validate on load, propagate NVS failures, init the mutex at
  boot (CODEX M-06).** `grp_load_all()` rejects a persisted `member_count` past
  the array bound; `grp_delete()` reports a failed blob-erase / bitmap-write
  instead of only the final commit; and the store mutex is created once from the
  single-task boot path (`grp_store_init()` in `app_main`), so two concurrent
  first requests can no longer each create and leak a mutex.
- **WiFi boot crash — set Wi-Fi mode before interface config (`ESP_ERR_WIFI_MODE`
  0x3005).** `start_ap_mode()` / `start_sta_mode()` called `configure_ap()` /
  `configure_sta()` — which call `esp_wifi_set_config(WIFI_IF_AP/STA)` — BEFORE
  `esp_wifi_set_mode(WIFI_MODE_APSTA)`. Per the ESP-IDF contract,
  `esp_wifi_set_config` may only be called once the target interface is enabled
  by the current mode, so on ESP-IDF v6.0 (stricter than earlier IDFs) the
  AP-config call aborted with `ESP_ERR_WIFI_MODE` at boot — a reboot loop as soon
  as the flow reached `configure_ap` (e.g. any unit with saved STA credentials).
  Reordered both paths to set_mode → configure → start. Latent since the initial
  commit; surfaced by the v6.0 toolchain. **HW-verified** — the S3 boots past
  `configure_ap` and reaches the gateway.

### Security

- **API auth is now secure-by-default (REPORT.md B2 / F1 reversal).** On a unit
  with no stored auth preference (fresh NVS / first boot), the REST + WebSocket
  API now requires a token, controlled by the new `CONFIG_ZHAC_API_AUTH_DEFAULT_ENABLED`
  (Kconfig, default `y`). A stored operator choice in NVS always wins, so units
  that were provisioned with auth off (or turn it off in the WebUI) keep that
  setting across reboots and updates. Previously auth defaulted OFF, leaving
  every endpoint — including firmware OTA, `zigbee.reset`, and `script.write`
  (Lua = RCE) — open to any LAN/RF client out of the box.
  - **BREAKING for units that never set an auth preference:** after this update
    they come up requiring a token. The token is printed to the **serial**
    console on boot (`*** ZHAC API TOKEN ... ***`); paste it into the WebUI
    (Settings → "This browser's token") to regain access, or set auth off there.
  - New `CONFIG_ZHAC_DEFAULT_API_TOKEN` (string, default empty) optionally seeds
    a **known 32-hex bootstrap token** at build time for fleet images; empty (the
    public default) generates a unique random token per device. Never commit a
    real token to the public tree — set it only in a private `sdkconfig.prod.defaults`.
  - `sdkconfig.defaults` documents the empty-token default; `sdkconfig.prod.defaults`
    documents the fleet-token option.

### Documentation

- **README auth section rewritten to match reality.** It now documents
  secure-by-default auth (above), the fresh-unit onboarding flow (serial or
  fleet token), and the WebSocket first-message handshake
  `{"cmd":"auth","args":{"token":"…"}}`. It previously described a retired
  "auth defaults OFF / any LAN client has RCE" posture and the old `?token=`
  URL scheme that FINDINGS.md **F18** already replaced.

### Added

- **Firmware version in the Info block (both cores).** `/api/status` now returns
  a top-level `fw_version` (S3's git-describe build version), and the existing
  `p4.fw` carries P4's real version (no longer the hardcoded `"0.4.0"`). Versions
  are baked from `git describe --tags --always --dirty` via `PROJECT_VER`. The
  SYNC fw_ver compat check (which gated on a `0.4.x` prefix) is dropped — it
  would false-warn on every release string; protocol compatibility is conveyed
  by `proto_mask`. `s_p4_fw_ver` widened to 32 bytes; www-spa shows both rows.

### Fixed

- **`/api/status` no longer leaks MQTT credentials (REPORT.md §2.2).** The
  unauthenticated status JSON echoed the raw `mqtt_broker` URI, which can embed
  `user:pass@host`. `sanitize_broker_url()` strips the userinfo before the echo.
- **Per-peer auth lockout (REPORT.md §2.2).** The brute-force lockout was a
  single global window, so one hostile LAN host tripping it locked out the
  legitimate operator too. It is now keyed by client IP (LRU-bounded buckets; a
  shared bucket covers the WebSocket first-message path, which has no peer
  address here).
- **Provisioning AP auto-drops once STA is stable (REPORT.md §2.2).** The SoftAP
  stayed up in APSTA for the whole runtime (idle attack surface) unless the
  operator manually set `ap_disabled`. It now drops to STA-only after STA holds a
  connection for a 2-minute grace period (cancelled on disconnect; the WiFi-reset
  button re-opens it).
- **REST bodies drained, not truncated (REPORT.md §2.2).** ~11 handlers read the
  body with a single `httpd_req_recv`, truncating multi-segment bodies into
  spurious 400s, and one used a file-static buffer. Routed through
  `rest_body_recv` (drains + 413/400s); the file-static buffer is now local.

## [v2026061401]

### Fixed

- **hap_bridge — force re-SYNC on detected P4 restart** — P4's heartbeat uptime
  going backwards is an unambiguous reboot signal, and the heartbeat is NO_ACK so
  it flows regardless of request traffic. On a successfully-decoded uptime
  regression S3 now forces `s_synced=false` to re-run the SYNC handshake, which
  resets the receive-side dedup window (see `zhac-components` hap_session). This
  is belt-and-suspenders for the peer-restart edge; the primary device.list/get/
  set "wedge after hours of uptime" fix (the uint16 high-water wrap) lives in
  `zhac-components` hap_session.

## [v2026061302] - 2026-06-13

### Fixed

- **hap_master — CRITICAL crash regression (revert of a P4-T31 over-reach)** —
  the re-entrancy `configASSERT(!s_in_dispatch)` added in T31 to "enforce" the
  `s_dispatch_buf` single-owner invariant crashed S3 (`abort` → reboot loop) on
  a normal path: an on_frame handler replying/forwarding via `hap_master_send`
  re-enters `do_two_stage_exchange` on the same task. Re-entry is expected and
  safe (callback consumes `dispatched_peer.payload` before the nested send;
  `s_spi_mutex` serialises the SPI). Removed the assert + guard, kept an accurate
  comment. The T31 finding only asked to *document* the invariant, not forbid
  re-entry. (Manifested as "device.list times out, no P4 log" — S3 was
  rebooting mid-roundtrip.)
- **main / hap_bridge (P5-T32, FINDINGS §5, LOW)** — post-release cleanup of
  the open LOW-severity tail (most LOW rows were already resolved incidentally
  during P0–P4):
  - `main.cpp` `task_ota` passed a hardcoded `payload_len 38` for the
    `ota.start` event literal that is only **36** bytes — the WS/cloud peer
    received the JSON plus two bytes of trailing garbage. Replaced with a named
    `static const char[]` + `sizeof - 1`, so the length now tracks the literal.
  - `hap_bridge.cpp` `alert_log_persist` ignored every `fwrite`/`fclose`
    result — a short write (SPIFFS full) or a close-time flush error left a
    truncated `alerts.bin` that `alert_log_load()` reads back as a corrupt
    ring. Now checks all three writes and the close; on any failure it logs and
    `unlink`s the partial file so the next boot starts empty (added
    `<unistd.h>`).
  - `hap_bridge.cpp` corrected the stale "HTTP server runs 4 worker threads"
    comment over `kHapWaiterCount` — the esp_http_server here uses
    `HTTPD_DEFAULT_CONFIG()` with no thread-pool override, so it runs a single
    worker task and serialises REST requests. Comment only.
  - `main.cpp` folded the two byte-wise constant-time token-compare loops
    (`check_auth` REST path + `auth_check_token` WS path, both comparing
    against the same `s_api_token`) into one `token_matches_ct()` helper. Pure
    compare, same translation unit — behaviour unchanged. The third copy in
    `components/ws_server/ws_server.cpp` is left as-is: it lives in a separate
    component with its own token storage and no shared header, so hoisting it
    across the module boundary is out of scope for a LOW cleanup.
- **hap_master (P4-T31, FINDINGS HAP, `hap_master.cpp` `do_two_stage_exchange`)**
  — turned the `s_dispatch_buf` consume-synchronously assumption into an
  ENFORCED invariant. The static `s_dispatch_buf` (PSRAM post-DMA copy target)
  is single-owner: the dispatch callback must consume it synchronously before
  the next exchange reuses it. The callback runs AFTER `s_spi_mutex` is released
  (synchronously, between release and function return), so a lightweight
  function-static re-entrancy guard now wraps the post-release `s_cb` call —
  `configASSERT(!s_in_dispatch)` fires if a future 2nd dispatch task ever
  re-enters while a callback is mid-flight. Comment + guard only; buffer
  ownership and the SPI flow are unchanged.
- **ws_bridge / remote_client (P4-T29, FINDINGS §5, SEC)** — the
  client-supplied envelope `id` was spliced **unescaped** into every response
  frame (`{"id":"<id>",...}`) on all three string-id paths (error reply, the
  auth-ok reply, and the success `data` envelope). A `"` (or `\`, control
  char) in the id closed the JSON string early and injected arbitrary
  structure into the frame echoed back to that client — and, via the
  remote/cloud sentinel path, to the cloud peer. All three sites now run the id
  through the existing `json_escape` (new `escape_id` wrapper, bounded to 64 B
  of escaped output). Int ids still pass through as numbers. Host-tested: a
  crafted `id` carrying `","ok":true,"data":{...}` is fully escaped inside the
  id value and the real top-level keys stay intact. (`ws_bridge.cpp` :54/:88/
  :185)
- **ws_bridge / remote_client (P4-T29, FINDINGS §5, SMELL)** — the
  `{"event":"<name>","data":<payload>}` frame was open-coded TWICE — once in
  `ws_event_broadcast` (local WS fan-out, 4 KB+128 cap) and once in
  `remote_client_publish_event` (cloud mirror, 8 KB cap) — with **divergent
  buffer caps**, the local-vs-cloud truncation asymmetry the finding flagged
  (a coalesced batch could fit one transport but be replaced by `data:null` on
  the other). Folded both onto ONE `static inline ws_event_envelope_build()`
  in `remote_client.h` (the public header both sites already include, reachable
  in both Kconfig states — no cross-repo move, no new dependency for the
  non-remote path). Format + overflow contract (`data:null,"trunc":true`
  fallback) are now identical; each caller still passes its own transport cap.
  Host-tested byte-identical for the same input.
- **api_devices (P4-T29, FINDINGS §6, BLOCK)** — `device.options.set`
  committed to NVS (`nvs_commit(s_nvs_zhac_opt)`) on **every** call — a flash
  page erase+write on the httpd task per device-option write, so a burst (SPA
  pushing several devices, or a script bulk-apply) meant flash wear plus
  per-call latency on the shared HTTP worker. The commit is now **debounced**:
  each write stages via `nvs_set_str` (immediately visible to the next read on
  the cached handle) and (re)arms a 2 s one-shot `esp_timer`; a burst coalesces
  into a single page write once writes go quiet. An explicit
  `api_device_opt_flush_now()` is called from the OTA and deferred-reboot paths
  (next to `rule_store_flush_now()`) so nothing staged is lost across a
  restart. (`api_devices.cpp` :503)
  - _Known limitation:_ a device-opt staged within the 2 s debounce window is
    discarded on a WiFi-forget / factory reboot — that path intentionally wipes
    NVS, so the staged write is dropped with the rest of config (no flush hook,
    by design).
- **api_system (P4-T29, FINDINGS §6, BLOCK)** — `api_settings_set` opened +
  committed + closed the `sys_cfg` namespace **three separate times** in one
  request (timezone / metrics_en / ap_disabled) — three flash-page writes for a
  single "save settings". This handler is operator-paced (a human pressing
  Save), so a background-flush timer is unwarranted; instead the three
  same-namespace writes are staged into locals and persisted under **one**
  open/commit/close at the end of the handler. Every live-apply side effect
  (`setenv`/`tzset`, the static flags, the MQTT/auth re-apply) is unchanged —
  only the NVS persistence is coalesced. (`api_system.cpp` :544)
- **groups_store / api_groups (P4-T29, FINDINGS §6, concurrency)** —
  `group.list` is remote-allow-listed, so it runs on BOTH the single httpd task
  and `task_remote` (cloud-invoked). The store was unsynchronised across those
  contexts: a httpd `group.create`/`update`/`delete` mutating the bitmap+blobs
  while `task_remote` walked the same namespace for a `group.list`, the
  file-static `all[]` scratch in `api_group_list` refilled mid-serialisation by
  a concurrent second list (torn read), and a non-atomic `grp_next_id()`→
  `grp_save()` in create that let two creators grab the same slot id. Added a
  store-wide **recursive** mutex (`GrpLockGuard`) held across each whole op,
  a new atomic `grp_create()` (id-alloc + save under one lock), and bracketed
  `api_group_list`'s load+serialise with `grp_store_lock()`/`unlock()` so the
  serialised snapshot is the one loaded. Mirrors the wifi-scan-mutex fix
  (P2-T16) for the same httpd-vs-`task_remote` race class. (`api_groups.cpp`
  :47, `groups_store.cpp`)
- **api_devices (P4-T29, FINDINGS §5, BLOCK — stall containment)** — the
  DEVICE_LIST paging hotfix turned `device.list` into up to `ZAP_MAX_DEVICES+8`
  (208) sequential 5 s `hap_roundtrip_v2` calls, all on the single httpd task.
  A dead or marginal P4 link (every page times out) would pin the httpd task —
  and therefore ALL REST + WS + queued async sends — for up to 208 × 5 s ≈ 17
  minutes. Added a cumulative **wall-clock budget** (8 s) to the paged loop:
  once exceeded it aborts and reports a failed fetch (caller serves the 1 s
  stale cache or a clean error) rather than emitting a half-fleet that looks
  complete. Worst-case stall is now ~8 s + one in-flight roundtrip ≈ 13 s; the
  healthy path (tens of ms) never trips it. **Recommended follow-up:** move
  multi-roundtrip commands (`device.list`) off the httpd task onto a worker so
  no single command can stall the shared HTTP/WS server at all — the budget is
  minimal containment, not the full fix. (`api_devices.cpp` `devlist_fetch_all`)

- **api_system (P4-T28, FINDINGS §8)** — updated `/api/status` for the
  caller-owned `sys_metrics` CPU% baseline. `zap_common/sys_metrics.h` dropped
  its shared per-translation-unit `static` baseline in favour of a
  caller-supplied `sys_metrics_cpu_ctx_t`. `api_status_get` now keeps a private
  function-static `s_status_cpu_ctx` (its own window, never crossing the P4
  heartbeat's) and passes it to `sys_metrics_sample_cpu_pct(ctx, c0, c1)`. The
  default single httpd worker serialises status requests; a fanned-out worker
  pool would at worst yield a transient bogus reading, not state corruption.
- **metrics_mqtt (P4-T28, FINDINGS §8)** — the snapshot publisher already
  skipped the publish when `mqtt_format_snapshot_json` returned 0 (the new
  truncation contract), but did so **silently**, so a chronically-oversized
  snapshot — e.g. after a metric is added that blows the 2 KB `s_buf` — would
  just stop appearing on the broker with no trace. `on_tick` now emits a
  **rate-limited** (one line / ~5 min) `ESP_LOGW` on the skip so the condition
  is diagnosable without log spam.
  `GET_DEVICES` protocol and reassembles every chunk into one full
  `{"devices":[...]}`. The device list previously timed out for anyone with
  ~15+ devices: the P4 could not fit the fleet in one 4096-byte SPI frame and
  silently dropped the reply. `api_device_list` now sends `GET_DEVICES` with a
  `uint16` LE `start_index`, parses each `{"next":N,"devices":[...]}` chunk,
  re-serialises every device element into a **PSRAM accumulator** (64 KB,
  sized for `ZAP_MAX_DEVICES` × ~320 B), and follows the `next` cursor until
  done. The loop is bounded (`ZAP_MAX_DEVICES + 8` pages) so it can never spin,
  and the accumulator is freed on every exit path. The reassembled output is
  the same `{"devices":[...]}` the SPA already consumes. The 1 s burst-coalesce
  cache now holds the full list (grown from `HAP_MAX_PAYLOAD` to the
  accumulator size). The WS `device.list` response cap (`ws_bridge.cpp`) and
  the REST `/api/devices` buffer (`rest_devices.cpp`) are bumped 8/16 KB → 40 KB
  (~125 devices) so the now-complete list isn't re-truncated at the transport;
  past that the accumulator truncates with a logged warning instead of
  emitting broken JSON.
  `CONFIG_LOG_DEFAULT_LEVEL` ≤ INFO (≤ 3) — `esp_http_client` logs full request
  URLs at DEBUG, and the `tg_gw` Telegram client carries the bot token in the
  URL, so a DEBUG/VERBOSE image leaks the token to serial / `/api/logs`.
  `sdkconfig.prod.defaults` already pins WARN (level 2); the risk and the rule
  are now noted in README "Known hardening gaps".

### Fixed — Medium (P4 findings review, T26 remote_client lifecycle/backoff)

- **remote_client** (Medium): the `BACKOFF` state slept with a bare
  `vTaskDelay` (up to `CONFIG_ZHAC_REMOTE_BACKOFF_MAX_S`) inside the
  state-machine task, so an `EVB_DISABLE` / `EVB_WIFI_DOWN` posted during a
  backoff window sat unprocessed for the whole sleep — a `remote.disconnect`
  or a Wi-Fi drop felt unresponsive for up to a full backoff. The task now
  waits out the backoff on the event group (`xEventGroupWaitBits` with the
  backoff as the timeout, watching `EVB_DISABLE | EVB_WIFI_DOWN`, clear-on-exit
  off); an abort bit wakes it immediately and is consumed + stepped at the top
  of the loop (→ `DISABLED` / `IDLE_NO_WIFI`), while a clean timeout fires
  `EVB_BACKOFF_DONE` → `CONNECTING` exactly as before. (FINDINGS §5, :491)
- **remote_client** (Medium): a disable→enable cycle leaked two queues and one
  event group each time. `DISABLED` self-deletes `task_remote`, and re-enable
  re-ran `remote_client_init`, which recreated `s_tx_queue` / `s_rx_queue` /
  `s_remote_evt` over the still-live old handles. The kernel objects are now
  created once (`remote_objects_init_once`, guarded by `s_objects_ready`) and
  reused across every task respawn; only the task itself is respawnable
  (`remote_task_spawn`). Keeping `s_remote_evt` stable also stops stranding the
  Wi-Fi bits that `app_main` posts to it via the captured `extern` handle.
  (FINDINGS §5, :514)
- **remote_client** (Medium): `remote_client_enable` reloaded NVS into `s_cfg`
  from the API caller's task while a live `task_remote` could concurrently read
  `s_cfg.url` / `.token` (`start_ws_client` / `send_auth_frame`), risking torn
  credentials. The reload is now deferred to the remote task via a new
  `EVB_RELOAD_CFG` bit: `enable()` no longer touches `s_cfg` (it just sets
  `EVB_RELOAD_CFG | EVB_ENABLE`), and the task reloads `s_cfg` from NVS at the
  top of its loop — before any connect — so `task_remote` is the sole accessor
  of `s_cfg`. `api_remote_connect` already persists the new creds to NVS before
  calling `enable()`, so the deferred reload always observes them. The task's
  body-entry `s_cfg` read was dropped (initial enable now rides the
  `EVB_ENABLE` bit), and `last_state` was made task-local so a respawn starts
  edge detection from `DISABLED`. (FINDINGS §5, :536)
- **remote_client** (Medium, documentation): the inbound WSS path drops frames
  with `data_len > 8192` and drops continuation/fragmented frames (op_code
  0x00) — messages > 8 KB never dispatch. Reassembly is out of scope for this
  batch; the 8 KB single-frame cap is now documented as a contract in
  `ws_event_handler` (both directions share the 8 KB bound) with a cross-ref to
  the `project_hub_cloud_devicesync` constraint (the reconcile relist /
  device.list must page to stay under the cap). (FINDINGS §5, :199)

### Fixed — High / Medium / Low (P2 findings review, T16 wifi_mgr event-loop unblock)

- **wifi_mgr** (High): the STA reconnect backoff did `vTaskDelay` (up to 60 s)
  INSIDE the `WIFI_EVENT_STA_DISCONNECTED` handler, which runs in the default
  `esp_event` loop task. Sleeping there stalled ALL system event dispatch for
  the whole STA-down window — IP got-IP, AP join/leave, the MQTT bring-up
  triggered off got-IP, and the remote_client wifi bits — and risked
  event-queue overflow. The handler now computes the backoff and arms a
  one-shot `esp_timer` (`wifi_reconnect`), then returns immediately; the timer
  callback (esp_timer task, not an ISR) issues `esp_wifi_connect()`. The
  backoff schedule is unchanged (2 s ≤5 retries, 10 s 6..30, 60 s after) — only
  WHERE the wait happens moved. The retry counter is now `std::atomic<int>`
  (defensive; still single-producer in the esp_event task) and got-IP cancels
  any pending reconnect. (FINDINGS §5.1, :103)
- **wifi_mgr** (Medium): `s_scan_results` / `s_scan_count` were unsynchronised
  statics reachable concurrently from the local httpd task and `task_remote`
  (`wifi.scan` is remote-allow-listed), so a concurrent scan could tear the
  array out from under a reader. Added `s_scan_mtx`; the results read-back +
  count store in `wifi_mgr_scan` is now mutex-guarded (the blocking
  `esp_wifi_scan_start` stays outside the lock — esp_wifi's scan engine is
  single-instance, so a concurrent start just errors), and
  `wifi_mgr_get_scan_results` was changed to copy a stable snapshot into a
  caller-owned buffer under the lock instead of returning the live pointer.
  Caller (`api_wifi_scan`) updated to the snapshot API. (FINDINGS §5.2, :487)
- **wifi_mgr** (Low): the captive-DNS task only re-checked `s_ap_mode` AFTER a
  blocking `recvfrom`, so after AP-disable it lingered (socket + ~3 KB stack)
  until a stray packet arrived, and the start guard then blocked respawn. The
  socket now carries `SO_RCVTIMEO` (1 s); the recv loop re-checks `s_ap_mode`
  each timeout and, on AP-stop, closes the socket, clears the (now file-scope)
  `s_dns_started`, and deletes the task so a later AP-enable respawns it. The
  1 s timeout paces the poll (no busy-spin). (FINDINGS §5.3, :319)

### Fixed — High / Medium (P2 findings review, T14 HAP correlation edges)

- **hap_bridge** (High): the per-request-seq WAITER table (`hap_roundtrip_v2`)
  drew its correlation seq from `hap_session_next_seq()` and claimed a slot
  without checking the seq was free. After 65534 sends the uint16 seq wraps
  and could alias a long-stalled in-flight waiter; `waiter_find_by_seq` then
  matched the OLDER slot and delivered one caller's response into another
  caller's buffer. `waiter_claim` now draws the seq UNDER the table mutex and
  rejects any candidate already held by a live slot (bounded 8-try loop,
  skips 0 to match the session convention; on exhaustion the claim fails and
  the roundtrip returns a clean error rather than risking a colliding
  correlation). (FINDINGS §1.1, :271)
- **hap_bridge** (Medium): response delivery in the `on_frame` callback
  (task_hap) found the waiter under the mutex, RELEASED it, then memcpy'd into
  `w->rsp_buf`, wrote `*rsp_len_out`, and gave the sem unlocked. If the caller
  timed out and `waiter_release`'d in that gap, the slot could be reclaimed and
  re-pointed at a different caller's buffer, so the copy landed in the wrong
  buffer (flagged by two reviewers). Delivery is now one atomic step
  (`waiter_deliver`): find + re-check `seq!=0 && seq==ack_seq` + type-check +
  bounded memcpy + len-write + sem-give all happen under
  `s_waiter_table_mutex`. A timed-out caller's release (also mutex-guarded)
  cannot interleave: it either ran first (seq==0 ⇒ no match, late frame
  dropped) or is blocked until delivery completes into the still-valid slot.
  The mutex is never held across a blocking call — the memcpy is bounded by
  `rsp_cap` (≤ HAP_MAX_PAYLOAD). (FINDINGS §1.2, :395)

### Changed — DRAM→PSRAM static buffer sweep (P1, T12)

- Routed cold/warm static buffers to PSRAM via `EXT_RAM_BSS_ATTR`
  (`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y`): `hap_bridge` attr.bulk
  coalescer (4 KB), `wifi_mgr` scan results (1.8 KB), `metrics_mqtt` snapshot
  scratch (2 KB), `api_groups` group-list records (9.5 KB — the single largest
  dram0 .bss symbol), `main` alert ring (3.5 KB), `rest_rules` group-handler
  scratch (3.2 KB), and `hap_master`'s frame dispatch buffer (4 KB — the
  post-DMA copy only; the actual SPI DMA buffers `s_tx_buf`/`s_rx_buf` stay
  internal/DMA-capable).
- `rest_ops` handler stack buffers (/api/status, /api/alerts,
  /api/diagnostics/unhandled, /api/wifi/scan — 7 KB total) converted to
  function-static PSRAM scratch. Safe because esp_http_server serialises all
  URI handlers on its single worker task (one httpd instance, ws_server);
  also relieves httpd-task stack pressure.
- Net effect (`idf.py size`, S3 app, includes the zhac-components moves):
  dram0 `.bss` 47,736 → 12,648 B (−35,088 B). The only remaining ≥1 KB
  internal .bss symbol is ESP-IDF's coredump stack, which must stay internal
  (panic path).

### Fixed — WebSocket subsystem hardening (P1)

- **ws_server**: broadcasts now skip sockets that opened `/ws` but never
  authenticated (when a token is configured) — previously an unauthed socket
  passively received every event: device states, alerts, log lines. The
  `{"cmd":"auth"}` handshake reply is a targeted `ws_server_reply` and is
  unaffected.
- **ws_server**: send-failure handling in `ws_server_broadcast` no longer
  logs inside the send loop. Failures are collected, the dead fd is removed
  FIRST, and the warning logged after — with the WS log sink enabled the old
  order re-entered the log pipeline against the same dead fd and recursed to
  stack overflow. A per-task re-entry guard (`ws_server_in_broadcast`)
  additionally drops WS-sink fan-out of any line logged from inside the
  broadcast path. `ws_server_reply` gets the same remove-then-log ordering.
- **log_ring**: the WS log sink no longer calls `ws_server_broadcast` from
  the logging task (which stalled arbitrary tasks behind a slow client and
  formed the recursion pair above) — it enqueues to the F33 WS-TX worker via
  the new `ws_bridge_broadcast_enqueue()`, which never blocks and never logs
  (drops are counted).
- **ws_bridge**: event envelope was 2048 B while hap_bridge's attr.bulk
  coalescer emits up to 4096 B — coalesced batches >~2 KB were silently
  replaced with `data:null`, losing live updates under load.
  `BULK_COALESCE_CAP` moved to `s3_internal.h`; the (PSRAM) envelope is now
  sized `BULK_COALESCE_CAP + 128` with an ESP_LOGW on the now-unreachable
  truncation fallback.
- **ws_bridge**: `on_mqtt_rx` MQTT→WS mirroring and the `device_deleted`
  event (api_devices) route through the WS-TX worker queue instead of
  fanning out on the esp-mqtt / httpd task. `ws_server_broadcast` is now
  called exclusively by the TX worker.
- **ws_server**: registered an httpd `close_fn` — abnormal disconnects
  (TCP RST, keepalive death, LRU purge) never hit `remove_fd`, leaking one
  of the 3 client-table slots until a later send happened to fail. The hook
  fires for every terminated session; `remove_fd` no-ops for non-WS fds.
- **ws_server**: a `/ws` open beyond the 3-client table is now actively
  closed (`httpd_sess_trigger_close` + warn) instead of staying half-alive
  (could send commands, never received broadcasts).
- **ws_server**: WS PING frames are answered with PONG again —
  `handle_ws_control_frames` flipped to `false` so esp_http_server
  auto-replies PONG/CLOSE per RFC 6455 (the old handler swallowed PING);
  CLOSE teardown lands in the new `close_fn`.
- **ws_bridge**: inbound envelope `args` larger than the 2 KB serialize
  buffer were silently truncated into corrupt JSON — now measured first and
  rejected with `err:"args too large"`.
- **main**: `status.get` gains `ws_tx_drops` (frames dropped on the WS-TX
  path: queue full / not started / snapshot alloc failure) so slow-client
  drop pressure is observable.

**NEEDS HARDWARE TEST** — PING/PONG keepalive, RST slot reclaim via the
httpd `close_fn`, and auth-gated broadcast filtering need on-device
verification.

### Added — Per-device flood control

- **device.options.set**: now accepts `throttle_ms` (per-device report
  rate-limit, milliseconds) and forwards it to P4 via `DEVICE_OPTIONS_SET`. Set
  e.g. `30000` on chatty Tuya-DP air-quality monitors to cap the update flood at
  one per 30 s. The `DEVICE_OPTIONS_SET` encode buffer was bumped 96→160 B for
  the 3-field payload, and an encode failure now returns an error instead of a
  false success. (#84)

### Added — Premium feature (Remote WSS client)

- **remote_client** (new component, `components/remote_client/`):
  opt-in outbound WSS client to a user-configured remote service.
  Default-disabled at both build time
  (`CONFIG_ZHAC_REMOTE_CLIENT_ENABLE=n`) and run time (NVS
  `enabled=false`); the LAN-only firmware pays zero flash + zero RAM
  for the feature. When active, one outbound `esp_websocket_client`
  instance hosts a six-state machine (DISABLED / IDLE_NO_WIFI /
  CONNECTING / AUTHENTICATING / READY / BACKOFF), authenticates
  in-band with a user-pasted token (first frame is `remote.auth`;
  mid-session re-auth challenges supported), and reuses the existing
  transport-agnostic `api_*` handler set via a
  `ws_server_register_reply_hook` plumbing addition. Cloud-originating
  commands and device-originating push events are gated by static
  allow-lists (`cmd` + `event`) in `remote_allow.cpp` — the
  `wifi.connect` / `wifi.disconnect` / `remote.*` admin commands are
  permanently excluded. The `ws_event_broadcast` hot path gains a
  one-atomic-load mirror call to `remote_client_publish_event`; below
  measurement noise on free-tier builds. Three new admin commands
  `remote.connect / .disconnect / .status` live in
  `main/api_remote.cpp` under the same Kconfig guard. Host tests
  under `components/remote_client/test/` cover the allow-lists,
  backoff math, state-machine transitions, and NVS round-trip.
  Integration smoke server at `extra/tools/mock_remote.mjs`. Design
  spec at
  `docs/superpowers/specs/2026-05-19-remote-client-design.md`,
  implementation plan at
  `docs/superpowers/plans/2026-05-19-remote-client-plan.md`.

- **ws_server**: new public API
  `ws_server_register_reply_hook(sentinel_fd, hook)` — single-slot
  table for routing `ws_server_reply` calls on a reserved fd
  (REMOTE_VIRTUAL_FD = -32768) through a registered callback instead
  of the local httpd send. The sentinel-fd fast path sits as the
  first statement of `ws_server_reply`; normal httpd fds are
  unaffected.

- **main**: `api_status_get` gains `remote_available` field
  (compile-time boolean from `CONFIG_ZHAC_REMOTE_CLIENT_ENABLE`) so
  the SPA can show/hide the cloud-settings panel without an extra
  round-trip.

### Fixed — Critical (regression follow-up)

- **hap_bridge**: remove the `device.list.snapshot` WS broadcast from
  the `DEVICE_LIST` per-type case. After F-01 v2 the v2 hook already
  delivers the payload to the requesting REST/WS caller via the round-
  trip envelope; the unconditional broadcast queued a second ~2.5 KB
  frame on the same WS back-to-back, overflowed the httpd send buffer,
  and dropped the client → SPA reconnect loop. The event had no SPA
  listener (the SPA tracks per-device deltas via `device.added` /
  `device.removed`). Surfaced after F-06 boot reorder because the pool
  is now populated before the SPA connects, growing `DEVICE_LIST` past
  the WS buffer threshold.

### Changed — Critical (HAP stack review, 02-hap-stack.md)

- **hap_bridge**: replace the per-type `s_ack_expected[256]` correlation
  table + family-wide `s_rule_script_mut_mutex` with a per-request-seq
  waiter slot table (`s_waiters[8]` + counting `s_waiter_free_sem`).
  Every REST/WS caller now owns its own response buffer and waits on a
  private semaphore keyed by the request seq; concurrent rule / script
  mutations no longer serialise behind a single mutex. New API
  `hap_roundtrip_v2(type, req, req_len, rsp_buf, rsp_cap, rsp_len_out,
  timeout_ms)`. Receiver dispatches by `ack_seq` at the top of
  `on_frame`; per-type case bodies retain only true side-effects
  (`HEARTBEAT` decode, `BULK_STATE_UPDATE`
  fanout, `ALERT` broadcast, `OTA_STATUS` log, `OTA_CHECKPOINT_RSP`
  legacy sem-give for the OTA flow which still uses raw `hap_send`).
  Legacy slot table, shared response buffers (`s_rule_rsp_*`,
  `s_script_rsp_*`, `s_devlist_rsp_*`, `s_devinfo_rsp_*`,
  `s_diag_rsp_*`, plus the per-type ack `ok` globals
  `s_setattr_rsp_ok` / `s_bind_rsp_ok` / `s_devdel_rsp_ok` /
  `s_devopt_rsp_ok` / `s_zbcfg_rsp_*`), per-type request mutexes
  (`s_rule_req_mutex`, `s_script_req_mutex`, `s_devlist_req_mutex`,
  `s_devinfo_req_mutex`, `s_setattr_req_mutex`, `s_bind_req_mutex`,
  `s_devdel_req_mutex`, `s_devopt_req_mutex`, `s_zbcfg_req_mutex`,
  `s_diag_req_mutex`), `hap_accept_response`, and the original
  `hap_roundtrip` function were all deleted. Migrated callers across
  `api_devices`, `api_rules`, `api_scripts`, `api_system`,
  `api_groups`, `rest_devices`, `rest_rules`. `SCRIPT_RUN_REQ` stays
  on `hap_send` fire-and-forget — its SCRIPT_ACK pairing isn't
  registered in `expected_response_for` (F-01-legacy marker in the
  handler). (HAP F-01 v2)

### Fixed — Critical (HAP stack review, 02-hap-stack.md)

- **hap_bridge**: serialise rule / script mutator REST handlers behind
  a single mutex inside `hap_roundtrip` whenever the expected response
  type is shared. `RULE_CREATE`/`UPDATE`/`UPDATE_DSL`/`DELETE` all map
  to `RULE_EXEC_RESULT` (0x32), and `SCRIPT_WRITE`/`DELETE` map to
  `SCRIPT_ACK` (0x37). Two concurrent REST threads issuing different
  mutations of the same family used to clobber each other's slot in
  `s_ack_expected[rsp_type]`; the first roundtrip then dropped its own
  reply as stale and burned through the full 8 s retry budget before
  giving up. Superseded by the per-request-seq rewrite above. (HAP F-01)
- **hap_bridge**: check the post-flush `bulk_append` return value in
  the `BULK_STATE_UPDATE` handler. Gate on payload size first — any
  frame whose serialised form would exceed `BULK_COALESCE_CAP` (4096 B)
  is now flushed and broadcast directly via `ws_event_broadcast`
  instead of being silently dropped. The previous code's contract
  comment ("never drop attrs silently") was being violated by a single
  `BULK_STATE_UPDATE` from a 40-device fleet. Logs the dropped size if
  the retry still fails (no `METRIC_BULK_DROP*` exists in the registry
  yet; can be added if the LOGE proves insufficient). (HAP F-02)

### Fixed — Medium (HAP stack review, 02-hap-stack.md)

- **hap_bridge**: switch `s_ack_expected[rsp_type]` writes from
  `memory_order_relaxed` to `memory_order_release`, and the matching
  read in `hap_accept_response` to `memory_order_acquire`. Sender
  (REST thread) and receiver (task_hap) may be on different cores on
  S3; without the release/acquire pair, the receiver could validate an
  incoming ack_seq against a stale `expected` slot on Xtensa /
  RISC-V where the ordering reordering is permitted by the model.
  (HAP F-07)
- **hap_bridge**: delete the strstr-`"ieee":"0x` scan that recomputed
  `s_p4_device_count` from the `DEVICE_LIST` payload. The scan
  overcounted whenever a friendly_name / manufacturer string carried
  the literal token, duplicated logic that the SPA already owns, and
  was one of three writers to the same atomic. Also remove the
  `SYNC_ACK` overwrite for the same atomic. `s_p4_device_count` is now
  written ONLY from the HEARTBEAT path (5 s cadence, P4's
  authoritative `pool_count_active()`). SPA polling /api/status reads
  the one atomic. (HAP F-05 + HAP F-10)

### Fixed — Critical (net-core glue review, 04-net-core-glue.md)

- **api_system**: drop the `api_token` field from `api_status_get`.
  `/api/status` is intentionally unauthenticated (discovery endpoint),
  but the response carried the bootstrap API token whenever auth was
  disabled — any LAN client could read it and unlock every
  authenticated endpoint the moment auth was turned on. Token now
  surfaces only on the serial console at first generation
  (`ESP_LOGI(TAG, "API TOKEN (first boot): %s", token)`) and via the
  authenticated `/api/system/token` rotation endpoint. (F-01)

### Fixed — Important (net-core glue review, 04-net-core-glue.md)

- **hap_bridge**: add `configASSERT(xSemaphoreGetMutexHolder(...) !=
  nullptr)` to the `DEVICE_LIST` and `DEVICE_INFO` response-writer
  paths in the frame callback. Catches any regression where a P4
  response arrives without an api-handler holding the corresponding
  request mutex (the shared `s_devlist_rsp_json` /
  `s_devinfo_rsp_json` slabs would otherwise be overwritten under a
  reader). The WS path is already safe — `dispatch_envelope` calls
  `api_device_list` / `api_device_get`, which acquire
  `s_devlist_req_mutex` / `s_devinfo_req_mutex` internally before
  invoking `hap_roundtrip`, identical to the REST path. (F-03)
- **main**: call `rule_store_flush_now()` before `vTaskDelay` +
  `esp_restart()` in `task_ota`. The deferred writeback task batches
  for up to 5 s; without an explicit flush, any rule edit landing in
  the last 5 s before an OTA-triggered reboot was silently lost. The
  P4 OTA task (`task_p4_ota`) does not reboot the S3 so does not need
  the call. (F-04)
- **ws_bridge**: stop the "looks-like-JSON splice" path in
  `on_mqtt_rx`. All MQTT payloads are now JSON-string-escaped via
  `json_escape` before being inlined into the `{"type":"mqtt",
  "topic":..., "data":...}` WS envelope. The previous fast path let a
  hostile broker or buggy publisher inject extra envelope fields (e.g.
  truncated `{` followed by an arbitrary key) into every connected WS
  client's stream. The SPA does `JSON.parse(msg.data)` regardless, so
  the change costs one decode hop on the client. (F-05)
- **hap_bridge**: call `esp_ota_mark_app_valid_cancel_rollback()` from
  the `on_sync` callback the first time SYNC_ACK arrives after boot,
  guarded by `#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` and a
  static-once atomic. SYNC_ACK is the strongest health signal (proves
  Wi-Fi + SPI + P4 firmware-compat path); without this, an OTA-flashed
  image rolled back on every subsequent boot when rollback was
  enabled. `app_update` added to the main component REQUIRES for
  explicitness (transitively pulled by `esp_https_ota` already). (F-08)
