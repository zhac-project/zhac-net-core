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
// sender has written ITS header (or 200 ms pass). Unserialised, the second does so at once
// and its whole frame lands inside the first: deterministic corruption. Serialised, the
// second blocks on the lock and the first simply times out. On failure the writer logs a
// W-level line from inside the send, as IDF does (httpd_ws.c "Failed to send WS header").
//
// Second part: a failed send must CLOSE the session (httpd_sess_trigger_close), not only drop
// the fd from the broadcast table. Otherwise the socket stays open, the browser's next frame
// re-adopts it with authed=0 and every command answers "auth required" until the page reloads.
// A second failed send for an fd that is already gone closes nothing: IDF queues a close by session
// pointer, so a duplicate could land on a slot a new connection has since taken.
//
// Third part: no log call while s_mutex (the fd table lock) is held. The wired WS log sink calls
// ws_server_broadcast from inside the log call (zhac-wired-core main/log_ring.cpp, dispatch_to_sinks),
// and broadcast takes s_mutex: a log line emitted under s_mutex would take it twice. The shim mutex is
// error-checking, so that is counted (g_self_deadlocks) instead of hanging the test.
#include "ws_server.h"
#include "esp_log.h"            // host_log(), defined below
#include "freertos/semphr.h"   // g_take_fails, g_self_deadlocks hooks

#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, msg) do {                                      \
    if (cond) { printf("PASS: %s\n", msg); }                       \
    else      { printf("FAIL: %s\n", msg); g_failures++; }         \
} while (0)

static std::atomic<int>  g_entered{0};     // senders that reached the writer (the first one parks)
static std::atomic<int>  g_headers{0};     // headers written so far
static std::atomic<bool> g_parked{false};  // the first sender sits between header and payload
static std::atomic<bool> g_gate{false};    // while set, the first sender stays parked: the test holds the TX lock
static std::atomic<int>  g_unmarked{0};    // sends entered without the in-broadcast marker
static int               g_fail_fd = -1;   // sends to this fd fail (a dead or stalled client)

static void nap() { std::this_thread::sleep_for(std::chrono::milliseconds(1)); }

esp_err_t httpd_ws_send_frame_async(httpd_handle_t, int fd, httpd_ws_frame_t* f) {
    // A log line emitted from in here must see the marker, or it re-enters the log sink.
    if (!ws_server_in_broadcast()) g_unmarked++;
    if (fd == g_fail_fd) {                                        // nothing written
        host_log("httpd_ws", "Failed to send WS header");
        return ESP_FAIL;
    }
    const int mine = ++g_entered;
    const bool big = f->len > 125;
    const uint8_t h[4] = {0x81, big ? uint8_t(126) : uint8_t(f->len), uint8_t(f->len >> 8), uint8_t(f->len)};
    send(fd, h, big ? 4 : 2, MSG_NOSIGNAL);                       // header ...
    ++g_headers;                            // counted AFTER the send: a second header on the wire is certain
    if (mine == 1) {
        g_parked = true;
        if (g_gate) { for (int i = 0; i < 5000 && g_gate; i++) nap(); }          // held until the test lets go
        else        { for (int i = 0; i < 200 && g_headers < 2; i++) nap(); }
    }
    send(fd, f->payload, f->len, MSG_NOSIGNAL);                   // ... then payload
    return ESP_OK;
}

// Session closes requested by ws_server, with what was true at the moment of the call.
struct Close { int fd; int clients; bool warned; bool locked; };
static std::vector<Close> g_closes;
static std::mutex g_closes_mu;
static std::atomic<bool> g_warned{false};   // the "send failed" warning has been logged
static std::atomic<bool> g_sink_on{false};  // host_log() behaves like the wired WS log sink

static const std::string kLog = R"({"type":"log","level":"I","entry":"x"})";

void host_log(const char*, const char* fmt, ...) {
    if (strstr(fmt, "send failed")) g_warned = true;
    // The wired sink (log_ring.cpp dispatch_to_sinks) broadcasts every log line from inside the log call.
    if (g_sink_on) ws_server_broadcast(kLog.data(), kLog.size());
}

esp_err_t httpd_sess_trigger_close(httpd_handle_t, int fd) {
    // `locked` = still inside the TX lock (the marker is set exactly while it is held).
    const Close c{fd, ws_server_client_count(), g_warned, ws_server_in_broadcast()};
    std::lock_guard<std::mutex> l(g_closes_mu);
    g_closes.push_back(c);
    return ESP_OK;
}

static const std::string kReply = R"({"id":41,"ok":true})";                               // 2-byte header
static const std::string kEvent = R"({"event":"attr.changed","data":")" + std::string(120, 'x') + "\"}";  // 4-byte header
static void do_reply(int fd)      { ws_server_reply(fd, kReply.data(), kReply.size()); }
static void do_broadcast(int)     { ws_server_broadcast(kEvent.data(), kEvent.size()); }

static void add_client(int fd) {            // the handshake: registers fd as a client
    httpd_req_t req{HTTP_GET, reinterpret_cast<void*>(static_cast<intptr_t>(fd))};
    g_ws_uri->handler(&req);
}

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
    g_headers = 0;
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

// A broadcaster snapshots the fd list, then waits for the TX lock behind a reply that the test holds mid-frame.
// While it waits, httpd ends A's session and lwIP hands A's number to a NEW connection. Once the broadcaster has the
// lock it must send that connection nothing: it is either not a client at all (an HTTP fetch: the frame would land
// inside its response) or a client that has not signed in yet (it would get what the auth gate withholds).
static void stale_snapshot_case(const char* name, bool new_conn_is_ws_client, int sv_server, int sv_peer) {
    int a[2], n[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, a);
    socketpair(AF_UNIX, SOCK_STREAM, 0, n);
    add_client(a[0]);                       // sign-in is on: registered, not signed in ...
    ws_server_fd_set_authed(a[0]);          // ... until it signs in
    const int a_fd = a[0];

    g_entered = 0;
    g_headers = 0;
    g_parked  = false;
    g_gate    = true;
    std::thread holder(do_reply, sv_server);                // a reply parked mid-frame, holding the TX lock
    for (int i = 0; i < 1000 && !g_parked; i++) nap();
    const int seen = g_fd_info_calls;
    std::thread b(do_broadcast, 0);                         // snapshots {sv, A}, then waits for the lock
    for (int i = 0; i < 2000 && g_fd_info_calls < seen + 2; i++) nap();

    g_close_fn(nullptr, a_fd);                              // httpd ends A's session: close_fn removes it, closes it
    dup2(n[0], a_fd);                                       // lwIP gives the same number to the next connection
    close(n[0]);
    if (new_conn_is_ws_client) add_client(a_fd);            // a WS client that has not signed in yet

    g_gate = false;
    holder.join();
    b.join();
    std::vector<std::string> f;
    CHECK(read_frames(n[1], f) && f.empty(), name);
    std::string what = std::string(name) + " (the signed-in client still gets reply and event, nothing is closed)";
    CHECK(read_frames(sv_peer, f) && f.size() == 2 && f[0] == kReply && f[1] == kEvent && g_closes.empty(), what.c_str());

    if (new_conn_is_ws_client) g_close_fn(nullptr, a_fd); else close(a_fd);
    close(a[1]);
    close(n[1]);
}

int main() {
    ws_server_init();                       // stub httpd captures the /ws handler
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    add_client(sv[0]);
    CHECK(ws_server_client_count() == 1, "client registered");

    scenario("reply parked mid-frame, broadcast arrives: frames intact", do_reply, do_broadcast, sv[0], sv[1]);
    scenario("broadcast parked mid-frame, reply arrives: frames intact", do_broadcast, do_reply, sv[0], sv[1]);
    CHECK(g_unmarked == 0, "every send runs with the in-broadcast marker set");
    CHECK(g_closes.empty(), "healthy sends close no session");

    // The lock cannot be taken: nothing goes out unlocked, and the healthy client is neither evicted nor closed.
    g_entered = 2;
    g_take_fails = 1;
    do_reply(sv[0]);
    g_take_fails = 0;
    std::vector<std::string> none;
    CHECK(read_frames(sv[1], none) && none.empty(), "lock not taken: reply dropped, not sent unlocked");
    CHECK(ws_server_client_count() == 1, "lock not taken: client stays registered");
    CHECK(g_closes.empty(), "lock not taken: the healthy session is not closed");

    // A failed send closes that session, and only that one: the fd leaves the table first, the close is
    // requested outside the TX lock, and the warning is logged last.
    int c2[2], c3[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, c2);
    socketpair(AF_UNIX, SOCK_STREAM, 0, c3);
    add_client(c2[0]);
    add_client(c3[0]);
    CHECK(ws_server_client_count() == 3, "three clients registered");

    g_entered = 2;                          // no parking: these are not race scenarios
    g_fail_fd = c2[0];
    do_broadcast(0);
    g_fail_fd = -1;
    CHECK(g_closes.size() == 1 && g_closes[0].fd == c2[0], "broadcast failure: only the failing fd's session is closed");
    CHECK(g_closes.size() == 1 && g_closes[0].clients == 2 && !g_closes[0].warned && !g_closes[0].locked && g_warned,
          "broadcast failure: fd removed first, closed outside the TX lock, then logged");
    std::vector<std::string> f;
    CHECK(read_frames(sv[1], f) && f.size() == 1 && f[0] == kEvent && read_frames(c3[1], f) && f.size() == 1 && f[0] == kEvent,
          "broadcast failure: the other clients still got the frame");

    g_closes.clear();
    g_warned  = false;
    g_fail_fd = c3[0];
    do_reply(c3[0]);
    g_fail_fd = -1;
    CHECK(g_closes.size() == 1 && g_closes[0].fd == c3[0], "reply failure: that fd's session is closed");
    CHECK(g_closes.size() == 1 && g_closes[0].clients == 1 && !g_closes[0].warned && !g_closes[0].locked && g_warned,
          "reply failure: fd removed first, closed outside the TX lock, then logged");

    // The same fd failing twice closes its session once: only the call that actually removed the fd closes.
    int c4[2], c5[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, c4);
    socketpair(AF_UNIX, SOCK_STREAM, 0, c5);
    add_client(c4[0]);
    g_closes.clear();
    g_fail_fd = c4[0];
    do_broadcast(0);                        // evicts and closes
    do_reply(c4[0]);                        // back to back: the fd is already out of the table
    g_fail_fd = -1;
    CHECK(g_closes.size() == 1 && g_closes[0].fd == c4[0], "same fd failing twice (broadcast, then reply): closed once");

    // The same through two broadcasts: B2 took its fd snapshot while B1 was still sending, so it fails on the
    // evicted fd as well once it gets the TX lock.
    add_client(c5[0]);
    g_closes.clear();
    g_entered = 0;
    g_headers = 0;
    g_parked  = false;
    g_fail_fd = c5[0];
    std::thread b1(do_broadcast, 0);
    for (int i = 0; i < 1000 && !g_parked; i++) nap();     // B1 sits mid-frame on sv[0], holding the TX lock
    std::thread b2(do_broadcast, 0);                        // snapshots both fds, then waits for the lock
    b1.join();
    b2.join();
    g_fail_fd = -1;
    CHECK(g_closes.size() == 1 && g_closes[0].fd == c5[0], "stale fd snapshot (two broadcasts): closed once");

    // The wired WS log sink broadcasts from inside the log call. A log line emitted while s_mutex is held would
    // take it a second time (the shim counts that instead of hanging): add_fd and remove_fd log after unlocking.
    int c6[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, c6);
    read_frames(sv[1], f);                  // forget what the scenarios above sent
    g_sink_on = true;
    add_client(c6[0]);                      // add_fd logs "WS client added"
    CHECK(g_self_deadlocks.exchange(0) == 0, "add_fd logs outside s_mutex (log sink on)");
    // A failing send logs from INSIDE the writer, on the task that holds the TX lock (marker set). The sink's
    // broadcast must return at once there: without the re-entry guard it would take s_tx_mutex a second time.
    g_fail_fd = c4[0];                      // open, but no longer in the table: only the in-send line and the warning
    do_reply(c4[0]);
    g_fail_fd = -1;
    CHECK(g_self_deadlocks.exchange(0) == 0, "a log line from inside a failing send is dropped by the marker (log sink on)");
    g_fail_fd = c6[0];
    do_broadcast(0);                        // evicts c6: remove_fd logs "WS client removed", then the warning
    g_fail_fd = -1;
    CHECK(g_self_deadlocks.exchange(0) == 0, "remove_fd logs outside s_mutex (log sink on)");
    g_sink_on = false;
    int logs = 0, events = 0;
    const bool got = read_frames(sv[1], f);
    for (const auto& m : f) { logs += m == kLog; events += m == kEvent; }
    CHECK(got && logs >= 4 && events == 1, "the sink hook ran: log frames and the event reached the healthy client");

    // Sign-in on. sv[0] registered while it was off, so it stays signed in; everything new starts signed out.
    ws_server_set_api_token("0123456789abcdef0123456789abcdef");
    int u[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, u);
    add_client(u[0]);
    g_entered = 2;                          // no parking
    do_reply(u[0]);
    CHECK(read_frames(u[1], f) && f.size() == 1 && f[0] == kReply,
          "a reply still reaches a client that has not signed in (only broadcasts are re-checked)");
    do_broadcast(0);
    CHECK(read_frames(u[1], f) && f.empty(), "auth gate: a client that has not signed in gets no broadcast");
    g_close_fn(nullptr, u[0]);
    read_frames(sv[1], f);                  // forget what was sent above
    g_closes.clear();

    stale_snapshot_case("stale fd snapshot, number reused by a client that has not signed in: nothing sent to it",
                        true, sv[0], sv[1]);
    stale_snapshot_case("stale fd snapshot, number reused by a non-WebSocket connection: nothing sent to it",
                        false, sv[0], sv[1]);
    ws_server_set_api_token(nullptr);

    return g_failures ? 1 : 0;
}
