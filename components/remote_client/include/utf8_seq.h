// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
//
// The ONE strict UTF-8 range table for what the hub puts into a WebSocket text frame: JsonWriter::str and
// utf8_safe_copy (main/json_buf.h) and the script page cutter (remote_xfer.cpp). Header-only and shared on
// purpose: Chrome and Tomcat close the socket on one invalid byte, so a copy that drifts brings the link
// drop back on just one path.

#include <cstddef>

// Strict RFC 3629 UTF-8 validator: returns the byte length (1..4) of the
// complete, valid sequence starting at s[0] given at most `avail` readable
// bytes, or 0 if the sequence is invalid (bad lead 0x80-0xC1/0xF5-0xFF, bad
// continuation, overlong form, UTF-16 surrogate, > U+10FFFF) or truncated
// (by `avail` or by a NUL — NUL fails the continuation range, so bytes past a
// terminator are never read). Strictness deliberately matches what WS text-
// frame validators (Chrome, Tomcat) enforce per RFC 6455 §8.1: anything they
// would reject, we must not emit.
inline size_t utf8_seq_len(const unsigned char* s, size_t avail) {
    if (avail == 0) return 0;
    unsigned char c = s[0];
    if (c < 0x80) return 1;
    size_t need; unsigned char lo = 0x80, hi = 0xBF;   // first-continuation range
    if      (c >= 0xC2 && c <= 0xDF) { need = 1; }
    else if (c == 0xE0)              { need = 2; lo = 0xA0; }   // no overlong
    else if (c >= 0xE1 && c <= 0xEC) { need = 2; }
    else if (c == 0xED)              { need = 2; hi = 0x9F; }   // no surrogates
    else if (c == 0xEE || c == 0xEF) { need = 2; }
    else if (c == 0xF0)              { need = 3; lo = 0x90; }   // no overlong
    else if (c >= 0xF1 && c <= 0xF3) { need = 3; }
    else if (c == 0xF4)              { need = 3; hi = 0x8F; }   // <= U+10FFFF
    else return 0;                       // 0x80-0xC1 (orphan/overlong) or 0xF5-0xFF
    if (avail < need + 1) return 0;      // truncated by the read bound
    for (size_t k = 1; k <= need; k++) {
        unsigned char cc = s[k];
        unsigned char l = (k == 1) ? lo : 0x80;
        unsigned char h = (k == 1) ? hi : 0xBF;
        if (cc < l || cc > h) return 0;  // includes NUL — stops before over-read
    }
    return need + 1;
}
