// net.cpp — see net.h.
#define WIN32_LEAN_AND_MEAN
#define SECURITY_WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <security.h>
#include <schannel.h>

#include "net.h"
#include "conn.h"
#include "inflate.h"
#include "cache.h"
#include "cookies.h"
#include "http2.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <memory>

namespace net {

namespace {

std::wstring widen(const std::string &s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string lower(std::string s) {
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

int default_port(const std::string &scheme) { return scheme == "https" ? 443 : scheme == "http" ? 80 : 0; }

bool has_scheme(const std::string &s) {
    size_t i = 0;
    while (i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '+' || s[i] == '-' || s[i] == '.')) i++;
    return i >= 2 && i < s.size() && s[i] == ':' && isalpha((unsigned char)s[0]);
}

std::string percent_decode(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            o += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else {
            o += s[i];
        }
    }
    return o;
}

std::string percent_encode_path(const std::string &s) {
    static const char *hexd = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (c <= 32 || c >= 127 || c == '"' || c == '<' || c == '>' || c == '\\' || c == '^' || c == '`' || c == '{' || c == '|' || c == '}') {
            o += '%';
            o += hexd[c >> 4];
            o += hexd[c & 15];
        } else {
            o += (char)c;
        }
    }
    return o;
}

// Removes "." and ".." segments from a path (the query is left alone).
std::string normalize_path(const std::string &path) {
    size_t q = path.find('?');
    std::string p = path.substr(0, q), query = q == std::string::npos ? "" : path.substr(q);
    std::vector<std::string> parts, out;
    size_t i = (!p.empty() && p[0] == '/') ? 1 : 0;
    for (;;) {
        size_t j = p.find('/', i);
        parts.push_back(p.substr(i, j == std::string::npos ? std::string::npos : j - i));
        if (j == std::string::npos) break;
        i = j + 1;
    }
    bool dir = false;
    for (size_t k = 0; k < parts.size(); k++) {
        const std::string &seg = parts[k];
        bool last = k + 1 == parts.size();
        if (seg == "..") { if (!out.empty()) out.pop_back(); dir = last; }
        else if (seg == "." || seg.empty()) dir = last;
        else { out.push_back(seg); dir = false; }
    }
    std::string r;
    for (auto &seg : out) r += "/" + seg;
    if (r.empty() || dir) r += "/";
    return r + query;
}

std::string content_type_for(const std::string &path) {
    std::string p = lower(path);
    auto ends = [&](const char *e) { size_t n = strlen(e); return p.size() >= n && p.compare(p.size() - n, n, e) == 0; };
    if (ends(".wasm")) return "application/wasm";
    if (ends(".txt") || ends(".md")) return "text/plain; charset=utf-8";
    if (ends(".png")) return "image/png";
    if (ends(".jpg") || ends(".jpeg")) return "image/jpeg";
    if (ends(".bmp")) return "image/bmp";
    if (ends(".html") || ends(".htm")) return "text/html";
    if (ends(".json")) return "application/json";
    if (ends(".mp4") || ends(".m4v")) return "video/mp4";
    if (ends(".webm")) return "video/webm";
    if (ends(".mov")) return "video/quicktime";
    if (ends(".mp3")) return "audio/mpeg";
    if (ends(".m4a")) return "audio/mp4";
    if (ends(".wav")) return "audio/wav";
    return "application/octet-stream";
}


void log_fetch(const std::string &what);

// The Windows path of a file:// URL.
std::string file_path(const Url &u) {
    std::string path = percent_decode(u.path);
    if (!u.host.empty()) path = "//" + u.host + path;                       // UNC
    else if (path.size() >= 3 && path[0] == '/' && path[2] == ':') path = path.substr(1);  // /C:/x -> C:/x
    return path;
}

// Whether `path` is in directory `dir` or below, once both are made absolute (so "..",
// "%2e%2e" and backslashes can't climb out).
bool path_inside(const std::string &path, const std::string &dir) {
    auto full = [](const std::string &p) {
        wchar_t buf[4096];
        DWORD n = GetFullPathNameW(widen(p).c_str(), 4096, buf, nullptr);
        std::wstring w = n && n < 4096 ? std::wstring(buf, n) : std::wstring();
        for (auto &ch : w) ch = ch == L'/' ? L'\\' : (wchar_t)towlower(ch);
        return w;
    };
    std::wstring f = full(path), d = full(dir);
    if (f.empty() || d.empty()) return false;
    if (d.back() != L'\\') d += L'\\';
    return f.compare(0, d.size(), d) == 0 || f + L'\\' == d;
}

Response blocked(const std::string &url, const std::string &why) {
    Response r;
    r.final_url = r.requested_url = url;
    r.error = "blocked: " + why;
    log_fetch("blocked " + url + ": " + why);
    return r;
}

Response load_file(const Url &u, Mode mode, size_t max_bytes, Stream *stream, const Access &access) {
    std::string path = file_path(u);
    std::string url = u.str();
    if (!access.files) return blocked(url, "only a page that is itself a file may open files");
    if (!access.file_root.empty()) {
        Url root;
        if (!root.parse(access.file_root) || root.scheme != "file" || !path_inside(path, file_path(root)))
            return blocked(url, "a file page may only open files in its own folder and below");
    }
    Response r;
    r.zone = Zone::Local;
    auto is_dir = [](const std::string &p) {
        DWORD at = GetFileAttributesW(widen(p).c_str());
        return at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY);
    };
    auto exists = [](const std::string &p) { return GetFileAttributesW(widen(p).c_str()) != INVALID_FILE_ATTRIBUTES; };
    if (!path.empty() && path.back() != '/' && is_dir(path)) {
        path += "/";
        url += "/";
    }
    if (!path.empty() && path.back() == '/') {
        // a directory: index.wasm (navigation only), else index.html
        if (mode == Mode::Page && exists(path + "index.wasm")) path += "index.wasm";
        else if (exists(path + "index.html")) path += "index.html";
        else {
            r.final_url = r.requested_url = url;
            r.error = "no index.wasm or index.html in " + path;
            return r;
        }
    }
    r.final_url = url;
    r.requested_url = "file:///" + path;
    HANDLE f = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        r.error = "file not found: " + path;
        return r;
    }
    LARGE_INTEGER sz;
    GetFileSizeEx(f, &sz);
    if (stream && stream->range_from >= 0) {  // media: piece by piece from the byte asked for
        r.status = 200;
        r.content_type = content_type_for(path);
        r.total_size = sz.QuadPart;
        r.range_start = std::min<int64_t>(stream->range_from, sz.QuadPart);
        if (stream->begin(r)) {
            LARGE_INTEGER at;
            at.QuadPart = r.range_start;
            SetFilePointerEx(f, at, nullptr, FILE_BEGIN);
            std::vector<uint8_t> buf(256 << 10);
            DWORD got = 0;
            while (!stream->stop && ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got) stream->data(buf.data(), got);
        }
        CloseHandle(f);
        if (stream->stop) { r.status = 0; r.error = "stopped"; }
        return r;
    }
    if ((uint64_t)sz.QuadPart > max_bytes) {
        CloseHandle(f);
        r.error = "file is too large";
        return r;
    }
    r.body.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    bool ok = r.body.empty() || (ReadFile(f, r.body.data(), (DWORD)r.body.size(), &got, nullptr) && got == r.body.size());
    CloseHandle(f);
    if (!ok) {
        r.error = "could not read " + path;
        r.body.clear();
        return r;
    }
    r.status = 200;
    r.content_type = content_type_for(path);
    if (stream) {
        std::vector<uint8_t> body;
        body.swap(r.body);
        if (stream->begin(r)) stream->data(body.data(), body.size());
        r.body.swap(body);
    }
    return r;
}

// ---- connection pool (HTTP keep-alive) ---------------------------------------------------
// A finished connection is kept for the next request to the same server, which saves the
// TCP and TLS handshakes (often 100-300 ms each for HTTPS).

struct IdleConn {
    std::string key;  // scheme://host:port
    std::unique_ptr<Conn> conn;
    ULONGLONG since;
};
std::mutex g_pool_m;
std::vector<IdleConn> g_pool;
const size_t kPoolMax = 16;
const ULONGLONG kIdleMs = 60000;

std::unique_ptr<Conn> pool_take(const std::string &key) {
    std::lock_guard<std::mutex> lock(g_pool_m);
    ULONGLONG now = GetTickCount64();
    for (size_t i = g_pool.size(); i-- > 0;) {  // newest first
        if (now - g_pool[i].since > kIdleMs) {
            g_pool.erase(g_pool.begin() + i);
            continue;
        }
        if (g_pool[i].key != key) continue;
        std::unique_ptr<Conn> c = std::move(g_pool[i].conn);
        g_pool.erase(g_pool.begin() + i);
        if (c->idle_ok()) return c;
    }
    return nullptr;
}

void pool_put(const std::string &key, std::unique_ptr<Conn> c) {
    std::lock_guard<std::mutex> lock(g_pool_m);
    if (g_pool.size() >= kPoolMax) g_pool.erase(g_pool.begin());  // the oldest
    g_pool.push_back({key, std::move(c), GetTickCount64()});
}

std::function<void(const std::string &)> g_logger;
bool g_keep_alive = true;
bool g_http2 = true;

// Origins that answered a TLS handshake with plain HTTP/1.1 (no need to wait on each other
// to find out whether an HTTP/2 connection appears), and one lock per origin so that
// requests made at once share the first HTTP/2 connection instead of each opening one.
std::mutex g_origin_m;
std::map<std::string, bool> g_h1_only;
std::map<std::string, std::unique_ptr<std::mutex>> g_origin_locks;

bool h1_only(const std::string &key) {
    std::lock_guard<std::mutex> lk(g_origin_m);
    return g_h1_only.count(key) > 0;
}

std::mutex &origin_lock(const std::string &key) {
    std::lock_guard<std::mutex> lk(g_origin_m);
    auto &m = g_origin_locks[key];
    if (!m) m = std::make_unique<std::mutex>();
    return *m;
}

void log_fetch(const std::string &what) {
    if (g_logger) g_logger("[net] " + what);
}

// Hands a stored response to a stream, as if it had just arrived.
void stream_whole(Stream *stream, const Response &r) {
    if (stream && stream->begin(r) && !r.body.empty()) stream->data(r.body.data(), r.body.size());
}

Response from_cache(const cache::Entry &e, std::string &location) {
    Response r;
    r.final_url = r.requested_url = e.url;
    r.status = e.status;
    r.content_type = e.content_type;
    r.body = e.body;
    r.from_cache = true;
    r.zone = (Zone)e.zone;
    location = e.location;
    return r;
}

// One HTTP/1.1 request, through the cache and on a pooled connection if there is one.
// Sets `location` for redirects.
// A pages sees a cached copy only if it was made for the same cookies ("Vary: Cookie").
std::string vary_key(const std::string &cookie_header) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char ch : cookie_header) h = (h ^ ch) * 1099511628211ull;
    char buf[24];
    snprintf(buf, sizeof buf, "c%016llx", (unsigned long long)h);
    return buf;
}

Response http_request(const Url &u, Mode mode, size_t max_bytes, const std::string *post, std::string &location,
                      Stream *stream, CacheMode cmode, const cookies::Context *who, const Access &access) {
    const std::string url = u.str();
    ULONGLONG t_start = GetTickCount64();
    const bool media = stream && stream->range_from >= 0;  // a range, not cached, not kept (net.h: Stream)
    cookies::Context ctx = who ? *who : cookies::Context{};
    ctx.unsafe_method = post != nullptr;
    const std::string cookie = cookies::header_for(url, ctx);

    // ---- the cache: fresh enough to use as it is? else ask "has it changed?"
    cache::Entry cached;
    bool have = !post && !media && cmode != CacheMode::Reload && cache::get(url, cached);
    if (have && !cached.vary.empty() && cached.vary != vary_key(cookie)) have = false;  // made for other cookies
    if (have && (Zone)cached.zone < access.lowest) have = false;  // from a server this request may not reach
    if (have) {
        int64_t now = cache::now_ms();
        bool usable = cmode == CacheMode::Normal ? now < cached.fresh_until
                    : cmode == CacheMode::PreferCached ? cached.fresh_until > cached.stored  // not "always ask"
                                                       : false;
        if (usable) {
            Response r = from_cache(cached, location);
            if (location.empty()) stream_whole(stream, r);
            log_fetch(std::to_string(r.status) + " " + url + " (cache)");
            return r;
        }
        if (cached.etag.empty() && cached.last_modified.empty()) have = false;
    }

    Response r;
    r.final_url = r.requested_url = url;
    std::string target = u.path;
    std::string host_hdr = u.host.find(':') != std::string::npos ? "[" + u.host + "]" : u.host;
    if (u.port != default_port(u.scheme)) host_hdr += ":" + std::to_string(u.port);
    std::string req = std::string(post ? "POST " : "GET ") + percent_encode_path(target) + " HTTP/1.1\r\n"
                      "Host: " + host_hdr + "\r\n"
                      "User-Agent: Low-web/0.1\r\n"
                      "Accept: " + std::string(mode == Mode::Page
                                                   ? "application/wasm, text/html;q=0.9, text/plain;q=0.8, image/*;q=0.8, */*;q=0.5"
                                                   : "*/*") + "\r\n"
                      "Accept-Encoding: " + std::string(media ? "identity" : "gzip, deflate") + "\r\n"
                      "Accept-Language: th, en;q=0.8\r\n";
    if (media) req += "Range: bytes=" + std::to_string(stream->range_from) + "-\r\n";
    if (have && !cached.etag.empty()) req += "If-None-Match: " + cached.etag + "\r\n";
    if (have && !cached.last_modified.empty()) req += "If-Modified-Since: " + cached.last_modified + "\r\n";
    if (cmode == CacheMode::Reload) req += "Cache-Control: no-cache\r\n";
    if (!cookie.empty()) req += "Cookie: " + cookie + "\r\n";
    if (post)
        req += "Content-Type: application/x-www-form-urlencoded\r\n"
               "Content-Length: " + std::to_string(post->size()) + "\r\n\r\n" + *post;
    else
        req += "\r\n";

    std::string head;
    std::vector<uint8_t> raw;  // body bytes as received
    size_t header_end = std::string::npos;
    std::vector<char> buf(65536);
    long long content_length = -1;
    bool chunked = false, done = false, no_body = false, last_chunk = false;
    size_t cpos = 0;       // chunked: next chunk header in raw
    size_t body_end = 0;   // chunked: where the message ends in raw (after the trailer)
    size_t dropped = 0;    // media: body bytes handed on and removed from the front of raw
    std::map<std::string, std::string> hdrs;
    std::vector<std::string> set_cookie;  // (a header that may come many times)

    // streaming: the body goes to `stream` as it arrives, decompressed if need be
    bool streaming = false;
    size_t fed = 0;  // body bytes (as sent, maybe compressed) handed on so far
    std::unique_ptr<deflate::Stream> dec;
    std::string dec_err;
    auto pump = [&](const std::vector<uint8_t> &body, size_t avail, bool final) {
        if (media && stream->stop) return false;
        if (!streaming || (avail <= fed && !final)) return true;
        const uint8_t *p = body.data() + fed;
        size_t n = avail - fed;
        fed = avail;
        if (!dec) {
            if (n) stream->data(p, n);
            return true;
        }
        if (!dec->feed(p, n, final, dec_err)) return false;
        const uint8_t *q = nullptr;
        if (size_t k = dec->take(q)) stream->data(q, k);
        return true;
    };

    // What every answer's headers lead to, over either protocol: cookies, and for a body to
    // be streamed, the stream's start and a decoder.
    auto headers_in = [&] {
        cookies::store(url, set_cookie, ctx);  // also from redirects (logins often set them there)
        no_body = r.status == 204 || r.status == 304 || (r.status >= 100 && r.status < 200);
        bool redirect = r.status >= 300 && r.status < 400 && hdrs.count("location");
        if (stream && !no_body && !redirect) {
            if (media) {  // which part of the whole this is: "Content-Range: bytes 100-999/1000"
                r.range_start = 0;
                r.total_size = -1;
                const std::string &cr = hdrs["content-range"];
                size_t sp = cr.find(' '), slash = cr.find('/');
                if (r.status == 206 && sp != std::string::npos && slash != std::string::npos) {
                    r.range_start = atoll(cr.c_str() + sp + 1);
                    if (cr[slash + 1] != '*') r.total_size = atoll(cr.c_str() + slash + 1);
                } else if (r.status == 200 && hdrs.count("content-length")) {
                    r.total_size = atoll(hdrs["content-length"].c_str());
                }
            }
            std::string enc = lower(hdrs["content-encoding"]);
            if (enc.empty() || enc == "identity" || enc == "gzip" || enc == "x-gzip" || enc == "deflate") {
                r.content_type = hdrs["content-type"];
                streaming = stream->begin(r);
                if (streaming && !enc.empty() && enc != "identity")
                    dec = std::make_unique<deflate::Stream>(enc == "deflate" ? deflate::Stream::Kind::ZlibOrRaw
                                                                             : deflate::Stream::Kind::Gzip, max_bytes);
            }
        }
    };

    const std::string pool_key = u.scheme + "://" + u.host + ":" + std::to_string(u.port);
    auto connect_failed = [&](Response &res) -> Response & {
        if (res.error.rfind("blocked: ", 0) == 0) log_fetch("blocked " + url + ": " + res.error.substr(9));
        return res;
    };
    std::unique_ptr<Conn> c, h1conn;  // h1conn: a new HTTPS connection whose server chose HTTP/1.1
    bool reused = false, reusable = false, via_h2 = false;
    std::string conn_note;

    // ---- HTTP/2, for HTTPS servers that offer it: one connection carries many requests
    if (u.scheme == "https" && g_http2 && g_keep_alive && !h1_only(pool_key)) {
        std::shared_ptr<h2::Connection> hc;
        std::shared_ptr<h2::Stream> st;
        h2::Headers hh = {{":method", post ? "POST" : "GET"}, {":scheme", "https"}, {":authority", host_hdr},
                          {":path", percent_encode_path(target)}, {"user-agent", "Low-web/0.1"},
                          {"accept", mode == Mode::Page ? "application/wasm, text/html;q=0.9, text/plain;q=0.8, image/*;q=0.8, */*;q=0.5" : "*/*"},
                          {"accept-encoding", media ? "identity" : "gzip, deflate"}, {"accept-language", "th, en;q=0.8"}};
        if (media) hh.push_back({"range", "bytes=" + std::to_string(stream->range_from) + "-"});
        if (have && !cached.etag.empty()) hh.push_back({"if-none-match", cached.etag});
        if (have && !cached.last_modified.empty()) hh.push_back({"if-modified-since", cached.last_modified});
        if (cmode == CacheMode::Reload) hh.push_back({"cache-control", "no-cache"});
        if (!cookie.empty()) hh.push_back({"cookie", cookie});
        if (post) {
            hh.push_back({"content-type", "application/x-www-form-urlencoded"});
            hh.push_back({"content-length", std::to_string(post->size())});
        }
        for (int attempt = 0; attempt < 2 && !h1conn; attempt++) {
            hc = attempt ? nullptr : h2::find(pool_key);
            if (hc && hc->zone() < access.lowest)
                return blocked(url, std::string("a page from ") + zone_name(access.lowest) + " may not reach " + zone_name(hc->zone()));
            reused = hc != nullptr;
            if (!hc) {
                std::lock_guard<std::mutex> one_at_a_time(origin_lock(pool_key));
                if (attempt == 0) hc = h2::find(pool_key);  // made by another request meanwhile?
                if (hc && hc->zone() < access.lowest)
                    return blocked(url, std::string("a page from ") + zone_name(access.lowest) + " may not reach " + zone_name(hc->zone()));
                reused = hc != nullptr;
                if (!hc) {
                    auto nc = std::make_unique<Conn>();
                    ULONGLONG t0 = GetTickCount64();
                    if (!nc->connect_to(u.host, u.port, r.error, access.lowest)) return connect_failed(r);
                    ULONGLONG t1 = GetTickCount64();
                    if (!nc->start_tls(u.host, r.error, true)) return r;
                    conn_note = "new connection: TCP " + std::to_string(t1 - t0) + " ms" +
                                (nc->resumed ? ", TLS resumed " : ", TLS ") + std::to_string(GetTickCount64() - t1) + " ms";
                    if (nc->alpn == "h2") {
                        hc = h2::adopt(std::move(nc), pool_key);
                        if (!hc) { r.error = "could not start HTTP/2"; return r; }
                    } else {
                        std::lock_guard<std::mutex> lk(g_origin_m);
                        g_h1_only[pool_key] = true;
                        h1conn = std::move(nc);  // HTTP/1.1 below, on this connection
                        break;
                    }
                }
            }
            st = hc->request(hh, post);
            if (st && st->wait_headers(60000)) break;
            bool again = !post && (!st || st->retry_ok);  // never processed: safe to send again
            if (!again || attempt == 1) {
                r.error = st ? st->error : "the HTTP/2 connection failed";
                return r;
            }
            st = nullptr;
        }
        if (st) {
            via_h2 = true;
            r.zone = hc->zone();
            conn_note = (reused ? "HTTP/2, shared connection" : "HTTP/2, " + conn_note) + ", stream " + std::to_string(st->id);
            reused = false;
            r.status = st->status;
            for (auto &h : st->headers) {
                if (h.first == "set-cookie") set_cookie.push_back(h.second);
                else hdrs[h.first] = h.second;
            }
            headers_in();
            std::vector<uint8_t> piece;
            for (;;) {
                int k = st->next_data(piece, 60000);
                if (k < 0) {
                    r.status = 0;
                    r.error = st->error;
                    return r;
                }
                if (k == 0) break;
                r.body.insert(r.body.end(), piece.begin(), piece.end());
                if (r.body.size() > max_bytes) { r.status = 0; r.error = "response is too large"; return r; }
                if (!pump(r.body, r.body.size(), false)) {  // (the decoder's error is reported below)
                    if (media && stream->stop) hc->cancel(st);  // else the server goes on sending it
                    break;
                }
                if (media && !dec) { r.body.clear(); fed = 0; }  // handed on: not kept
            }
        }
    }

    // ---- HTTP/1.1: a pooled connection if possible. A pooled connection the server has
    // closed meanwhile fails before any answer; then the request goes on a new one.
    if (!via_h2) {
    for (int attempt = 0;; attempt++) {
        c = post || attempt || !g_keep_alive ? nullptr : pool_take(pool_key);  // a POST is not sent twice
        if (attempt == 0 && h1conn) c = std::move(h1conn);
        if (c && c->zone < access.lowest) {
            Zone z = c->zone;
            pool_put(pool_key, std::move(c));
            return blocked(url, std::string("a page from ") + zone_name(access.lowest) + " may not reach " + zone_name(z));
        }
        reused = c != nullptr && conn_note.empty();
        if (!c) {
            c = std::make_unique<Conn>();
            ULONGLONG t0 = GetTickCount64();
            if (!c->connect_to(u.host, u.port, r.error, access.lowest)) return connect_failed(r);
            ULONGLONG t1 = GetTickCount64();
            conn_note = "new connection: TCP " + std::to_string(t1 - t0) + " ms";
            if (u.scheme == "https") {
                if (!c->start_tls(u.host, r.error)) return r;
                conn_note += std::string(c->resumed ? ", TLS resumed " : ", TLS ") + std::to_string(GetTickCount64() - t1) + " ms";
            }
        }
        bool sent = c->send_all(req);
        int n = sent ? c->read(buf.data(), (int)buf.size()) : -1;
        if (n > 0) {
            r.zone = c->zone;
            head.assign(buf.data(), n);
            break;
        }
        if (reused && attempt == 0) continue;
        r.error = !sent ? "could not send the request" : n < 0 ? "the connection failed while reading the response"
                                                              : "the server closed the connection without answering";
        return r;
    }

    for (bool first = true;; first = false) {
        if (!first) {
            int n = c->read(buf.data(), (int)buf.size());
            if (n < 0) {
                if (header_end == std::string::npos) { r.error = "the connection failed while reading the response"; return r; }
                break;
            }
            if (n == 0) break;
            if (header_end == std::string::npos) head.append(buf.data(), n);
            else raw.insert(raw.end(), buf.data(), buf.data() + n);
        }
        if (header_end == std::string::npos) {
            size_t e = head.find("\r\n\r\n");
            if (e == std::string::npos) {
                if (head.size() > 65536) { r.error = "response headers are too large"; return r; }
                continue;
            }
            header_end = e;
            raw.assign(head.begin() + e + 4, head.end());
            head.resize(e);
            // status line + headers
            size_t le = head.find("\r\n");
            std::string status_line = head.substr(0, le);
            if (status_line.compare(0, 5, "HTTP/") != 0 || status_line.size() < 12) { r.error = "not an HTTP server"; return r; }
            r.status = atoi(status_line.c_str() + 9);
            size_t pos = le == std::string::npos ? head.size() : le + 2;
            while (pos < head.size()) {
                size_t eol = head.find("\r\n", pos);
                if (eol == std::string::npos) eol = head.size();
                std::string line = head.substr(pos, eol - pos);
                size_t colon = line.find(':');
                if (colon != std::string::npos) {
                    std::string k = lower(trim(line.substr(0, colon))), v = trim(line.substr(colon + 1));
                    if (k == "set-cookie") set_cookie.push_back(v);
                    else hdrs[k] = v;
                }
                pos = eol + 2;
            }
            if (status_line.compare(0, 8, "HTTP/1.0") == 0 && lower(hdrs["connection"]).find("keep-alive") == std::string::npos)
                hdrs["connection"] = "close";
            if (hdrs.count("content-length")) content_length = atoll(hdrs["content-length"].c_str());
            chunked = lower(hdrs["transfer-encoding"]).find("chunked") != std::string::npos;
            headers_in();
        }
        if (raw.size() > max_bytes + 65536) { r.status = 0; r.error = "response is too large"; return r; }
        if (no_body) { done = true; break; }
        if (chunked) {
            while (!last_chunk) {
                auto it = std::search(raw.begin() + cpos, raw.end(), "\r\n", "\r\n" + 2);
                if (it == raw.end()) break;
                size_t line_end = (size_t)(it - raw.begin());
                size_t size = strtoull(std::string(raw.begin() + cpos, raw.begin() + line_end).c_str(), nullptr, 16);
                if (size == 0) { last_chunk = true; cpos = line_end; break; }
                if (raw.size() < line_end + 2 + size + 2) break;
                r.body.insert(r.body.end(), raw.begin() + line_end + 2, raw.begin() + line_end + 2 + size);
                cpos = line_end + 2 + size + 2;
                if (r.body.size() > max_bytes) { r.status = 0; r.error = "response is too large"; return r; }
            }
            if (last_chunk) {  // then optional trailer lines and an empty line
                auto t = std::search(raw.begin() + cpos, raw.end(), "\r\n\r\n", "\r\n\r\n" + 4);
                if (t != raw.end()) {
                    body_end = (size_t)(t - raw.begin()) + 4;
                    done = true;
                }
            }
            if (!pump(r.body, r.body.size(), false)) break;
            if (done) break;
            if (media && !dec) {  // handed on: not kept
                r.body.clear();
                fed = 0;
                raw.erase(raw.begin(), raw.begin() + cpos);
                cpos = 0;
            }
        } else {
            size_t avail = content_length >= 0 ? std::min(raw.size(), (size_t)content_length - dropped) : raw.size();
            if (!pump(raw, avail, false)) break;
            if (content_length >= 0 && raw.size() + dropped >= (size_t)content_length) {
                done = true;
                break;
            }
            if (media && !dec) {
                raw.erase(raw.begin(), raw.begin() + fed);
                dropped += fed;
                fed = 0;
            }
        }
    }
    if (header_end == std::string::npos) { r.status = 0; r.error = "the server closed the connection without answering"; return r; }
    if (chunked && !done && last_chunk) done = true;  // the connection ended in the trailer: fine, but don't reuse it
    else if (chunked && !done) {
        r.status = 0;
        r.error = "the response was cut off";
        return r;
    }
    // exactly one whole response read, and the server keeps the connection: reuse it
    reusable = done && lower(hdrs["connection"]).find("close") == std::string::npos &&
                    (no_body ? raw.empty() : chunked ? raw.size() == body_end : content_length >= 0 && raw.size() + dropped == (size_t)content_length);
    if (!chunked) {
        if (content_length >= 0) {
            if (raw.size() + dropped < (size_t)content_length) {
                r.status = 0;
                r.error = media && stream->stop ? "stopped" : "the response was cut off";
                return r;
            }
            raw.resize((size_t)content_length - dropped);
        }
        r.body.swap(raw);
    }
    }  // HTTP/1.1
    if (media && stream->stop) {  // (an HTTP/1.1 connection left mid-body is closed, not reused)
        r.status = 0;
        r.error = "stopped";
        return r;
    }
    r.content_type = hdrs["content-type"];
    location = hdrs["location"];
    std::string enc = lower(hdrs["content-encoding"]);
    auto finish = [&](Response &res) {
        if (reusable) pool_put(pool_key, std::move(c));
        log_fetch(std::to_string(res.status) + " " + url + (reused ? " (reused connection" : " (" + conn_note) +
                  (res.from_cache ? ", not modified: cache)" : ")") + " " + std::to_string(GetTickCount64() - t_start) + " ms");
    };

    if (r.status == 304 && have) {  // not modified: the stored copy is good for a while more
        int64_t now = cache::now_ms(), fresh = now;
        std::map<std::string, std::string> merged = hdrs;
        if (!merged.count("etag") && !cached.etag.empty()) merged["etag"] = cached.etag;
        if (!merged.count("last-modified") && !cached.last_modified.empty()) merged["last-modified"] = cached.last_modified;
        if (cache::freshness(cached.status, merged, now, fresh)) cache::set_fresh_until(url, fresh);
        Response hit = from_cache(cached, location);
        hit.zone = r.zone;
        if (location.empty()) stream_whole(stream, hit);
        finish(hit);
        return hit;
    }

    if (streaming) {  // the rest of the body, then the decoder's output is the body
        if (!pump(r.body, r.body.size(), true)) {
            r.status = 0;
            r.error = "could not decode the response: " + dec_err;
            r.body.clear();
            return r;
        }
        if (dec) {
            if (!dec->done() && !r.body.empty()) {
                r.status = 0;
                r.error = "could not decode the response: compressed data is truncated";
                r.body.clear();
                return r;
            }
            r.body.swap(dec->out);
        }
    } else if (!enc.empty() && enc != "identity" && !r.body.empty()) {
        std::vector<uint8_t> out;
        std::string err;
        bool ok = false;
        if (enc == "gzip" || enc == "x-gzip") ok = deflate::gzip(r.body.data(), r.body.size(), out, max_bytes, err);
        else if (enc == "deflate") {
            ok = deflate::zlib(r.body.data(), r.body.size(), out, max_bytes, err);
            if (!ok) { out.clear(); ok = deflate::raw(r.body.data(), r.body.size(), out, max_bytes, err); }
        } else err = "unsupported content encoding \"" + enc + "\"";
        if (!ok) {
            r.status = 0;
            r.error = "could not decode the response: " + err;
            r.body.clear();
            return r;
        }
        r.body.swap(out);
    }

    // ---- keep it for next time, if the server allows
    int64_t now = cache::now_ms(), fresh = 0;
    if (media) r.body.clear();
    if (!post && !media && cache::enabled() && cache::freshness(r.status, hdrs, now, fresh)) {
        cache::Entry e;
        e.url = url;
        e.status = r.status;
        e.content_type = r.content_type;
        e.location = location;
        e.etag = hdrs["etag"];
        e.last_modified = hdrs["last-modified"];
        if (lower(hdrs["vary"]).find("cookie") != std::string::npos) e.vary = vary_key(cookie);
        e.stored = now;
        e.fresh_until = fresh;
        e.zone = (int)r.zone;
        e.body = r.body;
        cache::put(e);
    } else if (!post && have) {
        cache::remove(url);  // changed into something that may not be stored
    }
    finish(r);
    return r;
}

}  // namespace

// ---------------------------------------------------------------------------------

bool Url::parse(const std::string &in) {
    std::string s = trim(in);
    size_t hash = s.find('#');
    if (hash != std::string::npos) s.resize(hash);
    size_t colon = s.find(':');
    if (colon == std::string::npos || !has_scheme(s)) return false;
    scheme = lower(s.substr(0, colon));
    std::string rest = s.substr(colon + 1);
    host.clear();
    port = 0;
    if (scheme == "about") { path = rest; return true; }
    if (rest.compare(0, 2, "//") != 0) {
        if (scheme != "file") return false;
        path = rest;  // file:C:/x
        if (path.empty() || path[0] != '/') path = "/" + path;
        return true;
    }
    rest = rest.substr(2);
    size_t end = rest.find_first_of("/?");
    std::string auth = rest.substr(0, end);
    path = end == std::string::npos ? "/" : rest.substr(end);
    if (path[0] == '?') path = "/" + path;
    size_t at = auth.rfind('@');
    if (at != std::string::npos) auth = auth.substr(at + 1);
    if (scheme == "file") {
        if (auth.size() == 2 && auth[1] == ':') { path = "/" + auth + path; auth.clear(); }  // file://C:/x
        host = auth;
        return true;
    }
    if (scheme != "http" && scheme != "https") return false;
    std::string port_str;
    if (!auth.empty() && auth[0] == '[') {
        size_t rb = auth.find(']');
        if (rb == std::string::npos) return false;
        host = auth.substr(1, rb - 1);
        if (rb + 1 < auth.size() && auth[rb + 1] == ':') port_str = auth.substr(rb + 2);
    } else {
        size_t pc = auth.rfind(':');
        host = auth.substr(0, pc);
        if (pc != std::string::npos) port_str = auth.substr(pc + 1);
    }
    host = lower(host);
    if (host.empty()) return false;
    path = normalize_path(path);
    port = default_port(scheme);
    if (!port_str.empty()) {
        for (char c : port_str) if (!isdigit((unsigned char)c)) return false;
        port = atoi(port_str.c_str());
        if (port <= 0 || port > 65535) return false;
    }
    return true;
}

std::string Url::str() const {
    if (scheme == "about") return "about:" + path;
    if (scheme == "file") return "file://" + host + path;
    std::string h = host.find(':') != std::string::npos ? "[" + host + "]" : host;
    if (port != default_port(scheme)) h += ":" + std::to_string(port);
    return scheme + "://" + h + path;
}

std::string Url::origin() const {
    Url u = *this;
    u.path = "/";
    return u.str();
}

std::string resolve(const std::string &base, const std::string &ref_in) {
    std::string ref = trim(ref_in);
    if (ref.empty()) return base;
    if (has_scheme(ref)) return ref;
    Url b;
    if (!b.parse(base)) return ref;
    if (ref.compare(0, 2, "//") == 0) return b.scheme + ":" + ref;
    if (b.scheme == "about") return ref;
    if (ref[0] == '#') return b.str();
    std::string path;
    std::string bpath = b.path.substr(0, b.path.find('?'));
    if (ref[0] == '/') path = ref;
    else if (ref[0] == '?') path = bpath + ref;
    else path = bpath.substr(0, bpath.rfind('/') + 1) + ref;
    b.path = normalize_path(path);
    return b.str();
}

std::string from_user_input(const std::string &typed) {
    std::string s = trim(typed);
    if (s.empty()) return s;
    bool drive = s.size() >= 3 && isalpha((unsigned char)s[0]) && s[1] == ':' && (s[2] == '\\' || s[2] == '/');
    bool unc = s.compare(0, 2, "\\\\") == 0;
    if (drive || unc) {
        std::replace(s.begin(), s.end(), '\\', '/');
        return drive ? "file:///" + s : "file:" + s;
    }
    if (has_scheme(s) && (s.find("://") != std::string::npos || lower(s).compare(0, 6, "about:") == 0 || lower(s).compare(0, 5, "file:") == 0))
        return s;
    return "http://" + s;
}

void init() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
}

namespace {

// GET/POST with redirects, no index.wasm handling.
Response fetch_exact(const std::string &url_in, Mode mode, size_t max_bytes, const std::string *post, Stream *stream,
                     CacheMode cmode, const cookies::Context *who, const Access &access) {
    std::string url = url_in;
    for (int hop = 0; hop < 10; hop++) {
        Url u;
        if (!u.parse(url)) {
            Response r;
            r.final_url = r.requested_url = url;
            r.error = "that is not a valid address";
            return r;
        }
        if (u.scheme == "file") return load_file(u, mode, max_bytes, stream, access);
        if (u.scheme != "http" && u.scheme != "https") {
            Response r;
            r.final_url = r.requested_url = url;
            r.error = "unsupported address scheme \"" + u.scheme + "\"";
            return r;
        }
        std::string location;
        Response r = http_request(u, mode, max_bytes, post, location, stream, cmode, who, access);
        if (r.status >= 300 && r.status < 400 && r.status != 304 && !location.empty()) {
            url = resolve(u.str(), location);
            if (r.status != 307 && r.status != 308) post = nullptr;  // "see other": continue with GET
            continue;
        }
        return r;
    }
    Response r;
    r.final_url = r.requested_url = url;
    r.error = "too many redirects";
    return r;
}

bool is_wasm(const Response &r) { return r.body.size() >= 4 && std::memcmp(r.body.data(), "\0asm", 4) == 0; }

}  // namespace

Response fetch(const std::string &url, Mode mode, size_t max_bytes, const std::string *post, Stream *stream, CacheMode cmode,
               const cookies::Context *who, const Access &access) {
    Url u;
    if (mode == Mode::Page && !post && u.parse(url) && (u.scheme == "http" || u.scheme == "https")) {
        std::string path = u.path.substr(0, u.path.find('?'));
        if (!path.empty() && path.back() == '/') {
            Url wasm_url = u;
            wasm_url.path.insert(path.size(), "index.wasm");
            Response r = fetch_exact(wasm_url.str(), mode, max_bytes, nullptr, nullptr, cmode, who, access);  // not streamed: may be a 404 page
            if (r.status == 200 && is_wasm(r)) {
                // show the directory, not ".../index.wasm"
                std::string f = r.final_url;
                std::string fp = f.substr(0, f.find('?'));
                if (fp.size() >= 10 && fp.compare(fp.size() - 10, 10, "index.wasm") == 0) f.erase(fp.size() - 10, 10);
                r.final_url = f;
                return r;
            }
            if (r.status == 0) {  // the server is unreachable; asking again would not help
                r.final_url = url;
                return r;
            }
        }
    }
    return fetch_exact(url, mode, max_bytes, post, stream, cmode, who, access);
}

Access access_for_page(const std::string &page_url, Zone page_zone) {
    Access a;
    Url u;
    if (u.parse(page_url) && u.scheme == "file") {
        std::string self = u.str();
        a.file_root = self.substr(0, self.rfind('/') + 1);
        return a;
    }
    a.lowest = page_zone;
    a.files = false;
    return a;
}

Zone zone_of_ip(const std::string &ip) {
    sockaddr_in v4{};
    sockaddr_in6 v6{};
    if (InetPtonA(AF_INET, ip.c_str(), &v4.sin_addr) == 1) {
        v4.sin_family = AF_INET;
        return zone_of((const sockaddr *)&v4);
    }
    if (InetPtonA(AF_INET6, ip.c_str(), &v6.sin6_addr) == 1) {
        v6.sin6_family = AF_INET6;
        return zone_of((const sockaddr *)&v6);
    }
    return Zone::Public;
}

void set_logger(std::function<void(const std::string &)> log) { g_logger = std::move(log); }
void set_keep_alive(bool on) { g_keep_alive = on; }
void set_http2(bool on) { g_http2 = on; }

}  // namespace net
