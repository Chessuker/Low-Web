// viewerbench [--no-fuse] VIEWER.wasm PAGE.html [URL] [FETCHMAP] — times the HTML viewer on a saved page
// without a window: parse, CSS and the whole layout, split into time spent in the
// interpreter and in host functions. FETCHMAP (lines "href<TAB>file") answers the page's
// lw_fetch calls (stylesheets) from local files; everything else gets a 404.
// Text is measured with a simple approximation instead of GDI, so the numbers are about
// the interpreter. Built with -DWASM_PROFILE, it also prints which instructions ran most.
#include <chrono>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <string>

#include "../browser/wasm.h"

using namespace wasm;

static double now_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

static std::vector<uint8_t> read_file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::string str_at(Instance &in, uint64_t p, uint64_t n) {
    if (!in.in_memory((uint32_t)p, (uint32_t)n)) return "";
    return std::string((const char *)in.memory() + (uint32_t)p, (uint32_t)n);
}

static double t0, host_ms;
static uint64_t frame_hash;  // of the last frame presented
static bool layout_done;
static int next_fetch = 1;
static std::map<std::string, std::string> fetchmap;
struct Pending { int id; std::string href; };
static std::deque<Pending> pending;

int main(int argc, char **argv) {
    if (argc > 1 && std::string(argv[1]) == "--no-fuse") {  // run without fused instructions
        wasm::set_fusion(false);
        argv++;
        argc--;
    }
    if (argc < 3) { printf("usage: viewerbench [--no-fuse] VIEWER.wasm PAGE.html [URL] [FETCHMAP]\n"); return 2; }
    std::vector<uint8_t> module = read_file(argv[1]), page = read_file(argv[2]);
    std::string url = argc > 3 ? argv[3] : "https://example.com/page";
    if (argc > 4) {
        std::ifstream m(argv[4]);
        std::string line;
        while (std::getline(m, line)) {
            size_t tab = line.find('\t');
            if (tab != std::string::npos) fetchmap[line.substr(0, tab)] = line.substr(tab + 1);
        }
    }
    std::vector<HostImport> im;
    // every host function is timed, to tell the interpreter's time from the host's
    auto add = [&](const char *name, const char *sig, HostFn fn) {
        im.push_back({"lw", name, sig, [fn](Instance &in, uint64_t *a) {
                          double s = now_ms();
                          fn(in, a);
                          host_ms += now_ms() - s;
                      }});
    };
    add("present", "iii:", [](Instance &in, uint64_t *a) {
        uint64_t n = (uint64_t)(uint32_t)a[1] * (uint32_t)a[2] * 4, h = 1469598103934665603ull;
        if (in.in_memory((uint32_t)a[0], n))
            for (uint64_t i = 0; i < n; i++) h = (h ^ in.memory()[(uint32_t)a[0] + i]) * 1099511628211ull;
        frame_hash = h;
    });
    add("set_title", "ii:", [](Instance &, uint64_t *) {});
    add("log", "ii:", [](Instance &in, uint64_t *a) {
        std::string s = str_at(in, a[0], a[1]);
        printf("  [+%5.0f ms] %s\n", now_ms() - t0, s.c_str());
        if (s.find("layout done") != std::string::npos) layout_done = true;
    });
    add("now", ":F", [](Instance &, uint64_t *a) { a[0] = from_f64(now_ms() - t0); });
    add("scale", ":F", [](Instance &, uint64_t *a) { a[0] = from_f64(1.0); });
    add("mods", ":i", [](Instance &, uint64_t *a) { a[0] = 0; });
    add("navigate", "ii:", [](Instance &, uint64_t *) {});
    add("navigate_post", "iiii:", [](Instance &, uint64_t *) {});
    add("open_tab", "iii:", [](Instance &, uint64_t *) {});
    add("open_file", "ii:", [](Instance &, uint64_t *) {});
    add("save_file", "iiii:", [](Instance &, uint64_t *) {});
    add("fetch", "ii:i", [](Instance &in, uint64_t *a) {
        pending.push_back({next_fetch, str_at(in, a[0], a[1])});
        a[0] = (uint32_t)next_fetch++;
    });
    add("image_decode", "iiii:i", [](Instance &, uint64_t *a) { a[0] = 0; });
    add("image_read", "ii:", [](Instance &, uint64_t *) {});
    add("image_free", "i:", [](Instance &, uint64_t *) {});
    add("text", "iiiiiiiiii:i", [](Instance &, uint64_t *a) { a[0] = 0; });
    add("line_breaks", "iii:", [](Instance &in, uint64_t *a) {
        if (in.in_memory((uint32_t)a[2], (uint32_t)a[1])) std::memset(in.memory() + (uint32_t)a[2], 0, (size_t)a[1]);
    });
    add("text_width", "iiii:i", [](Instance &in, uint64_t *a) {  // ~ average glyph width, per code point
        const uint8_t *s = in.memory() + (uint32_t)a[0];
        uint32_t n = (uint32_t)a[1], cps = 0;
        for (uint32_t i = 0; i < n; i++) cps += (s[i] & 0xC0) != 0x80;
        a[0] = (uint32_t)(cps * (int32_t)a[2] * 52 / 100);
    });

    Instance inst;
    inst.time_limit_ms = 60000;
    std::string err = inst.load(module.data(), module.size(), im);
    if (!err.empty()) { printf("load failed: %s\n", err.c_str()); return 1; }
    auto fn = [&](const char *n) { return inst.export_func(n); };
    auto call = [&](const char *n, std::initializer_list<uint64_t> args) {
        uint64_t in_[16] = {}, out[2] = {};
        size_t k = 0;
        for (uint64_t v : args) in_[k++] = v;
        inst.call((uint32_t)fn(n), in_, out);
        return out[0];
    };
    // hands bytes and a string to the page through lw_alloc, as the browser does
    auto give = [&](const std::vector<uint8_t> &data, const std::string &meta, uint32_t &meta_at) {
        uint32_t p = (uint32_t)call("lw_alloc", {(uint64_t)std::max<size_t>(data.size() + meta.size(), 1)});
        std::memcpy(inst.memory() + p, data.data(), data.size());
        std::memcpy(inst.memory() + p + data.size(), meta.data(), meta.size());
        meta_at = p + (uint32_t)data.size();
        return p;
    };
    auto deliver = [&](int id, int status, const std::vector<uint8_t> &body, const std::string &type, const std::string &u) {
        uint32_t meta_at;
        uint32_t p = give(body, type + u, meta_at);
        call("lw_on_fetch_ex", {(uint64_t)id, (uint64_t)status, p, body.size(), meta_at, type.size(), meta_at + type.size(), u.size()});
    };

    try {
        t0 = now_ms();
        call("lw_start", {});
        call("lw_resize", {1100, 760});
        deliver(0, 200, page, "text/html; charset=utf-8", url);
        int frames = 0;
        for (; frames < 20000 && !(layout_done && pending.empty()); frames++) {
            while (!pending.empty()) {
                Pending p = pending.front();
                pending.pop_front();
                auto it = fetchmap.find(p.href);
                if (it != fetchmap.end()) deliver(p.id, 200, read_file(it->second), "text/css", p.href);
                else deliver(p.id, 404, {}, "text/plain", p.href);
            }
            call("lw_frame", {from_f64(now_ms() - t0)});
        }
        double total = now_ms() - t0;
        // then scrolled to the end and drawn: a check that different runs computed the same
        call("lw_key", {35 /* LW_KEY_END */, 0, 1});
        for (int k = 0; k < 60; k++) call("lw_frame", {from_f64(now_ms() - t0 + 1000)});
        printf("final frame fnv %016llx\n", (unsigned long long)frame_hash);
        printf("total %.0f ms in %d frames: interpreter %.0f ms, host functions %.0f ms\n", total, frames, total - host_ms, host_ms);
    } catch (const Trap &t) {
        printf("trap: %s\n", t.msg.c_str());
        return 1;
    }
#ifdef WASM_PROFILE
    wasm::profile_dump(stdout);
#endif
}
