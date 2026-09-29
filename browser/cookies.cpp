// cookies.cpp — see cookies.h.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "cookies.h"

#include <algorithm>
#include <mutex>

#include "cache.h"
#include "net.h"

namespace cookies {

namespace {

struct Cookie {
    std::string name, value, domain, path;
    int64_t expires = 0;  // unix ms; 0 = session cookie
    int64_t created = 0, last_used = 0;
    bool host_only = true, secure = false, http_only = false;
    int same_site = 1;  // 0 None, 1 Lax (also the default), 2 Strict
};

std::mutex g_m;
std::vector<Cookie> g_jar;
std::wstring g_file;
const size_t kPerDomain = 180, kTotal = 3000;

std::string lower(std::string s) {
    for (char &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

bool ends_with(const std::string &s, const std::string &tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

bool is_ip(const std::string &host) {
    if (host.find(':') != std::string::npos) return true;  // IPv6
    return !host.empty() && host.find_first_not_of("0123456789.") == std::string::npos;
}

// Suffixes under which anyone can register a name (a small part of the Public Suffix List:
// every top-level domain, plus common second-level ones and shared hosting domains).
const char *kSuffixes[] = {
    "co.th", "ac.th", "go.th", "in.th", "or.th", "net.th", "mi.th", "co.uk", "org.uk", "ac.uk", "gov.uk", "me.uk",
    "ltd.uk", "plc.uk", "com.au", "net.au", "org.au", "edu.au", "gov.au", "co.jp", "ne.jp", "or.jp", "ac.jp", "go.jp",
    "co.nz", "org.nz", "com.br", "com.cn", "net.cn", "org.cn", "com.sg", "com.my", "com.hk", "com.tw", "co.kr", "or.kr",
    "co.in", "net.in", "org.in", "co.za", "com.mx", "com.ar", "com.tr", "com.vn", "co.id", "com.ph", "com.pk",
    "github.io", "gitlab.io", "blogspot.com", "pages.dev", "workers.dev", "netlify.app", "vercel.app", "web.app",
    "firebaseapp.com", "herokuapp.com", "appspot.com", "azurewebsites.net", "cloudfront.net", "s3.amazonaws.com",
};

bool is_public_suffix(const std::string &d) {
    if (d.find('.') == std::string::npos) return true;  // a top-level domain (or "localhost")
    for (const char *s : kSuffixes)
        if (d == s) return true;
    return false;
}

std::string host_of(const std::string &url) {
    net::Url u;
    return u.parse(url) ? lower(u.host) : std::string();
}

bool domain_match(const std::string &host, const std::string &domain) {
    if (host == domain) return true;
    return !is_ip(host) && ends_with(host, "." + domain);
}

bool path_match(const std::string &req, const std::string &cookie) {
    if (req == cookie) return true;
    if (req.compare(0, cookie.size(), cookie) != 0) return false;
    return cookie.back() == '/' || (req.size() > cookie.size() && req[cookie.size()] == '/');
}

std::string default_path(const std::string &path) {  // the "directory" of the request path
    std::string p = path.substr(0, path.find('?'));
    if (p.empty() || p[0] != '/') return "/";
    size_t slash = p.rfind('/');
    return slash == 0 ? "/" : p.substr(0, slash);
}

// ---- the file: one cookie per line, tab-separated -------------------------------------

void save_locked() {
    if (g_file.empty()) return;
    std::string out = "# Low-web cookies: domain host_only path secure http_only same_site expires created name value\n";
    int64_t now = cache::now_ms();
    for (const Cookie &c : g_jar) {
        if (!c.expires || c.expires <= now) continue;  // session cookies end with the browser
        out += c.domain + "\t" + (c.host_only ? "1" : "0") + "\t" + c.path + "\t" + (c.secure ? "1" : "0") + "\t" +
               (c.http_only ? "1" : "0") + "\t" + std::to_string(c.same_site) + "\t" + std::to_string(c.expires) + "\t" +
               std::to_string(c.created) + "\t" + c.name + "\t" + c.value + "\n";
    }
    std::wstring tmp = g_file + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    bool ok = WriteFile(f, out.data(), (DWORD)out.size(), &put, nullptr) && put == out.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), g_file.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
}

void load_locked() {
    HANDLE f = CreateFileW(g_file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    std::string d;
    if (GetFileSizeEx(f, &sz) && sz.QuadPart < (16 << 20)) {
        d.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        if (!ReadFile(f, d.data(), (DWORD)d.size(), &got, nullptr)) d.clear();
        d.resize(got);
    }
    CloseHandle(f);
    int64_t now = cache::now_ms();
    size_t p = 0;
    while (p < d.size()) {
        size_t e = d.find('\n', p);
        if (e == std::string::npos) e = d.size();
        std::string line = d.substr(p, e - p);
        p = e + 1;
        if (line.empty() || line[0] == '#') continue;
        std::vector<std::string> f;
        size_t q = 0;
        for (int i = 0; i < 9; i++) {
            size_t t = line.find('\t', q);
            if (t == std::string::npos) break;
            f.push_back(line.substr(q, t - q));
            q = t + 1;
        }
        if (f.size() != 9) continue;
        Cookie c;
        c.domain = f[0];
        c.host_only = f[1] == "1";
        c.path = f[2];
        c.secure = f[3] == "1";
        c.http_only = f[4] == "1";
        c.same_site = atoi(f[5].c_str());
        c.expires = _atoi64(f[6].c_str());
        c.created = c.last_used = _atoi64(f[7].c_str());
        c.name = f[8];
        c.value = line.substr(q);
        if (c.expires > now) g_jar.push_back(c);
    }
}

// One Set-Cookie line (RFC 6265 section 5.2, with the SameSite and prefix rules of 6265bis).
bool parse(const std::string &line, const std::string &host, const std::string &path, bool https, Cookie &c,
           bool &remove) {
    size_t semi = line.find(';');
    std::string pair = line.substr(0, semi);
    size_t eq = pair.find('=');
    c.name = eq == std::string::npos ? "" : trim(pair.substr(0, eq));
    c.value = trim(eq == std::string::npos ? pair : pair.substr(eq + 1));
    if (c.name.empty() && c.value.empty()) return false;
    if (c.name.size() + c.value.size() > 4096) return false;
    int64_t now = cache::now_ms();
    bool have_max_age = false, have_domain = false;
    int64_t max_age = 0, expires = 0;
    c.same_site = 1;
    bool same_site_given = false;
    while (semi != std::string::npos) {
        size_t next = line.find(';', semi + 1);
        std::string av = line.substr(semi + 1, next == std::string::npos ? std::string::npos : next - semi - 1);
        semi = next;
        size_t e = av.find('=');
        std::string k = lower(trim(av.substr(0, e))), v = e == std::string::npos ? "" : trim(av.substr(e + 1));
        if (k == "expires") {
            int64_t t = cache::parse_http_date(v);
            if (t >= 0) expires = t * 1000;
        } else if (k == "max-age") {
            if (!v.empty() && (isdigit((unsigned char)v[0]) || v[0] == '-')) {
                have_max_age = true;
                max_age = _atoi64(v.c_str());
            }
        } else if (k == "domain") {
            std::string d = lower(v);
            if (!d.empty() && d[0] == '.') d.erase(0, 1);
            if (!d.empty()) { c.domain = d; have_domain = true; }
        } else if (k == "path") {
            if (!v.empty() && v[0] == '/') c.path = v;
        } else if (k == "secure") {
            c.secure = true;
        } else if (k == "httponly") {
            c.http_only = true;
        } else if (k == "samesite") {
            std::string s = lower(v);
            same_site_given = true;
            c.same_site = s == "none" ? 0 : s == "strict" ? 2 : 1;
        }
    }
    (void)same_site_given;
    // domain: a cookie may be for this host's own site, never for a public suffix like "co.th"
    if (have_domain) {
        if (is_ip(host) || is_public_suffix(c.domain)) {
            if (c.domain != host) return false;
            have_domain = false;  // "Domain=" naming the host itself: just this host
        } else if (!domain_match(host, c.domain)) {
            return false;
        }
    }
    if (!have_domain) c.domain = host;
    c.host_only = !have_domain;
    if (c.path.empty()) c.path = default_path(path);
    // security rules
    if (c.secure && !https) return false;
    if (c.same_site == 0 && !c.secure) return false;  // SameSite=None needs Secure
    if (c.name.compare(0, 9, "__Secure-") == 0 && !c.secure) return false;
    if (c.name.compare(0, 7, "__Host-") == 0 && (!c.secure || !c.host_only || c.path != "/")) return false;
    // lifetime: Max-Age wins over Expires; one in the past deletes the cookie
    remove = false;
    if (have_max_age) {
        if (max_age <= 0) remove = true;
        else c.expires = now + std::min<int64_t>(max_age, 400ll * 86400) * 1000;  // at most 400 days
    } else if (expires) {
        if (expires <= now) remove = true;
        else c.expires = std::min<int64_t>(expires, now + 400ll * 86400 * 1000);
    }
    c.created = c.last_used = now;
    return true;
}

}  // namespace

std::string registrable_domain(const std::string &host_in) {
    std::string host = lower(host_in);
    if (is_ip(host) || host.find('.') == std::string::npos) return host;
    for (const char *s : kSuffixes) {
        std::string suf = s;
        if (host == suf) return host;
        if (ends_with(host, "." + suf)) {
            std::string rest = host.substr(0, host.size() - suf.size() - 1);
            size_t dot = rest.rfind('.');
            return (dot == std::string::npos ? rest : rest.substr(dot + 1)) + "." + suf;
        }
    }
    size_t last = host.rfind('.');
    size_t prev = host.rfind('.', last - 1);
    return prev == std::string::npos ? host : host.substr(prev + 1);
}

bool same_site(const std::string &a, const std::string &b) { return registrable_domain(a) == registrable_domain(b); }

void init(const std::wstring &file) {
    std::lock_guard<std::mutex> lock(g_m);
    g_file = file;
    g_jar.clear();
    if (!file.empty()) load_locked();
}

std::string header_for(const std::string &url, const Context &ctx) {
    net::Url u;
    if (!u.parse(url) || (u.scheme != "http" && u.scheme != "https")) return "";
    std::string host = lower(u.host), path = u.path.substr(0, u.path.find('?'));
    if (path.empty()) path = "/";
    bool https = u.scheme == "https";
    bool cross = !ctx.site_for.empty() && !same_site(host, host_of(ctx.site_for));
    if (cross && !ctx.navigation) return "";  // third-party: nothing
    int64_t now = cache::now_ms();
    std::lock_guard<std::mutex> lock(g_m);
    std::vector<Cookie *> out;
    for (Cookie &c : g_jar) {
        if (c.expires && c.expires <= now) continue;
        if (c.host_only ? host != c.domain : !domain_match(host, c.domain)) continue;
        if (!path_match(path, c.path)) continue;
        if (c.secure && !https) continue;
        if (cross && (c.same_site == 2 || (c.same_site == 1 && ctx.unsafe_method))) continue;
        out.push_back(&c);
    }
    std::stable_sort(out.begin(), out.end(), [](const Cookie *a, const Cookie *b) {
        return a->path.size() != b->path.size() ? a->path.size() > b->path.size() : a->created < b->created;
    });
    std::string h;
    for (Cookie *c : out) {
        c->last_used = now;
        if (!h.empty()) h += "; ";
        h += c->name.empty() ? c->value : c->name + "=" + c->value;
    }
    return h;
}

void store(const std::string &url, const std::vector<std::string> &set_cookie, const Context &ctx) {
    if (set_cookie.empty()) return;
    net::Url u;
    if (!u.parse(url) || (u.scheme != "http" && u.scheme != "https")) return;
    std::string host = lower(u.host);
    bool cross = !ctx.site_for.empty() && !same_site(host, host_of(ctx.site_for));
    if (cross && !ctx.navigation) return;  // third-party: refused
    std::lock_guard<std::mutex> lock(g_m);
    bool persistent_changed = false;
    for (const std::string &line : set_cookie) {
        Cookie c;
        bool remove = false;
        if (!parse(line, host, u.path, u.scheme == "https", c, remove)) continue;
        auto same = [&](const Cookie &o) { return o.name == c.name && o.domain == c.domain && o.path == c.path; };
        auto it = std::find_if(g_jar.begin(), g_jar.end(), same);
        if (it != g_jar.end()) {
            persistent_changed |= it->expires != 0;
            if (remove) { g_jar.erase(it); continue; }
            c.created = it->created;  // an update keeps its place in the order
            *it = c;
        } else if (!remove) {
            g_jar.push_back(c);
        }
        persistent_changed |= c.expires != 0;
    }
    // limits: per domain and in all; the least recently used go first
    int64_t now = cache::now_ms();
    g_jar.erase(std::remove_if(g_jar.begin(), g_jar.end(), [&](const Cookie &c) { return c.expires && c.expires <= now; }),
                g_jar.end());
    auto evict = [&](auto pred, size_t limit) {
        std::vector<size_t> idx;
        for (size_t i = 0; i < g_jar.size(); i++)
            if (pred(g_jar[i])) idx.push_back(i);
        if (idx.size() <= limit) return;
        std::sort(idx.begin(), idx.end(), [](size_t a, size_t b) { return g_jar[a].last_used < g_jar[b].last_used; });
        std::vector<bool> drop(g_jar.size());
        for (size_t k = 0; k < idx.size() - limit; k++) drop[idx[k]] = true;
        size_t w = 0;
        for (size_t i = 0; i < g_jar.size(); i++)
            if (!drop[i]) g_jar[w++] = g_jar[i];
        g_jar.resize(w);
        persistent_changed = true;
    };
    std::string site = registrable_domain(host);
    evict([&](const Cookie &c) { return registrable_domain(c.domain) == site; }, kPerDomain);
    evict([](const Cookie &) { return true; }, kTotal);
    if (persistent_changed) save_locked();
}

void clear() {
    std::lock_guard<std::mutex> lock(g_m);
    g_jar.clear();
    save_locked();
}

size_t count() {
    std::lock_guard<std::mutex> lock(g_m);
    return g_jar.size();
}

}  // namespace cookies
