// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

// True iff `cmd` is in the static allow-list of commands the remote
// channel may invoke on this device. Returns false for any cmd
// starting with "remote." (the admin-only family).
//
// LINEAR SCAN over a static array. Acceptable: list is ~40 entries,
// invoked at most once per cloud-originating frame. Frequency is
// bounded by the network, not by app logic.
bool remote_cmd_allowed(const char* cmd);

// True iff the event name should be mirrored to the remote sink.
bool remote_event_allowed(const char* name);

#ifdef __cplusplus
}
#endif
