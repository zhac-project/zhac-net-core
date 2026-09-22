// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#include "remote_allow.h"
#include <cassert>
#include <cstdio>

int main() {
    // Spot-check a representative cmd from each family is allowed.
    assert(remote_cmd_allowed("status.get"));
    assert(remote_cmd_allowed("device.list"));
    assert(remote_cmd_allowed("device.attr.set"));
    assert(remote_cmd_allowed("rule.create"));
    // script.run is privileged (F9: arbitrary Lua = RCE); denied unless the
    // build sets CONFIG_ZHAC_REMOTE_ALLOW_PRIVILEGED, which this test does not.
    assert(!remote_cmd_allowed("script.run"));
    assert(remote_cmd_allowed("group.cmd"));
    assert(remote_cmd_allowed("alerts.get"));

    // The wifi RO subset is allowed.
    assert(remote_cmd_allowed("wifi.status"));
    assert(remote_cmd_allowed("wifi.scan"));

    // The wifi mutators are NOT (orphan-the-device risk).
    assert(!remote_cmd_allowed("wifi.connect"));
    assert(!remote_cmd_allowed("wifi.disconnect"));

    // The remote.* admin family is NEVER allowed.
    assert(!remote_cmd_allowed("remote.connect"));
    assert(!remote_cmd_allowed("remote.disconnect"));
    assert(!remote_cmd_allowed("remote.status"));
    assert(!remote_cmd_allowed("remote.auth"));

    // Unknown / typo'd commands are not allowed.
    assert(!remote_cmd_allowed(""));
    assert(!remote_cmd_allowed("not.a.command"));
    assert(!remote_cmd_allowed("device.lis"));

    // Events: device.* / rule.* / group.* / alert allowed.
    assert(remote_event_allowed("device.added"));
    assert(remote_event_allowed("device.attribute_change"));
    assert(remote_event_allowed("rule.fired"));
    assert(remote_event_allowed("group.changed"));
    assert(remote_event_allowed("alert"));

    // Names the single-chip builds (wired S31/P4, mono) push: per-attribute
    // attr.changed {ieee,key,value,ts} and the rule/group edit events. The
    // cloud's LiveEventAdapter consumes all of them; filtered here, a hub on
    // those builds never updated the cloud shadow after the first sync.
    assert(remote_event_allowed("attr.changed"));
    assert(remote_event_allowed("attr.bulk"));
    assert(remote_event_allowed("rule.updated"));
    assert(remote_event_allowed("rule.deleted"));
    assert(remote_event_allowed("group.updated"));
    assert(remote_event_allowed("group.deleted"));

    // log / wifi / mqtt / bulk events stay LAN-only.
    assert(!remote_event_allowed("log.line"));
    assert(!remote_event_allowed("log.level"));
    assert(!remote_event_allowed("wifi.connected"));
    assert(!remote_event_allowed("mqtt.connected"));
    assert(!remote_event_allowed("bulk.state"));

    // Unknown event not allowed.
    assert(!remote_event_allowed(""));
    assert(!remote_event_allowed("nonsense"));

    printf("remote_allow_tests: ok\n");
    return 0;
}
