// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Single source of truth for what the remote channel may touch.
//
// MAINTENANCE: when a new row is added to
// zhac-net-core/main/api_routes.def, decide whether the remote
// channel should expose it. If yes, add the cmd name to
// kRemoteAllowedCmds[] below. If no, no change here — the cmd is
// implicitly denied.
//
// PERMANENT EXCLUDES (security-driven, do not allow-list):
//   - wifi.connect / wifi.disconnect : orphan-the-device risk.
//   - remote.connect / remote.disconnect / remote.status : admin-only
//     on LAN. Re-exposing them via remote = self foot-gun.

#include "remote_allow.h"
#include <cstring>

namespace {

constexpr const char* kRemoteAllowedCmds[] = {
    // Status / system (reads only)
    "status.get", "alerts.get", "logs.get", "diagnostics.unhandled.get",
    // WiFi reads only (mutators excluded — see comment above)
    "wifi.status", "wifi.scan",
    // Zigbee — open the join window + read; network reset is privileged
    "zigbee.permit_join", "zigbee.permit_join.status",
    // Devices — recoverable mutations; device.delete is privileged
    "device.list", "device.get", "device.bind",
    "device.rename", "device.attr.set", "device.options.set",
    "device.reinterview", "device.configure",
    // Rules
    "rule.list", "rule.create", "rule.delete", "rule.enable", "rule.update",
    // Scripts — list/read/delete/check; run + write are privileged (RCE)
    "script.list", "script.read", "script.delete", "script.check",
    // Groups
    "group.list", "group.create", "group.get", "group.update",
    "group.delete", "group.cmd",
};

// F9 (FINDINGS.md): privileged ops the cloud channel must NOT reach by
// default — firmware flash (ota.*), network wipe (zigbee.reset), arbitrary
// Lua execution (script.run/write), device removal, and auth/config changes.
// A compromised or spoofed cloud peer (or anyone holding the bearer token)
// would otherwise gain RCE-equivalent control. Exposed only when
// CONFIG_ZHAC_REMOTE_ALLOW_PRIVILEGED is set, for operators who explicitly
// trust the cloud endpoint.
constexpr const char* kRemotePrivilegedCmds[] = {
    "ota.s3", "ota.p4", "zigbee.reset",
    "script.run", "script.write",
    "device.delete", "settings.set", "system.token.rotate",
};

constexpr const char* kRemoteAllowedEvents[] = {
    // NOTE: these must match the names hap_bridge.cpp actually broadcasts. The live attribute
    // stream is "attr.bulk" (the ~100 ms coalescer) — without it the cloud shadow never sees
    // on/off / sensor changes and can only refresh on reconnect. "device.attribute_change" below
    // is not currently emitted (kept for forward-compat).
    "attr.bulk",
    "device.added", "device.removed", "device.attribute_change",
    "device.online", "device.offline", "device.renamed",
    "device.configure_progress", "device.bound", "device.unbound",
    "rule.added", "rule.removed", "rule.fired", "rule.error",
    "group.added", "group.removed", "group.changed",
    "alert",
};

constexpr size_t kCmdCount   = sizeof(kRemoteAllowedCmds)   / sizeof(kRemoteAllowedCmds[0]);
constexpr size_t kPrivCount  = sizeof(kRemotePrivilegedCmds)/ sizeof(kRemotePrivilegedCmds[0]);
constexpr size_t kEventCount = sizeof(kRemoteAllowedEvents) / sizeof(kRemoteAllowedEvents[0]);

} // namespace

extern "C" bool remote_cmd_allowed(const char* cmd) {
    if (!cmd || !*cmd) return false;
    for (size_t i = 0; i < kCmdCount; i++) {
        if (std::strcmp(cmd, kRemoteAllowedCmds[i]) == 0) return true;
    }
#ifdef CONFIG_ZHAC_REMOTE_ALLOW_PRIVILEGED
    // F9: only reachable when the operator explicitly trusts the cloud peer.
    for (size_t i = 0; i < kPrivCount; i++) {
        if (std::strcmp(cmd, kRemotePrivilegedCmds[i]) == 0) return true;
    }
#else
    (void)kPrivCount;
#endif
    return false;
}

extern "C" bool remote_event_allowed(const char* name) {
    if (!name || !*name) return false;
    for (size_t i = 0; i < kEventCount; i++) {
        if (std::strcmp(name, kRemoteAllowedEvents[i]) == 0) return true;
    }
    return false;
}
