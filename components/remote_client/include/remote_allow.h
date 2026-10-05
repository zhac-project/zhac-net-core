// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

// The relay's answer to one cloud command: nullptr = pass it to the dispatcher, else the error code:
//   "local_only"      settings.set carrying the key only the hub's own page may change
//                     (remote_scripts), whatever the build trusts the cloud with;
//   "scripts_off"     a switch-gated op (script.write / run / reload, system.restart) while the hub's
//                     "Allow script changes from the cloud" switch is off;
//   "cmd_not_allowed" anything else the relay does not pass.
// `local_only_key`: the args carry `remote_scripts`. `scripts_on`: the switch.
// LINEAR SCANS over static arrays (~70 entries), once per cloud frame.
const char* remote_cmd_refusal(const char* cmd, bool local_only_key, bool scripts_on);

// True iff `cmd` passes now: remote_cmd_refusal(cmd, false, remote_scripts_allowed()) == nullptr.
bool remote_cmd_allowed(const char* cmd);

// "Allow script changes from the cloud" (spec 2026-10-05 §3.1). Weak default: false. Dual-chip and mono
// define nothing, so script writes stay closed there; zhac-wired-core returns its NVS flag.
bool remote_scripts_allowed(void);

// True iff the event name should be mirrored to the remote sink.
bool remote_event_allowed(const char* name);

// The hub-log line for a change the cloud relays (§3.7), e.g.
//   "script.write by 00000000-0000-4000-8000-000000000001 name=heating"
// `by` is printed when it looks like an account id (1-40 of [0-9a-f-]), else "-"; ` id=<id>` (rules,
// id >= 0) and ` name=<name>` (only a valid script name) follow when present. False when `cmd` is not a
// logged change (reads, checks, script.part).
bool remote_change_log_line(const char* cmd, const char* by, long id, const char* name, char* out, size_t cap);

// A name the hub stores a script under: [A-Za-z0-9_-]{1,24}.
bool remote_script_name_ok(const char* name);

#ifdef __cplusplus
}
#endif
