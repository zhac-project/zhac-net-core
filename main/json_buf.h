// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdarg>
#include <cstdio>

// Checked bounded JSON append (CODEX H-01/H-02).
//
// The hand-rolled `pos += snprintf(buf + pos, cap - pos, ...)` idiom is unsafe:
// on truncation snprintf returns the length it WOULD have written, so `pos` can
// exceed `cap`; the next `buf + pos` then points past the buffer and
// `cap - pos` underflows to a huge size_t, turning the following write into an
// out-of-bounds write. JsonWriter keeps `pos_ <= cap_` at all times: any append
// that would not fit latches `ok_ = false` and stops advancing, so callers fail
// closed (finish() returns 0) instead of walking off the end. Responses are
// length-delimited, so no trailing NUL is required — `fmt()` may write one
// within `cap_` transiently but it is always inside the buffer.
class JsonWriter {
public:
    JsonWriter(char* buf, size_t cap) : buf_(buf), cap_(cap) {}

    // Raw literal (no escaping).
    JsonWriter& raw(const char* s) {
        if (!ok_) return *this;
        for (; *s; ++s) if (!put(*s)) break;
        return *this;
    }
    JsonWriter& ch(char c) { if (ok_) put(c); return *this; }

    // printf-style. Fails closed on truncation.
    JsonWriter& fmt(const char* f, ...) __attribute__((format(printf, 2, 3))) {
        if (!ok_) return *this;
        va_list ap; va_start(ap, f);
        size_t rem = cap_ - pos_;                 // pos_ <= cap_ invariant
        int n = vsnprintf(buf_ + pos_, rem, f, ap);
        va_end(ap);
        if (n < 0 || (size_t)n >= rem) { ok_ = false; return *this; }
        pos_ += (size_t)n;
        return *this;
    }

    // JSON-escaped string CONTENT (caller supplies the surrounding quotes).
    // Bounded by `max` source bytes so a non-terminated on-flash string can't
    // be over-read (H-01: NVS-loaded GrpRecord::name).
    JsonWriter& str(const char* s, size_t max) {
        if (!ok_) return *this;
        for (size_t i = 0; i < max && s[i]; ++i) {
            unsigned char c = (unsigned char)s[i];
            if (c == '"' || c == '\\' || c == '\n' || c == '\r' || c == '\t') {
                char e = (c == '"') ? '"' : (c == '\\') ? '\\' :
                         (c == '\n') ? 'n' : (c == '\r') ? 'r' : 't';
                if (!put('\\') || !put(e)) break;
            } else if (c < 0x20) {
                char u[8];
                int n = snprintf(u, sizeof(u), "\\u%04x", c);
                if (n != 6) { ok_ = false; break; }
                bool fit = true;
                for (int k = 0; k < 6 && fit; ++k) fit = put(u[k]);
                if (!fit) break;
            } else {
                if (!put((char)c)) break;
            }
        }
        return *this;
    }

    bool   ok()  const { return ok_; }
    size_t len() const { return pos_; }
    // Final length, or 0 if any append overflowed (fail closed).
    size_t finish() const { return ok_ ? pos_ : 0; }

private:
    bool put(char c) {
        if (pos_ >= cap_) { ok_ = false; return false; }
        buf_[pos_++] = c;
        return true;
    }
    char*  buf_;
    size_t cap_;
    size_t pos_ = 0;
    bool   ok_  = true;
};
