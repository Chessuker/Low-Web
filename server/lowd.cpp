// lowd — a small static file server for Low-web sites (HTTP/1.1, GET/HEAD).
// Builds on Windows (MinGW/MSVC) and Linux/macOS with no dependencies.
//
//   lowd [-p PORT] [-b ADDRESS] [SITE_DIR]
//
// Defaults: port 8080, all interfaces (IPv4 + IPv6), directory "www".
// "/dir/" serves "/dir/index.wasm"; "/dir" redirects to "/dir/".
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define CLOSESOCK close
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

static std::string g_root = "www";
static std::mutex g_log_mutex;
static std::atomic<int> g_active{0};

static void log_line(const std::string &peer, const std::string &line, int status, long long bytes) {
    time_t t = time(nullptr);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&t));
    std::lock_guard<std::mutex> lock(g_log_mutex);
    printf("%s  %-40s %d %lld  \"%s\"\n", ts, peer.c_str(), status, bytes, line.c_str());
    fflush(stdout);
}

static std::string lower(std::string s) {
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static const char *content_type(const std::string &path) {
    std::string p = lower(path);
    struct { const char *ext, *type; } map[] = {
        {".wasm", "application/wasm"}, {".txt", "text/plain; charset=utf-8"}, {".md", "text/plain; charset=utf-8"},
        {".png", "image/png"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".bmp", "image/bmp"},
        {".json", "application/json"}, {".bin", "application/octet-stream"},
    };
    for (auto &m : map) {
        size_t n = strlen(m.ext);
        if (p.size() >= n && p.compare(p.size() - n, n, m.ext) == 0) return m.type;
    }
    return "application/octet-stream";
}

// Decodes %XX and rejects anything that could escape the site directory.
static bool safe_path(const std::string &raw, std::string &out) {
    std::string p;
    for (size_t i = 0; i < raw.size(); i++) {
        char c = raw[i];
        if (c == '%' && i + 2 < raw.size() && isxdigit((unsigned char)raw[i + 1]) && isxdigit((unsigned char)raw[i + 2])) {
            c = (char)strtol(raw.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        }
        if (c == '\0' || c == '\\' || c == ':') return false;
        p += c;
    }
    if (p.empty() || p[0] != '/') return false;
    size_t i = 0;
    while (i < p.size()) {
        size_t j = p.find('/', i + 1);
        std::string seg = p.substr(i + 1, (j == std::string::npos ? p.size() : j) - i - 1);
        if (seg == ".." || seg == "." || (!seg.empty() && seg[0] == '.')) return false;  // no dot-files either
        if (j == std::string::npos) break;
        i = j;
    }
    out = p;
    return true;
}

enum Kind { NONE, FILE_, DIR_ };

static Kind stat_path(const std::string &path, long long &size) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExW(w.c_str(), GetFileExInfoStandard, &a)) return NONE;
    if (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return DIR_;
    size = ((long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    return FILE_;
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return NONE;
    if (S_ISDIR(st.st_mode)) return DIR_;
    size = (long long)st.st_size;
    return S_ISREG(st.st_mode) ? FILE_ : NONE;
#endif
}

static FILE *open_file(const std::string &path) {
#ifdef _WIN32
    int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), n);
    return _wfopen(w.c_str(), L"rb");
#else
    return fopen(path.c_str(), "rb");
#endif
}

static void set_timeouts(sock_t s, int ms) {
#ifdef _WIN32
    DWORD v = (DWORD)ms;
#else
    timeval v{ms / 1000, (ms % 1000) * 1000};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&v, sizeof v);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&v, sizeof v);
}

static bool send_all(sock_t s, const char *p, size_t n) {
    while (n) {
        int k = (int)send(s, p, (int)(n > (1 << 20) ? (1 << 20) : n), 0);
        if (k <= 0) return false;
        p += k;
        n -= (size_t)k;
    }
    return true;
}

static void respond_simple(sock_t s, int status, const char *reason, const std::string &body, const std::string &extra = "") {
    std::string h = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\n"
                    "Content-Type: text/plain; charset=utf-8\r\n"
                    "Content-Length: " + std::to_string(body.size()) + "\r\n" + extra +
                    "Server: lowd/0.1\r\nConnection: close\r\n\r\n" + body;
    send_all(s, h.data(), h.size());
}

static void handle(sock_t s, std::string peer) {
    std::string req;
    char buf[8192];
    while (req.find("\r\n\r\n") == std::string::npos) {
        int n = (int)recv(s, buf, sizeof buf, 0);
        if (n <= 0 || req.size() > 16384) { CLOSESOCK(s); g_active--; return; }
        req.append(buf, n);
    }
    std::string line = req.substr(0, req.find("\r\n"));
    size_t sp1 = line.find(' '), sp2 = line.rfind(' ');
    std::string method = line.substr(0, sp1);
    std::string target = sp1 != std::string::npos && sp2 > sp1 ? line.substr(sp1 + 1, sp2 - sp1 - 1) : "";
    target = target.substr(0, target.find('?'));
    int status = 200;
    long long sent = 0;

    std::string path;
    if (method != "GET" && method != "HEAD") {
        status = 405;
        respond_simple(s, 405, "Method Not Allowed", "only GET and HEAD are supported\n", "Allow: GET, HEAD\r\n");
    } else if (!safe_path(target, path)) {
        status = 400;
        respond_simple(s, 400, "Bad Request", "bad path\n");
    } else {
        if (path.back() == '/') path += "index.wasm";
        long long size = 0;
        std::string fs = g_root + path;
        Kind k = stat_path(fs, size);
        if (k == DIR_) {
            status = 301;
            respond_simple(s, 301, "Moved Permanently", "moved\n", "Location: " + target + "/\r\n");
        } else if (k != FILE_) {
            status = 404;
            respond_simple(s, 404, "Not Found", "not found: " + target + "\n");
        } else if (FILE *f = open_file(fs)) {
            std::string h = "HTTP/1.1 200 OK\r\n"
                            "Content-Type: " + std::string(content_type(path)) + "\r\n"
                            "Content-Length: " + std::to_string(size) + "\r\n"
                            "Cache-Control: no-cache\r\n"
                            "Server: lowd/0.1\r\nConnection: close\r\n\r\n";
            bool ok = send_all(s, h.data(), h.size());
            if (method == "GET") {
                std::vector<char> chunk(1 << 16);
                size_t n;
                while (ok && (n = fread(chunk.data(), 1, chunk.size(), f)) > 0) {
                    ok = send_all(s, chunk.data(), n);
                    sent += (long long)n;
                }
            }
            fclose(f);
        } else {
            status = 403;
            respond_simple(s, 403, "Forbidden", "cannot read file\n");
        }
    }
    log_line(peer, line, status, sent);
#ifdef _WIN32
    shutdown(s, SD_SEND);
#else
    shutdown(s, SHUT_WR);
#endif
    // drain briefly so the client sees a clean close
    set_timeouts(s, 1000);
    while (recv(s, buf, sizeof buf, 0) > 0) {}
    CLOSESOCK(s);
    g_active--;
}

static std::string peer_name(const sockaddr_storage &a) {
    char host[INET6_ADDRSTRLEN] = "?";
    if (a.ss_family == AF_INET) inet_ntop(AF_INET, &((const sockaddr_in *)&a)->sin_addr, host, sizeof host);
    else inet_ntop(AF_INET6, &((const sockaddr_in6 *)&a)->sin6_addr, host, sizeof host);
    std::string h = host;
    if (h.rfind("::ffff:", 0) == 0) h = h.substr(7);
    return h;
}

int main(int argc, char **argv) {
    int port = 8080;
    std::string bind_addr;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if ((a == "-p" || a == "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if ((a == "-b" || a == "--bind") && i + 1 < argc) bind_addr = argv[++i];
        else if (a == "-h" || a == "--help") {
            printf("usage: lowd [-p PORT] [-b ADDRESS] [SITE_DIR]\n");
            return 0;
        } else g_root = a;
    }
    while (g_root.size() > 1 && (g_root.back() == '/' || g_root.back() == '\\')) g_root.pop_back();

#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SetConsoleOutputCP(CP_UTF8);
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    long long dummy;
    if (stat_path(g_root, dummy) != DIR_) {
        fprintf(stderr, "lowd: site directory \"%s\" not found\n", g_root.c_str());
        return 1;
    }

    addrinfo hints{}, *res = nullptr;
    hints.ai_family = bind_addr.empty() ? AF_INET6 : AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    std::string ps = std::to_string(port);
    if (getaddrinfo(bind_addr.empty() ? nullptr : bind_addr.c_str(), ps.c_str(), &hints, &res) != 0 || !res) {
        fprintf(stderr, "lowd: bad bind address\n");
        return 1;
    }
    sock_t ls = socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
    int zero = 0, one = 1;
    if (res->ai_family == AF_INET6) setsockopt(ls, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&zero, sizeof zero);
#ifndef _WIN32
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
#else
    (void)one;
#endif
    if (ls == INVALID_SOCKET || bind(ls, res->ai_addr, (int)res->ai_addrlen) != 0 || listen(ls, 64) != 0) {
        fprintf(stderr, "lowd: cannot listen on port %d (already in use?)\n", port);
        return 1;
    }
    freeaddrinfo(res);
    printf("lowd: serving \"%s\" on port %d%s\n", g_root.c_str(), port,
           bind_addr.empty() ? " (all interfaces)" : (" (" + bind_addr + ")").c_str());
    printf("      open  http://localhost:%d/  in Low-web\n", port);
    fflush(stdout);

    for (;;) {
        sockaddr_storage peer{};
        socklen_t plen = sizeof peer;
        sock_t c = accept(ls, (sockaddr *)&peer, &plen);
        if (c == INVALID_SOCKET) continue;
        if (g_active >= 256) { CLOSESOCK(c); continue; }
        set_timeouts(c, 15000);
        g_active++;
        std::thread(handle, c, peer_name(peer)).detach();
    }
}
