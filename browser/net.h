// net.h — URLs and fetching: http:// and https:// over raw Winsock sockets (TLS via
// the Windows SChannel API), plus file:// for local development.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "cookies.h"

namespace net {

struct Url {
    std::string scheme;  // "http", "https", "file", "about"
    std::string host;    // without brackets for IPv6
    int port = 0;
    std::string path;    // starts with '/', includes the query; for file: "/C:/dir/x.wasm"

    bool parse(const std::string &s);
    std::string str() const;
    std::string origin() const;
};

// Resolves `ref` against `base` (RFC 3986-style, simplified).
std::string resolve(const std::string &base, const std::string &ref);

// Turns what the user typed into a URL: adds http://, converts Windows paths to file://.
std::string from_user_input(const std::string &typed);

struct Response {
    int status = 0;                 // HTTP status, or 200 for files; 0 if the request failed
    std::string error;              // set when status == 0
    std::string content_type;
    std::string final_url;          // the document's address after redirects (what the user sees)
    std::string requested_url;      // the last URL actually requested (may end in index.wasm)
    std::vector<uint8_t> body;
    bool from_cache = false;        // served from the HTTP cache (maybe after a "not modified")
};

enum class Mode {
    // Navigation: for an address ending in '/', first try "<dir>/index.wasm" (a Low-web
    // page). If that is missing or not WebAssembly, load the address itself, so ordinary
    // web sites answer with their normal page.
    Page,
    // Exactly the URL given (lw_fetch, images, ...).
    Exact,
};

// How to use the HTTP cache (cache.h) for one request.
enum class CacheMode {
    Normal,        // use a fresh copy; else ask the server whether a stored one changed
    Revalidate,    // always ask first (reload)
    Reload,        // don't use stored copies (hard reload); the answer is still stored
    PreferCached,  // back/forward: a stored copy even if old, unless it says "always ask"
};

// Receives a body while it downloads (already de-chunked and decompressed).
struct Stream {
    virtual ~Stream() = default;
    // Called once the final response's headers are in (not for redirects). `head` has
    // everything but the body. Return true to get the body through data().
    virtual bool begin(const Response &head) = 0;
    virtual void data(const uint8_t *bytes, size_t n) = 0;
};

// Blocking GET (or POST when `post` is given: an application/x-www-form-urlencoded
// body). Follows redirects and decodes gzip/deflate bodies. With a `stream`, the body is
// also handed over piece by piece as it arrives; the returned Response still has all of it.
// `who`: the page the request is for (cookies are sent and stored only for its own site);
// null = the user (a typed address): all of the site's cookies.
Response fetch(const std::string &url, Mode mode, size_t max_bytes = 256u << 20, const std::string *post = nullptr,
               Stream *stream = nullptr, CacheMode cache = CacheMode::Normal, const cookies::Context *who = nullptr);

// Where "[net] ..." lines about each request go (off by default).
void set_logger(std::function<void(const std::string &)> log);
// Keep-alive can be switched off (tests: to see every connection's handshake).
void set_keep_alive(bool on);
// HTTP/2 can be switched off (tests: to compare with HTTP/1.1).
void set_http2(bool on);

// Call once at startup (WSAStartup).
void init();

}  // namespace net
