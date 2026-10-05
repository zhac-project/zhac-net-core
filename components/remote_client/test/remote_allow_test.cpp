// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "remote_allow.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <initializer_list>

static bool is(const char* got, const char* want) { return got && std::strcmp(got, want) == 0; }

int main() {
    // Spot-check a representative cmd from each family is allowed.
    assert(remote_cmd_allowed("status.get"));
    assert(remote_cmd_allowed("device.list"));
    assert(remote_cmd_allowed("device.attr.set"));
    assert(remote_cmd_allowed("rule.create"));
    assert(remote_cmd_allowed("group.cmd"));
    assert(remote_cmd_allowed("alerts.get"));

    // Cloud automations and remote rule editing: every rule command, run counters, Run now, the check.
    for (const char* c : {"rule.list", "rule.create", "rule.update", "rule.enable",
                          "rule.delete", "rules.status", "rule.run", "rule.check"}) {
        assert(remote_cmd_allowed(c));
    }
    // Scripts: reading, deleting (runs no code), checking and staging a part pass on any build.
    for (const char* c : {"script.list", "script.read", "script.delete", "script.check", "script.part"}) {
        assert(remote_cmd_allowed(c));
    }

    // Switch-gated (spec 2026-10-05 §3.1): refused with the build's default switch (none: the weak
    // remote_scripts_allowed() says no), refused scripts_off with a closed switch, passed with an open one.
    assert(!remote_scripts_allowed());
    for (const char* c : {"script.write", "script.run", "script.reload", "system.restart"}) {
        assert(!remote_cmd_allowed(c));
        assert(is(remote_cmd_refusal(c, false, false), "scripts_off"));
        assert(remote_cmd_refusal(c, false, true) == nullptr);
    }

    // The destructive set stays privileged: no switch opens it (denied without the Kconfig).
    for (const char* c : {"ota.s3", "ota.p4", "zigbee.reset", "device.delete", "settings.set",
                          "system.token.rotate", "uplink.set", "deploy.apply", "deploy.etag"}) {
        assert(!remote_cmd_allowed(c));
        assert(is(remote_cmd_refusal(c, false, true), "cmd_not_allowed"));
    }

    // The switch is the hub page's alone: settings.set carrying it is local_only, switch on or off.
    assert(is(remote_cmd_refusal("settings.set", true, true), "local_only"));
    assert(is(remote_cmd_refusal("settings.set", true, false), "local_only"));
    assert(is(remote_cmd_refusal("settings.set", false, true), "cmd_not_allowed"));
    assert(remote_cmd_refusal("status.get", true, false) == nullptr);   // only settings.set is touched

    // The wifi RO subset is allowed; the mutators are NOT (orphan-the-device risk).
    assert(remote_cmd_allowed("wifi.status"));
    assert(remote_cmd_allowed("wifi.scan"));
    assert(!remote_cmd_allowed("wifi.connect"));
    assert(!remote_cmd_allowed("wifi.disconnect"));

    // The remote.* admin family is NEVER allowed.
    for (const char* c : {"remote.connect", "remote.disconnect", "remote.status", "remote.auth"}) {
        assert(is(remote_cmd_refusal(c, false, true), "cmd_not_allowed"));
    }

    // Unknown / typo'd commands are not allowed.
    assert(!remote_cmd_allowed(""));
    assert(!remote_cmd_allowed(nullptr));
    assert(!remote_cmd_allowed("not.a.command"));
    assert(!remote_cmd_allowed("device.lis"));
    assert(!remote_cmd_allowed("device.listing"));                    // exact names: no prefix match

    // Events: device.* / rule.* / group.* / alert, the single-chip names, and remote editing's.
    for (const char* e : {"device.added", "device.attribute_change", "rule.fired", "group.changed", "alert",
                          "attr.changed", "attr.bulk", "rule.updated", "rule.deleted", "group.updated",
                          "group.deleted", "script.added", "script.updated", "script.deleted",
                          "script.error", "hub.caps"}) {
        assert(remote_event_allowed(e));
    }
    // log / wifi / mqtt / bulk events stay LAN-only; unknown ones too.
    for (const char* e : {"log.line", "log.level", "wifi.connected", "mqtt.connected", "bulk.state", "", "nonsense"}) {
        assert(!remote_event_allowed(e));
    }

    // Script names: [A-Za-z0-9_-]{1,24}.
    assert(remote_script_name_ok("heating"));
    assert(remote_script_name_ok("Door_log-2"));
    assert(remote_script_name_ok("abcdefghijklmnopqrstuvwx"));     // 24
    assert(!remote_script_name_ok("abcdefghijklmnopqrstuvwxy"));   // 25
    assert(!remote_script_name_ok(""));
    assert(!remote_script_name_ok(nullptr));
    assert(!remote_script_name_ok("a b"));
    assert(!remote_script_name_ok("x.lua"));
    assert(!remote_script_name_ok("\xd0\xb6"));

    // The hub-log line for a relayed change (§3.7).
    char line[160];
    assert(remote_change_log_line("script.write", "00000000-0000-4000-8000-000000000001", -1, "heating",
                                  line, sizeof line));
    assert(std::strcmp(line, "script.write by 00000000-0000-4000-8000-000000000001 name=heating") == 0);
    assert(remote_change_log_line("rule.update", "00000000-0000-4000-8000-000000000001", 12, "Porch light",
                                  line, sizeof line));
    assert(std::strcmp(line, "rule.update by 00000000-0000-4000-8000-000000000001 id=12") == 0);
    assert(remote_change_log_line("system.restart", "", -1, "", line, sizeof line));
    assert(std::strcmp(line, "system.restart by -") == 0);
    // `by` that is not an account id is never printed (no forged log lines).
    assert(remote_change_log_line("script.run", "x\nI (1) cloud: fake", -1, "heating", line, sizeof line));
    assert(std::strcmp(line, "script.run by - name=heating") == 0);
    assert(remote_change_log_line("script.run", "00000000-0000-4000-8000-00000000000100000", -1, "a", line, sizeof line));
    assert(std::strcmp(line, "script.run by - name=a") == 0);      // 41 characters
    assert(remote_change_log_line("script.run", "0123456789abcdef0123456789abcdef01234567", -1, "a", line, sizeof line));
    assert(std::strcmp(line, "script.run by 0123456789abcdef0123456789abcdef01234567 name=a") == 0);   // 40 is fine
    assert(remote_change_log_line("script.run", "ABCDEF", -1, "a", line, sizeof line));
    assert(std::strcmp(line, "script.run by - name=a") == 0);      // only lowercase hex, as the cloud sends it
    // Reads, checks and parts are not logged.
    for (const char* c : {"rule.list", "rule.check", "script.read", "script.check", "script.part", "status.get"}) {
        assert(!remote_change_log_line(c, "00000000-0000-4000-8000-000000000001", -1, "", line, sizeof line));
    }

    printf("remote_allow_tests: ok\n");
    return 0;
}
