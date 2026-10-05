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
#include <cstdio>
#include <cstring>
// CONFIG_ZHAC_REMOTE_ALLOW_PRIVILEGED. Nothing included sdkconfig.h here before, so the option never
// took effect (fail-closed). Absent in the host tests.
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

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
    "device.groups.list", "device.groups.add", "device.groups.remove",
    "device.groups.refresh",
    // Rules
    "rule.list", "rule.create", "rule.delete", "rule.enable", "rule.update",
    // Cloud automations: per-rule run counters (a read) and "Run now", which
    // runs one stored rule's own actions. A rule's `script.run` action runs a
    // stored script, and the hub switch below does NOT gate it: cloud
    // automations rely on it. The cloud, not the hub, limits rules with that
    // action to owners and admins.
    "rules.status", "rule.run",
    // Remote editing (spec 2026-10-05 §3.1): a parse check stores nothing, and a
    // staged part reaches flash only through script.write, which the switch gates.
    "rule.check", "script.part",
    // Scripts — reading, deleting (runs no code) and checking; changing them and a
    // direct run are switch-gated below
    "script.list", "script.read", "script.delete", "script.check",
    // Groups
    "group.list", "group.create", "group.get", "group.update",
    "group.delete", "group.cmd",
    // RainMaker bridge (Task 18) — reads + recoverable mutations, matching
    // the group.*/device.bind bar: exposing/unexposing a device to
    // RainMaker (add/remove) and starting an association handshake
    // (assoc.set) are all reversible from the app/API with no orphan or
    // destructive-reset risk, same as device.bind or group.delete.
    // uplink.set is deliberately NOT here — see kRemotePrivilegedCmds
    // below: it is a fundamental cloud-uplink mode switch (can force a
    // reboot_required state) closer in kind to settings.set/wifi.connect
    // than to a routine device-management action.
    "uplink.get", "rainmaker.status", "rainmaker.assoc.set",
    "device.rainmaker.list", "device.rainmaker.add", "device.rainmaker.remove",
};

// Switch-gated (spec 2026-10-05 §3.1): changing Lua and controlling the engine from
// the cloud (script.write, script.reload, system.restart), and a direct script.run.
// They pass only while the hub's own page has "Allow script changes from the cloud"
// on (remote_scripts_allowed()); otherwise they are refused scripts_off, so the
// cloud can say "turn it on" rather than "not supported". The switch does not stop
// a stored rule's script.run action from running a script (see "Cloud automations"
// above).
constexpr const char* kRemoteScriptCmds[] = {
    "script.write", "script.run", "script.reload", "system.restart",
};

// F9 (FINDINGS.md): privileged ops the cloud channel must NOT reach by
// default — firmware flash (ota.*), network wipe (zigbee.reset), device
// removal, and auth/config changes. A compromised or spoofed cloud peer (or
// anyone holding the bearer token) would otherwise gain destructive control.
// Exposed only when CONFIG_ZHAC_REMOTE_ALLOW_PRIVILEGED is set, for operators
// who explicitly trust the cloud endpoint. (Lua moved to the switch above.)
constexpr const char* kRemotePrivilegedCmds[] = {
    "ota.s3", "ota.p4", "zigbee.reset",
    "device.delete", "settings.set", "system.token.rotate",
    "uplink.set",
};

constexpr const char* kRemoteAllowedEvents[] = {
    // NOTE: these must match the names hap_bridge.cpp actually broadcasts. The live attribute
    // stream is "attr.bulk" (the ~100 ms coalescer) — without it the cloud shadow never sees
    // on/off / sensor changes and can only refresh on reconnect. "device.attribute_change" below
    // is not currently emitted (kept for forward-compat).
    "attr.bulk",
    // The single-chip builds (wired S31/P4, mono) have no coalescer: they push one
    // attr.changed {ieee,key,value,ts} per attribute, and name rule/group edits
    // *.updated / *.deleted. The cloud's LiveEventAdapter takes all of these.
    "attr.changed",
    "device.added", "device.removed", "device.attribute_change",
    "device.online", "device.offline", "device.renamed",
    "device.configure_progress", "device.bound", "device.unbound",
    "rule.added", "rule.removed", "rule.fired", "rule.error",
    "rule.updated", "rule.deleted",
    "group.added", "group.removed", "group.changed",
    "group.updated", "group.deleted",
    "alert",
    // Remote editing (spec 2026-10-05 §3.6): the cloud's script mirror and the switch.
    "script.added", "script.updated", "script.deleted", "script.error", "hub.caps",
};

// Changes the hub logs with the `cloud` tag (spec 2026-10-05 §3.7).
constexpr const char* kLoggedCmds[] = {
    "rule.create", "rule.update", "rule.enable", "rule.delete", "rule.run",
    "script.write", "script.delete", "script.run", "script.reload", "system.restart",
};

template <size_t N>
bool listed(const char* const (&list)[N], const char* s) {
    for (const char* x : list) {
        if (std::strcmp(x, s) == 0) return true;
    }
    return false;
}

// An account id as the cloud sends it (a UUID): 1-40 of [0-9a-f-].
bool account_id_ok(const char* by) {
    size_t n = 0;
    for (; by && by[n]; n++) {
        const char c = by[n];
        if (n >= 40 || !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || c == '-')) return false;
    }
    return n > 0;
}

} // namespace

extern "C" __attribute__((weak)) bool remote_scripts_allowed(void) { return false; }

extern "C" const char* remote_cmd_refusal(const char* cmd, bool local_only_key, bool scripts_on) {
    if (!cmd || !*cmd) return "cmd_not_allowed";
    // Only the hub's own page turns the script switch on, whatever the build trusts the cloud with.
    if (local_only_key && std::strcmp(cmd, "settings.set") == 0) return "local_only";
    if (listed(kRemoteAllowedCmds, cmd)) return nullptr;
    if (listed(kRemoteScriptCmds, cmd)) return scripts_on ? nullptr : "scripts_off";
#ifdef CONFIG_ZHAC_REMOTE_ALLOW_PRIVILEGED
    // F9: only reachable when the operator explicitly trusts the cloud peer.
    if (listed(kRemotePrivilegedCmds, cmd)) return nullptr;
#else
    (void)kRemotePrivilegedCmds;
#endif
    return "cmd_not_allowed";
}

extern "C" bool remote_cmd_allowed(const char* cmd) {
    return remote_cmd_refusal(cmd, false, remote_scripts_allowed()) == nullptr;
}

extern "C" bool remote_event_allowed(const char* name) {
    return name && *name && listed(kRemoteAllowedEvents, name);
}

extern "C" bool remote_script_name_ok(const char* name) {
    size_t n = 0;
    for (; name && name[n]; n++) {
        const char c = name[n];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '-';
        if (!ok || n >= 24) return false;
    }
    return n > 0;
}

extern "C" bool remote_change_log_line(const char* cmd, const char* by, long id, const char* name,
                                       char* out, size_t cap) {
    if (!cmd || !out || cap == 0 || !listed(kLoggedCmds, cmd)) return false;
    int n = std::snprintf(out, cap, "%s by %s", cmd, account_id_ok(by) ? by : "-");
    if (n > 0 && (size_t)n < cap && id >= 0) n += std::snprintf(out + n, cap - n, " id=%ld", id);
    if (n > 0 && (size_t)n < cap && remote_script_name_ok(name)) {
        n += std::snprintf(out + n, cap - n, " name=%s", name);
    }
    return n > 0;
}
