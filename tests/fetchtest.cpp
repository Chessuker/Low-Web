// fetchtest URL...   — fetch with the browser's network stack and print a summary.
// fetchtest --resolve BASE REF...  — print resolved URLs.
// fetchtest --stream URL...  — also receive the body piece by piece and check it matches.
// fetchtest --parallel URL...  — fetch them all at once (HTTP/2 shares one connection)
// fetchtest --save DIR URL...  — write each body to DIR/N.body (N counts from 0)
// fetchtest --cache DIR URL...  — use (and fill) an HTTP cache in DIR; --revalidate / --reload
//   choose how (see net::CacheMode). Each request is logged: cache hit, reused connection...
// fetchtest --page PAGE ZONE URL...  — fetch as page PAGE would (lw_fetch): with the limits of
//   net::access_for_page(PAGE, ZONE), ZONE = local, private or public.
// fetchtest --zone IP...  — print the address space of each IP address.
#include <chrono>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include "../browser/cache.h"
#include <cstdio>
#include <cstring>
#include "../browser/net.h"

static const char *zone_word(net::Zone z) {
    return z == net::Zone::Local ? "local" : z == net::Zone::Private ? "private" : "public";
}

struct Collect : net::Stream {
    std::vector<uint8_t> got;
    int pieces = 0;
    bool began = false;
    double t0 = now(), first_ms = -1;
    static double now() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
    bool begin(const net::Response &) override { began = true; return true; }
    void data(const uint8_t *d, size_t n) override {
        if (first_ms < 0) first_ms = now() - t0;
        got.insert(got.end(), d, d + n);
        pieces++;
    }
};
int main(int argc, char **argv) {
    net::init();
    if (argc >= 3 && !strcmp(argv[1], "--resolve")) {
        for (int i = 3; i < argc; i++) printf("%-14s -> %s\n", argv[i], net::resolve(argv[2], argv[i]).c_str());
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "--zone")) {
        for (int i = 2; i < argc; i++) printf("%-24s -> %s\n", argv[i], zone_word(net::zone_of_ip(argv[i])));
        return 0;
    }
    if (argc >= 2 && !strcmp(argv[1], "--input")) {
        for (int i = 2; i < argc; i++) printf("%-24s -> %s\n", argv[i], net::from_user_input(argv[i]).c_str());
        return 0;
    }
    net::Mode mode = net::Mode::Page;
    net::CacheMode cmode = net::CacheMode::Normal;
    cookies::Context who;
    net::Access access;
    net::set_logger([](const std::string &s) { printf("  %s\n", s.c_str()); });
    bool stream = false, parallel = false;
    std::string save_dir;
    std::vector<std::string> urls;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--exact")) { mode = net::Mode::Exact; continue; }
        if (!strcmp(argv[i], "--stream")) { stream = true; continue; }
        if (!strcmp(argv[i], "--cookies") && i + 1 < argc) {  // a cookie file
            std::string f = argv[++i];
            cookies::init(std::wstring(f.begin(), f.end()));
            continue;
        }
        if (!strcmp(argv[i], "--site") && i + 1 < argc) { who.site_for = argv[++i]; continue; }  // requests made for this page
        if (!strcmp(argv[i], "--nav")) { who.navigation = true; continue; }
        if (!strcmp(argv[i], "--page") && i + 2 < argc) {
            std::string page = argv[++i], z = argv[++i];
            access = net::access_for_page(page, z == "local" ? net::Zone::Local : z == "private" ? net::Zone::Private : net::Zone::Public);
            continue;
        }
        if (!strcmp(argv[i], "--parallel")) { parallel = true; continue; }
        if (!strcmp(argv[i], "--save") && i + 1 < argc) { save_dir = argv[++i]; continue; }
        if (!strcmp(argv[i], "--no-h2")) { net::set_http2(false); continue; }
        if (!strcmp(argv[i], "--no-keepalive")) { net::set_keep_alive(false); continue; }
        if (!strcmp(argv[i], "--revalidate")) { cmode = net::CacheMode::Revalidate; continue; }
        if (!strcmp(argv[i], "--reload")) { cmode = net::CacheMode::Reload; continue; }
        if (!strcmp(argv[i], "--cache") && i + 1 < argc) {
            std::string d = argv[++i];
            cache::init(std::wstring(d.begin(), d.end()), 256u << 20);
            continue;
        }
        if (parallel) { urls.push_back(argv[i]); continue; }
        double t0 = Collect::now();
        Collect c;
        net::Response r = net::fetch(argv[i], mode, 256u << 20, nullptr, stream ? &c : nullptr, cmode, &who, access);
        printf("  %.0f ms%s\n", Collect::now() - t0, r.from_cache ? ", from the cache" : "");
        if (stream)
            printf("  stream: %s, %d pieces, first after %.0f ms, total %.0f ms, %s\n", c.began ? "began" : "not begun", c.pieces,
                   c.first_ms, Collect::now() - c.t0, c.got == r.body ? "same bytes as the body" : "DIFFERENT FROM THE BODY");
        if (!r.status) { printf("%s\n  ERROR: %s\n", argv[i], r.error.c_str()); continue; }
        std::string head(r.body.begin(), r.body.begin() + std::min<size_t>(r.body.size(), 60));
        for (char &c : head) if (c < 32) c = '.';
        printf("%s\n  %d  %s  %zu bytes  final=%s  requested=%s  zone=%s\n  \"%s\"\n", argv[i], r.status, r.content_type.c_str(),
               r.body.size(), r.final_url.c_str(), r.requested_url.c_str(), zone_word(r.zone), head.c_str());
    }
    if (parallel) {
        double t0 = Collect::now();
        std::vector<net::Response> rs(urls.size());
        std::vector<std::thread> ts;
        for (size_t k = 0; k < urls.size(); k++)
            ts.emplace_back([&, k] { rs[k] = net::fetch(urls[k], mode, 256u << 20, nullptr, nullptr, cmode, &who, access); });
        for (auto &t : ts) t.join();
        size_t bytes = 0, ok = 0;
        for (size_t k = 0; k < rs.size(); k++) {
            bytes += rs[k].body.size();
            ok += rs[k].status == 200;
            if (!rs[k].status) printf("  ERROR %s: %s\n", urls[k].c_str(), rs[k].error.c_str());
            if (!save_dir.empty()) {
                std::ofstream f(save_dir + "/" + std::to_string(k) + ".body", std::ios::binary);
                f.write((const char *)rs[k].body.data(), (std::streamsize)rs[k].body.size());
            }
        }
        printf("  parallel: %zu of %zu answered 200, %zu bytes, %.0f ms\n", ok, rs.size(), bytes, Collect::now() - t0);
    }
}
