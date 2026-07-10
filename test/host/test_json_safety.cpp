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

    printf("\n%s — %d failure(s)\n", s_fail ? "FAILED" : "PASSED", s_fail);
    return s_fail ? 1 : 0;
}
