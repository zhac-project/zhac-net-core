// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Improv Wi-Fi serial framing (main/improv_proto.h). Packets are built the way
// the browser SDK (improv-wifi/sdk-serial-js writePacketToStream) builds them.
#include "improv_proto.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int s_fail = 0;
#define CHECK(c, m) do { if (!(c)) { s_fail++; printf("  FAIL: %s\n", m); } else printf("  ok:   %s\n", m); } while (0)

// IMPROV, version 1, type, length, data, checksum over all of it, newline.
static std::vector<uint8_t> client_packet(uint8_t type, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> p = {'I', 'M', 'P', 'R', 'O', 'V', 1, type, (uint8_t)data.size()};
    p.insert(p.end(), data.begin(), data.end());
    uint8_t sum = 0;
    for (uint8_t b : p) sum += b;
    p.push_back(sum);
    p.push_back('\n');
    return p;
}

// Feeds bytes; returns how many complete packets the parser reported.
static int feed_all(improv::Parser& p, const std::vector<uint8_t>& bytes, bool* saw_bad = nullptr) {
    int got = 0;
    for (uint8_t b : bytes) {
        if (p.feed(b)) got++;
        if (p.bad_checksum && saw_bad) *saw_bad = true;
    }
    return got;
}

int main() {
    // The spec's example: SSID MyWirelessAP, password mysecurepassword.
    std::vector<uint8_t> rpc = {0x01, 0x1E, 0x0C};
    for (char c : std::string("MyWirelessAP")) rpc.push_back((uint8_t)c);
    rpc.push_back(0x10);
    for (char c : std::string("mysecurepassword")) rpc.push_back((uint8_t)c);

    {
        // Boot-log noise, a false start, and a truncated header directly
        // before the real packet (its 'I' must restart the match).
        std::string noise = "I (512) wifi_mgr: IMPR started\nIMPROX\r\nIMPR";
        std::vector<uint8_t> in(noise.begin(), noise.end());
        const auto pkt = client_packet(improv::TYPE_RPC, rpc);
        in.insert(in.end(), pkt.begin(), pkt.end());

        improv::Parser p;
        int got = 0;
        uint8_t cmd = 0, alen = 0;
        const uint8_t* arg = nullptr;
        char ssid[33] = {}, pass[65] = {};
        bool ok = false;
        for (uint8_t b : in) {
            if (!p.feed(b)) continue;
            got++;
            arg = improv::rpc_arg(p.data(), p.data_len(), &cmd, &alen);
            ok = p.type() == improv::TYPE_RPC && arg && cmd == improv::CMD_WIFI_SETTINGS &&
                 improv::parse_wifi_settings(arg, alen, ssid, pass);
        }
        CHECK(got == 1, "one packet found after log noise and a truncated header");
        CHECK(ok && std::strcmp(ssid, "MyWirelessAP") == 0 && std::strcmp(pass, "mysecurepassword") == 0,
              "spec example decodes to its SSID and password");
    }
    {
        auto pkt = client_packet(improv::TYPE_RPC, {improv::CMD_GET_STATE, 0});
        pkt[pkt.size() - 2] ^= 0x55;   // corrupt the checksum
        improv::Parser p;
        bool bad = false;
        CHECK(feed_all(p, pkt, &bad) == 0 && bad, "corrupt checksum is reported, not delivered");
        CHECK(feed_all(p, client_packet(improv::TYPE_RPC, {improv::CMD_GET_STATE, 0})) == 1,
              "parser recovers for the next packet");
    }
    {
        uint8_t cmd = 0, alen = 0;
        const uint8_t bad_len[] = {improv::CMD_GET_INFO, 3};
        CHECK(improv::rpc_arg(bad_len, 2, &cmd, &alen) == nullptr, "RPC with a wrong length is rejected");
        char ssid[33], pass[65];
        const uint8_t short_pw[] = {1, 'a', 5, 'x'};
        CHECK(!improv::parse_wifi_settings(short_pw, sizeof(short_pw), ssid, pass),
              "password length past the end is rejected");
        uint8_t long_ssid[40] = {33};
        CHECK(!improv::parse_wifi_settings(long_ssid, sizeof(long_ssid), ssid, pass),
              "SSID over 32 bytes is rejected");
        const uint8_t open_net[] = {3, 'l', 'a', 'b', 0};
        CHECK(improv::parse_wifi_settings(open_net, sizeof(open_net), ssid, pass) &&
              std::strcmp(ssid, "lab") == 0 && pass[0] == '\0', "open network with empty password accepted");
    }
    {
        // Device-to-client: result body inside a frame parses back to the strings.
        const char* strs[] = {"http://192.168.1.20/"};
        uint8_t body[255];
        const size_t bn = improv::result(improv::CMD_WIFI_SETTINGS, strs, 1, body);
        uint8_t wire[255 + 12];
        const size_t wn = improv::frame(improv::TYPE_RESULT, body, (uint8_t)bn, wire);
        CHECK(wire[0] == '\n' && std::memcmp(wire + 1, "IMPROV", 6) == 0 && wire[wn - 1] == '\n',
              "frame starts a line and ends one");
        improv::Parser p;
        int got = 0;
        for (size_t i = 0; i < wn; i++) got += p.feed(wire[i]);
        const uint8_t* d = p.data();
        CHECK(got == 1 && p.type() == improv::TYPE_RESULT && d[0] == improv::CMD_WIFI_SETTINGS &&
              d[1] == 1 + 20 && d[2] == 20 && std::memcmp(d + 3, "http://192.168.1.20/", 20) == 0,
              "URL result round-trips through frame and parser");

        std::string big(200, 'x');
        const char* two[] = {big.c_str(), big.c_str()};
        CHECK(improv::result(improv::CMD_GET_INFO, two, 2, body) == 0, "oversized result is refused");
        CHECK(improv::result(improv::CMD_SCAN, nullptr, 0, body) == 2 && body[1] == 0,
              "empty result ends a scan list");
    }

    printf("\n%s - %d failure(s)\n", s_fail ? "FAILED" : "PASSED", s_fail);
    return s_fail ? 1 : 0;
}
