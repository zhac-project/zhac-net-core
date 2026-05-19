# Changelog

All notable changes to `zhac-net-core` (ESP32-S3 firmware) are documented
in this file. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow
the platform-wide `vYYYYMMDDVV` scheme tagged from `zhac-platform`.

## [Unreleased]

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
</content>
