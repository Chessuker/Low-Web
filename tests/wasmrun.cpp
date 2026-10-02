// wasmrun — run exports of a module with the Low-web interpreter, from the command line.
//   wasmrun module.wasm "func(1, 2.5)" "other()" ...
// Low-web imports ("lw.*") are stubbed: present() prints a checksum of the frame,
// save_file() writes to the requested file name inside the current directory,
// image_decode() uses the browser's decoders.
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

#include "../browser/image.h"
#include "../browser/wasm.h"

using namespace wasm;

static std::vector<uint8_t> read_file(const char *path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static std::string str_at(Instance &in, uint64_t p, uint64_t n) {
    if (!in.in_memory((uint32_t)p, (uint32_t)n)) return "<bad string>";
    return std::string((const char *)in.memory() + (uint32_t)p, (uint32_t)n);
}

static std::map<int, image::Image> images;

static std::vector<HostImport> lw_stubs() {
    std::vector<HostImport> v;
    auto add = [&](const char *name, const char *sig, HostFn fn) { v.push_back({"lw", name, sig, fn}); };
    add("present", "iii:", [](Instance &in, uint64_t *a) {
        uint32_t p = (uint32_t)a[0], w = (uint32_t)a[1], h = (uint32_t)a[2];
        uint64_t hsh = 1469598103934665603ull;
        if (in.in_memory(p, (uint64_t)w * h * 4))
            for (uint64_t i = 0; i < (uint64_t)w * h * 4; i++) hsh = (hsh ^ in.memory()[p + i]) * 1099511628211ull;
        printf("present %ux%u fnv=%016llx\n", w, h, (unsigned long long)hsh);
    });
    add("set_title", "ii:", [](Instance &in, uint64_t *a) { printf("title: %s\n", str_at(in, a[0], a[1]).c_str()); });
    add("log", "ii:", [](Instance &in, uint64_t *a) { printf("log: %s\n", str_at(in, a[0], a[1]).c_str()); });
    add("now", ":F", [](Instance &, uint64_t *a) { a[0] = from_f64(0.0); });
    add("navigate", "ii:", [](Instance &in, uint64_t *a) { printf("navigate: %s\n", str_at(in, a[0], a[1]).c_str()); });
    add("open_file", "ii:", [](Instance &in, uint64_t *a) { printf("open_file: %s\n", str_at(in, a[0], a[1]).c_str()); });
    add("save_file", "iiii:", [](Instance &in, uint64_t *a) {
        std::string name = str_at(in, a[0], a[1]);
        for (char &c : name) if (c == '/' || c == '\\' || c == ':') c = '_';
        if (!in.in_memory((uint32_t)a[2], (uint32_t)a[3])) { printf("save_file: bad range\n"); return; }
        std::ofstream f("saved_" + name, std::ios::binary);
        f.write((const char *)in.memory() + (uint32_t)a[2], (uint32_t)a[3]);
        printf("save_file: saved_%s (%u bytes)\n", name.c_str(), (unsigned)a[3]);
    });
    add("fetch", "ii:i", [](Instance &in, uint64_t *a) { printf("fetch: %s\n", str_at(in, a[0], a[1]).c_str()); a[0] = 1; });
    add("image_decode", "iiii:i", [](Instance &in, uint64_t *a) {
        image::Image img;
        std::string err;
        uint32_t p = (uint32_t)a[0], n = (uint32_t)a[1];
        a[0] = 0;
        if (!in.in_memory(p, n) || !image::decode(in.memory() + p, n, img, err)) { printf("image_decode failed: %s\n", err.c_str()); return; }
        int id = (int)images.size() + 1;
        int32_t w = img.w, h = img.h;
        std::memcpy(in.memory() + (uint32_t)a[2], &w, 4);
        std::memcpy(in.memory() + (uint32_t)a[3], &h, 4);
        images[id] = std::move(img);
        a[0] = (uint32_t)id;
    });
    add("image_read", "ii:", [](Instance &in, uint64_t *a) {
        auto it = images.find((int)a[0]);
        if (it == images.end()) return;
        size_t n = it->second.rgba.size();
        if (in.in_memory((uint32_t)a[1], n)) std::memcpy(in.memory() + (uint32_t)a[1], it->second.rgba.data(), n);
        images.erase(it);
    });
    add("image_free", "i:", [](Instance &, uint64_t *a) { images.erase((int)a[0]); });
    add("text", "iiiiiiiiii:i", [](Instance &, uint64_t *a) { a[0] = 0; });
    add("clipboard_set", "ii:", [](Instance &, uint64_t *) {});
    add("clipboard_get", "ii:i", [](Instance &, uint64_t *a) { a[0] = (uint32_t)-1; });
    add("text_width", "iiii:i", [](Instance &, uint64_t *a) { a[0] = 0; });
    return v;
}

int main(int argc, char **argv) {
    if (argc > 1 && std::string(argv[1]) == "--no-fuse") {  // run without fused instructions
        wasm::set_fusion(false);
        argv++;
        argc--;
    }
    if (argc < 2) {
        fprintf(stderr, "usage: wasmrun module.wasm \"func(args)\" ...\n");
        return 2;
    }
    std::vector<uint8_t> bytes = read_file(argv[1]);
    Instance in;
    in.time_limit_ms = 3000;
    std::string err = in.load(bytes.data(), bytes.size(), lw_stubs());
    if (!err.empty()) {
        printf("load error: %s\n", err.c_str());
        return 1;
    }
    for (int i = 2; i < argc; i++) {
        std::string cmd = argv[i];
        size_t lp = cmd.find('('), rp = cmd.rfind(')');
        std::string name = cmd.substr(0, lp);
        std::vector<std::string> sargs;
        if (lp != std::string::npos && rp != std::string::npos && rp > lp + 1) {
            std::stringstream ss(cmd.substr(lp + 1, rp - lp - 1));
            std::string t;
            while (std::getline(ss, t, ',')) sargs.push_back(t);
        }
        int f = in.export_func(name);
        if (f < 0) { printf("%s: no such export\n", name.c_str()); continue; }
        const FuncType &t = in.func_type(f);
        if (sargs.size() != t.params.size()) { printf("%s: expects %zu args\n", name.c_str(), t.params.size()); continue; }
        std::vector<uint64_t> args(t.params.size()), res(t.results.size() + 1);
        for (size_t k = 0; k < args.size(); k++) {
            switch (t.params[k]) {
            case 0x7F: args[k] = (uint32_t)std::stoll(sargs[k], nullptr, 0); break;
            case 0x7E: args[k] = (uint64_t)std::stoll(sargs[k], nullptr, 0); break;
            case 0x7D: args[k] = from_f32(std::stof(sargs[k])); break;
            default: args[k] = from_f64(std::stod(sargs[k])); break;
            }
        }
        auto t0 = std::chrono::steady_clock::now();
        try {
            in.call((uint32_t)f, args.data(), res.data());
        } catch (const Trap &tr) {
            printf("%s -> trap: %s\n", cmd.c_str(), tr.msg.c_str());
            continue;
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        printf("%s ->", cmd.c_str());
        for (size_t k = 0; k < t.results.size(); k++) {
            switch (t.results[k]) {
            case 0x7F: printf(" %d", (int32_t)res[k]); break;
            case 0x7E: printf(" %llu", (unsigned long long)res[k]); break;
            case 0x7D: printf(" %.9g", to_f32(res[k])); break;
            default: printf(" %.17g", to_f64(res[k])); break;
            }
        }
        if (ms > 5) printf("   (%.1f ms)", ms);
        printf("\n");
    }
    return 0;
}
