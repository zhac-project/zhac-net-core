// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// CODEX H-01: bounded JSON writer + groups serialization safety.
// Built with ASan+UBSan. Case B2 (max-size group into the list handler's
// tmp[256]) reproduced a stack-buffer-overflow against the pre-fix
// grp_to_json; the fix makes it fail closed instead.
#include "json_buf.h"
#include "groups_store.h"
#include <cstdio>
#include <cstring>

static int s_fail = 0;
#define CHECK(c, m) do { if (!(c)) { s_fail++; printf("  FAIL: %s\n", m); } \
                         else       printf("  ok:   %s\n", m); } while (0)

static GrpRecord make_group(const char* name, uint8_t members) {
    GrpRecord r{};
    r.id = 7;
    snprintf(r.name, sizeof(r.name), "%s", name);
    r.member_count = members;
    for (uint8_t i = 0; i < members && i < GRP_MAX_MEMBERS; i++) {
        r.members[i].ieee = 0xA4C138D750FECC00ULL + i;
        r.members[i].ep   = (uint8_t)(i + 1);
    }
    return r;
}
static bool has(const char* hay, const char* needle) { return strstr(hay, needle) != nullptr; }

int main() {
    printf("test_json_safety\n");

    // ── A: JsonWriter never advances past cap; fails closed ──────────────
    {
        printf("\nA JsonWriter bounds\n");
        char b[16];
        JsonWriter w(b, sizeof(b));
        w.raw("{\"a\":").fmt("%d", 42).ch('}');
        CHECK(w.ok() && w.finish() == 8, "small write fits (len 8)");

        char s[8];
        JsonWriter o(s, sizeof(s));
        o.fmt("%d", 12345678);          // 8 digits + NUL > 8 → truncates
        CHECK(!o.ok() && o.finish() == 0, "fmt overflow latches fail-closed");

        char e[32];
        JsonWriter je(e, sizeof(e));
        je.ch('"').str("a\"b\\c\nd", 16).ch('"');
        CHECK(je.ok(), "escaping fits");
        e[je.finish()] = '\0';
        CHECK(has(e, "a\\\"b\\\\c\\nd"), "quote/backslash/newline escaped");

        char t[2];
        JsonWriter jt(t, sizeof(t));
        jt.raw("abc");
        CHECK(!jt.ok() && jt.finish() == 0, "one-byte-short raw fails closed");

        char noterm[4] = {'x', 'y', 'z', 'w'};   // no NUL
        char big[32];
        JsonWriter jb(big, sizeof(big));
        jb.str(noterm, 4);
        CHECK(jb.ok() && jb.finish() == 4, "str respects max on non-terminated source");
    }

    // ── B: grp_to_json ───────────────────────────────────────────────────
    {
        printf("\nB grp_to_json\n");
        GrpRecord g = make_group("Kitchen", 2);
        char buf[512];
        size_t n = grp_to_json(g, buf, sizeof(buf));
        CHECK(n > 0, "small group serializes");
        buf[n] = '\0';
        CHECK(has(buf, "\"name\":\"Kitchen\""), "name round-trips (not empty)");
        CHECK(has(buf, "\"ep\":1") && has(buf, "\"ep\":2"), "members present");

        // Max group (16 members) into the list handler's tmp[256].
        // Pre-fix: stack-buffer-overflow (H-01). Post-fix: fail closed.
        GrpRecord full = make_group("LivingRoomEverythingGroup", GRP_MAX_MEMBERS);
        char tmp[256];
        size_t nf = grp_to_json(full, tmp, sizeof(tmp));
        CHECK(nf == 0, "max group into tmp[256] fails closed (no overflow)");

        char bigb[1024];
        size_t nb = grp_to_json(full, bigb, sizeof(bigb));
        CHECK(nb > 0 && nb < sizeof(bigb), "max group fits a 1KB buffer");
        bigb[nb] = '\0';
        CHECK(has(bigb, "\"ep\":16"), "16th member present in big buffer");

        // Corrupt member_count must not index past members[GRP_MAX_MEMBERS].
        GrpRecord corrupt = make_group("bad", GRP_MAX_MEMBERS);
        corrupt.member_count = 200;
        char cb[1024];
        size_t nc = grp_to_json(corrupt, cb, sizeof(cb));   // ASan: no OOB read
        CHECK(nc > 0, "corrupt member_count clamped, no OOB read");

        char sb[10];
        CHECK(grp_to_json(g, sb, sizeof(sb)) == 0, "too-small buffer fails closed");

        GrpRecord esc = make_group("a\"b\\c\td", 1);
        char eb[256];
        size_t ne = grp_to_json(esc, eb, sizeof(eb));
        CHECK(ne > 0, "escaping-heavy name serializes");
        eb[ne] = '\0';
        CHECK(has(eb, "a\\\"b\\\\c\\td"), "name escaped in output");
    }

    // ── C: JsonWriter::str emits only valid UTF-8 (WS text-frame safety) ──
    // RFC 6455 §8.1: a WS TEXT frame must be valid UTF-8 — Chrome and Tomcat
    // both close the socket (1007) on violation. A group name strncpy-truncated
    // mid-multibyte-sequence (name[32], Cyrillic = 2 B/char) stored a dangling
    // lead byte in NVS; every group.list reply then killed the WS session
    // (local SPA "not connected" loop + cloud hub-link reconnect loop).
    // str() must pass valid UTF-8 through byte-identical and replace any
    // invalid sequence with the JSON escape � (U+FFFD), never emitting
    // raw invalid bytes.
    {
        printf("\nC JsonWriter UTF-8 sanitization\n");
        auto render = [](const char* src, size_t max, char* out, size_t outsz) -> size_t {
            JsonWriter w(out, outsz);
            w.str(src, max);
            size_t n = w.finish();
            if (n < outsz) out[n] = '\0';
            return n;
        };
        auto no_high_bytes = [](const char* s) {
            for (; *s; ++s) if ((unsigned char)*s >= 0x80) return false;
            return true;
        };

        char out[256];

        // Valid 2-byte sequences (Cyrillic) pass byte-identical.
        const char* cyr = "\xD0\x9A\xD1\x83\xD1\x85\xD0\xBD\xD1\x8F";   // "Кухня"
        render(cyr, 64, out, sizeof(out));
        CHECK(strcmp(out, cyr) == 0, "valid Cyrillic passes byte-identical");

        // Valid 3-byte (€) and 4-byte (emoji) sequences pass.
        render("\xE2\x82\xAC", 64, out, sizeof(out));
        CHECK(strcmp(out, "\xE2\x82\xAC") == 0, "valid 3-byte sequence passes");
        render("\xF0\x9F\x98\x80", 64, out, sizeof(out));
        CHECK(strcmp(out, "\xF0\x9F\x98\x80") == 0, "valid 4-byte sequence passes");

        // THE bug shape: name truncated mid-sequence — dangling lead byte at end.
        render("ok\xD0", 64, out, sizeof(out));
        CHECK(has(out, "\\ufffd"), "dangling lead byte becomes \\ufffd escape");
        CHECK(no_high_bytes(out), "no raw invalid byte reaches the output");
        CHECK(has(out, "ok"), "preceding ASCII preserved");

        // Orphan continuation byte mid-string.
        render("a\x80z", 64, out, sizeof(out));
        CHECK(has(out, "\\ufffd") && out[0] == 'a' && has(out, "z"),
              "orphan continuation byte replaced, neighbors kept");
        CHECK(no_high_bytes(out), "orphan continuation: output pure");

        // Overlong encoding (C0 AF) must not pass (Tomcat rejects overlongs).
        render("\xC0\xAF", 64, out, sizeof(out));
        CHECK(has(out, "\\ufffd") && no_high_bytes(out), "overlong encoding rejected");

        // CESU-8 surrogate half (ED A0 80) must not pass (invalid per RFC 3629).
        render("\xED\xA0\x80", 64, out, sizeof(out));
        CHECK(has(out, "\\ufffd") && no_high_bytes(out), "UTF-16 surrogate half rejected");

        // Out-of-range lead (F5) rejected.
        render("\xF5\x80\x80\x80", 64, out, sizeof(out));
        CHECK(has(out, "\\ufffd") && no_high_bytes(out), "lead byte > U+10FFFF rejected");

        // A valid sequence CUT by the `max` bound must not emit a partial char.
        render("\xE2\x82\xAC", 2, out, sizeof(out));   // € with only 2 bytes visible
        CHECK(has(out, "\\ufffd") && no_high_bytes(out),
              "sequence truncated by max bound is replaced, not half-emitted");

        // Mixed: valid Cyrillic + dangling lead + ASCII — only the bad byte replaced.
        render("\xD0\x94\xD0\xB0\xD1\x82\xD0", 64, out, sizeof(out));   // "Дат" + dangling D0
        CHECK(has(out, "\xD0\x94\xD0\xB0\xD1\x82") && has(out, "\\ufffd"),
              "valid prefix kept byte-identical, only dangling tail replaced");
    }

    // ── D: utf8_safe_copy — store-time truncation lands on a char boundary ──
    {
        printf("\nD utf8_safe_copy\n");
        char dst[32];

        // 16 Cyrillic chars = 32 B — must cut after the 15th char (30 B), not
        // strncpy's blind 31 B (which strands a lead byte).
        const char* n16 = "\xD0\x94\xD0\xB0\xD1\x82\xD1\x87\xD0\xB8\xD0\xBA\xD0\xB8"
                          "\xD0\xB3\xD0\xBE\xD1\x81\xD1\x82\xD0\xB8\xD0\xBD\xD0\xBE"
                          "\xD0\xB9\xD1\x8F";   // 16 × 2 B
        utf8_safe_copy(dst, sizeof(dst), n16);
        CHECK(strlen(dst) == 30, "16-char Cyrillic name cut at 15 chars (30 B), not 31 B");
        CHECK((unsigned char)dst[29] >= 0x80 && (unsigned char)dst[28] >= 0x80,
              "last stored char is a complete 2-byte sequence");

        // Plain ASCII: behaves like strncpy (31 B + NUL).
        utf8_safe_copy(dst, sizeof(dst), "abcdefghijklmnopqrstuvwxyz0123456789");
        CHECK(strlen(dst) == 31 && dst[30] == '4', "ASCII truncates at 31 bytes");

        // Exact fit passes untouched.
        utf8_safe_copy(dst, sizeof(dst), "Kitchen");
        CHECK(strcmp(dst, "Kitchen") == 0, "short name copied verbatim");

        // 4-byte emoji straddling the boundary is dropped whole.
        char small[6];
        utf8_safe_copy(small, sizeof(small), "ab\xF0\x9F\x98\x80");   // 'a','b' + 4-byte emoji
        CHECK(strcmp(small, "ab") == 0, "straddling 4-byte char dropped whole");

        // Degenerate caps.
        utf8_safe_copy(small, 1, "abc");
        CHECK(small[0] == '\0', "cap=1 yields empty string");
    }

    // ── E: a poisoned NVS blob (pre-fix stored name) heals at serialize time ─
    // The user's existing group was stored with a dangling lead byte BEFORE the
    // store-time fix existed. grp_to_json must still emit a valid-UTF-8 frame —
    // no reflash-time NVS migration required.
    {
        printf("\nE grp_to_json heals a poisoned stored name\n");
        GrpRecord g = make_group("", 1);
        // Simulate the pre-fix strncpy result: 14 full Cyrillic chars (28 B) +
        // a dangling lead byte — the shape a byte-blind truncation stores.
        memcpy(g.name, "\xD0\x94\xD0\xB0\xD1\x82\xD1\x87\xD0\xB8\xD0\xBA\xD0\xB8"
                       "\xD0\xB3\xD0\xBE\xD1\x81\xD1\x82\xD0\xB8\xD0\xBD\xD0\xBE"
                       "\xD0", 29);
        g.name[29] = '\0';
        char buf[512];
        size_t n = grp_to_json(g, buf, sizeof(buf));
        CHECK(n > 0, "poisoned-name group still serializes");
        buf[n] = '\0';
        CHECK(has(buf, "\\ufffd"), "dangling byte emitted as \\ufffd escape");
        bool clean = true;
        for (size_t i = 0; i + 1 < n; i++) {
            unsigned char c = (unsigned char)buf[i];
            if (c < 0x80) continue;
            // any high byte must open a complete valid sequence; cheap check:
            // a lead byte 0xD0-0xD1 followed by 0x80-0xBF is the only form we
            // planted — anything else high is a leak of the dangling byte.
            unsigned char c2 = (unsigned char)buf[i + 1];
            if (!((c == 0xD0 || c == 0xD1) && c2 >= 0x80 && c2 <= 0xBF)) { clean = false; break; }
            i++;   // skip continuation
        }
        CHECK(clean, "serialized JSON contains no invalid UTF-8 bytes");
    }

    printf("\n%s — %d failure(s)\n", s_fail ? "FAILED" : "PASSED", s_fail);
    return s_fail ? 1 : 0;
}
