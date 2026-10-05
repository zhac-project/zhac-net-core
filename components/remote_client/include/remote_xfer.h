// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// Remote script editing (spec 2026-10-05 §3.4): the staging slot that assembles a long text from
// script.part frames, and the page cutter for script.read. Pure, host-tested in test/remote_xfer_test.cpp.
// The cloud relay drops any frame over 8,192 bytes, so a 16 KB script travels in parts, and every
// part or page carries at most 6,144 JSON-encoded bytes.

#include <cstddef>
#include <cstdint>

constexpr size_t   kRemoteXferMax    = 16384;   // == LUA_SCRIPT_SRC_MAX
constexpr size_t   kRemotePageBudget = 6144;    // JSON-encoded bytes in one part or page
constexpr uint32_t kRemoteXferIdleMs = 30000;   // a slot unused this long is dropped

// script.part {xfer, part, parts, src}: stages one part of transfer `xfer` ([A-Za-z0-9]{1,16}), parts
// 2-16, in order. Part 0 opens the slot (PSRAM, 16,385 bytes) and drops any other transfer. nullptr =
// staged (*received = bytes so far); else "bad_part" (another xfer, an index other than the next, a
// changed parts count, a bad shape), "too_large" (over 16,384 bytes in total) or "oom". Every error
// drops the slot. One slot per hub, guarded by a mutex (the hub page and the relay dispatch on
// different tasks).
const char* remote_xfer_part(const char* xfer, int part, int parts, const char* src, size_t len,
                             uint32_t now_ms, size_t* received);

// Hands the complete staged text of `xfer` (NUL-terminated, `len` bytes) to fn, under the slot's lock.
// consume: drop the slot afterwards (script.write); else keep it (script.check) and count this as a use.
// False when there is no complete text for this xfer: "no_xfer".
bool remote_xfer_use(const char* xfer, uint32_t now_ms, bool consume,
                     void (*fn)(const char* text, size_t len, void* ctx), void* ctx);

// Drops any staged text.
void remote_xfer_reset();

// script.read {offset}: writes the JSON-escaped text (no quotes) of the page that starts at byte `offset`
// into out -- as many whole characters as fit `budget` escaped bytes; an invalid UTF-8 byte is written
// as �. Returns the escaped length and sets *next to the byte offset after the page (== len on the
// last page). `out` holds at least `budget` bytes.
size_t remote_xfer_page(const char* text, size_t len, size_t offset, size_t budget, char* out, size_t* next);
