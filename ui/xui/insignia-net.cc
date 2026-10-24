//
// xemu User Interface - Insignia network check
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "insignia-net.hh"
#include "i18n.hh"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define CLOSE_SOCK closesocket
#define BAD_SOCK INVALID_SOCKET
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
#define CLOSE_SOCK close
#define BAD_SOCK (-1)
#endif

#include "../xemu-net.h"

namespace {

bool SkipName(const std::vector<uint8_t> &m, size_t *p)
{
    for (int guard = 0; guard < 128; guard++) {
        if (*p >= m.size()) {
            return false;
        }
        uint8_t len = m[*p];
        if (len == 0) {
            (*p)++;
            return true;
        }
        if ((len & 0xC0) == 0xC0) { // pointer: two bytes, then done
            *p += 2;
            return *p <= m.size();
        }
        *p += 1 + len;
    }
    return false;
}

uint16_t Get16(const std::vector<uint8_t> &m, size_t p)
{
    return (uint16_t)((m[p] << 8) | m[p + 1]);
}

} // namespace

DnsQueryResult DnsQueryA(const char *server, int port, const char *name,
                         int timeout_ms, std::string *ip, std::string *err)
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa); // counted; QEMU has usually done it
#endif
    // The query: one question, recursion desired
    std::random_device rd;
    uint16_t id = (uint16_t)rd();
    std::vector<uint8_t> q = { (uint8_t)(id >> 8), (uint8_t)id, 0x01, 0x00,
                               0, 1, 0, 0, 0, 0, 0, 0 };
    for (const char *p = name; *p;) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len == 0 || len > 63) {
            *err = "bad name";
            return DnsQueryResult::Error;
        }
        q.push_back((uint8_t)len);
        q.insert(q.end(), p, p + len);
        p += len + (dot ? 1 : 0);
    }
    q.insert(q.end(), { 0, 0, 1, 0, 1 }); // end, type A, class IN

    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == BAD_SOCK) {
        *err = "socket";
        return DnsQueryResult::Error;
    }
#ifdef _WIN32
    DWORD tv = (DWORD)timeout_ms;
#else
    struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof(tv));
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, server, &to.sin_addr) != 1) {
        CLOSE_SOCK(s);
        *err = "bad server address";
        return DnsQueryResult::Error;
    }
    DnsQueryResult result = DnsQueryResult::NoReply;
    // Two tries, as UDP can drop one
    for (int attempt = 0; attempt < 2 && result == DnsQueryResult::NoReply;
         attempt++) {
        if (sendto(s, (const char *)q.data(), (int)q.size(), 0,
                   (const sockaddr *)&to, sizeof(to)) < 0) {
            *err = "send failed";
            result = DnsQueryResult::Error;
            break;
        }
        for (;;) {
            std::vector<uint8_t> m(1500);
            sockaddr_in from = {};
            socklen_t fl = sizeof(from);
            int n = (int)recvfrom(s, (char *)m.data(), (int)m.size(), 0,
                                  (sockaddr *)&from, &fl);
            if (n < 12) {
                break; // timed out (or junk): try again or give up
            }
            m.resize(n);
            if (Get16(m, 0) != id || !(m[2] & 0x80)) {
                continue; // not our answer
            }
            int rcode = m[3] & 0x0F;
            int qd = Get16(m, 4), an = Get16(m, 6);
            size_t p = 12;
            bool ok = true;
            for (int i = 0; i < qd && ok; i++) {
                ok = SkipName(m, &p) && (p += 4) <= m.size();
            }
            result = DnsQueryResult::NoAnswer;
            *err = rcode == 3 ? "no such name" : rcode ? "server error" : "no address";
            for (int i = 0; i < an && ok; i++) {
                ok = SkipName(m, &p) && p + 10 <= m.size();
                if (!ok) {
                    break;
                }
                uint16_t type = Get16(m, p), rdlen = Get16(m, p + 8);
                p += 10;
                if (p + rdlen > m.size()) {
                    break;
                }
                if (type == 1 && rdlen == 4) {
                    char b[32];
                    snprintf(b, sizeof(b), "%u.%u.%u.%u", m[p], m[p + 1],
                             m[p + 2], m[p + 3]);
                    *ip = b;
                    result = DnsQueryResult::Answer;
                    break;
                }
                p += rdlen;
            }
            break;
        }
    }
    if (result == DnsQueryResult::NoReply) {
        *err = "no reply";
    }
    CLOSE_SOCK(s);
    return result;
}

namespace {
std::mutex g_mutex;
std::atomic<int> g_state{ 0 };
std::string g_msg;
} // namespace

void InsigniaDnsTestStart()
{
    if (g_state == 1) {
        return;
    }
    g_state = 1;
    // Detached: it ends by itself within a few seconds
    std::thread([] {
        std::string ip, err;
        DnsQueryResult r = DnsQueryA(XEMU_INSIGNIA_DNS, 53, INSIGNIA_TEST_NAME,
                                     2500, &ip, &err);
        std::string msg;
        if (r == DnsQueryResult::Answer) {
            char b[300];
            snprintf(b, sizeof(b),
                     tr("Insignia's DNS answered from this PC (Xbox Live is at "
                        "%s)."),
                     ip.c_str());
            msg = b;
        } else if (r == DnsQueryResult::NoAnswer) {
            msg = tr("Something answered, but not with Insignia's addresses. Your "
                     "network or internet provider may be redirecting DNS. "
                     "Insignia's Connect guide explains what to do.");
        } else {
            msg = tr("No answer from Insignia's DNS. A firewall, VPN or your "
                     "internet provider may be blocking it.");
        }
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_msg = msg;
        }
        g_state = r == DnsQueryResult::Answer ? 2 : 3;
    }).detach();
}

int InsigniaDnsTestState(std::string *msg)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    if (msg) {
        *msg = g_msg;
    }
    return g_state;
}
