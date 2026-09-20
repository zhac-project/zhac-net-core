// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// improv_proto.h -- Improv Wi-Fi serial framing (https://www.improv-wifi.com/serial/).
// Pure: no ESP-IDF, host-tested in test/host/test_improv_proto.cpp. The UART
// task that uses it lives in wifi_mgr.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace improv {

enum : uint8_t { TYPE_STATE = 0x01, TYPE_ERROR = 0x02, TYPE_RPC = 0x03, TYPE_RESULT = 0x04 };
enum : uint8_t { STATE_READY = 0x02, STATE_PROVISIONING = 0x03, STATE_PROVISIONED = 0x04 };
enum : uint8_t { ERR_NONE = 0x00, ERR_INVALID_RPC = 0x01, ERR_UNKNOWN_RPC = 0x02,
                 ERR_UNABLE_TO_CONNECT = 0x03 };
enum : uint8_t { CMD_WIFI_SETTINGS = 0x01, CMD_GET_STATE = 0x02, CMD_GET_INFO = 0x03,
                 CMD_SCAN = 0x04 };

// Byte-at-a-time receiver. feed() returns true when a complete packet with a
// valid checksum is in buf; read it through type()/data()/data_len() before the
// next feed(). Anything that is not a packet is skipped. bad_checksum is set on
// the byte that completed a corrupt frame.
struct Parser {
    uint8_t buf[9 + 255 + 1];
    size_t  n = 0;
    bool    bad_checksum = false;

    bool feed(uint8_t b) {
        bad_checksum = false;
        if (n < 6) {
            if (b == static_cast<uint8_t>("IMPROV"[n])) { buf[n++] = b; return false; }
            n = 0;
            if (b == 'I') buf[n++] = b;          // a new header may start here
            return false;
        }
        buf[n++] = b;
        if (n == 7 && b != 1) { n = 0; return false; }   // protocol version 1 only
        if (n < 9) return false;
        const size_t total = 9 + static_cast<size_t>(buf[8]) + 1;
        if (n < total) return false;
        n = 0;
        uint8_t sum = 0;
        for (size_t i = 0; i + 1 < total; i++) sum += buf[i];
        if (sum != buf[total - 1]) { bad_checksum = true; return false; }
        return true;
    }
    uint8_t        type() const     { return buf[7]; }
    const uint8_t* data() const     { return buf + 9; }
    uint8_t        data_len() const { return buf[8]; }
};

// The RPC body is command, argument length, argument. Returns the argument or
// nullptr when the stated length disagrees with the packet.
inline const uint8_t* rpc_arg(const uint8_t* d, uint8_t n, uint8_t* cmd, uint8_t* arg_len) {
    if (n < 2 || d[1] != n - 2) return nullptr;
    *cmd = d[0];
    *arg_len = d[1];
    return d + 2;
}

// Send-Wi-Fi-settings argument: ssid length, ssid, password length, password.
// False when the lengths do not add up or exceed what Wi-Fi allows.
inline bool parse_wifi_settings(const uint8_t* p, size_t n, char (&ssid)[33], char (&pass)[65]) {
    if (n < 2) return false;
    const size_t sl = p[0];
    if (sl == 0 || sl > 32 || sl + 2 > n) return false;
    const size_t pl = p[1 + sl];
    if (pl > 64 || sl + 2 + pl != n) return false;
    std::memcpy(ssid, p + 1, sl);
    ssid[sl] = '\0';
    std::memcpy(pass, p + 2 + sl, pl);
    pass[pl] = '\0';
    return true;
}

// RPC result body: command, length, then each string as length + bytes.
// Returns 0 when the strings do not fit one packet.
inline size_t result(uint8_t cmd, const char* const* strs, size_t count, uint8_t (&out)[255]) {
    size_t n = 2;
    for (size_t i = 0; i < count; i++) {
        const size_t l = std::strlen(strs[i]);
        if (n + 1 + l > sizeof(out)) return 0;
        out[n++] = static_cast<uint8_t>(l);
        std::memcpy(out + n, strs[i], l);
        n += l;
    }
    out[0] = cmd;
    out[1] = static_cast<uint8_t>(n - 2);
    return n;
}

// One packet on the wire, wrapped in newlines. The leading one matters: the
// browser client only recognises a packet at the start of a line, so a packet
// written after half a log line would be ignored.
inline size_t frame(uint8_t type, const uint8_t* data, uint8_t len, uint8_t (&out)[255 + 12]) {
    size_t n = 0;
    out[n++] = '\n';
    std::memcpy(out + n, "IMPROV", 6);
    n += 6;
    out[n++] = 1;
    out[n++] = type;
    out[n++] = len;
    if (len) std::memcpy(out + n, data, len);
    n += len;
    uint8_t sum = 0;
    for (size_t i = 1; i < n; i++) sum += out[i];
    out[n++] = sum;
    out[n++] = '\n';
    return n;
}

}  // namespace improv
