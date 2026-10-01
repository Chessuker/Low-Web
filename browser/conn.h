// conn.h — one TCP connection, optionally with TLS (Windows SChannel). Used by net.cpp
// (HTTP/1.1) and http2.cpp. Blocking; one thread may read while another writes.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <security.h>
#include <schannel.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "net.h"

namespace net {

// ---------------------------------------------------------------------------------
// Address spaces (net.h: Zone)
// ---------------------------------------------------------------------------------

inline Zone zone_of_v4(const uint8_t *a) {
    if (a[0] == 127 || a[0] == 0) return Zone::Local;  // loopback; 0.0.0.0 also reaches this computer
    if (a[0] == 10 || (a[0] == 172 && (a[1] & 0xF0) == 16) || (a[0] == 192 && a[1] == 168) ||
        (a[0] == 169 && a[1] == 254) || (a[0] == 100 && (a[1] & 0xC0) == 64) || (a[0] == 198 && (a[1] & 0xFE) == 18))
        return Zone::Private;  // RFC 1918, link-local, carrier-grade NAT, benchmarking
    return Zone::Public;
}

inline Zone zone_of(const sockaddr *sa) {
    if (sa->sa_family == AF_INET) return zone_of_v4((const uint8_t *)&((const sockaddr_in *)sa)->sin_addr);
    if (sa->sa_family != AF_INET6) return Zone::Local;
    const uint8_t *a = (const uint8_t *)&((const sockaddr_in6 *)sa)->sin6_addr;
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (std::memcmp(a, mapped, 12) == 0) return zone_of_v4(a + 12);  // ::ffff:a.b.c.d
    bool zero15 = true;
    for (int i = 0; i < 15; i++) zero15 &= a[i] == 0;
    if (zero15 && a[15] <= 1) return Zone::Local;                  // ::1 and ::
    if ((a[0] & 0xFE) == 0xFC || (a[0] == 0xFE && (a[1] & 0xC0) == 0x80)) return Zone::Private;  // fc00::/7, fe80::/10
    return Zone::Public;
}

inline const char *zone_name(Zone z) { return z == Zone::Local ? "this computer" : z == Zone::Private ? "the local network" : "the internet"; }

// ---------------------------------------------------------------------------------
// TLS over SChannel
// ---------------------------------------------------------------------------------

inline std::string sec_error(SECURITY_STATUS ss) {
    switch ((unsigned long)ss) {
    case (unsigned long)SEC_E_UNTRUSTED_ROOT: return "the server's certificate is not trusted";
    case (unsigned long)SEC_E_CERT_EXPIRED: return "the server's certificate has expired";
    case (unsigned long)SEC_E_WRONG_PRINCIPAL: return "the server's certificate is for a different name";
    case (unsigned long)SEC_E_ILLEGAL_MESSAGE: return "the server sent an invalid TLS message";
    case (unsigned long)SEC_E_ALGORITHM_MISMATCH: return "no TLS cipher in common with the server";
    default: {
        char buf[64];
        snprintf(buf, sizeof buf, "TLS error 0x%08lX", (unsigned long)ss);
        return buf;
    }
    }
}

struct Conn {
    SOCKET s = INVALID_SOCKET;
    Zone zone = Zone::Public;  // of the address it is connected to
    bool tls = false;
    CredHandle cred{};
    CtxtHandle ctx{};
    bool have_cred = false, have_ctx = false;
    SecPkgContext_StreamSizes sizes{};
    std::vector<char> enc;    // received, still encrypted
    std::vector<char> plain;  // decrypted, not yet consumed
    size_t plain_off = 0;

    ~Conn() {
        if (have_ctx) DeleteSecurityContext(&ctx);
        if (s != INVALID_SOCKET) closesocket(s);
    }

    // Connects to all resolved addresses at once and keeps the first that answers
    // (a simple form of "Happy Eyeballs", RFC 8305). This matters on Windows, where a
    // refused connection (e.g. "localhost" -> ::1 with an IPv4-only server) takes ~2 s.
    // Addresses more private than `lowest` are not tried. The check is on the addresses
    // themselves, so a name that resolves to 127.0.0.1 ("DNS rebinding") is caught too.
    bool connect_to(const std::string &host, int port, std::string &err, Zone lowest = Zone::Local) {
        addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) {
            err = "could not find the server \"" + host + "\"";
            return false;
        }
        std::vector<SOCKET> pending;
        std::vector<Zone> zones;  // of each pending socket
        SOCKET winner = INVALID_SOCKET;
        Zone winner_zone = Zone::Public, blocked = Zone::Public;
        bool any_blocked = false;
        for (addrinfo *a = res; a && pending.size() < 8 && winner == INVALID_SOCKET; a = a->ai_next) {
            Zone z = zone_of(a->ai_addr);
            if (z < lowest) {
                any_blocked = true;
                blocked = z;
                continue;
            }
            SOCKET t = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            if (t == INVALID_SOCKET) continue;
            u_long nb = 1;
            ioctlsocket(t, FIONBIO, &nb);
            if (connect(t, a->ai_addr, (int)a->ai_addrlen) == 0) {
                winner = t;
                winner_zone = z;
            } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
                pending.push_back(t);
                zones.push_back(z);
            } else {
                closesocket(t);
            }
        }
        freeaddrinfo(res);
        if (winner == INVALID_SOCKET && pending.empty() && any_blocked) {
            err = std::string("blocked: a page from ") + zone_name(lowest) + " may not reach " + host + " (" + zone_name(blocked) + ")";
            return false;
        }
        bool refused = false;
        ULONGLONG deadline = GetTickCount64() + 10000;
        while (winner == INVALID_SOCKET && !pending.empty()) {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline) break;
            fd_set wr, ex;
            FD_ZERO(&wr);
            FD_ZERO(&ex);
            for (SOCKET t : pending) { FD_SET(t, &wr); FD_SET(t, &ex); }
            long left = (long)(deadline - now);
            timeval tv{left / 1000, (left % 1000) * 1000};
            if (select(0, nullptr, &wr, &ex, &tv) <= 0) break;
            for (size_t i = 0; i < pending.size();) {
                SOCKET t = pending[i];
                int soerr = 0, len = sizeof soerr;
                bool done = FD_ISSET(t, &wr) || FD_ISSET(t, &ex);
                if (done) getsockopt(t, SOL_SOCKET, SO_ERROR, (char *)&soerr, &len);
                if (done && soerr == 0 && FD_ISSET(t, &wr) && winner == INVALID_SOCKET) {
                    winner = t;
                    winner_zone = zones[i];
                    pending.erase(pending.begin() + i);
                    zones.erase(zones.begin() + i);
                } else if (done) {
                    refused |= soerr == WSAECONNREFUSED;
                    closesocket(t);
                    pending.erase(pending.begin() + i);
                    zones.erase(zones.begin() + i);
                } else {
                    i++;
                }
            }
        }
        for (SOCKET t : pending) closesocket(t);
        if (winner == INVALID_SOCKET) {
            err = "could not connect to " + host + ":" + std::to_string(port) + (refused ? " (connection refused)" : " (timed out)");
            return false;
        }
        u_long nb = 0;
        ioctlsocket(winner, FIONBIO, &nb);
        DWORD tmo = 20000;
        setsockopt(winner, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tmo, sizeof tmo);
        setsockopt(winner, SOL_SOCKET, SO_SNDTIMEO, (const char *)&tmo, sizeof tmo);
        int one = 1;
        setsockopt(winner, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
        s = winner;
        zone = winner_zone;
        return true;
    }

    bool raw_send(const char *p, size_t n) {
        while (n) {
            int k = ::send(s, p, (int)std::min<size_t>(n, 1 << 20), 0);
            if (k <= 0) return false;
            p += k;
            n -= (size_t)k;
        }
        return true;
    }

    int raw_recv(char *p, int cap) { return ::recv(s, p, cap, 0); }

    // For a kept-alive connection about to be used again: still open, nothing unread.
    bool idle_ok() {
        if (plain_off < plain.size() || !enc.empty()) return false;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s, &rd);
        timeval tv{0, 0};
        return select(0, &rd, nullptr, nullptr, &tv) == 0;  // readable now = closed (or stray data)
    }

    // One credentials handle for the whole process: SChannel keeps its TLS session cache per
    // handle, so sharing it lets a new connection to a server resume the last session
    // (an abbreviated handshake: one round trip instead of two, and no certificate work).
    static bool shared_cred(CredHandle &out, std::string &err) {
        static std::once_flag once;
        static CredHandle cred;
        static SECURITY_STATUS status;
        std::call_once(once, [] {
            SCHANNEL_CRED sc{};
            sc.dwVersion = SCHANNEL_CRED_VERSION;
            sc.dwFlags = SCH_CRED_AUTO_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
            status = AcquireCredentialsHandleW(nullptr, (LPWSTR)UNISP_NAME_W, SECPKG_CRED_OUTBOUND, nullptr, &sc, nullptr,
                                               nullptr, &cred, nullptr);
        });
        if (status != SEC_E_OK) { err = "TLS is unavailable: " + sec_error(status); return false; }
        out = cred;
        return true;
    }

    bool resumed = false;  // the handshake resumed an earlier TLS session

    std::string alpn;  // the application protocol agreed in the handshake ("h2", "http/1.1" or "")

    // `offer_h2`: offer HTTP/2 through ALPN; the server picks (see `alpn`).
    bool start_tls(const std::string &host, std::string &err, bool offer_h2 = false) {
        if (!shared_cred(cred, err)) return false;
        SECURITY_STATUS ss;
        tls = true;

        // ALPN: the protocols we speak, as length-prefixed names
        alignas(8) unsigned char alpn_buf[64] = {};
        static const unsigned char names[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
        const size_t names_len = sizeof names;
        auto *protos = (SEC_APPLICATION_PROTOCOLS *)alpn_buf;
        protos->ProtocolListsSize = (ULONG)(offsetof(SEC_APPLICATION_PROTOCOL_LIST, ProtocolList) + names_len);
        protos->ProtocolLists[0].ProtoNegoExt = SecApplicationProtocolNegotiationExt_ALPN;
        protos->ProtocolLists[0].ProtocolListSize = (unsigned short)names_len;
        std::memcpy(protos->ProtocolLists[0].ProtocolList, names, names_len);
        SecBuffer alpn_in[1] = {{(ULONG)(offsetof(SEC_APPLICATION_PROTOCOLS, ProtocolLists) + protos->ProtocolListsSize),
                                 SECBUFFER_APPLICATION_PROTOCOLS, alpn_buf}};
        SecBufferDesc alpn_desc = {SECBUFFER_VERSION, 1, alpn_in};

        std::wstring target(host.size() + 1, 0);
        target.resize((size_t)MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, target.data(), (int)target.size()) - 1);
        const DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                            ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
        DWORD attrs = 0;
        bool first = true, need_read = true;
        enc.clear();
        for (;;) {
            if (!first && need_read) {
                char buf[16384];
                int n = raw_recv(buf, sizeof buf);
                if (n <= 0) { err = "the server closed the connection during the TLS handshake"; return false; }
                enc.insert(enc.end(), buf, buf + n);
            }
            SecBuffer inb[2] = {{(ULONG)enc.size(), SECBUFFER_TOKEN, enc.data()}, {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc ind = {SECBUFFER_VERSION, 2, inb};
            SecBuffer outb[1] = {{0, SECBUFFER_TOKEN, nullptr}};
            SecBufferDesc outd = {SECBUFFER_VERSION, 1, outb};
            ss = InitializeSecurityContextW(&cred, first ? nullptr : &ctx, target.data(), flags, 0, 0,
                                            first ? (offer_h2 ? &alpn_desc : nullptr) : &ind, 0, first ? &ctx : nullptr,
                                            &outd, &attrs, nullptr);
            if (first) have_ctx = true;
            first = false;
            if (ss == SEC_E_INCOMPLETE_MESSAGE) { need_read = true; continue; }
            if (outb[0].cbBuffer && outb[0].pvBuffer) {
                bool sent = raw_send((const char *)outb[0].pvBuffer, outb[0].cbBuffer);
                FreeContextBuffer(outb[0].pvBuffer);
                if (!sent) { err = "connection lost during the TLS handshake"; return false; }
            }
            if (ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED) {
                if (inb[1].BufferType == SECBUFFER_EXTRA && inb[1].cbBuffer) {
                    std::vector<char> extra(enc.end() - inb[1].cbBuffer, enc.end());
                    enc.swap(extra);
                } else {
                    enc.clear();
                }
                if (ss == SEC_E_OK) break;
                need_read = enc.empty();
                continue;
            }
            if (ss == SEC_I_INCOMPLETE_CREDENTIALS) { need_read = false; continue; }
            err = sec_error(ss);
            return false;
        }
        ss = QueryContextAttributesW(&ctx, SECPKG_ATTR_STREAM_SIZES, &sizes);
        if (ss != SEC_E_OK) { err = sec_error(ss); return false; }
        SecPkgContext_SessionInfo info{};
        if (QueryContextAttributesW(&ctx, SECPKG_ATTR_SESSION_INFO, &info) == SEC_E_OK) resumed = info.dwFlags & SSL_SESSION_RECONNECT;
        SecPkgContext_ApplicationProtocol ap{};
        if (offer_h2 && QueryContextAttributesW(&ctx, SECPKG_ATTR_APPLICATION_PROTOCOL, &ap) == SEC_E_OK &&
            ap.ProtoNegoStatus == SecApplicationProtocolNegotiationStatus_Success)
            alpn.assign((const char *)ap.ProtocolId, ap.ProtocolIdSize);
        return true;
    }

    bool send_all(const std::string &data) {
        if (!tls) return raw_send(data.data(), data.size());
        std::vector<char> buf(sizes.cbHeader + sizes.cbMaximumMessage + sizes.cbTrailer);
        size_t off = 0;
        while (off < data.size()) {
            size_t n = std::min<size_t>(data.size() - off, sizes.cbMaximumMessage);
            std::memcpy(buf.data() + sizes.cbHeader, data.data() + off, n);
            SecBuffer b[4] = {{sizes.cbHeader, SECBUFFER_STREAM_HEADER, buf.data()},
                              {(ULONG)n, SECBUFFER_DATA, buf.data() + sizes.cbHeader},
                              {sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, buf.data() + sizes.cbHeader + n},
                              {0, SECBUFFER_EMPTY, nullptr}};
            SecBufferDesc d = {SECBUFFER_VERSION, 4, b};
            if (EncryptMessage(&ctx, 0, &d, 0) != SEC_E_OK) return false;
            if (!raw_send(buf.data(), b[0].cbBuffer + b[1].cbBuffer + b[2].cbBuffer)) return false;
            off += n;
        }
        return true;
    }

    // Returns bytes read, 0 at end of stream, -1 on error.
    int read(char *out, int cap) {
        if (!tls) return raw_recv(out, cap);
        for (;;) {
            if (plain_off < plain.size()) {
                int n = (int)std::min<size_t>(plain.size() - plain_off, (size_t)cap);
                std::memcpy(out, plain.data() + plain_off, n);
                plain_off += n;
                return n;
            }
            plain.clear();
            plain_off = 0;
            if (!enc.empty()) {
                SecBuffer b[4] = {{(ULONG)enc.size(), SECBUFFER_DATA, enc.data()}, {0, SECBUFFER_EMPTY, nullptr},
                                  {0, SECBUFFER_EMPTY, nullptr}, {0, SECBUFFER_EMPTY, nullptr}};
                SecBufferDesc d = {SECBUFFER_VERSION, 4, b};
                SECURITY_STATUS ss = DecryptMessage(&ctx, &d, 0, nullptr);
                if (ss == SEC_E_OK) {
                    std::vector<char> extra;
                    for (auto &x : b) {
                        if (x.BufferType == SECBUFFER_DATA && x.cbBuffer)
                            plain.insert(plain.end(), (char *)x.pvBuffer, (char *)x.pvBuffer + x.cbBuffer);
                        if (x.BufferType == SECBUFFER_EXTRA && x.cbBuffer)
                            extra.assign((char *)x.pvBuffer, (char *)x.pvBuffer + x.cbBuffer);
                    }
                    enc.swap(extra);
                    continue;
                }
                if (ss == SEC_I_CONTEXT_EXPIRED) return 0;  // close_notify
                if (ss != SEC_E_INCOMPLETE_MESSAGE) return -1;
            }
            char buf[16384];
            int n = raw_recv(buf, sizeof buf);
            if (n <= 0) return n == 0 ? 0 : -1;
            enc.insert(enc.end(), buf, buf + n);
        }
    }
};

}  // namespace net
