// SPDX-FileCopyrightText: 2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Staging slot (script.part) and page cutter (script.read), spec 2026-10-05 §3.4.
#include "remote_xfer.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

struct Seen { std::string text; int calls = 0; };
static void keep(const char* t, size_t n, void* ctx) {
    auto* s = static_cast<Seen*>(ctx);
    s->text.assign(t, n);
    s->calls++;
}
static bool is(const char* got, const char* want) { return got && std::strcmp(got, want) == 0; }

static void staging() {
    size_t got = 0;
    Seen s;
    remote_xfer_reset();

    // In-order assembly; the text is complete only after the last part.
    assert(remote_xfer_part("ab12", 0, 3, "one ", 4, 1000, &got) == nullptr && got == 4);
    assert(remote_xfer_part("ab12", 1, 3, "two ", 4, 1001, &got) == nullptr && got == 8);
    assert(!remote_xfer_use("ab12", 1002, false, keep, &s));
    assert(remote_xfer_part("ab12", 2, 3, "three", 5, 1002, &got) == nullptr && got == 13);
    // A check reads it and keeps it; a write consumes it.
    assert(remote_xfer_use("ab12", 1003, false, keep, &s) && s.text == "one two three");
    assert(remote_xfer_use("ab12", 1004, true, keep, &s) && s.calls == 2);
    assert(!remote_xfer_use("ab12", 1005, true, keep, &s));
    assert(!remote_xfer_use("other", 1005, false, keep, &s));

    // Another xfer, a skipped or repeated index, a changed parts count: bad_part, and the slot is gone.
    assert(remote_xfer_part("t1", 0, 3, "a", 1, 0, &got) == nullptr);
    assert(is(remote_xfer_part("t2", 1, 3, "b", 1, 0, &got), "bad_part"));
    assert(is(remote_xfer_part("t1", 1, 3, "b", 1, 0, &got), "bad_part"));        // dropped with the error
    assert(remote_xfer_part("t1", 0, 3, "a", 1, 0, &got) == nullptr);
    assert(is(remote_xfer_part("t1", 2, 3, "c", 1, 0, &got), "bad_part"));        // index 1 expected
    assert(remote_xfer_part("t1", 0, 3, "a", 1, 0, &got) == nullptr);
    assert(remote_xfer_part("t1", 1, 3, "b", 1, 0, &got) == nullptr);
    assert(is(remote_xfer_part("t1", 1, 3, "b", 1, 0, &got), "bad_part"));        // repeated
    assert(remote_xfer_part("t1", 0, 3, "a", 1, 0, &got) == nullptr);
    assert(is(remote_xfer_part("t1", 1, 4, "b", 1, 0, &got), "bad_part"));        // parts changed
    assert(!remote_xfer_use("t1", 0, false, keep, &s));

    // The xfer's shape, the parts range, a missing src.
    assert(is(remote_xfer_part("", 0, 2, "x", 1, 0, &got), "bad_part"));
    assert(is(remote_xfer_part("12345678901234567", 0, 2, "x", 1, 0, &got), "bad_part"));   // 17 chars
    assert(remote_xfer_part("1234567890123456", 0, 2, "x", 1, 0, &got) == nullptr);         // 16 chars
    assert(is(remote_xfer_part("ab-1", 0, 2, "x", 1, 0, &got), "bad_part"));
    assert(is(remote_xfer_part("1234567890123456", 1, 2, "y", 1, 0, &got), "bad_part"));   // the bad frame dropped the open transfer
    assert(is(remote_xfer_part("ok", 0, 1, "x", 1, 0, &got), "bad_part"));
    assert(is(remote_xfer_part("ok", 0, 17, "x", 1, 0, &got), "bad_part"));
    assert(remote_xfer_part("ok", 0, 16, "x", 1, 0, &got) == nullptr);
    assert(is(remote_xfer_part("ok", 1, 16, nullptr, 0, 0, &got), "bad_part"));

    // Over 16,384 bytes in total: too_large, and the slot is gone. Exactly 16,384 is fine.
    const std::string half(8192, 'a');
    assert(remote_xfer_part("big", 0, 3, half.data(), half.size(), 0, &got) == nullptr);
    assert(remote_xfer_part("big", 1, 3, half.data(), half.size(), 1, &got) == nullptr && got == 16384);
    assert(is(remote_xfer_part("big", 2, 3, "x", 1, 2, &got), "too_large"));
    assert(is(remote_xfer_part("big", 2, 3, "x", 1, 3, &got), "bad_part"));       // too_large dropped it: the same part finds no slot
    assert(!remote_xfer_use("big", 3, false, keep, &s));
    assert(remote_xfer_part("fit", 0, 2, half.data(), half.size(), 0, &got) == nullptr);
    assert(remote_xfer_part("fit", 1, 2, half.data(), half.size(), 0, &got) == nullptr);
    assert(remote_xfer_use("fit", 0, true, keep, &s) && s.text.size() == 16384);

    // 30 s after its last use the slot is gone (checked when an op next touches it); a check is a use.
    assert(remote_xfer_part("w", 0, 2, "a", 1, 5000, &got) == nullptr);
    assert(remote_xfer_part("w", 1, 2, "b", 1, 5000 + 29999, &got) == nullptr);
    assert(remote_xfer_use("w", 5000 + 29999 + 29999, false, keep, &s) && s.text == "ab");
    assert(!remote_xfer_use("w", 5000 + 29999 + 29999 + 30000, false, keep, &s));
    // A check renews the 30 s (59,998 ms after the last part, still there); a part after 30 s of silence finds no slot.
    assert(remote_xfer_part("c", 0, 2, "a", 1, 100, &got) == nullptr);
    assert(remote_xfer_part("c", 1, 2, "b", 1, 100, &got) == nullptr);
    assert(remote_xfer_use("c", 100 + 29999, false, keep, &s));
    assert(remote_xfer_use("c", 100 + 29999 + 29999, false, keep, &s) && s.text == "ab");
    assert(remote_xfer_part("d", 0, 3, "a", 1, 200, &got) == nullptr);
    assert(is(remote_xfer_part("d", 1, 3, "b", 1, 200 + 30000, &got), "bad_part"));
    // The clock may wrap.
    assert(remote_xfer_part("z", 0, 2, "a", 1, 0xFFFFFFF0u, &got) == nullptr);
    assert(remote_xfer_part("z", 1, 2, "b", 1, 0x10u, &got) == nullptr);

    // Part 0 replaces any other transfer.
    assert(remote_xfer_part("old", 0, 2, "a", 1, 0, &got) == nullptr);
    assert(remote_xfer_part("new", 0, 2, "b", 1, 1, &got) == nullptr);
    assert(is(remote_xfer_part("old", 1, 2, "c", 1, 2, &got), "bad_part"));
    remote_xfer_reset();
}

static std::string page(const std::string& text, size_t offset, size_t budget, size_t* next) {
    std::string out(budget, '\0');
    out.resize(remote_xfer_page(text.data(), text.size(), offset, budget, out.data(), next));
    return out;
}

static void pages() {
    size_t next = 0;
    // ASCII; an empty text; an exact fit; the rest on the next page.
    assert(page("hello", 0, 6144, &next) == "hello" && next == 5);
    assert(page("", 0, 6144, &next).empty() && next == 0);
    assert(page("abcdef", 0, 6, &next) == "abcdef" && next == 6);
    assert(page("abcdefg", 0, 6, &next) == "abcdef" && next == 6);
    assert(page("abcdefg", 6, 6, &next) == "g" && next == 7);
    assert(page("abc", 9, 6, &next).empty() && next == 3);                  // past the end

    // Quotes, backslashes and control bytes are escaped, and their escaped size counts.
    assert(page("a\"b\\c\nd\te\x01", 0, 6144, &next) == "a\\\"b\\\\c\\nd\\te\\u0001");
    assert(page("ab\"", 0, 3, &next) == "ab" && next == 2);                // \" (2 bytes) does not fit 1

    // 2-, 3- and 4-byte characters at the edge stay whole.
    const std::string two = "a\xd0\xb6";                                    // a ж
    assert(page(two, 0, 2, &next) == "a" && next == 1);
    assert(page(two, 0, 3, &next) == two && next == 3);
    const std::string three = "ab\xe2\x82\xac";                             // ab €
    assert(page(three, 0, 4, &next) == "ab" && next == 2);
    const std::string four = "a\xf0\x9f\x98\x80";                           // a 😀
    assert(page(four, 0, 4, &next) == "a" && next == 1);
    assert(page(four, 0, 5, &next) == four && next == 5);

    // An invalid byte goes out as �: one bad byte in a WebSocket text frame closes the socket.
    assert(page("a\xff" "b", 0, 6144, &next) == "a\\ufffdb" && next == 3);
    assert(page("\xd0", 0, 6144, &next) == "\\ufffd" && next == 1);       // a character cut by the file's end

    // Strict RFC 3629, as Chrome and Tomcat check it: overlong forms, surrogates and anything past U+10FFFF
    // are invalid (one � per byte); the first and last character of every valid range stay whole.
    for (const char* bad : {"\xc0\x80", "\xc1\xbf", "\xe0\x9f\xbf", "\xed\xa0\x80", "\xf0\x8f\xbf\xbf",
                            "\xf4\x90\x80\x80", "\xf5\x80\x80\x80"}) {
        std::string want;
        for (size_t k = 0; k < std::strlen(bad); k++) want += "\\ufffd";
        assert(page(bad, 0, 6144, &next) == want && next == std::strlen(bad));
    }
    for (const char* ok : {"\xc2\x80", "\xdf\xbf", "\xe0\xa0\x80", "\xed\x9f\xbf", "\xee\x80\x80", "\xf0\x90\x80\x80",
                           "\xf4\x8f\xbf\xbf"})
        assert(page(ok, 0, 6144, &next) == ok && next == std::strlen(ok));

    // Small pages put together give the one-page escape, and never pass the budget.
    const std::string mixed = std::string("x\"\xd0\xb6\n") + "\xf0\x9f\x98\x80" + "\xe2\x82\xac" + "\x01" + "\\";
    size_t at = 0, guard = 0;
    std::string joined;
    while (at < mixed.size() && guard++ < 100) {
        size_t nx = 0;
        const std::string p = page(mixed, at, 7, &nx);
        assert(p.size() <= 7 && nx > at);
        joined += p;
        at = nx;
    }
    assert(at == mixed.size() && joined == page(mixed, 0, 6144, &next));
}

int main() {
    staging();
    pages();
    printf("remote_xfer_tests: ok\n");
    return 0;
}
