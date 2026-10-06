// SPDX-FileCopyrightText: 2025-2026 Evgenij Cjura and project contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
//
// Host regression test: ws_server's two writers -- ws_server_reply (httpd task) and
// ws_server_broadcast (event bus, status tick, log sink) -- must take turns on a socket.
// IDF's httpd_ws_send_frame_async writes a frame as TWO send()s (header, then payload);
// when two tasks overlap, one frame's header lands inside the other's payload and the
// browser fails the connection (close 1006, "Could not decode a text frame as UTF-8").
// Builds the real ws_server.cpp against the shims in stubs/.
//
// The writer below is IDF's, plus a rendezvous that stands in for the blocking lwIP round
// trip between the two send()s: the FIRST sender parks after its header until a second
// sender enters the writer (or 200 ms pass). Unserialised, the second enters at once and
// its whole frame lands inside the first: deterministic corruption. Serialised, the second
// blocks on the lock and the first simply times out.
#include "ws_server.h"
#include "freertos/semphr.h"   // g_take_fails hook

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, msg) do {                                      \
    if (cond) { printf("PASS: %s\n", msg); }                       \
    else      { printf("FAIL: %s\n", msg); g_failures++; }         \
} while (0)

static std::atomic<int>  g_entered{0};     // senders that reached the writer
static std::atomic<bool> g_parked{false};  // the first sender sits between header and payload
static std::atomic<int>  g_unmarked{0};    // sends entered without the in-broadcast marker

static void nap() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }

esp_err_t httpd_ws_send_frame_async(httpd_handle_t, int fd, httpd_ws_frame_t* f) {
    // A log line emitted from in here must see the marker, or it re-enters the log sink.
    if (!ws_server_in_broadcast()) g_unmarked++;
    const int mine = ++g_entered;
    const bool big = f->len > 125;
    const uint8_t h[4] = {0x81, big ? uint8_t(126) : uint8_t(f->len), uint8_t(f->len >> 8), uint8_t(f->len)};
    send(fd, h, big ? 4 : 2, MSG_NOSIGNAL);                       // header ...
    if (mine == 1) {
        g_parked = true;
        for (int i = 0; i < 200 && g_entered < 2; i++) nap();
    }
    send(fd, f->payload, f->len, MSG_NOSIGNAL);                   // ... then payload
    return ESP_OK;
}

static const std::string kReply = R"({"id":41,"ok":true})";                               // 2-byte header
static const std::string kEvent = R"({"event":"attr.changed","data":")" + std::string(120, 'x') + "\"}";  // 4-byte header
static void do_reply(int fd)      { ws_server_reply(fd, kReply.data(), kReply.size()); }
static void do_broadcast(int)     { ws_server_broadcast(kEvent.data(), kEvent.size()); }

// Drain the peer and split it into frames; false unless it is a clean run of `81 len {...}` frames.
static bool read_frames(int peer, std::vector<std::string>& out) {
    std::string raw;
    char buf[512];
    for (ssize_t r; (r = recv(peer, buf, sizeof buf, MSG_DONTWAIT)) > 0;) raw.append(buf, size_t(r));
    out.clear();
    const auto* b = reinterpret_cast<const uint8_t*>(raw.data());
    for (size_t i = 0; i < raw.size();) {
        if (i + 2 > raw.size() || b[i] != 0x81) return false;
        size_t len = b[i + 1], h = 2;
        if (len == 126) { if (i + 4 > raw.size()) return false; len = size_t(b[i + 2]) << 8 | b[i + 3]; h = 4; }
        if (i + h + len > raw.size() || b[i + h] != '{') return false;
        out.emplace_back(raw, i + h, len);
        i += h + len;
    }
    return true;
}

// `first` is parked between its header and payload; `second` is then released against it.
static void scenario(const char* name, void (*first)(int), void (*second)(int), int fd, int peer) {
    g_entered = 0;
    g_parked  = false;
    std::thread a(first, fd);
    for (int i = 0; i < 1000 && !g_parked; i++) nap();
    std::thread b(second, fd);
    a.join();
    b.join();
    std::vector<std::string> f;
    const bool clean = read_frames(peer, f);
    CHECK(clean && f.size() == 2 &&
          ((f[0] == kReply && f[1] == kEvent) || (f[0] == kEvent && f[1] == kReply)), name);
}

int main() {
    ws_server_init();                       // stub httpd captures the /ws handler
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    httpd_req_t req{HTTP_GET, reinterpret_cast<void*>(static_cast<intptr_t>(sv[0]))};
    g_ws_uri->handler(&req);                // the handshake: registers sv[0] as a client
    CHECK(ws_server_client_count() == 1, "client registered");

    scenario("reply parked mid-frame, broadcast arrives: frames intact", do_reply, do_broadcast, sv[0], sv[1]);
    scenario("broadcast parked mid-frame, reply arrives: frames intact", do_broadcast, do_reply, sv[0], sv[1]);
    CHECK(g_unmarked == 0, "every send runs with the in-broadcast marker set");

    // The lock cannot be taken: nothing goes out unlocked, and the healthy client is not evicted.
    g_entered = 2;
    g_take_fails = 1;
    do_reply(sv[0]);
    g_take_fails = 0;
    std::vector<std::string> none;
    CHECK(read_frames(sv[1], none) && none.empty(), "lock not taken: reply dropped, not sent unlocked");
    CHECK(ws_server_client_count() == 1, "lock not taken: client stays registered");

    return g_failures ? 1 : 0;
}
