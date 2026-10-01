// cache.h — the HTTP cache: responses kept in memory and on disk, so pages, stylesheets
// and images need not be downloaded again. net.cpp decides what may be stored and for how
// long (Cache-Control, Expires, ETag, Last-Modified); this file only keeps entries.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace cache {

struct Entry {
    std::string url;
    int status = 0;
    std::string content_type, location;  // location: for cached redirects
    std::string etag, last_modified;     // validators, for "has it changed?" requests
    std::string vary;                    // "Vary: Cookie": which cookies it was made for (a hash)
    int64_t stored = 0;                  // when it was received (unix ms)
    int64_t fresh_until = 0;             // usable without asking the server until then (unix ms)
    int zone = 0;                        // net::Zone of the server that sent it (older entries: 0 = this computer)
    std::vector<uint8_t> body;           // decoded (not compressed)
};

// `dir` empty = keep nothing (--no-cache). The disk part is trimmed to `max_disk_bytes`.
void init(const std::wstring &dir, uint64_t max_disk_bytes);
bool enabled();

bool get(const std::string &url, Entry &out);  // a stored entry, fresh or not
void put(const Entry &e);
void set_fresh_until(const std::string &url, int64_t fresh_until);  // after a 304 Not Modified
void remove(const std::string &url);

int64_t now_ms();  // unix time in ms
int64_t parse_http_date(const std::string &s);  // unix seconds, or -1

// How long a response may be used without asking the server again, from its headers
// (lower-case names). Returns false if it must not be stored at all.
bool freshness(int status, const std::map<std::string, std::string> &hdrs, int64_t now, int64_t &fresh_until);

}  // namespace cache
