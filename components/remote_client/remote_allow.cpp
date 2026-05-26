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
    // Status / system
    "status.get", "alerts.get", "logs.get", "diagnostics.unhandled.get",
    "settings.set", "system.token.rotate",
    // WiFi reads only (mutators excluded — see comment above)
    "wifi.status", "wifi.scan",
    // OTA
    "ota.s3", "ota.p4",
    // Zigbee
    "zigbee.permit_join", "zigbee.permit_join.status", "zigbee.reset",
    // Devices
    "device.list", "device.get", "device.bind", "device.delete",
    "device.rename", "device.attr.set", "device.options.set",
    "device.reinterview", "device.configure",
    // Rules
    "rule.list", "rule.create", "rule.delete", "rule.enable", "rule.update",
    // Scripts
    "script.list", "script.read", "script.delete", "script.write",
    "script.run", "script.check",
    // Groups
    "group.list", "group.create", "group.get", "group.update",
    "group.delete", "group.cmd",
};

constexpr const char* kRemoteAllowedEvents[] = {
    "device.added", "device.removed", "device.attribute_change",
    "device.online", "device.offline", "device.renamed",
    "device.configure_progress", "device.bound", "device.unbound",
    "rule.added", "rule.removed", "rule.fired", "rule.error",
    "group.added", "group.removed", "group.changed",
    "alert",
};

constexpr size_t kCmdCount   = sizeof(kRemoteAllowedCmds)   / sizeof(kRemoteAllowedCmds[0]);
constexpr size_t kEventCount = sizeof(kRemoteAllowedEvents) / sizeof(kRemoteAllowedEvents[0]);

} // namespace

extern "C" bool remote_cmd_allowed(const char* cmd) {
    if (!cmd || !*cmd) return false;
    for (size_t i = 0; i < kCmdCount; i++) {
        if (std::strcmp(cmd, kRemoteAllowedCmds[i]) == 0) return true;
    }
    return false;
}

extern "C" bool remote_event_allowed(const char* name) {
    if (!name || !*name) return false;
    for (size_t i = 0; i < kEventCount; i++) {
        if (std::strcmp(name, kRemoteAllowedEvents[i]) == 0) return true;
    }
    return false;
}
