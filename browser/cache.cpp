// cache.cpp — see cache.h. Recently used entries stay in memory (up to 48 MB); every entry
// is also a file <hash>.lwc in the cache folder (a few text header lines, then the body),
// trimmed oldest-first when the folder grows past its limit.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "cache.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <list>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace cache {

namespace {

std::mutex g_m;
std::wstring g_dir;
bool g_on = false;
uint64_t g_max_disk = 0;
std::atomic<uint64_t> g_disk_bytes{0};
std::atomic<bool> g_trimming{false};

const size_t kMemBytes = 16u << 20;  // the disk layer has the rest, a few ms away
const size_t kMaxEntry = 64u << 20;  // bigger bodies are not stored

struct MemEntry {
    Entry e;
    std::list<std::string>::iterator pos;
};
std::list<std::string> g_lru;  // front = most recently used
std::unordered_map<std::string, MemEntry> g_mem;
size_t g_mem_bytes = 0;

std::wstring path_for(const std::string &url) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : url) h = (h ^ c) * 1099511628211ull;
    wchar_t name[32];
    swprintf(name, 32, L"%016llx.lwc", (unsigned long long)h);
    return g_dir + L"\\" + name;
}

// ---- memory (g_m held) ----

void mem_drop(const std::string &url) {
    auto it = g_mem.find(url);
    if (it == g_mem.end()) return;
    g_mem_bytes -= it->second.e.body.size();
    g_lru.erase(it->second.pos);
    g_mem.erase(it);
}

void mem_put(const Entry &e) {
    mem_drop(e.url);
    if (e.body.size() > kMemBytes / 8) return;
    g_lru.push_front(e.url);
    g_mem[e.url] = MemEntry{e, g_lru.begin()};
    g_mem_bytes += e.body.size();
    while (g_mem_bytes > kMemBytes && !g_lru.empty()) mem_drop(g_lru.back());
}

bool mem_get(const std::string &url, Entry &out) {
    auto it = g_mem.find(url);
    if (it == g_mem.end()) return false;
    g_lru.splice(g_lru.begin(), g_lru, it->second.pos);
    out = it->second.e;
    return true;
}

// ---- disk ----

bool read_file(const std::wstring &path, std::vector<uint8_t> &out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    bool ok = GetFileSizeEx(f, &sz) && sz.QuadPart < (1ll << 31);
    if (ok) {
        out.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        ok = out.empty() || (ReadFile(f, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size());
    }
    if (ok) {  // remember the use: trimming removes the least recently used files first
        FILETIME now;
        GetSystemTimeAsFileTime(&now);
        HANDLE w = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (w != INVALID_HANDLE_VALUE) {
            SetFileTime(w, nullptr, nullptr, &now);
            CloseHandle(w);
        }
    }
    CloseHandle(f);
    return ok;
}

bool disk_get(const std::string &url, Entry &e) {
    std::vector<uint8_t> d;
    if (!read_file(path_for(url), d)) return false;
    std::string head;
    size_t p = 0;
    for (;;) {  // header lines "key value", ended by an empty line
        size_t nl = p;
        while (nl < d.size() && d[nl] != '\n') nl++;
        if (nl >= d.size()) return false;
        std::string line(d.begin() + p, d.begin() + nl);
        p = nl + 1;
        if (line.empty()) break;
        size_t sp = line.find(' ');
        std::string k = line.substr(0, sp), v = sp == std::string::npos ? "" : line.substr(sp + 1);
        if (k == "LWC1") continue;
        if (k == "url") e.url = v;
        else if (k == "status") e.status = atoi(v.c_str());
        else if (k == "type") e.content_type = v;
        else if (k == "location") e.location = v;
        else if (k == "etag") e.etag = v;
        else if (k == "lastmod") e.last_modified = v;
        else if (k == "vary") e.vary = v;
        else if (k == "stored") e.stored = _atoi64(v.c_str());
        else if (k == "fresh") e.fresh_until = _atoi64(v.c_str());
        else if (k == "zone") e.zone = atoi(v.c_str());
    }
    if (e.url != url) return false;  // another URL with the same hash
    e.body.assign(d.begin() + p, d.end());
    return true;
}

void trim() {  // oldest files first, down to 80% of the limit
    struct F { FILETIME t; uint64_t size; std::wstring name; };
    std::vector<F> files;
    uint64_t total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((g_dir + L"\\*.lwc").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            uint64_t size = (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
            files.push_back({fd.ftLastWriteTime, size, fd.cFileName});
            total += size;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    if (total > g_max_disk) {
        std::sort(files.begin(), files.end(), [](const F &a, const F &b) { return CompareFileTime(&a.t, &b.t) < 0; });
        for (const F &f : files) {
            if (total <= g_max_disk / 10 * 8) break;
            if (DeleteFileW((g_dir + L"\\" + f.name).c_str())) total -= f.size;
        }
    }
    g_disk_bytes = total;
}

void trim_later() {
    if (g_trimming.exchange(true)) return;
    std::thread([] {
        trim();
        g_trimming = false;
    }).detach();
}

void disk_put(const Entry &e) {
    std::string head = "LWC1\nurl " + e.url + "\nstatus " + std::to_string(e.status) + "\ntype " + e.content_type +
                       "\nlocation " + e.location + "\netag " + e.etag + "\nlastmod " + e.last_modified + "\nvary " + e.vary + "\nstored " +
                       std::to_string(e.stored) + "\nfresh " + std::to_string(e.fresh_until) + "\nzone " + std::to_string(e.zone) + "\n\n";
    std::wstring path = path_for(e.url);
    wchar_t suffix[40];
    swprintf(suffix, 40, L".%lu.tmp", GetCurrentThreadId());
    std::wstring tmp = path + suffix;
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    bool ok = WriteFile(f, head.data(), (DWORD)head.size(), &put, nullptr) && put == head.size();
    if (ok && !e.body.empty()) ok = WriteFile(f, e.body.data(), (DWORD)e.body.size(), &put, nullptr) && put == e.body.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return;
    }
    if ((g_disk_bytes += head.size() + e.body.size()) > g_max_disk) trim_later();
}

// ---- header parsing ----

std::string lower(std::string s) {
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string header(const std::map<std::string, std::string> &h, const char *name) {
    auto it = h.find(name);
    return it == h.end() ? std::string() : it->second;
}

// Cache-Control: finds a directive; `value` gets what follows '=' (without quotes).
bool directive(const std::string &cc, const char *name, std::string *value = nullptr) {
    size_t p = 0;
    while (p < cc.size()) {
        size_t e = cc.find(',', p);
        if (e == std::string::npos) e = cc.size();
        std::string part = cc.substr(p, e - p);
        p = e + 1;
        size_t a = part.find_first_not_of(" \t"), b = part.find_last_not_of(" \t");
        if (a == std::string::npos) continue;
        part = part.substr(a, b - a + 1);
        size_t eq = part.find('=');
        std::string key = part.substr(0, eq);
        while (!key.empty() && key.back() == ' ') key.pop_back();
        if (key != name) continue;
        if (value) {
            std::string v = eq == std::string::npos ? "" : part.substr(eq + 1);
            v.erase(0, v.find_first_not_of(" \t\""));
            while (!v.empty() && (v.back() == '"' || v.back() == ' ')) v.pop_back();
            *value = v;
        }
        return true;
    }
    return false;
}

// HTTP dates: "Sun, 06 Nov 1994 08:49:37 GMT", "Sunday, 06-Nov-94 08:49:37 GMT" and
// "Sun Nov  6 08:49:37 1994". Returns unix seconds, or -1.
int64_t http_date(const std::string &s) {
    static const char *months[12] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
    int day = -1, mon = -1, year = -1, hh = -1, mm = -1, ss = -1;
    std::string t = lower(s);
    size_t i = 0;
    while (i < t.size()) {
        while (i < t.size() && (t[i] == ' ' || t[i] == ',' || t[i] == '-')) i++;
        size_t j = i;
        while (j < t.size() && t[j] != ' ' && t[j] != ',' && t[j] != '-') j++;
        std::string tok = t.substr(i, j - i);
        i = j;
        if (tok.empty()) continue;
        if (tok.find(':') != std::string::npos) sscanf(tok.c_str(), "%d:%d:%d", &hh, &mm, &ss);
        else if (isdigit((unsigned char)tok[0])) {
            int v = atoi(tok.c_str());
            if (tok.size() >= 3 || day >= 0) year = tok.size() == 2 ? (v < 70 ? 2000 + v : 1900 + v) : v;
            else day = v;
        } else {
            for (int m = 0; m < 12; m++)
                if (tok.compare(0, 3, months[m]) == 0 && tok.size() <= 9 && mon < 0 && tok != "mon") mon = m;
        }
    }
    if (day < 1 || mon < 0 || year < 1970 || hh < 0 || mm < 0 || ss < 0) return -1;
    // days from 1970-01-01 (Howard Hinnant's days_from_civil)
    int y = year - (mon < 2), m = mon + 1;
    int era = y / 400, yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = (int64_t)era * 146097 + doe - 719468;
    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

}  // namespace

int64_t now_ms() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;  // 100 ns since 1601
    return (int64_t)(t / 10000) - 11644473600000ll;
}

void init(const std::wstring &dir, uint64_t max_disk_bytes) {
    std::lock_guard<std::mutex> lock(g_m);
    g_dir = dir;
    g_max_disk = max_disk_bytes;
    g_on = !dir.empty();
    if (!g_on) return;
    // create the folder (and its parents)
    for (size_t p = dir.find(L'\\', 3); ; p = dir.find(L'\\', p + 1)) {
        CreateDirectoryW(dir.substr(0, p).c_str(), nullptr);
        if (p == std::wstring::npos) break;
    }
    trim_later();  // also counts what is there
}

bool enabled() { return g_on; }

size_t memory_bytes() {
    std::lock_guard<std::mutex> lock(g_m);
    return g_mem_bytes;
}

int64_t parse_http_date(const std::string &s) { return http_date(s); }

bool get(const std::string &url, Entry &out) {
    if (!g_on) return false;
    {
        std::lock_guard<std::mutex> lock(g_m);
        if (mem_get(url, out)) return true;
    }
    if (!disk_get(url, out)) return false;
    std::lock_guard<std::mutex> lock(g_m);
    mem_put(out);
    return true;
}

void put(const Entry &e) {
    if (!g_on || e.body.size() > kMaxEntry) return;
    {
        std::lock_guard<std::mutex> lock(g_m);
        mem_put(e);
    }
    disk_put(e);
}

void set_fresh_until(const std::string &url, int64_t fresh_until) {
    Entry e;
    if (!get(url, e)) return;
    e.fresh_until = fresh_until;
    put(e);
}

void remove(const std::string &url) {
    if (!g_on) return;
    {
        std::lock_guard<std::mutex> lock(g_m);
        mem_drop(url);
    }
    DeleteFileW(path_for(url).c_str());
}

bool freshness(int status, const std::map<std::string, std::string> &hdrs, int64_t now, int64_t &fresh_until) {
    static const int storable[] = {200, 203, 301, 308, 404, 410};
    if (std::find(std::begin(storable), std::end(storable), status) == std::end(storable)) return false;
    std::string cc = lower(header(hdrs, "cache-control"));
    if (directive(cc, "no-store") || header(hdrs, "vary").find('*') != std::string::npos) return false;
    int64_t date = http_date(header(hdrs, "date"));
    if (date < 0) date = now / 1000;
    int64_t lifetime = 0;  // seconds
    std::string v;
    if (directive(cc, "max-age", &v)) lifetime = _atoi64(v.c_str());
    else if (!header(hdrs, "expires").empty()) lifetime = std::max<int64_t>(0, http_date(header(hdrs, "expires")) - date);
    else if (int64_t lm = http_date(header(hdrs, "last-modified")); lm > 0 && status == 200)
        lifetime = std::min<int64_t>(std::max<int64_t>(0, (date - lm) / 10), 86400);  // the usual 10% rule, at most a day
    if (directive(cc, "no-cache") || lower(header(hdrs, "pragma")).find("no-cache") != std::string::npos) lifetime = 0;
    lifetime -= _atoi64(header(hdrs, "age").c_str());
    if (lifetime <= 0 && header(hdrs, "etag").empty() && header(hdrs, "last-modified").empty())
        return false;  // stale at once and no way to check it later: no use keeping
    fresh_until = now + std::max<int64_t>(0, lifetime) * 1000;
    return true;
}

}  // namespace cache
