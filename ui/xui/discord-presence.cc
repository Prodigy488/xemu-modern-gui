//
// xemu User Interface - Discord Rich Presence
//
// Discord's local IPC: frames of [uint32 opcode][uint32 length][JSON], little
// endian. Opcode 0 = handshake, 1 = frame, 2 = close, 3 = ping, 4 = pong.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "discord-presence.hh"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

using json = nlohmann::json;

bool DiscordActivity::operator==(const DiscordActivity &o) const
{
    return details == o.details && state == o.state &&
           large_image == o.large_image && large_text == o.large_text &&
           small_image == o.small_image && small_text == o.small_text &&
           start == o.start;
}

namespace {

enum Op : uint32_t { kHandshake = 0, kFrame = 1, kClose = 2, kPing = 3, kPong = 4 };

int64_t Pid()
{
#ifdef _WIN32
    return (int64_t)GetCurrentProcessId();
#else
    return (int64_t)getpid();
#endif
}

// Discord wants 2-128 characters per text field
std::string Field(std::string s)
{
    if (s.size() > 128) {
        size_t cut = 125;
        while (cut > 0 && ((unsigned char)s[cut] & 0xC0) == 0x80) {
            cut--; // don't split a UTF-8 sequence
        }
        s = s.substr(0, cut) + "...";
    }
    if (!s.empty() && s.size() < 2) {
        s += " ";
    }
    return s;
}

class Connection {
public:
    ~Connection() { Close(); }

    bool Open(const std::string &test_path)
    {
        Close();
#ifdef _WIN32
        (void)test_path;
        for (int i = 0; i < 10; i++) {
            wchar_t name[64];
            swprintf(name, 64, L"\\\\.\\pipe\\discord-ipc-%d", i);
            HANDLE h = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL,
                                   OPEN_EXISTING, 0, NULL);
            if (h != INVALID_HANDLE_VALUE) {
                m_pipe = h;
                return true;
            }
        }
        return false;
#else
        std::vector<std::string> paths;
        if (!test_path.empty()) {
            paths.push_back(test_path);
        } else {
            std::string dir = "/tmp";
            for (const char *var : { "XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP" }) {
                const char *v = getenv(var);
                if (v && v[0]) {
                    dir = v;
                    break;
                }
            }
            for (int i = 0; i < 10; i++) {
                paths.push_back(dir + "/discord-ipc-" + std::to_string(i));
            }
        }
        for (auto &p : paths) {
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) {
                return false;
            }
            sockaddr_un addr = {};
            addr.sun_family = AF_UNIX;
            if (p.size() >= sizeof(addr.sun_path)) {
                close(fd);
                continue;
            }
            memcpy(addr.sun_path, p.c_str(), p.size() + 1);
            if (connect(fd, (sockaddr *)&addr, sizeof(addr)) == 0) {
                m_fd = fd;
                return true;
            }
            close(fd);
        }
        return false;
#endif
    }

    void Close()
    {
#ifdef _WIN32
        if (m_pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(m_pipe);
            m_pipe = INVALID_HANDLE_VALUE;
        }
#else
        if (m_fd >= 0) {
            close(m_fd);
            m_fd = -1;
        }
#endif
    }

    bool IsOpen() const
    {
#ifdef _WIN32
        return m_pipe != INVALID_HANDLE_VALUE;
#else
        return m_fd >= 0;
#endif
    }

    bool Send(uint32_t op, const std::string &payload)
    {
        std::string frame(8, '\0');
        uint32_t len = (uint32_t)payload.size();
        for (int i = 0; i < 4; i++) {
            frame[i] = (char)((op >> (8 * i)) & 0xFF);
            frame[4 + i] = (char)((len >> (8 * i)) & 0xFF);
        }
        frame += payload;
        return WriteAll(frame.data(), frame.size());
    }

    // Read one frame, waiting up to timeout_ms for it to start arriving.
    // Returns false on timeout (got_none = true) or on error.
    bool Receive(int timeout_ms, uint32_t &op, std::string &payload, bool &got_none)
    {
        got_none = false;
        uint8_t hdr[8];
        size_t n = 0;
        if (!ReadSome(hdr, 8, timeout_ms, n)) {
            return false;
        }
        if (n == 0) {
            got_none = true;
            return false;
        }
        if (n < 8 && !ReadExact(hdr + n, 8 - n)) {
            return false;
        }
        op = hdr[0] | hdr[1] << 8 | hdr[2] << 16 | (uint32_t)hdr[3] << 24;
        uint32_t len = hdr[4] | hdr[5] << 8 | hdr[6] << 16 | (uint32_t)hdr[7] << 24;
        if (len > 64 * 1024) {
            return false;
        }
        payload.assign(len, '\0');
        return len == 0 || ReadExact(&payload[0], len);
    }

private:
#ifdef _WIN32
    HANDLE m_pipe = INVALID_HANDLE_VALUE;
#else
    int m_fd = -1;
#endif

    bool WriteAll(const char *p, size_t len)
    {
        while (len) {
#ifdef _WIN32
            DWORD wrote = 0;
            if (!WriteFile(m_pipe, p, (DWORD)len, &wrote, NULL) || wrote == 0) {
                return false;
            }
#else
            ssize_t wrote = send(m_fd, p, len, MSG_NOSIGNAL);
            if (wrote <= 0) {
                return false;
            }
#endif
            p += wrote;
            len -= (size_t)wrote;
        }
        return true;
    }

    // Read whatever is available (up to len) within timeout_ms; n = 0 means
    // nothing arrived. False on a closed or broken connection.
    bool ReadSome(void *buf, size_t len, int timeout_ms, size_t &n)
    {
        n = 0;
#ifdef _WIN32
        auto until = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(timeout_ms);
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(m_pipe, NULL, 0, NULL, &avail, NULL)) {
                return false;
            }
            if (avail > 0) {
                DWORD got = 0;
                if (!ReadFile(m_pipe, buf, (DWORD)std::min<size_t>(len, avail), &got,
                              NULL) ||
                    got == 0) {
                    return false;
                }
                n = got;
                return true;
            }
            if (std::chrono::steady_clock::now() >= until) {
                return true;
            }
            Sleep(10);
        }
#else
        pollfd pfd = { m_fd, POLLIN, 0 };
        int r = poll(&pfd, 1, timeout_ms);
        if (r < 0) {
            return false;
        }
        if (r == 0) {
            return true;
        }
        ssize_t got = recv(m_fd, buf, len, 0);
        if (got <= 0) {
            return false;
        }
        n = (size_t)got;
        return true;
#endif
    }

    bool ReadExact(void *buf, size_t len)
    {
        char *p = (char *)buf;
        while (len) {
            size_t n = 0;
            if (!ReadSome(p, len, 2000, n) || n == 0) {
                return false;
            }
            p += n;
            len -= n;
        }
        return true;
    }
};

std::mutex g_mutex;
std::condition_variable g_cv;
std::thread g_thread;
bool g_running = false;           // under g_mutex
DiscordActivity g_wanted;         // under g_mutex
uint64_t g_wanted_version = 0;    // under g_mutex
std::string g_client_id;
std::string g_test_path;
std::atomic<bool> g_connected{ false };

std::string HandshakeJson(const std::string &client_id)
{
    return json({ { "v", 1 }, { "client_id", client_id } }).dump();
}

void Worker()
{
    Connection conn;
    uint64_t sent_version = 0;
    bool sent_any = false;
    uint64_t nonce = 0;
    auto last_send = std::chrono::steady_clock::now() - std::chrono::seconds(60);
    auto next_try = std::chrono::steady_clock::now();

    std::unique_lock<std::mutex> lock(g_mutex);
    while (g_running) {
        DiscordActivity wanted = g_wanted;
        uint64_t version = g_wanted_version;
        lock.unlock();

        auto now = std::chrono::steady_clock::now();
        if (!conn.IsOpen() && now >= next_try) {
            bool ok = false;
            if (conn.Open(g_test_path) && conn.Send(kHandshake, HandshakeJson(g_client_id))) {
                uint32_t op = 0;
                std::string body;
                bool none = false;
                if (conn.Receive(3000, op, body, none) && op == kFrame &&
                    body.find("\"READY\"") != std::string::npos) {
                    ok = true;
                }
            }
            if (!ok) {
                conn.Close();
                next_try = now + std::chrono::seconds(15); // Discord not running
            } else {
                sent_any = false; // new session: send the current activity
            }
            g_connected = ok;
        }

        if (conn.IsOpen()) {
            // Discord allows a handful of updates per 20 s; space them out
            bool due = !sent_any || version != sent_version;
            if (due && now - last_send >= std::chrono::seconds(4)) {
                std::string payload =
                    DiscordActivityJson(wanted, Pid(), std::to_string(++nonce));
                if (conn.Send(kFrame, payload)) {
                    sent_version = version;
                    sent_any = true;
                    last_send = now;
                } else {
                    conn.Close();
                    g_connected = false;
                    next_try = now + std::chrono::seconds(5);
                }
            }
            // Answer pings and notice when Discord closes the connection
            for (int i = 0; i < 8 && conn.IsOpen(); i++) {
                uint32_t op = 0;
                std::string body;
                bool none = false;
                if (!conn.Receive(0, op, body, none)) {
                    if (!none) {
                        conn.Close();
                        g_connected = false;
                        next_try = now + std::chrono::seconds(5);
                    }
                    break;
                }
                if (op == kPing) {
                    conn.Send(kPong, body);
                } else if (op == kClose) {
                    conn.Close();
                    g_connected = false;
                    next_try = now + std::chrono::seconds(15);
                }
            }
        }

        lock.lock();
        g_cv.wait_for(lock, std::chrono::milliseconds(500), [&] {
            return !g_running || g_wanted_version != version;
        });
    }
    lock.unlock();
    if (conn.IsOpen()) {
        conn.Send(kFrame, DiscordActivityJson(DiscordActivity(), Pid(), "clear"));
        conn.Close();
    }
    g_connected = false;
}

} // namespace

std::string DiscordActivityJson(const DiscordActivity &a, int64_t pid,
                                const std::string &nonce)
{
    json activity = nullptr; // null clears the status
    if (!a.details.empty()) {
        activity = json::object();
        activity["type"] = 0; // "Playing"
        activity["details"] = Field(a.details);
        if (!a.state.empty()) {
            activity["state"] = Field(a.state);
        }
        if (a.start > 0) {
            activity["timestamps"] = { { "start", a.start } };
        }
        json assets = json::object();
        if (!a.large_image.empty()) {
            assets["large_image"] = a.large_image;
            if (!a.large_text.empty()) {
                assets["large_text"] = Field(a.large_text);
            }
        }
        if (!a.small_image.empty()) {
            assets["small_image"] = a.small_image;
            if (!a.small_text.empty()) {
                assets["small_text"] = Field(a.small_text);
            }
        }
        if (!assets.empty()) {
            activity["assets"] = assets;
        }
    }
    json msg = { { "cmd", "SET_ACTIVITY" },
                 { "args", { { "pid", pid }, { "activity", activity } } },
                 { "nonce", nonce } };
    return msg.dump();
}

void DiscordPresenceStart(const std::string &client_id)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_running) {
        return;
    }
    g_client_id = client_id;
    g_running = true;
    g_thread = std::thread(Worker);
}

void DiscordPresenceSet(const DiscordActivity &activity)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (activity != g_wanted) {
        g_wanted = activity;
        g_wanted_version++;
        g_cv.notify_all();
    }
}

void DiscordPresenceStop(void)
{
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_running) {
            return;
        }
        g_running = false;
        g_cv.notify_all();
    }
    if (g_thread.joinable()) {
        g_thread.join();
    }
}

void DiscordPresenceSetSocketPathForTest(const std::string &path)
{
    g_test_path = path;
}

bool DiscordPresenceConnected(void)
{
    return g_connected;
}
