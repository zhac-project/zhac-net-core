// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Staging slot and page cutter for remote script editing (spec 2026-10-05 §3.4). See remote_xfer.h.

#include "remote_xfer.h"
#include "utf8_seq.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

namespace {

struct Slot {
    char*    buf = nullptr;     // kRemoteXferMax + 1 bytes while a transfer is open
    char     xfer[17] = {};
    int      parts = 0;
    int      next = 0;          // the part expected next; == parts when complete
    size_t   len = 0;
    uint32_t last_ms = 0;
};
Slot       s_slot;
std::mutex s_mu;

char* alloc_text() {
#ifdef ESP_PLATFORM
    return static_cast<char*>(heap_caps_malloc(kRemoteXferMax + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    return static_cast<char*>(std::malloc(kRemoteXferMax + 1));
#endif
}

void drop() {
#ifdef ESP_PLATFORM
    heap_caps_free(s_slot.buf);
#else
    std::free(s_slot.buf);
#endif
    s_slot = Slot{};
}

// Checked when an op next touches the slot; unsigned subtraction survives the clock wrapping.
void expire(uint32_t now_ms) {
    if (s_slot.buf && now_ms - s_slot.last_ms >= kRemoteXferIdleMs) drop();
}

bool xfer_ok(const char* x) {
    size_t n = 0;
    for (; x && x[n]; n++) {
        const char c = x[n];
        const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!alnum || n >= 16) return false;
    }
    return n > 0;
}

}  // namespace

const char* remote_xfer_part(const char* xfer, int part, int parts, const char* src, size_t len,
                             uint32_t now_ms, size_t* received) {
    std::lock_guard<std::mutex> lock(s_mu);
    expire(now_ms);
    if (!xfer_ok(xfer) || parts < 2 || parts > 16 || part < 0 || part >= parts || !src) {
        drop();
        return "bad_part";
    }
    if (part == 0) {
        drop();
        s_slot.buf = alloc_text();
        if (!s_slot.buf) return "oom";
        std::memcpy(s_slot.xfer, xfer, std::strlen(xfer) + 1);   // xfer_ok: at most 16 characters
        s_slot.parts = parts;
    } else if (!s_slot.buf || std::strcmp(s_slot.xfer, xfer) != 0 || part != s_slot.next || parts != s_slot.parts) {
        drop();
        return "bad_part";
    }
    if (s_slot.len + len > kRemoteXferMax) {
        drop();
        return "too_large";
    }
    std::memcpy(s_slot.buf + s_slot.len, src, len);
    s_slot.len += len;
    s_slot.next = part + 1;
    s_slot.last_ms = now_ms;
    if (received) *received = s_slot.len;
    return nullptr;
}

bool remote_xfer_use(const char* xfer, uint32_t now_ms, bool consume,
                     void (*fn)(const char* text, size_t len, void* ctx), void* ctx) {
    std::lock_guard<std::mutex> lock(s_mu);
    expire(now_ms);
    if (!s_slot.buf || !xfer || std::strcmp(s_slot.xfer, xfer) != 0 || s_slot.next != s_slot.parts) return false;
    s_slot.buf[s_slot.len] = '\0';
    fn(s_slot.buf, s_slot.len, ctx);
    if (consume) drop();
    else s_slot.last_ms = now_ms;
    return true;
}

void remote_xfer_reset() {
    std::lock_guard<std::mutex> lock(s_mu);
    drop();
}

size_t remote_xfer_page(const char* text, size_t len, size_t offset, size_t budget, char* out, size_t* next) {
    size_t i = offset < len ? offset : len, o = 0;
    while (i < len) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        char esc[8];
        const char* piece = esc;
        size_t pn = 0, adv = 1;
        switch (c) {
            case '"':  esc[0] = '\\'; esc[1] = '"';  pn = 2; break;
            case '\\': esc[0] = '\\'; esc[1] = '\\'; pn = 2; break;
            case '\n': esc[0] = '\\'; esc[1] = 'n';  pn = 2; break;
            case '\r': esc[0] = '\\'; esc[1] = 'r';  pn = 2; break;
            case '\t': esc[0] = '\\'; esc[1] = 't';  pn = 2; break;
            case '\b': esc[0] = '\\'; esc[1] = 'b';  pn = 2; break;
            case '\f': esc[0] = '\\'; esc[1] = 'f';  pn = 2; break;
            default:
                if (c < 0x20) {
                    pn = static_cast<size_t>(std::snprintf(esc, sizeof(esc), "\\u%04x", c));
                } else if (c < 0x80) {
                    esc[0] = static_cast<char>(c);
                    pn = 1;
                } else if ((adv = utf8_seq_len(reinterpret_cast<const unsigned char*>(text) + i, len - i)) != 0) {
                    piece = text + i;          // a whole character, as is
                    pn = adv;
                } else {
                    std::memcpy(esc, "\\ufffd", 6);   // an invalid byte: U+FFFD
                    pn = 6;
                    adv = 1;
                }
        }
        if (o + pn > budget) break;
        std::memcpy(out + o, piece, pn);
        o += pn;
        i += adv;
    }
    if (next) *next = i;
    return o;
}
