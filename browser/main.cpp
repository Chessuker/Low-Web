// main.cpp — the Low-web browser: a Win32 window with tabs, the browser chrome, and the
// host side of the page ABI (sdk/lowweb.h). No frameworks: user32/gdi32 for the window,
// comdlg32 for file dialogs, our own interpreter, network stack and image decoders.
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shellapi.h>
#include <usp10.h>

#ifndef EM_SETCUEBANNER
#define EM_SETCUEBANNER 0x1501
#endif

#include <algorithm>
#include <array>
#include <unordered_map>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "cache.h"
#include "cookies.h"
#include "image.h"
#include "net.h"
#include "wasm.h"

namespace {

// =====================================================================================
// small utilities
// =====================================================================================

std::wstring widen(const std::string &s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const std::wstring &w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

double steady_ms() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

FILE *g_log_file;  // --log FILE

void log_line(const std::string &s) {
    std::string line = s + "\n";
    fputs(line.c_str(), stderr);
    fflush(stderr);
    if (g_log_file) {
        fputs(line.c_str(), g_log_file);
        fflush(g_log_file);
    }
    OutputDebugStringW(widen(line).c_str());
}

enum : UINT {
    WM_APP_LOADED = WM_APP + 1,  // lParam: LoadResult*
    WM_APP_FETCHED,              // lParam: FetchResult*
    WM_APP_NAVIGATE,             // lParam: NavRequest*
    WM_APP_OPENFILE,             // lParam: OpenRequest*
    WM_APP_SAVEFILE,             // lParam: SaveRequest*
    WM_APP_STREAM,               // lParam: StreamMsg* (an HTML document arriving piece by piece)
};

enum { ID_BACK = 101, ID_FORWARD, ID_RELOAD, ID_HOME, ID_URL, ID_ENGINE };
enum { ID_ENGINE_FIRST = 200 };
enum { TIMER_FRAME = 1, TIMER_SCRIPT = 2 };

const COLORREF kAccent = RGB(14, 165, 233);

// =====================================================================================
// global state
// =====================================================================================

HINSTANCE g_inst;
HWND g_main, g_view, g_url, g_btn[4], g_engine_btn;
HFONT g_ui_font, g_doc_font, g_doc_bold, g_mono_font, g_tab_font;
int g_dpi = 96;
WNDPROC g_url_proc;

int S(int v) { return MulDiv(v, g_dpi, 96); }

std::string g_home_url = "about:home";
std::string g_local_site;  // file:// URL of the bundled sample site, if found

// search engines for the address bar
struct Engine {
    const char *key;
    const wchar_t *label, *name;
    const char *url;  // the query is appended, URL-encoded
};
const Engine kEngines[] = {
    {"duckduckgo", L"DDG", L"DuckDuckGo", "https://lite.duckduckgo.com/lite/?q="},
    {"google", L"G", L"Google (its results need JavaScript)", "https://www.google.com/search?q="},
    {"bing", L"Bing", L"Bing", "https://www.bing.com/search?q="},
};
int g_engine = 0;

// test / automation mode (--script)
std::deque<std::string> g_script;
double g_script_wait_until = 0;
bool g_script_mode = false;
bool g_script_async = false;
std::string g_screenshot_path, g_save_dir;
int g_fake_mods = 0;

int current_mods() {
    int m = g_fake_mods;
    if (GetKeyState(VK_SHIFT) < 0) m |= 1;
    if (GetKeyState(VK_CONTROL) < 0) m |= 2;
    if (GetKeyState(VK_MENU) < 0) m |= 4;
    return m;
}

// =====================================================================================
// text rendering for pages (lw_text): GDI with font linking, so every script works
// =====================================================================================

struct TextEngine {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr;
    uint32_t *bits = nullptr;
    int bw = 0, bh = 0;
    std::map<int, HFONT> fonts;

    // flags: 1 bold, 2 italic, 4 monospace (sdk/lowweb.h LW_TEXT_*).
    // Segoe UI (or Consolas) for Latin text; Leelawadee UI (which has Thai glyphs of its
    // own) whenever the string contains Thai, so marks stack correctly in every weight.
    HFONT font(int px, int flags, bool thai) {
        px = std::clamp(px, 4, 512);
        flags &= 7;
        int key = px * 16 + flags * 2 + (thai ? 1 : 0);
        auto it = fonts.find(key);
        if (it != fonts.end()) return it->second;
        bool bold = flags & 1;
        // Leelawadee UI Bold drops the marks above the line at small sizes under GDI;
        // its Semibold face renders them correctly and still reads as bold.
        int weight = bold ? (thai ? FW_SEMIBOLD : FW_BOLD) : FW_NORMAL;
        const wchar_t *face = thai ? L"Leelawadee UI" : (flags & 4) ? L"Consolas" : L"Segoe UI";
        HFONT f = CreateFontW(-px, 0, 0, 0, weight, (flags & 2) ? TRUE : FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
        fonts[key] = f;
        return f;
    }
    static bool has_thai(const std::wstring &s) {
        for (wchar_t c : s)
            if (c >= 0x0E00 && c <= 0x0E7F) return true;
        return false;
    }
    void init() {
        if (dc) return;
        dc = CreateCompatibleDC(nullptr);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
    }
    SIZE measure(const std::wstring &s, int px, int flags) {
        init();
        SelectObject(dc, font(px, flags, has_thai(s)));
        RECT r{0, 0, 0, 0};
        DrawTextW(dc, s.c_str(), (int)s.size(), &r, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        return {r.right, r.bottom};
    }

    // Width only, and fast: layout asks for tens of thousands of words, many of them again
    // and again (on every relayout). GDI's answer is remembered. (Adding up per-character
    // widths would be much faster but is not what DrawText draws: it kerns, e.g. "To".)
    std::unordered_map<std::wstring, int> widths;  // key: size and flags, then the text

    int width(const std::wstring &s, int px, int flags) {
        px = std::clamp(px, 4, 512);
        flags &= 7;
        std::wstring key;
        key += (wchar_t)(px * 16 + flags);
        key += s;
        auto hit = widths.find(key);
        if (hit != widths.end()) return hit->second;
        if (widths.size() > 200000) widths.clear();
        int w = measure(s, px, flags).cx;
        widths.emplace(std::move(key), w);
        return w;
    }
    void ensure(int w, int h) {
        if (w <= bw && h <= bh) return;
        bw = std::max(w, bw);
        bh = std::max(h, bh);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = bw;
        bi.bmiHeader.biHeight = -bh;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        HBITMAP nb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&bits, nullptr, 0);
        SelectObject(dc, nb);
        if (bmp) DeleteObject(bmp);
        bmp = nb;
    }
    // Blends the text into an RGBA buffer. Returns the advance width.
    int draw(uint8_t *dst, int dw, int dh, int x, int y, const std::wstring &s, int px, int flags, uint32_t rgba) {
        SIZE sz = measure(s, px, flags);
        if (sz.cx <= 0) return 0;
        int pad = px / 2 + 2;
        int w = sz.cx + 2 * pad, h = sz.cy + 2 * pad;
        if (w > 16384 || h > 4096) return sz.cx;
        ensure(w, h);
        std::memset(bits, 0, (size_t)bw * h * 4);
        SelectObject(dc, font(px, flags, has_thai(s)));
        RECT r{pad, pad, pad + sz.cx, pad + sz.cy};
        DrawTextW(dc, s.c_str(), (int)s.size(), &r, DT_SINGLELINE | DT_NOPREFIX | DT_NOCLIP);
        GdiFlush();
        uint32_t cr = rgba & 255, cg = rgba >> 8 & 255, cb = rgba >> 16 & 255, ca = rgba >> 24;
        for (int j = 0; j < h; j++) {
            int ty = y - pad + j;
            if (ty < 0 || ty >= dh) continue;
            const uint32_t *src = bits + (size_t)j * bw;
            for (int i = 0; i < w; i++) {
                int tx = x - pad + i;
                if (tx < 0 || tx >= dw) continue;
                uint32_t cov = src[i] >> 8 & 255;
                if (!cov) continue;
                uint32_t a = cov * ca / 255, na = 255 - a;
                uint8_t *o = dst + ((size_t)ty * dw + tx) * 4;
                o[0] = (uint8_t)((cr * a + o[0] * na) / 255);
                o[1] = (uint8_t)((cg * a + o[1] * na) / 255);
                o[2] = (uint8_t)((cb * a + o[2] * na) / 255);
                o[3] = (uint8_t)(a + o[3] * na / 255);
            }
        }
        return sz.cx;
    }
};
TextEngine g_text;

// Line-break opportunities for UTF-8 text, using Uniscribe (the OS's text engine), which
// knows where Thai, Lao, Khmer... words end. out[i] = 1 if a line may break before byte i.
void line_breaks(const uint8_t *s, int len, uint8_t *out) {
    std::memset(out, 0, (size_t)len);
    std::wstring w;
    std::vector<int> byte_of;  // UTF-16 index -> byte offset
    for (int i = 0; i < len;) {
        uint32_t c = s[i];
        int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (i + n > len) n = len - i;
        if (n == 2) c = (c & 0x1F) << 6 | (s[i + 1] & 0x3F);
        else if (n == 3) c = (c & 0x0F) << 12 | (s[i + 1] & 0x3F) << 6 | (s[i + 2] & 0x3F);
        else if (n == 4) c = (c & 0x07) << 18 | (s[i + 1] & 0x3F) << 12 | (s[i + 2] & 0x3F) << 6 | (s[i + 3] & 0x3F);
        if (c >= 0x10000) {
            w += (wchar_t)(0xD800 + ((c - 0x10000) >> 10));
            byte_of.push_back(i);
            w += (wchar_t)(0xDC00 + ((c - 0x10000) & 0x3FF));
            byte_of.push_back(-1);
        } else {
            w += (wchar_t)c;
            byte_of.push_back(i);
        }
        i += n;
    }
    if (w.empty()) return;
    std::vector<SCRIPT_ITEM> items(w.size() + 1);
    int nitems = 0;
    if (ScriptItemize(w.c_str(), (int)w.size(), (int)items.size() - 1, nullptr, nullptr, items.data(), &nitems) != S_OK) return;
    std::vector<SCRIPT_LOGATTR> la(w.size());
    for (int k = 0; k < nitems; k++) {
        int a = items[k].iCharPos, b = items[k + 1].iCharPos;
        if (ScriptBreak(w.c_str() + a, b - a, &items[k].a, la.data() + a) != S_OK) continue;
    }
    for (size_t i = 1; i < w.size(); i++)
        if (la[i].fSoftBreak && byte_of[i] >= 0) out[byte_of[i]] = 1;
}

// =====================================================================================
// tabs and the page host
// =====================================================================================

// what a tab shows: a frame (page pixels or an image) or a native document
struct Frame {
    int w = 0, h = 0;
    std::vector<uint32_t> px;  // BGRA
    bool is_image = false;
};

struct Doc {
    std::wstring heading, body;
    bool mono = false, error = false;
    int scroll = 0;
};

struct Tab;

struct Page {
    wasm::Instance inst;
    Tab *tab = nullptr;
    std::string url;
    net::Zone zone = net::Zone::Public;  // where the page came from: decides what it may fetch (net.h: Access)
    uint64_t gen = 0;
    double t0 = 0;
    int cursor = 0;
    bool crashed = false;
    int f_start = -1, f_resize = -1, f_frame = -1, f_pointer = -1, f_key = -1, f_char = -1, f_alloc = -1,
        f_on_file = -1, f_on_fetch = -1, f_on_fetch_ex = -1,
        f_on_fetch_begin = -1, f_on_fetch_data = -1, f_on_fetch_end = -1;
    std::map<int, image::Image> images;
    int next_image = 1, next_fetch = 1;
};

struct Tab {
    int id = 0;
    std::unique_ptr<Page> page;
    Frame frame;
    Doc doc;
    std::vector<std::string> history;
    int hist_idx = -1;
    std::string current_url;
    std::wstring typed;      // what the user typed in the address bar and has not submitted
    bool typed_dirty = false;
    std::wstring title;
    uint64_t nav_gen = 0;
    bool loading = false;
    uint64_t stream_gen = 0;  // the load (nav_gen) whose document is still streaming into the page
    net::CacheMode sub_cache = net::CacheMode::Normal;  // for the page's own fetches (hard reload: skip the cache)
    double nav_t0 = 0;        // when the current navigation started (for the log)
};

std::vector<std::unique_ptr<Tab>> g_tabs;
int g_active = 0, g_next_tab_id = 1;
std::vector<std::string> g_closed;  // for Ctrl+Shift+T

Tab &T() { return *g_tabs[g_active]; }
Tab *tab_by_id(int id) {
    for (auto &t : g_tabs)
        if (t->id == id) return t.get();
    return nullptr;
}
bool is_active(const Tab *t) { return !g_tabs.empty() && g_tabs[g_active].get() == t; }

struct NavRequest {
    int tab_id = 0;
    uint64_t gen = 0;
    std::string url;
    bool post = false;
    std::string body;
    int new_tab = 0;  // 0 same tab, 1 new foreground tab, 2 new background tab
    std::string initiator;  // the page that asked (for SameSite cookies)
    net::Access access;     // what the page may send the tab to
};
struct LoadResult {
    int tab_id;
    uint64_t gen;
    std::string requested;
    int mode;  // 0 new entry, 1 reload, 2 history move
    int hist_target;
    net::Response r;
};
// The first message of a streamed document carries its head (begin); the rest carry bytes.
struct StreamMsg {
    int tab_id;
    uint64_t gen;
    bool begin;
    LoadResult head;  // begin only (no body)
    std::vector<uint8_t> data;
};
struct FetchResult {
    int tab_id;
    uint64_t gen;
    int id;
    net::Response r;
};
struct OpenRequest {
    int tab_id;
    uint64_t gen;
    std::string accept;
};
struct SaveRequest {
    int tab_id;
    uint64_t gen;
    std::string name;
    std::vector<uint8_t> data;
};

void give_response(Tab &t, int id, const net::Response &r);
bool read_file(const std::wstring &path, std::vector<uint8_t> &out);
void invalidate_tabs();

std::wstring tab_label(const Tab &t) {
    if (!t.title.empty()) return t.title;
    if (t.loading && t.current_url.empty()) return L"Loading…";
    return t.current_url.empty() ? L"New tab" : widen(t.current_url);
}

void update_title() {
    if (g_tabs.empty()) return;
    std::wstring t = T().title.empty() ? L"Low-web" : T().title + L" — Low-web";
    SetWindowTextW(g_main, t.c_str());
    invalidate_tabs();
}

void update_buttons() {
    EnableWindow(g_btn[0], T().hist_idx > 0);
    EnableWindow(g_btn[1], T().hist_idx + 1 < (int)T().history.size());
}

// For display: %XX sequences that spell non-ASCII UTF-8 are shown as the characters
// themselves (the network code escapes them again when the address is used).
std::wstring display_url(const std::string &u) {
    std::string out;
    for (size_t i = 0; i < u.size(); i++) {
        if (u[i] == '%' && i + 2 < u.size() && isxdigit((unsigned char)u[i + 1]) && isxdigit((unsigned char)u[i + 2])) {
            // collect a run of escaped bytes and keep it only if it is valid non-ASCII UTF-8
            std::string run;
            size_t j = i;
            while (j + 2 < u.size() && u[j] == '%' && isxdigit((unsigned char)u[j + 1]) && isxdigit((unsigned char)u[j + 2])) {
                unsigned v = (unsigned)std::stoi(u.substr(j + 1, 2), nullptr, 16);
                if (v < 0x80) break;
                run += (char)v;
                j += 3;
            }
            if (!run.empty() && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, run.data(), (int)run.size(), nullptr, 0) > 0) {
                out += run;
                i = j - 1;
                continue;
            }
        }
        out += u[i];
    }
    return widen(out);
}

void set_url_bar(const std::string &u) { SetWindowTextW(g_url, display_url(u).c_str()); }

void show_doc(Tab &t, const std::wstring &heading, const std::wstring &body, bool error, bool mono = false) {
    t.frame = Frame();
    t.doc = Doc{heading, body, mono, error, 0};
    if (is_active(&t)) InvalidateRect(g_view, nullptr, FALSE);
}

// Checks an export's parameter types; results may be empty or one i32.
int find_export(Page &p, const char *name, const char *params) {
    int f = p.inst.export_func(name);
    if (f < 0) return -1;
    const wasm::FuncType &t = p.inst.func_type((uint32_t)f);
    std::vector<uint8_t> want;
    for (const char *c = params; *c; c++) want.push_back(*c == 'i' ? 0x7F : *c == 'I' ? 0x7E : *c == 'f' ? 0x7D : 0x7C);
    if (t.params != want || t.results.size() > 1 || (t.results.size() == 1 && t.results[0] != 0x7F)) {
        log_line(std::string("[low-web] ignoring export ") + name + ": unexpected signature");
        return -1;
    }
    return f;
}

void page_crashed(Tab &t, const std::string &why) {
    if (!t.page || t.page->crashed) return;
    t.page->crashed = true;
    log_line("[low-web] page crashed: " + why);
    show_doc(t, L"This page stopped working", widen(why) + L"\n\n" + widen(t.page->url) + L"\n\nPress F5 to load it again.", true);
}

// Calls into a tab's page. Returns false if there is no such function or the page trapped.
bool page_call(Tab &t, int f, std::initializer_list<uint64_t> args, uint64_t *result = nullptr) {
    if (!t.page || t.page->crashed || f < 0) return false;
    uint64_t res[2] = {0, 0};
    try {
        t.page->inst.call((uint32_t)f, args.begin(), res);
    } catch (const wasm::Trap &e) {
        page_crashed(t, e.msg);
        return false;
    }
    if (result) *result = res[0];
    return true;
}

// Gives bytes to the page through lw_alloc. Returns the address or 0.
uint32_t page_give(Tab &t, const uint8_t *data, size_t len, const std::string &extra, uint32_t &extra_at) {
    if (!t.page || t.page->f_alloc < 0) return 0;
    uint64_t total = (uint64_t)len + extra.size();
    if (total > 0x7FFFFFFF) return 0;
    uint64_t p = 0;
    if (!page_call(t, t.page->f_alloc, {std::max<uint64_t>(total, 1)}, &p) || !p || !t.page) return 0;
    uint32_t addr = (uint32_t)p;
    if (!t.page->inst.in_memory(addr, total)) {
        page_crashed(t, "lw_alloc returned memory outside the page");
        return 0;
    }
    if (len) std::memcpy(t.page->inst.memory() + addr, data, len);
    if (!extra.empty()) std::memcpy(t.page->inst.memory() + addr + len, extra.data(), extra.size());
    extra_at = addr + (uint32_t)len;
    return addr;
}

void deliver_file(Tab &t, const std::vector<uint8_t> &bytes, const std::string &name) {
    if (!t.page || t.page->f_on_file < 0) return;
    uint32_t name_at = 0;
    uint32_t p = page_give(t, bytes.data(), bytes.size(), name, name_at);
    if (!p) {
        log_line("[low-web] the page refused the file (lw_alloc failed)");
        return;
    }
    page_call(t, t.page->f_on_file, {p, (uint32_t)bytes.size(), name_at, (uint32_t)name.size()});
}

bool read_file(const std::wstring &path, std::vector<uint8_t> &out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(f, &sz);
    if (sz.QuadPart > (256ll << 20)) { CloseHandle(f); return false; }
    out.resize((size_t)sz.QuadPart);
    DWORD got = 0;
    bool ok = out.empty() || (ReadFile(f, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size());
    CloseHandle(f);
    return ok;
}

std::string base_name(const std::wstring &path) {
    size_t s = path.find_last_of(L"\\/");
    return narrow(s == std::wstring::npos ? path : path.substr(s + 1));
}

// ---- host functions ------------------------------------------------------------------

std::string mem_str(wasm::Instance &in, uint64_t p, uint64_t n) {
    if (n > (1u << 24) || !in.in_memory((uint32_t)p, (uint32_t)n)) throw wasm::Trap{"bad string passed to the browser"};
    return std::string((const char *)in.memory() + (uint32_t)p, (uint32_t)n);
}

std::vector<wasm::HostImport> make_imports(Page *page) {
    using wasm::Instance;
    std::vector<wasm::HostImport> v;
    auto add = [&](const char *name, const char *sig, wasm::HostFn fn) { v.push_back({"lw", name, sig, std::move(fn)}); };

    add("present", "iii:", [page](Instance &in, uint64_t *a) {
        uint32_t p = (uint32_t)a[0], w = (uint32_t)a[1], h = (uint32_t)a[2];
        if (w == 0 || h == 0 || w > 8192 || h > 8192 || !in.in_memory(p, (uint64_t)w * h * 4))
            throw wasm::Trap{"lw_present: bad frame"};
        Tab *t = page->tab;
        if (!t || t->page.get() != page || page->crashed) return;
        Frame &f = t->frame;
        f.w = (int)w;
        f.h = (int)h;
        f.is_image = false;
        f.px.resize((size_t)w * h);
        const uint32_t *src = (const uint32_t *)(in.memory() + p);
        uint32_t *dst = f.px.data();
        size_t n = (size_t)w * h;
        for (size_t i = 0; i < n; i++) {  // RGBA -> BGRA
            uint32_t c = src[i];
            dst[i] = (c & 0xFF00FF00u) | (c >> 16 & 0xFF) | (c & 0xFF) << 16;
        }
        if (is_active(t)) InvalidateRect(g_view, nullptr, FALSE);
    });
    add("set_title", "ii:", [page](Instance &in, uint64_t *a) {
        std::string title = mem_str(in, a[0], a[1]);
        Tab *t = page->tab;
        if (!t || t->page.get() != page) return;
        t->title = widen(title.substr(0, 200));
        if (is_active(t)) update_title();
        else invalidate_tabs();
    });
    add("log", "ii:", [page](Instance &in, uint64_t *a) {  // stamped with the time since the navigation began
        double since = page->tab ? steady_ms() - page->tab->nav_t0 : 0;
        log_line("[page +" + std::to_string((int)since) + " ms] " + mem_str(in, a[0], a[1]));
    });
    add("now", ":F", [page](Instance &, uint64_t *a) { a[0] = wasm::from_f64(steady_ms() - page->t0); });
    add("scale", ":F", [](Instance &, uint64_t *a) { a[0] = wasm::from_f64(g_dpi / 96.0); });
    add("mods", ":i", [](Instance &, uint64_t *a) {
        int m = g_fake_mods;
        if (GetKeyState(VK_SHIFT) < 0) m |= 1;
        if (GetKeyState(VK_CONTROL) < 0) m |= 2;
        if (GetKeyState(VK_MENU) < 0) m |= 4;
        a[0] = (uint32_t)m;
    });
    auto nav = [](std::string url) {
        auto *r = new NavRequest;
        r->url = std::move(url);
        return r;
    };
    // A page may send the tab anywhere on the web, as a link can. But not to the user's
    // files unless it is a file itself, and a form it sends (POST) may not go to a server
    // more private than the page: that is how a page from the internet would change a
    // router's settings.
    auto post_nav = [page](NavRequest *req) {
        req->tab_id = page->tab ? page->tab->id : 0;
        req->gen = page->gen;
        req->initiator = page->url;
        req->access.files = req->url.rfind("file:", 0) != 0 || page->url.rfind("file:", 0) == 0;
        if (req->post) req->access.lowest = page->zone;
        PostMessageW(g_main, WM_APP_NAVIGATE, 0, (LPARAM)req);
    };
    add("navigate", "ii:", [page, post_nav, nav](Instance &in, uint64_t *a) {
        post_nav(nav(net::resolve(page->url, mem_str(in, a[0], a[1]))));
    });
    add("navigate_post", "iiii:", [page, post_nav, nav](Instance &in, uint64_t *a) {
        auto *r = nav(net::resolve(page->url, mem_str(in, a[0], a[1])));
        r->post = true;
        r->body = mem_str(in, a[2], a[3]);
        post_nav(r);
    });
    add("open_tab", "iii:", [page, post_nav, nav](Instance &in, uint64_t *a) {
        auto *r = nav(net::resolve(page->url, mem_str(in, a[0], a[1])));
        r->new_tab = a[2] ? 1 : 2;
        post_nav(r);
    });
    add("open_file", "ii:", [page](Instance &in, uint64_t *a) {
        auto *r = new OpenRequest{page->tab ? page->tab->id : 0, page->gen, mem_str(in, a[0], a[1])};
        PostMessageW(g_main, WM_APP_OPENFILE, 0, (LPARAM)r);
    });
    add("save_file", "iiii:", [page](Instance &in, uint64_t *a) {
        std::string name = mem_str(in, a[0], a[1]);
        uint32_t p = (uint32_t)a[2], n = (uint32_t)a[3];
        if (!in.in_memory(p, n)) throw wasm::Trap{"lw_save_file: bad data"};
        auto *req = new SaveRequest{page->tab ? page->tab->id : 0, page->gen, name,
                                    std::vector<uint8_t>(in.memory() + p, in.memory() + p + n)};
        PostMessageW(g_main, WM_APP_SAVEFILE, 0, (LPARAM)req);
    });
    add("fetch", "ii:i", [page](Instance &in, uint64_t *a) {
        std::string url = net::resolve(page->url, mem_str(in, a[0], a[1]));
        int id = page->next_fetch++;
        uint64_t gen = page->gen;
        int tab_id = page->tab ? page->tab->id : 0;
        net::CacheMode cm = page->tab ? page->tab->sub_cache : net::CacheMode::Normal;
        cookies::Context who{page->url, false, false};  // cookies only for the page's own site
        net::Access access = net::access_for_page(page->url, page->zone);
        std::thread([url, id, gen, tab_id, cm, who, access] {
            auto *res = new FetchResult{tab_id, gen, id, net::fetch(url, net::Mode::Exact, 64u << 20, nullptr, nullptr, cm, &who, access)};
            PostMessageW(g_main, WM_APP_FETCHED, 0, (LPARAM)res);
        }).detach();
        a[0] = (uint32_t)id;
    });
    add("image_decode", "iiii:i", [page](Instance &in, uint64_t *a) {
        uint32_t p = (uint32_t)a[0], n = (uint32_t)a[1], wp = (uint32_t)a[2], hp = (uint32_t)a[3];
        if (!in.in_memory(p, n) || !in.in_memory(wp, 4) || !in.in_memory(hp, 4)) throw wasm::Trap{"lw_image_decode: bad pointer"};
        image::Image img;
        std::string err;
        a[0] = 0;
        if (!image::decode(in.memory() + p, n, img, err)) {
            log_line("[low-web] image_decode: " + err);
            return;
        }
        if (page->images.size() > 64) page->images.erase(page->images.begin());
        int32_t w = img.w, h = img.h;
        std::memcpy(in.memory() + wp, &w, 4);
        std::memcpy(in.memory() + hp, &h, 4);
        int id = page->next_image++;
        page->images[id] = std::move(img);
        a[0] = (uint32_t)id;
    });
    add("image_read", "ii:", [page](Instance &in, uint64_t *a) {
        auto it = page->images.find((int)a[0]);
        if (it == page->images.end()) throw wasm::Trap{"lw_image_read: unknown image"};
        size_t n = it->second.rgba.size();
        if (!in.in_memory((uint32_t)a[1], n)) throw wasm::Trap{"lw_image_read: destination outside memory"};
        std::memcpy(in.memory() + (uint32_t)a[1], it->second.rgba.data(), n);
        page->images.erase(it);
    });
    add("image_free", "i:", [page](Instance &, uint64_t *a) { page->images.erase((int)a[0]); });
    add("text", "iiiiiiiiii:i", [](Instance &in, uint64_t *a) {
        uint32_t fb = (uint32_t)a[0];
        int fw = (int32_t)a[1], fh = (int32_t)a[2], x = (int32_t)a[3], y = (int32_t)a[4];
        std::string s = mem_str(in, a[5], a[6]);
        int size = (int32_t)a[7], flags = (int32_t)a[8];
        uint32_t color = (uint32_t)a[9];
        if (fw <= 0 || fh <= 0 || fw > 16384 || fh > 16384 || !in.in_memory(fb, (uint64_t)fw * fh * 4))
            throw wasm::Trap{"lw_text: bad framebuffer"};
        a[0] = (uint32_t)g_text.draw(in.memory() + fb, fw, fh, x, y, widen(s), size, flags, color);
    });
    add("line_breaks", "iii:", [](Instance &in, uint64_t *a) {
        uint32_t p = (uint32_t)a[0], n = (uint32_t)a[1], out = (uint32_t)a[2];
        if (n > (1u << 24) || !in.in_memory(p, n) || !in.in_memory(out, n)) throw wasm::Trap{"lw_line_breaks: bad buffer"};
        std::vector<uint8_t> tmp(in.memory() + p, in.memory() + p + n), flags(n);
        line_breaks(tmp.data(), (int)n, flags.data());
        std::memcpy(in.memory() + out, flags.data(), n);
    });
    add("text_width", "iiii:i", [](Instance &in, uint64_t *a) {
        std::string s = mem_str(in, a[0], a[1]);
        a[0] = (uint32_t)g_text.width(widen(s), (int32_t)a[2], (int32_t)a[3]);
    });
    return v;
}

// =====================================================================================
// the address bar: URL or search?
// =====================================================================================

std::string url_encode_query(const std::string &s) {
    static const char *hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else if (c == ' ') o += '+';
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

std::string search_url(const std::string &query) { return std::string(kEngines[g_engine].url) + url_encode_query(query); }

// Chrome-style: addresses, paths and host names go to the site; anything else is a search.
std::string address_or_search(const std::string &typed) {
    size_t a = typed.find_first_not_of(" \t"), b = typed.find_last_not_of(" \t");
    if (a == std::string::npos) return "";
    std::string s = typed.substr(a, b - a + 1);
    if (s[0] == '?') return search_url(s.substr(1));
    std::string low = s;
    for (char &c : low) c = (char)tolower((unsigned char)c);
    if (low.find("://") != std::string::npos || low.rfind("about:", 0) == 0 || low.rfind("file:", 0) == 0) return s;
    bool drive = s.size() >= 3 && isalpha((unsigned char)s[0]) && s[1] == ':' && (s[2] == '\\' || s[2] == '/');
    if (drive || s.rfind("\\\\", 0) == 0) return net::from_user_input(s);
    if (s.find_first_of(" \t") != std::string::npos) return search_url(s);
    std::string host = low.substr(0, low.find_first_of("/?#"));
    size_t colon = host.rfind(':');
    std::string port = colon != std::string::npos && host.find(']') == std::string::npos ? host.substr(colon + 1) : "";
    if (!port.empty()) host = host.substr(0, colon);
    bool port_ok = port.empty() || port.find_first_not_of("0123456789") == std::string::npos;
    if (port_ok && (host == "localhost" || host[0] == '[')) return "http://" + s;
    size_t dot = host.rfind('.');
    if (port_ok && dot != std::string::npos && dot > 0 && dot + 1 < host.size()) {
        std::string tld = host.substr(dot + 1);
        bool alpha = tld.size() >= 2 && tld.find_first_not_of("abcdefghijklmnopqrstuvwxyz") == std::string::npos;
        bool ipv4 = host.find_first_not_of("0123456789.") == std::string::npos && std::count(host.begin(), host.end(), '.') == 3;
        if (alpha || ipv4 || (unsigned char)tld[0] >= 0x80) return "http://" + s;
    }
    return search_url(s);
}

// settings: %APPDATA%\Low-web\settings.ini (key=value lines)
std::wstring settings_path() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, (DWORD)std::size(buf));
    if (!n || n >= std::size(buf)) return L"";
    std::wstring dir = std::wstring(buf) + L"\\Low-web";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\settings.ini";
}

void load_settings() {
    std::vector<uint8_t> bytes;
    std::wstring path = settings_path();
    if (path.empty() || !read_file(path, bytes)) return;
    std::string s(bytes.begin(), bytes.end());
    size_t p = s.find("search=");
    if (p == std::string::npos) return;
    std::string v = s.substr(p + 7, s.find_first_of("\r\n", p) - p - 7);
    for (int i = 0; i < (int)std::size(kEngines); i++)
        if (v == kEngines[i].key) g_engine = i;
}

void save_settings() {
    if (g_script_mode) return;  // tests must not change the user's settings
    std::wstring path = settings_path();
    if (path.empty()) return;
    std::string s = std::string("search=") + kEngines[g_engine].key + "\r\n";
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wrote;
    WriteFile(f, s.data(), (DWORD)s.size(), &wrote, nullptr);
    CloseHandle(f);
}

// =====================================================================================
// navigation
// =====================================================================================

void view_size(int &w, int &h) {
    RECT rc;
    GetClientRect(g_view, &rc);
    w = std::max(1, (int)rc.right);
    h = std::max(1, (int)rc.bottom);
}

bool is_html_type(const std::string &ct) {
    std::string t;
    for (char c : ct.substr(0, ct.find(';'))) t += (char)tolower((unsigned char)c);
    while (!t.empty() && t.back() == ' ') t.pop_back();
    return t == "text/html" || t == "application/xhtml+xml";
}

// Streams HTML documents to the UI thread while they download, so the viewer can start
// parsing and showing them early. Other content arrives whole (WM_APP_LOADED).
struct DocStream : net::Stream {
    int tab_id = 0;
    uint64_t gen = 0;
    std::string url;
    int mode = 0, hist_target = -1;
    bool begin(const net::Response &head) override {
        if (!is_html_type(head.content_type)) return false;
        auto *m = new StreamMsg{tab_id, gen, true, LoadResult{tab_id, gen, url, mode, hist_target, head}, {}};
        PostMessageW(g_main, WM_APP_STREAM, 0, (LPARAM)m);
        return true;
    }
    void data(const uint8_t *d, size_t n) override {
        auto *m = new StreamMsg{tab_id, gen, false, {}, std::vector<uint8_t>(d, d + n)};
        PostMessageW(g_main, WM_APP_STREAM, 0, (LPARAM)m);
    }
};

// mode: 0 new history entry, 1 reload, 2 history move. A reload asks the server whether the
// page changed; a hard reload (Ctrl+F5) ignores the cache; back/forward prefer it.
// `initiator`: the page whose link or form started this ("" = the user); `access`: where it may lead.
void start_load(Tab &t, const std::string &url, int mode, int hist_target = -1, const std::string *post = nullptr,
                bool hard = false, const std::string &initiator = std::string(), const net::Access &access = net::Access()) {
    net::CacheMode cm = hard ? net::CacheMode::Reload : mode == 1 ? net::CacheMode::Revalidate
                      : mode == 2 ? net::CacheMode::PreferCached : net::CacheMode::Normal;
    t.sub_cache = hard ? net::CacheMode::Reload : net::CacheMode::Normal;
    uint64_t gen = ++t.nav_gen;
    t.nav_t0 = steady_ms();
    t.loading = true;
    t.typed_dirty = false;
    if (t.current_url.empty()) t.current_url = url;
    if (is_active(&t)) {
        set_url_bar(url);
        InvalidateRect(g_view, nullptr, FALSE);
    }
    invalidate_tabs();
    bool is_post = post != nullptr;
    std::string body = post ? *post : std::string();
    int tab_id = t.id;
    cookies::Context who{initiator, true, is_post};
    std::thread([url, gen, mode, hist_target, is_post, body, tab_id, cm, who, access] {
        auto *res = new LoadResult{tab_id, gen, url, mode, hist_target, {}};
        if (url.rfind("about:", 0) == 0) {
            res->r.status = 200;
            res->r.final_url = url;
            res->r.content_type = "about";
        } else {
            DocStream stream;
            stream.tab_id = tab_id;
            stream.gen = gen;
            stream.url = url;
            stream.mode = mode;
            stream.hist_target = hist_target;
            res->r = net::fetch(url, net::Mode::Page, 256u << 20, is_post ? &body : nullptr, &stream, cm, &who, access);
        }
        PostMessageW(g_main, WM_APP_LOADED, 0, (LPARAM)res);
    }).detach();
}

void navigate(const std::string &url) { start_load(T(), url, 0); }
void reload(bool hard = false) { if (T().hist_idx >= 0) start_load(T(), T().history[T().hist_idx], 1, -1, nullptr, hard); }
void go_history(int delta) {
    Tab &t = T();
    int k = t.hist_idx + delta;
    if (k < 0 || k >= (int)t.history.size()) return;
    start_load(t, t.history[k], 2, k);
}

std::wstring about_page(const std::string &which, std::wstring &heading) {
    if (which == "about:blank") { heading.clear(); return L""; }
    heading = L"Low-web";
    std::wstring b =
        L"A browser for a web without HTML, CSS or JavaScript. Every page is a WebAssembly "
        L"program (index.wasm) that draws its own pixels; this browser runs it in its own "
        L"interpreter and gives it a window, input, files, text and the network.\n\n"
        L"Try one of these addresses:\n\n";
    if (!g_local_site.empty()) b += L"    " + widen(g_local_site) + L"\n        the sample site, straight from disk\n\n";
    b += L"    http://localhost:8080/\n        after starting the server:  bin\\lowd.exe sites\\www\n\n"
         L"    https://lite.duckduckgo.com/lite/\n        an ordinary HTML site: shown by the built-in viewer page (reader view)\n\n"
         L"Type words instead of an address to search the web.\n\n"
         L"Keys:  Ctrl+L address bar  ·  Ctrl+T new tab  ·  Ctrl+W close tab  ·  Ctrl+Tab next tab  ·  "
         L"F5 reload  ·  Alt+← back  ·  Alt+→ forward  ·  Alt+Home home\n"
         L"Drop a .wasm file on the window to run it.";
    return b;
}

bool looks_like_text(const std::vector<uint8_t> &b) {
    size_t n = std::min<size_t>(b.size(), 4096);
    for (size_t i = 0; i < n; i++)
        if (b[i] == 0) return false;
    return true;
}

bool is_html(const std::string &ct, const std::vector<uint8_t> &body) {
    if (is_html_type(ct)) return true;
    std::string t;
    for (char c : ct.substr(0, ct.find(';'))) t += (char)tolower((unsigned char)c);
    if (!t.empty() && t != "application/octet-stream") return false;
    std::string head;  // no usable type: sniff
    for (size_t i = 0; i < body.size() && head.size() < 512; i++) head += (char)tolower(body[i]);
    return head.find("<!doctype html") != std::string::npos || head.find("<html") != std::string::npos;
}

// The HTML viewer is itself a Low-web page. A viewer.wasm next to the exe wins (handy
// while working on it); otherwise the copy built into the exe is used.
std::vector<uint8_t> viewer_module() {
    std::vector<uint8_t> bytes;
    wchar_t exe[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, exe, (DWORD)std::size(exe));
    std::wstring path(exe, n);
    path = path.substr(0, path.find_last_of(L"\\/") + 1) + L"viewer.wasm";
    if (read_file(path, bytes) && !bytes.empty()) return bytes;
    bytes.clear();
    if (HRSRC res = FindResourceW(nullptr, L"VIEWER", MAKEINTRESOURCEW(10) /* RT_RCDATA */)) {
        if (HGLOBAL h = LoadResource(nullptr, res)) {
            const uint8_t *d = (const uint8_t *)LockResource(h);
            bytes.assign(d, d + SizeofResource(nullptr, res));
        }
    }
    return bytes;
}

void stream_begin(Tab &t, const net::Response &head);

// Instantiates a page in a tab. `doc`, if given, is a document the page should display
// (the HTML viewer gets the HTML this way, as fetch id 0). With `streamed`, `doc` has no
// body yet: it follows through stream_data() and stream_end().
void start_wasm_page(Tab &t, const std::vector<uint8_t> &module, const std::string &url, net::Zone zone,
                     const net::Response *doc, bool streamed = false) {
    auto page = std::make_unique<Page>();
    page->tab = &t;
    page->url = url;
    page->zone = zone;
    page->gen = t.nav_gen;
    page->t0 = steady_ms();
    page->inst.time_limit_ms = doc ? 20000 : 5000;  // the viewer may need a while for huge documents
    std::string err = page->inst.load(module.data(), module.size(), make_imports(page.get()));
    if (!err.empty()) {
        t.title = L"Page error";
        show_doc(t, L"This page could not be started", widen(err) + L"\n\n" + widen(url), true);
        return;
    }
    Page &p = *page;
    p.f_start = find_export(p, "lw_start", "");
    p.f_resize = find_export(p, "lw_resize", "ii");
    p.f_frame = find_export(p, "lw_frame", "F");
    p.f_pointer = find_export(p, "lw_pointer", "iffi");
    p.f_key = find_export(p, "lw_key", "iii");
    p.f_char = find_export(p, "lw_char", "i");
    p.f_alloc = find_export(p, "lw_alloc", "i");
    p.f_on_file = find_export(p, "lw_on_file", "iiii");
    p.f_on_fetch = find_export(p, "lw_on_fetch", "iiii");
    p.f_on_fetch_ex = find_export(p, "lw_on_fetch_ex", "iiiiiiii");
    p.f_on_fetch_begin = find_export(p, "lw_on_fetch_begin", "iiiiii");
    p.f_on_fetch_data = find_export(p, "lw_on_fetch_data", "iii");
    p.f_on_fetch_end = find_export(p, "lw_on_fetch_end", "ii");
    if (p.f_on_fetch_begin < 0 || p.f_on_fetch_data < 0 || p.f_on_fetch_end < 0)
        p.f_on_fetch_begin = p.f_on_fetch_data = p.f_on_fetch_end = -1;  // all or nothing
    t.page = std::move(page);
    t.doc = Doc{};
    if (t.page->f_start < 0) {
        page_crashed(t, "the module does not export lw_start()");
        return;
    }
    if (!page_call(t, t.page->f_start, {})) return;
    int w, h;
    view_size(w, h);
    page_call(t, t.page->f_resize, {(uint32_t)w, (uint32_t)h});
    if (doc && streamed) stream_begin(t, *doc);
    else if (doc) give_response(t, 0, *doc);
    page_call(t, t.page->f_frame, {wasm::from_f64(steady_ms() - t.page->t0)});
    if (is_active(&t)) InvalidateRect(g_view, nullptr, FALSE);
}

// Shows what a load brought. With `streamed`, lr.r is just the head of an HTML document
// whose body is still arriving (WM_APP_STREAM).
void commit(Tab &t, LoadResult &lr, bool streamed = false) {
    net::Response &r = lr.r;
    // the network drops the #fragment; like browsers, keep it (also across redirects)
    size_t hash = lr.requested.find('#');
    if (hash != std::string::npos && !r.final_url.empty() && r.final_url.find('#') == std::string::npos)
        r.final_url += lr.requested.substr(hash);
    std::string url = r.final_url.empty() ? lr.requested : r.final_url;
    t.loading = streamed;
    t.stream_gen = streamed ? lr.gen : 0;

    // history
    if (lr.mode == 0) {
        if (t.hist_idx + 1 < (int)t.history.size()) t.history.resize(t.hist_idx + 1);
        if (t.history.empty() || t.history.back() != url) t.history.push_back(url);
        t.hist_idx = (int)t.history.size() - 1;
    } else if (lr.mode == 2) {
        t.hist_idx = lr.hist_target;
        t.history[t.hist_idx] = url;
    } else if (t.hist_idx >= 0) {
        t.history[t.hist_idx] = url;
    }
    t.current_url = url;
    bool active = is_active(&t);
    if (active) {
        if (!t.typed_dirty) set_url_bar(url);
        update_buttons();
        SetCursor(LoadCursor(nullptr, IDC_ARROW));
    }

    t.page.reset();
    t.frame = Frame();
    t.title.clear();

    const std::string ct = r.content_type;
    if (r.status == 0) {
        t.title = L"Can't reach this page";
        show_doc(t, L"Can't reach this page", widen(r.error) + L"\n\n" + widen(r.requested_url.empty() ? url : r.requested_url), true);
    } else if (ct == "about") {
        std::wstring h, body = about_page(url, h);
        t.title = h;
        show_doc(t, h, body, false);
    } else if (r.body.size() >= 4 && std::memcmp(r.body.data(), "\0asm", 4) == 0) {
        start_wasm_page(t, r.body, url, r.zone, nullptr);
    } else if (is_html(ct, r.body)) {
        std::vector<uint8_t> viewer = viewer_module();
        if (viewer.empty()) {
            t.title = L"HTML page";
            show_doc(t, L"This is an HTML page", L"The HTML viewer (viewer.wasm) is missing from this build.\n\n" + widen(url), true);
        } else {
            start_wasm_page(t, viewer, url, r.zone, &r, streamed);
        }
    } else if (r.status >= 400) {
        t.title = widen("HTTP " + std::to_string(r.status));
        std::wstring detail = looks_like_text(r.body) && r.body.size() < 2000 && ct.rfind("text/plain", 0) == 0
                                  ? widen(std::string(r.body.begin(), r.body.end())) + L"\n"
                                  : L"";
        std::string asked = r.requested_url.empty() ? url : r.requested_url;
        show_doc(t, L"The server answered " + widen(std::to_string(r.status)), detail + L"Requested: " + widen(asked), true);
    } else if (ct.rfind("image/", 0) == 0 || (!looks_like_text(r.body) && ct.rfind("text/", 0) != 0)) {
        image::Image img;
        std::string err;
        if (image::decode(r.body.data(), r.body.size(), img, err)) {
            t.frame.w = img.w;
            t.frame.h = img.h;
            t.frame.is_image = true;
            t.frame.px.resize((size_t)img.w * img.h);
            for (size_t i = 0; i < t.frame.px.size(); i++) {
                const uint8_t *s = img.rgba.data() + i * 4;
                uint32_t a = s[3], na = 255 - a;  // over a light grey
                uint32_t r8 = (s[0] * a + 238 * na) / 255, g8 = (s[1] * a + 238 * na) / 255, b8 = (s[2] * a + 238 * na) / 255;
                t.frame.px[i] = 0xFF000000u | r8 << 16 | g8 << 8 | b8;
            }
            size_t slash = url.find_last_of('/');
            t.title = widen(url.substr(slash + 1) + " (" + std::to_string(img.w) + "×" + std::to_string(img.h) + ")");
            if (active) InvalidateRect(g_view, nullptr, FALSE);
        } else {
            t.title = L"Unsupported file";
            show_doc(t, L"Low-web can't show this file", widen("Content type: " + (ct.empty() ? std::string("unknown") : ct) +
                                                               "\n" + std::to_string(r.body.size()) + " bytes\n\n" + url), true);
        }
    } else {
        size_t slash = url.find_last_of('/');
        t.title = widen(url.substr(slash + 1));
        std::wstring body = widen(std::string(r.body.begin(), r.body.end()));
        body.erase(std::remove(body.begin(), body.end(), L'\r'), body.end());
        show_doc(t, L"", body, false, true);
    }
    if (active) {
        update_title();
        if (!g_script_mode && GetFocus() != g_url) SetFocus(g_view);
    } else {
        invalidate_tabs();
    }
}

// ---- tabs --------------------------------------------------------------------------------

void activate_tab(int index) {
    if (index < 0 || index >= (int)g_tabs.size()) return;
    // keep what the user was typing in the tab we leave
    if (!g_tabs.empty() && g_active < (int)g_tabs.size()) {
        Tab &old = T();
        int n = GetWindowTextLengthW(g_url);
        std::wstring text(n, 0);
        GetWindowTextW(g_url, text.data(), n + 1);
        old.typed_dirty = text != display_url(old.current_url) && !old.loading;
        old.typed = text;
    }
    g_active = index;
    Tab &t = T();
    SetWindowTextW(g_url, t.typed_dirty ? t.typed.c_str() : display_url(t.current_url).c_str());
    update_buttons();
    update_title();
    if (t.page && !t.page->crashed) {  // the view may have changed size while it was hidden
        int w, h;
        view_size(w, h);
        page_call(t, t.page->f_resize, {(uint32_t)w, (uint32_t)h});
    }
    InvalidateRect(g_view, nullptr, FALSE);
    invalidate_tabs();
}

Tab &new_tab(const std::string &url, bool foreground) {
    auto t = std::make_unique<Tab>();
    t->id = g_next_tab_id++;
    Tab &ref = *t;
    int insert_at = g_tabs.empty() ? 0 : g_active + 1;
    // background tabs opened from the same tab go after each other
    while (!foreground && insert_at < (int)g_tabs.size() && g_tabs[insert_at]->loading && g_tabs[insert_at]->history.empty()) insert_at++;
    g_tabs.insert(g_tabs.begin() + insert_at, std::move(t));
    if (foreground || g_tabs.size() == 1) activate_tab(insert_at);
    else if (insert_at <= g_active) g_active++;
    if (!url.empty()) start_load(ref, url, 0);
    invalidate_tabs();
    return ref;
}

void close_tab(int index) {
    if (index < 0 || index >= (int)g_tabs.size()) return;
    if (!g_tabs[index]->current_url.empty()) g_closed.push_back(g_tabs[index]->current_url);
    if (g_tabs.size() == 1) {
        PostMessageW(g_main, WM_CLOSE, 0, 0);
        return;
    }
    bool was_active = index == g_active;
    g_tabs.erase(g_tabs.begin() + index);
    if (index < g_active || (was_active && g_active >= (int)g_tabs.size())) g_active--;
    if (was_active) {
        int a = std::min(g_active, (int)g_tabs.size() - 1);
        g_active = a;
        Tab &t = T();
        SetWindowTextW(g_url, t.typed_dirty ? t.typed.c_str() : display_url(t.current_url).c_str());
        update_buttons();
        update_title();
        if (t.page && !t.page->crashed) {
            int w, h;
            view_size(w, h);
            page_call(t, t.page->f_resize, {(uint32_t)w, (uint32_t)h});
        }
        InvalidateRect(g_view, nullptr, FALSE);
    }
    invalidate_tabs();
}

// =====================================================================================
// view window: shows the active tab's frame or native document, turns input into calls
// =====================================================================================

RECT frame_rect(const Frame &f) {
    int vw, vh;
    view_size(vw, vh);
    RECT d{0, 0, vw, vh};
    if (!f.w) return d;
    double s = std::min((double)vw / f.w, (double)vh / f.h);
    if (f.is_image && s > 1) s = 1;
    if (f.w == vw && f.h == vh) s = 1;
    int dw = std::max(1, (int)std::lround(f.w * s)), dh = std::max(1, (int)std::lround(f.h * s));
    d.left = (vw - dw) / 2;
    d.top = (vh - dh) / 2;
    d.right = d.left + dw;
    d.bottom = d.top + dh;
    return d;
}

void paint_doc(HDC hdc, const RECT &rc, Doc &doc) {
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    HBRUSH bg = CreateSolidBrush(RGB(248, 248, 246));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);
    SetBkMode(mem, TRANSPARENT);

    int margin = S(40), maxw = std::min<int>(rc.right - 2 * margin, S(820));
    int x = std::max<int>(margin, ((int)rc.right - maxw) / 2);
    int y = S(36) - doc.scroll;
    if (doc.mono) { x = S(24); maxw = rc.right - S(48); y = S(20) - doc.scroll; }
    if (doc.error) {
        RECT bar{x - S(20), y + S(4), x - S(14), y + S(34)};
        HBRUSH b = CreateSolidBrush(RGB(220, 38, 38));
        FillRect(mem, &bar, b);
        DeleteObject(b);
    }
    if (!doc.heading.empty()) {
        SelectObject(mem, g_doc_bold);
        SetTextColor(mem, RGB(17, 24, 39));
        RECT hr{x, y, x + maxw, y + S(400)};
        DrawTextW(mem, doc.heading.c_str(), -1, &hr, DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
        DrawTextW(mem, doc.heading.c_str(), -1, &hr, DT_WORDBREAK | DT_NOPREFIX);
        y = hr.bottom + S(16);
    }
    SelectObject(mem, doc.mono ? g_mono_font : g_doc_font);
    SetTextColor(mem, RGB(55, 65, 81));
    RECT br{x, y, x + maxw, y + 100000};
    DrawTextW(mem, doc.body.c_str(), -1, &br, DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS | DT_CALCRECT);
    int content_bottom = br.bottom + doc.scroll + S(40);
    if (doc.scroll > 0 && content_bottom - doc.scroll < rc.bottom) doc.scroll = std::max(0, content_bottom - (int)rc.bottom);
    DrawTextW(mem, doc.body.c_str(), -1, &br, DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS);

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

int doc_max_scroll(const Doc &doc) {
    HDC dc = GetDC(g_view);
    RECT rc;
    GetClientRect(g_view, &rc);
    SelectObject(dc, doc.mono ? g_mono_font : g_doc_font);
    int maxw = doc.mono ? rc.right - S(48) : std::min<int>(rc.right - 2 * S(40), S(820));
    RECT br{0, 0, maxw, 100000};
    DrawTextW(dc, doc.body.c_str(), -1, &br, DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS | DT_CALCRECT);
    ReleaseDC(g_view, dc);
    int total = br.bottom + S(doc.heading.empty() ? 60 : 110);
    return std::max(0, total - (int)rc.bottom);
}

void scroll_doc(int dy) {
    Tab &t = T();
    if (t.frame.w) return;
    t.doc.scroll = std::clamp(t.doc.scroll + dy, 0, doc_max_scroll(t.doc));
    InvalidateRect(g_view, nullptr, FALSE);
}

void paint_view(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (g_tabs.empty()) { EndPaint(hwnd, &ps); return; }
    Tab &t = T();
    if (t.frame.w) {
        RECT d = frame_rect(t.frame);
        HBRUSH dark = CreateSolidBrush(t.frame.is_image ? RGB(32, 33, 36) : RGB(24, 24, 27));
        RECT parts[4] = {{0, 0, rc.right, d.top}, {0, d.bottom, rc.right, rc.bottom}, {0, d.top, d.left, d.bottom}, {d.right, d.top, rc.right, d.bottom}};
        for (auto &p : parts)
            if (p.right > p.left && p.bottom > p.top) FillRect(hdc, &p, dark);
        DeleteObject(dark);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = t.frame.w;
        bi.bmiHeader.biHeight = -t.frame.h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        int dw = d.right - d.left, dh = d.bottom - d.top;
        SetStretchBltMode(hdc, dw < t.frame.w ? HALFTONE : COLORONCOLOR);
        SetBrushOrgEx(hdc, 0, 0, nullptr);
        StretchDIBits(hdc, d.left, d.top, dw, dh, 0, 0, t.frame.w, t.frame.h, t.frame.px.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
    } else if (t.page && !t.page->crashed) {
        HBRUSH b = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(hdc, &rc, b);
        DeleteObject(b);
    } else if (t.doc.heading.empty() && t.doc.body.empty() && t.loading) {
        HBRUSH b = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(hdc, &rc, b);
        DeleteObject(b);
    } else {
        paint_doc(hdc, rc, t.doc);
    }
    if (t.loading) {
        RECT bar{0, 0, rc.right, S(3)};
        HBRUSH b = CreateSolidBrush(kAccent);
        FillRect(hdc, &bar, b);
        DeleteObject(b);
    }
    EndPaint(hwnd, &ps);
}

void set_cursor_kind(int kind) {
    LPCWSTR id = IDC_ARROW;
    switch (kind) {
    case 1: id = IDC_HAND; break;
    case 2: SetCursor(nullptr); return;
    case 3: id = IDC_CROSS; break;
    case 4: id = IDC_IBEAM; break;
    }
    SetCursor(LoadCursor(nullptr, id));
}

void pointer_event(int kind, int vx, int vy, int button) {
    if (g_tabs.empty()) return;
    Tab &t = T();
    if (!t.page || t.page->crashed || t.page->f_pointer < 0 || !t.frame.w) return;
    RECT d = frame_rect(t.frame);
    float fx = (float)((vx + 0.5 - d.left) * t.frame.w / std::max<LONG>(1, d.right - d.left));
    float fy = (float)((vy + 0.5 - d.top) * t.frame.h / std::max<LONG>(1, d.bottom - d.top));
    uint64_t cur = 0;
    if (page_call(t, t.page->f_pointer, {(uint32_t)kind, wasm::from_f32(fx), wasm::from_f32(fy), (uint32_t)button}, &cur)) {
        if (t.page) t.page->cursor = (int)cur;
        if (kind != 3) set_cursor_kind((int)cur);
    }
}

void focus_url_bar() {
    SetFocus(g_url);
    SendMessageW(g_url, EM_SETSEL, 0, -1);
}

void go_home() { navigate(g_home_url); }

void new_tab_command() {
    new_tab(g_home_url, true);
    if (!g_script_mode) focus_url_bar();
}

// Browser-level shortcuts. Returns true if handled.
bool browser_key(WPARAM vk, int mods) {
    bool ctrl = mods & 2, alt = mods & 4, shift = mods & 1;
    if ((ctrl && vk == 'L') || vk == VK_F6 || (alt && vk == 'D')) { focus_url_bar(); return true; }
    if (vk == VK_F5 || (ctrl && vk == 'R')) { reload(ctrl && (vk == VK_F5 || shift)); return true; }  // Ctrl+F5, Ctrl+Shift+R: hard
    if ((alt && vk == VK_LEFT) || vk == VK_BROWSER_BACK) { go_history(-1); return true; }
    if ((alt && vk == VK_RIGHT) || vk == VK_BROWSER_FORWARD) { go_history(1); return true; }
    if ((alt && vk == VK_HOME) || vk == VK_BROWSER_HOME) { go_home(); return true; }
    if (ctrl && !shift && vk == 'T') { new_tab_command(); return true; }
    if (ctrl && shift && vk == 'T') {
        if (!g_closed.empty()) { std::string u = g_closed.back(); g_closed.pop_back(); new_tab(u, true); }
        return true;
    }
    if (ctrl && (vk == 'W' || vk == VK_F4)) { close_tab(g_active); return true; }
    if (ctrl && vk == VK_TAB) { activate_tab((g_active + (shift ? (int)g_tabs.size() - 1 : 1)) % (int)g_tabs.size()); return true; }
    if (ctrl && vk == VK_NEXT) { activate_tab((g_active + 1) % (int)g_tabs.size()); return true; }
    if (ctrl && vk == VK_PRIOR) { activate_tab((g_active + (int)g_tabs.size() - 1) % (int)g_tabs.size()); return true; }
    if (ctrl && vk >= '1' && vk <= '8') { activate_tab(std::min((int)(vk - '1'), (int)g_tabs.size() - 1)); return true; }
    if (ctrl && vk == '9') { activate_tab((int)g_tabs.size() - 1); return true; }
    return false;
}

LRESULT CALLBACK view_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static bool tracking = false;
    static int buttons_down = 0;
    static wchar_t high_surrogate = 0;
    if (g_tabs.empty()) return DefWindowProcW(hwnd, msg, wp, lp);
    Tab &t = T();
    switch (msg) {
    case WM_PAINT: paint_view(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            set_cursor_kind(t.page && t.frame.w && !t.page->crashed ? t.page->cursor : 0);
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE:
        if (!tracking && !g_script_mode) {  // scripted moves have no real cursor: it would "leave" at once
            TRACKMOUSEEVENT tme{sizeof tme, TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            tracking = true;
        }
        pointer_event(1, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), 0);
        return 0;
    case WM_MOUSELEAVE:
        tracking = false;
        pointer_event(3, -1, -1, 0);
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: {
        SetFocus(hwnd);
        SetCapture(hwnd);
        buttons_down++;
        int b = msg == WM_LBUTTONDOWN ? 0 : msg == WM_MBUTTONDOWN ? 1 : 2;
        pointer_event(0, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), b);
        return 0;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
        int b = msg == WM_LBUTTONUP ? 0 : msg == WM_MBUTTONUP ? 1 : 2;
        if (--buttons_down <= 0) { buttons_down = 0; ReleaseCapture(); }
        pointer_event(2, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), b);
        return 0;
    }
    case WM_XBUTTONUP:
        go_history(GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? -1 : 1);
        return TRUE;
    case WM_MOUSEWHEEL: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ScreenToClient(hwnd, &pt);
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (t.frame.w) pointer_event(4, pt.x, pt.y, delta);
        else scroll_doc(-delta * S(48) / 120);
        return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        int mods = current_mods();
        if (browser_key(wp, mods)) return 0;
        Tab &a = T();  // browser_key may have switched tabs
        if (a.page && a.page->f_key >= 0 && !a.page->crashed) {
            page_call(a, a.page->f_key, {(uint32_t)wp, (uint32_t)mods, 1u});
            if (msg == WM_KEYDOWN) return 0;
        } else if (!a.frame.w) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            switch (wp) {
            case VK_DOWN: scroll_doc(S(40)); return 0;
            case VK_UP: scroll_doc(-S(40)); return 0;
            case VK_NEXT: case VK_SPACE: scroll_doc(rc.bottom - S(40)); return 0;
            case VK_PRIOR: scroll_doc(-(rc.bottom - S(40))); return 0;
            case VK_HOME: scroll_doc(-1000000); return 0;
            case VK_END: scroll_doc(1000000); return 0;
            }
        }
        break;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (t.page && t.page->f_key >= 0) page_call(t, t.page->f_key, {(uint32_t)wp, (uint32_t)current_mods(), 0u});
        break;
    case WM_CHAR: {
        wchar_t c = (wchar_t)wp;
        if (c >= 0xD800 && c < 0xDC00) { high_surrogate = c; return 0; }
        uint32_t cp = c;
        if (c >= 0xDC00 && c < 0xE000 && high_surrogate) cp = 0x10000 + ((high_surrogate - 0xD800) << 10) + (c - 0xDC00);
        high_surrogate = 0;
        if (cp >= 32 && cp != 127 && t.page && t.page->f_char >= 0) page_call(t, t.page->f_char, {cp});
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        wchar_t path[MAX_PATH * 4];
        if (DragQueryFileW(drop, 0, path, (UINT)std::size(path))) {
            std::wstring p = path;
            std::wstring lower_p = p;
            std::transform(lower_p.begin(), lower_p.end(), lower_p.begin(), ::towlower);
            bool is_wasm = lower_p.size() > 5 && lower_p.compare(lower_p.size() - 5, 5, L".wasm") == 0;
            if (!is_wasm && t.page && t.page->f_on_file >= 0 && !t.page->crashed) {
                std::vector<uint8_t> bytes;
                if (read_file(p, bytes)) deliver_file(t, bytes, base_name(p));
            } else {
                navigate(net::from_user_input(narrow(p)));
            }
        }
        DragFinish(drop);
        return 0;
    }
    case WM_SIZE:
        if (t.page && !t.page->crashed) page_call(t, t.page->f_resize, {(uint32_t)LOWORD(lp), (uint32_t)HIWORD(lp)});
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// =====================================================================================
// the tab strip (drawn by us at the top of the main window)
// =====================================================================================

int g_tab_hover = -1, g_tab_hover_close = -1;
bool g_plus_hover = false, g_strip_tracking = false;

int tabstrip_height() { return S(36); }
int toolbar_height() { return S(44); }

struct TabGeom {
    RECT tab, close;
};

std::vector<TabGeom> tab_geometry(RECT &plus) {
    RECT rc;
    GetClientRect(g_main, &rc);
    int n = (int)g_tabs.size();
    int left = S(8), top = S(6), h = tabstrip_height() - top;
    int avail = rc.right - left - S(48);
    int w = n ? std::clamp(avail / n, S(48), S(230)) : S(230);
    std::vector<TabGeom> g(n);
    for (int i = 0; i < n; i++) {
        RECT r{left + i * w, top, left + (i + 1) * w, top + h};
        int cs = S(16);
        RECT c{r.right - S(8) - cs, r.top + (h - cs) / 2, r.right - S(8), r.top + (h - cs) / 2 + cs};
        g[i] = {r, c};
    }
    int px = left + n * w + S(4);
    plus = RECT{px, top + S(4), px + S(26), top + S(4) + S(26)};
    return g;
}

void invalidate_tabs() {
    if (!g_main) return;
    RECT r;
    GetClientRect(g_main, &r);
    r.bottom = tabstrip_height();
    InvalidateRect(g_main, &r, FALSE);
}

void paint_tabstrip(HDC hdc) {
    RECT rc;
    GetClientRect(g_main, &rc);
    int H = tabstrip_height();
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, H);
    HGDIOBJ old_bmp = SelectObject(mem, bmp);
    RECT all{0, 0, rc.right, H};
    HBRUSH strip = CreateSolidBrush(RGB(222, 225, 230));
    FillRect(mem, &all, strip);
    DeleteObject(strip);
    SetBkMode(mem, TRANSPARENT);
    SelectObject(mem, g_tab_font);
    RECT plus;
    std::vector<TabGeom> geo = tab_geometry(plus);
    HBRUSH active_b = CreateSolidBrush(RGB(241, 243, 244)), hover_b = CreateSolidBrush(RGB(234, 236, 239));
    HPEN sep = CreatePen(PS_SOLID, 1, RGB(170, 174, 180));
    HGDIOBJ old_pen = SelectObject(mem, sep);
    for (int i = 0; i < (int)geo.size(); i++) {
        const Tab &t = *g_tabs[i];
        RECT r = geo[i].tab;
        bool active = i == g_active, hover = i == g_tab_hover;
        if (active || hover) {
            HRGN rgn = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + S(10), S(12), S(12));
            FillRgn(mem, rgn, active ? active_b : hover_b);
            DeleteObject(rgn);
        } else if (i + 1 != g_active && i + 1 < (int)geo.size()) {  // separator between inactive tabs
            MoveToEx(mem, r.right, r.top + S(8), nullptr);
            LineTo(mem, r.right, r.bottom - S(6));
        }
        // loading indicator or a small page glyph
        int cx = r.left + S(18), cy = (r.top + r.bottom) / 2;
        HBRUSH dot = CreateSolidBrush(t.loading ? kAccent : RGB(150, 155, 160));
        RECT d{cx - S(4), cy - S(4), cx + S(4), cy + S(4)};
        HRGN drgn = CreateEllipticRgnIndirect(&d);
        FillRgn(mem, drgn, dot);
        DeleteObject(drgn);
        DeleteObject(dot);
        // title
        std::wstring label = tab_label(t);
        RECT tr{r.left + S(30), r.top, geo[i].close.left - S(4), r.bottom};
        SetTextColor(mem, active ? RGB(32, 33, 36) : RGB(80, 84, 90));
        DrawTextW(mem, label.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        // close button (on the active and the hovered tab)
        if (active || hover) {
            RECT c = geo[i].close;
            if (i == g_tab_hover_close) {
                HRGN crgn = CreateEllipticRgn(c.left - S(2), c.top - S(2), c.right + S(2), c.bottom + S(2));
                HBRUSH cb = CreateSolidBrush(RGB(210, 213, 218));
                FillRgn(mem, crgn, cb);
                DeleteObject(cb);
                DeleteObject(crgn);
            }
            SetTextColor(mem, RGB(60, 64, 67));
            DrawTextW(mem, L"✕", -1, &c, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        }
    }
    if (g_plus_hover) {
        HRGN prgn = CreateRoundRectRgn(plus.left, plus.top, plus.right, plus.bottom, S(8), S(8));
        FillRgn(mem, prgn, hover_b);
        DeleteObject(prgn);
    }
    SetTextColor(mem, RGB(60, 64, 67));
    DrawTextW(mem, L"+", -1, &plus, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
    SelectObject(mem, old_pen);
    DeleteObject(sep);
    DeleteObject(active_b);
    DeleteObject(hover_b);
    BitBlt(hdc, 0, 0, rc.right, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// Returns the tab index under (x, y), -1 for none; *on_close / *on_plus are set as well.
int tabstrip_hit(int x, int y, bool *on_close, bool *on_plus) {
    RECT plus;
    std::vector<TabGeom> geo = tab_geometry(plus);
    POINT pt{x, y};
    *on_close = *on_plus = false;
    if (PtInRect(&plus, pt)) { *on_plus = true; return -1; }
    for (int i = 0; i < (int)geo.size(); i++)
        if (PtInRect(&geo[i].tab, pt)) {
            RECT c = geo[i].close;
            InflateRect(&c, S(3), S(3));
            *on_close = PtInRect(&c, pt) && (i == g_active || i == g_tab_hover);
            return i;
        }
    return -1;
}

// =====================================================================================
// the address bar and the main window
// =====================================================================================

LRESULT CALLBACK url_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    static bool select_on_click = false;
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_RETURN) {
            int n = GetWindowTextLengthW(hwnd);
            std::wstring text(n, 0);
            GetWindowTextW(hwnd, text.data(), n + 1);
            std::string url = address_or_search(narrow(text));
            if (!url.empty()) {
                SetFocus(g_view);
                if (current_mods() & 4) new_tab(url, true);  // Alt+Enter: in a new tab
                else navigate(url);
            }
            return 0;
        }
        if (wp == VK_ESCAPE) {
            set_url_bar(T().current_url);
            T().typed_dirty = false;
            SetFocus(g_view);
            return 0;
        }
        if (browser_key(wp, current_mods()) && wp != 'L') return 0;
        break;
    case WM_CHAR:
        if (wp == '\r' || wp == 27) return 0;
        break;
    case WM_SETFOCUS:
        select_on_click = true;
        PostMessageW(hwnd, EM_SETSEL, 0, -1);
        break;
    case WM_LBUTTONUP:
        if (select_on_click) {
            select_on_click = false;
            DWORD s = 0, e = 0;
            SendMessageW(hwnd, EM_GETSEL, (WPARAM)&s, (LPARAM)&e);
            if (s == e) SendMessageW(hwnd, EM_SETSEL, 0, -1);
        }
        break;
    }
    return CallWindowProcW(g_url_proc, hwnd, msg, wp, lp);
}

void make_fonts() {
    for (HFONT f : {g_ui_font, g_doc_font, g_doc_bold, g_mono_font, g_tab_font})
        if (f) DeleteObject(f);
    auto mk = [](int pt, int weight, const wchar_t *face) {
        return CreateFontW(-MulDiv(pt, g_dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, face);
    };
    g_ui_font = mk(10, FW_NORMAL, L"Segoe UI");
    g_doc_font = mk(11, FW_NORMAL, L"Segoe UI");
    g_doc_bold = mk(20, FW_SEMIBOLD, L"Segoe UI");
    g_mono_font = mk(10, FW_NORMAL, L"Consolas");
    g_tab_font = mk(9, FW_NORMAL, L"Segoe UI");
    for (HWND h : {g_url, g_btn[0], g_btn[1], g_btn[2], g_btn[3], g_engine_btn})
        if (h) SendMessageW(h, WM_SETFONT, (WPARAM)g_ui_font, TRUE);
}

void update_engine_button() {
    if (g_engine_btn) SetWindowTextW(g_engine_btn, kEngines[g_engine].label);
}

void layout_children() {
    RECT rc;
    GetClientRect(g_main, &rc);
    int top = tabstrip_height(), th = toolbar_height(), bs = S(32), pad = S(6), y = top + (th - bs) / 2;
    int x = pad;
    for (int i = 0; i < 4; i++) {
        MoveWindow(g_btn[i], x, y, bs, bs, TRUE);
        x += bs + S(2);
    }
    x += S(6);
    int uh = S(28), ew = S(46);
    MoveWindow(g_engine_btn, x, top + (th - uh) / 2, ew, uh, TRUE);
    x += ew + S(4);
    MoveWindow(g_url, x, top + (th - uh) / 2, std::max<int>(50, rc.right - x - pad), uh, TRUE);
    MoveWindow(g_view, 0, top + th, rc.right, std::max<int>(1, rc.bottom - top - th), TRUE);
    InvalidateRect(g_main, nullptr, FALSE);
}

void show_engine_menu() {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Search with:");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (int i = 0; i < (int)std::size(kEngines); i++)
        AppendMenuW(m, MF_STRING | (i == g_engine ? MF_CHECKED : 0), ID_ENGINE_FIRST + i, kEngines[i].name);
    RECT r;
    GetWindowRect(g_engine_btn, &r);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, r.left, r.bottom, 0, g_main, nullptr);
    DestroyMenu(m);
    if (cmd >= ID_ENGINE_FIRST && cmd < ID_ENGINE_FIRST + (int)std::size(kEngines)) {
        g_engine = cmd - ID_ENGINE_FIRST;
        update_engine_button();
        save_settings();
    }
}

void on_open_file(OpenRequest &req) {
    Tab *t = tab_by_id(req.tab_id);
    if (!t || !t->page || t->page->gen != req.gen || t->page->crashed) return;
    std::wstring exts;
    size_t i = 0;
    const std::string &accept = req.accept;
    while (i <= accept.size()) {
        size_t j = accept.find(';', i);
        if (j == std::string::npos) j = accept.size();
        std::string e = accept.substr(i, j - i);
        e.erase(std::remove_if(e.begin(), e.end(), [](char c) { return c == '.' || c == '*' || c == ' '; }), e.end());
        if (!e.empty()) exts += (exts.empty() ? L"*." : L";*.") + widen(e);
        i = j + 1;
    }
    std::wstring filter;
    if (!exts.empty()) filter += L"Supported files (" + exts + L")" + std::wstring(1, L'\0') + exts + std::wstring(1, L'\0');
    filter += std::wstring(L"All files (*.*)") + L'\0' + L"*.*" + L'\0' + L'\0';
    wchar_t file[MAX_PATH * 4] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = (DWORD)std::size(file);
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return;
    std::vector<uint8_t> bytes;
    if (!read_file(file, bytes)) {
        MessageBoxW(g_main, L"Could not read that file (is it larger than 256 MB?).", L"Low-web", MB_ICONWARNING);
        return;
    }
    t = tab_by_id(req.tab_id);  // the dialog ran a message loop: the tab may be gone
    if (t && t->page && t->page->gen == req.gen) deliver_file(*t, bytes, base_name(file));
}

void on_save_file(SaveRequest &req) {
    Tab *t = tab_by_id(req.tab_id);
    if (!t || !t->page || t->page->gen != req.gen) return;
    std::wstring name = widen(req.name);
    for (wchar_t &c : name)
        if (wcschr(L"\\/:*?\"<>|", c) || c < 32) c = L'_';
    if (name.empty()) name = L"download";
    std::wstring path;
    if (g_script_mode) {
        if (g_save_dir.empty()) return;
        path = widen(g_save_dir) + L"\\" + name;
    } else {
        std::wstring ext;
        size_t dot = name.rfind(L'.');
        if (dot != std::wstring::npos) ext = name.substr(dot + 1);
        std::wstring filter;
        if (!ext.empty()) filter += ext + L" file (*." + ext + L")" + std::wstring(1, L'\0') + L"*." + ext + std::wstring(1, L'\0');
        filter += std::wstring(L"All files (*.*)") + L'\0' + L"*.*" + L'\0' + L'\0';
        wchar_t file[MAX_PATH * 4];
        wcsncpy(file, name.c_str(), std::size(file) - 1);
        file[std::size(file) - 1] = 0;
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof ofn;
        ofn.hwndOwner = g_main;
        ofn.lpstrFilter = filter.c_str();
        ofn.lpstrFile = file;
        ofn.nMaxFile = (DWORD)std::size(file);
        ofn.lpstrDefExt = ext.empty() ? nullptr : ext.c_str();
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
        if (!GetSaveFileNameW(&ofn)) return;
        path = file;
    }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD wrote = 0;
    bool ok = f != INVALID_HANDLE_VALUE && WriteFile(f, req.data.data(), (DWORD)req.data.size(), &wrote, nullptr) && wrote == req.data.size();
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!ok && !g_script_mode) MessageBoxW(g_main, (L"Could not save " + path).c_str(), L"Low-web", MB_ICONWARNING);
    log_line("[low-web] saved " + narrow(path) + (ok ? "" : " (FAILED)"));
}

// ---- streamed documents -------------------------------------------------------------------
// The viewer exports lw_on_fetch_begin/data/end and gets an HTML document while it
// downloads. A page without them gets the whole document at the end instead.

void stream_begin(Tab &t, const net::Response &head) {
    if (!t.page || t.page->f_on_fetch_begin < 0) return;
    std::string meta = head.content_type + head.final_url;
    uint32_t meta_at = 0;
    if (!page_give(t, nullptr, 0, meta, meta_at) || !t.page) return;
    page_call(t, t.page->f_on_fetch_begin, {0u, (uint32_t)head.status, meta_at, (uint32_t)head.content_type.size(),
                                            meta_at + (uint32_t)head.content_type.size(), (uint32_t)head.final_url.size()});
}

void stream_data(Tab &t, const std::vector<uint8_t> &data) {
    if (!t.page || t.page->f_on_fetch_data < 0 || data.empty()) return;
    uint32_t unused = 0;
    uint32_t p = page_give(t, data.data(), data.size(), std::string(), unused);
    if (p && t.page) page_call(t, t.page->f_on_fetch_data, {0u, p, (uint32_t)data.size()});
}

void stream_end(Tab &t, const net::Response &r) {
    t.stream_gen = 0;
    t.loading = false;
    if (r.status == 0) log_line("[low-web] the document did not finish loading: " + r.error);
    if (t.page && t.page->f_on_fetch_end >= 0) page_call(t, t.page->f_on_fetch_end, {0u, (uint32_t)r.status});
    else if (t.page) give_response(t, 0, r);  // a page that doesn't take streams
    if (is_active(&t)) update_title();
    invalidate_tabs();
}

// Hands a response to the page: lw_on_fetch_ex (with content type and final URL) if the
// page has it, else lw_on_fetch.
void give_response(Tab &t, int id, const net::Response &r) {
    if (!t.page || t.page->crashed || (t.page->f_on_fetch_ex < 0 && t.page->f_on_fetch < 0)) return;
    std::vector<uint8_t> data = r.status ? r.body : std::vector<uint8_t>(r.error.begin(), r.error.end());
    std::string meta = r.content_type + r.final_url;
    uint32_t meta_at = 0;
    uint32_t p = page_give(t, data.data(), data.size(), meta, meta_at);
    if (!p || !t.page) return;
    if (t.page->f_on_fetch_ex >= 0)
        page_call(t, t.page->f_on_fetch_ex, {(uint32_t)id, (uint32_t)r.status, p, (uint32_t)data.size(), meta_at,
                                             (uint32_t)r.content_type.size(), meta_at + (uint32_t)r.content_type.size(),
                                             (uint32_t)r.final_url.size()});
    else
        page_call(t, t.page->f_on_fetch, {(uint32_t)id, (uint32_t)r.status, p, (uint32_t)data.size()});
}

// ---- automation (used by tests): --script "wait 500; click 100 200; key 83 ctrl; ..." --------

void save_screenshot(const std::string &path) {
    RECT rc;
    GetClientRect(g_main, &rc);
    HDC wdc = GetDC(g_main);
    HDC mem = CreateCompatibleDC(wdc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = rc.right;
    bi.bmiHeader.biHeight = rc.bottom;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void *bits = nullptr;
    HBITMAP bmp = CreateDIBSection(wdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    PrintWindow(g_main, mem, 1 /* PW_CLIENTONLY */ | 2 /* PW_RENDERFULLCONTENT */);
    GdiFlush();
    FILE *f = _wfopen(widen(path).c_str(), L"wb");
    if (f) {
        uint32_t size = (uint32_t)rc.right * rc.bottom * 4;
        BITMAPFILEHEADER fh{0x4D42, (DWORD)(sizeof fh + sizeof bi.bmiHeader + size), 0, 0, (DWORD)(sizeof fh + sizeof bi.bmiHeader)};
        fwrite(&fh, sizeof fh, 1, f);
        fwrite(&bi.bmiHeader, sizeof bi.bmiHeader, 1, f);
        fwrite(bits, 1, size, f);
        fclose(f);
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(g_main, wdc);
}

void script_step() {
    if ((T().loading && !g_script_async) || steady_ms() < g_script_wait_until) return;
    if (g_script.empty()) {
        if (!g_screenshot_path.empty()) save_screenshot(g_screenshot_path);
        PostMessageW(g_main, WM_CLOSE, 0, 0);
        KillTimer(g_main, TIMER_SCRIPT);
        return;
    }
    std::string cmd = g_script.front();
    g_script.pop_front();
    char op[32] = "", s1[512] = "";
    int a = 0, b = 0, c = 0, d = 0;
    sscanf(cmd.c_str(), " %31s", op);
    std::string o = op;
    auto lp = [](int x, int y) { return (LPARAM)MAKELPARAM(x, y); };
    if (o == "wait") { sscanf(cmd.c_str(), " %*s %d", &a); g_script_wait_until = steady_ms() + a; }
    else if (o == "async") g_script_async = true;  // from now on, don't wait for loads to finish (to watch them)
    else if (o == "move") { sscanf(cmd.c_str(), " %*s %d %d", &a, &b); SendMessageW(g_view, WM_MOUSEMOVE, 0, lp(a, b)); }
    else if (o == "click" || o == "rclick" || o == "mclick") {
        sscanf(cmd.c_str(), " %*s %d %d", &a, &b);
        UINT down = o == "rclick" ? WM_RBUTTONDOWN : o == "mclick" ? WM_MBUTTONDOWN : WM_LBUTTONDOWN;
        SendMessageW(g_view, WM_MOUSEMOVE, 0, lp(a, b));
        SendMessageW(g_view, down, 0, lp(a, b));
        SendMessageW(g_view, down + 1, 0, lp(a, b));
    } else if (o == "drag") {
        sscanf(cmd.c_str(), " %*s %d %d %d %d", &a, &b, &c, &d);
        SendMessageW(g_view, WM_MOUSEMOVE, 0, lp(a, b));
        SendMessageW(g_view, WM_LBUTTONDOWN, MK_LBUTTON, lp(a, b));
        for (int k = 1; k <= 12; k++) SendMessageW(g_view, WM_MOUSEMOVE, MK_LBUTTON, lp(a + (c - a) * k / 12, b + (d - b) * k / 12));
        SendMessageW(g_view, WM_LBUTTONUP, 0, lp(c, d));
    } else if (o == "key") {  // key VK [ctrl|shift|ctrlshift|alt]
        sscanf(cmd.c_str(), " %*s %d %511s", &a, s1);
        std::string m = s1;
        g_fake_mods = (m.find("ctrl") != std::string::npos ? 2 : 0) | (m.find("shift") != std::string::npos ? 1 : 0) | (m == "alt" ? 4 : 0);
        SendMessageW(g_view, WM_KEYDOWN, (WPARAM)a, 0);
        SendMessageW(g_view, WM_KEYUP, (WPARAM)a, 0);
        g_fake_mods = 0;
    } else if (o == "char") { sscanf(cmd.c_str(), " %*s %511s", s1); SendMessageW(g_view, WM_CHAR, (WPARAM)(unsigned char)s1[0], 0); }
    else if (o == "wheel") { sscanf(cmd.c_str(), " %*s %d %d %d", &a, &b, &c); POINT pt{a, b}; ClientToScreen(g_view, &pt); SendMessageW(g_view, WM_MOUSEWHEEL, MAKEWPARAM(0, c), lp(pt.x, pt.y)); }
    else if (o == "nav") { sscanf(cmd.c_str(), " %*s %511s", s1); navigate(net::from_user_input(s1)); }
    else if (o == "type") {  // type TEXT...: into the address bar, then Enter
        std::string text = cmd.substr(cmd.find("type") + 5);
        SetWindowTextW(g_url, widen(text).c_str());
        SendMessageW(g_url, WM_KEYDOWN, VK_RETURN, 0);
    } else if (o == "engine") { sscanf(cmd.c_str(), " %*s %d", &a); g_engine = std::clamp(a, 0, (int)std::size(kEngines) - 1); update_engine_button(); }
    else if (o == "tabclick") {  // tabclick X Y: a click on the tab strip (main window coordinates)
        sscanf(cmd.c_str(), " %*s %d %d", &a, &b);
        SendMessageW(g_main, WM_LBUTTONDOWN, 0, lp(a, b));
        SendMessageW(g_main, WM_LBUTTONUP, 0, lp(a, b));
    } else if (o == "back") go_history(-1);
    else if (o == "drop") {  // deliver a file as if dropped
        std::string path = cmd.substr(cmd.find("drop") + 5);
        std::vector<uint8_t> bytes;
        if (read_file(widen(path), bytes)) deliver_file(T(), bytes, base_name(widen(path)));
        else log_line("[script] cannot read " + path);
    } else if (o == "shot") { sscanf(cmd.c_str(), " %*s %511s", s1); save_screenshot(s1); }
    else log_line("[script] unknown command: " + cmd);
}

LRESULT CALLBACK main_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_main = hwnd;
        const wchar_t *labels[4] = {L"←", L"→", L"↻", L"⌂"};
        for (int i = 0; i < 4; i++)
            g_btn[i] = CreateWindowW(L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, hwnd,
                                     (HMENU)(INT_PTR)(ID_BACK + i), g_inst, nullptr);
        g_engine_btn = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, hwnd,
                                     (HMENU)(INT_PTR)ID_ENGINE, g_inst, nullptr);
        g_url = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 10, 10, hwnd,
                                (HMENU)ID_URL, g_inst, nullptr);
        SendMessageW(g_url, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search or type an address");
        g_url_proc = (WNDPROC)SetWindowLongPtrW(g_url, GWLP_WNDPROC, (LONG_PTR)url_proc);
        g_view = CreateWindowExW(WS_EX_ACCEPTFILES, L"LowWebView", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10,
                                 hwnd, nullptr, g_inst, nullptr);
        make_fonts();
        update_engine_button();
        SetTimer(hwnd, TIMER_FRAME, 15, nullptr);
        return 0;
    }
    case WM_SIZE: layout_children(); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        paint_tabstrip(hdc);
        RECT rc;
        GetClientRect(hwnd, &rc);
        RECT bar{0, tabstrip_height(), rc.right, tabstrip_height() + toolbar_height()};
        HBRUSH b = CreateSolidBrush(RGB(241, 243, 244));
        FillRect(hdc, &bar, b);
        DeleteObject(b);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        bool on_close = false, on_plus = false;
        int hit = y < tabstrip_height() ? tabstrip_hit(x, y, &on_close, &on_plus) : -1;
        int hc = on_close ? hit : -1;
        if (hit != g_tab_hover || hc != g_tab_hover_close || on_plus != g_plus_hover) {
            g_tab_hover = hit;
            g_tab_hover_close = hc;
            g_plus_hover = on_plus;
            // hovering a tab shows its close button, which changes the hit test: redo it
            tabstrip_hit(x, y, &on_close, &on_plus);
            g_tab_hover_close = on_close ? hit : -1;
            invalidate_tabs();
        }
        if (!g_strip_tracking && !g_script_mode) {
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&t);
            g_strip_tracking = true;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_strip_tracking = false;
        g_tab_hover = g_tab_hover_close = -1;
        g_plus_hover = false;
        invalidate_tabs();
        return 0;
    case WM_LBUTTONDOWN: case WM_MBUTTONUP: case WM_LBUTTONDBLCLK: {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (y >= tabstrip_height()) break;
        bool on_close = false, on_plus = false;
        g_tab_hover = -1;
        int hit = tabstrip_hit(x, y, &on_close, &on_plus);
        if (msg == WM_MBUTTONUP) { if (hit >= 0) close_tab(hit); return 0; }
        if (on_plus || (msg == WM_LBUTTONDBLCLK && hit < 0)) { new_tab_command(); return 0; }
        if (hit >= 0) {
            if (on_close) close_tab(hit);
            else if (hit != g_active) activate_tab(hit);
            if (!g_tabs.empty() && GetFocus() != g_url) SetFocus(g_view);
        }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto *mm = (MINMAXINFO *)lp;
        mm->ptMinTrackSize = {S(480), S(320)};
        return 0;
    }
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        make_fonts();
        RECT *r = (RECT *)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout_children();
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_BACK: go_history(-1); SetFocus(g_view); return 0;
        case ID_FORWARD: go_history(1); SetFocus(g_view); return 0;
        case ID_RELOAD: reload(); SetFocus(g_view); return 0;
        case ID_HOME: go_home(); SetFocus(g_view); return 0;
        case ID_ENGINE: show_engine_menu(); SetFocus(g_url); return 0;
        }
        break;
    case WM_TIMER:
        if (wp == TIMER_FRAME && !g_tabs.empty()) {  // only the visible tab's page animates
            Tab &t = T();
            if (t.page && !t.page->crashed && t.page->f_frame >= 0) page_call(t, t.page->f_frame, {wasm::from_f64(steady_ms() - t.page->t0)});
        }
        if (wp == TIMER_SCRIPT) script_step();
        return 0;
    case WM_APP_LOADED: {
        std::unique_ptr<LoadResult> lr((LoadResult *)lp);
        Tab *t = tab_by_id(lr->tab_id);
        if (t && lr->gen == t->nav_gen) {
            if (t->stream_gen == lr->gen) stream_end(*t, lr->r);
            else commit(*t, *lr);
        }
        return 0;
    }
    case WM_APP_STREAM: {
        std::unique_ptr<StreamMsg> m((StreamMsg *)lp);
        Tab *t = tab_by_id(m->tab_id);
        if (!t || m->gen != t->nav_gen) return 0;
        if (m->begin) commit(*t, m->head, true);
        else if (t->stream_gen == m->gen) stream_data(*t, m->data);
        return 0;
    }
    case WM_APP_FETCHED: {
        std::unique_ptr<FetchResult> fr((FetchResult *)lp);
        Tab *t = tab_by_id(fr->tab_id);
        if (t && t->page && t->page->gen == fr->gen) give_response(*t, fr->id, fr->r);
        return 0;
    }
    case WM_APP_NAVIGATE: {
        std::unique_ptr<NavRequest> req((NavRequest *)lp);
        Tab *t = tab_by_id(req->tab_id);
        if (!t || !t->page || t->page->gen != req->gen) return 0;
        if (req->new_tab) {
            Tab &nt = new_tab("", req->new_tab == 1);
            start_load(nt, req->url, 0, -1, req->post ? &req->body : nullptr, false, req->initiator, req->access);
        } else {
            start_load(*t, req->url, 0, -1, req->post ? &req->body : nullptr, false, req->initiator, req->access);
        }
        return 0;
    }
    case WM_APP_OPENFILE: {
        std::unique_ptr<OpenRequest> req((OpenRequest *)lp);
        if (!g_script_mode) on_open_file(*req);
        return 0;
    }
    case WM_APP_SAVEFILE: {
        std::unique_ptr<SaveRequest> req((SaveRequest *)lp);
        on_save_file(*req);
        return 0;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE && GetFocus() != g_url) SetFocus(g_view);
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

std::string exe_dir() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    std::wstring p(buf, n);
    return narrow(p.substr(0, p.find_last_of(L"\\/")));
}

std::string find_local_site() {
    std::string dir = exe_dir();
    for (const char *rel : {"\\..\\sites\\www", "\\sites\\www", "\\www"}) {
        std::string d = dir + rel;
        wchar_t full[MAX_PATH * 4];
        if (!GetFullPathNameW(widen(d).c_str(), (DWORD)std::size(full), full, nullptr)) continue;
        std::wstring idx = std::wstring(full) + L"\\index.wasm";
        if (GetFileAttributesW(idx.c_str()) != INVALID_FILE_ATTRIBUTES)
            return net::from_user_input(narrow(full) + "\\");
    }
    return "";
}

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    g_inst = inst;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        freopen("CONOUT$", "w", stderr);
        SetConsoleOutputCP(CP_UTF8);
    }
    using SetCtx = BOOL(WINAPI *)(HANDLE);
    if (auto f = (SetCtx)(void *)GetProcAddress(GetModuleHandleW(L"user32"), "SetProcessDpiAwarenessContext"))
        f((HANDLE)-4 /* PER_MONITOR_AWARE_V2 */);
    else
        SetProcessDPIAware();
    net::init();
    net::set_logger([](const std::string &line) { log_line(line); });

    // command line: [URL...] [--size WxH] [--script "..."] [--screenshot out.bmp] [--save-dir DIR] [--log FILE]
    //               [--no-cache] [--cache-dir DIR] [--cookie-file FILE] [--no-http2]
    std::wstring cache_dir, cookie_file;
    bool cookie_file_given = false;
    {
        wchar_t buf[MAX_PATH * 2];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, (DWORD)std::size(buf));
        if (n && n < std::size(buf)) {
            cache_dir = std::wstring(buf) + L"\\Low-web\\Cache";
            cookie_file = std::wstring(buf) + L"\\Low-web\\cookies.txt";
        }
    }
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> start_urls;
    int cw = 1180, ch = 800;
    for (int i = 1; i < argc; i++) {
        std::string a = narrow(argv[i]);
        if (a == "--size" && i + 1 < argc) sscanf(narrow(argv[++i]).c_str(), "%dx%d", &cw, &ch);
        else if (a == "--script" && i + 1 < argc) {
            g_script_mode = true;
            std::string s = narrow(argv[++i]);
            size_t p = 0;
            while (p <= s.size()) {
                size_t q = s.find(';', p);
                if (q == std::string::npos) q = s.size();
                std::string c = s.substr(p, q - p);
                if (c.find_first_not_of(' ') != std::string::npos) g_script.push_back(c);
                p = q + 1;
            }
        } else if (a == "--screenshot" && i + 1 < argc) { g_script_mode = true; g_screenshot_path = narrow(argv[++i]); }
        else if (a == "--save-dir" && i + 1 < argc) g_save_dir = narrow(argv[++i]);
        else if (a == "--log" && i + 1 < argc) g_log_file = _wfopen(argv[++i], L"w");
        else if (a == "--no-cache") cache_dir.clear();
        else if (a == "--no-http2") net::set_http2(false);
        else if (a == "--cache-dir" && i + 1 < argc) cache_dir = argv[++i];
        else if (a == "--cookie-file" && i + 1 < argc) { cookie_file = argv[++i]; cookie_file_given = true; }
        else start_urls.push_back(net::from_user_input(a));
    }
    LocalFree(argv);
    cache::init(cache_dir, 256ull << 20);  // HTTP cache: %LOCALAPPDATA%\Low-web\Cache, at most 256 MB
    // cookies: %LOCALAPPDATA%\Low-web\cookies.txt; test runs (--script) keep theirs in memory
    cookies::init(g_script_mode && !cookie_file_given ? std::wstring() : cookie_file);

    g_local_site = find_local_site();
    if (!g_local_site.empty()) g_home_url = g_local_site;
    if (!g_script_mode) load_settings();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = view_proc;
    wc.hInstance = inst;
    wc.hCursor = nullptr;
    wc.lpszClassName = L"LowWebView";
    RegisterClassExW(&wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = main_proc;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"LowWebMain";
    wc.hIcon = LoadIconW(inst, L"APPICON");
    wc.hIconSm = wc.hIcon;
    RegisterClassExW(&wc);

    HDC sdc = GetDC(nullptr);
    g_dpi = GetDeviceCaps(sdc, LOGPIXELSX);
    ReleaseDC(nullptr, sdc);
    RECT r{0, 0, S(cw), S(ch) + tabstrip_height() + toolbar_height()};
    if (g_script_mode) r = RECT{0, 0, cw, ch + tabstrip_height() + toolbar_height()};
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, L"LowWebMain", L"Low-web", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    using GetDpi = UINT(WINAPI *)(HWND);
    if (auto f = (GetDpi)(void *)GetProcAddress(GetModuleHandleW(L"user32"), "GetDpiForWindow")) {
        UINT d = f(hwnd);
        if (d && (int)d != g_dpi) {
            g_dpi = (int)d;
            make_fonts();
        }
    }
    if (g_script_mode) {  // exact client size for reproducible screenshots
        RECT want{0, 0, cw, ch + tabstrip_height() + toolbar_height()};
        AdjustWindowRect(&want, WS_OVERLAPPEDWINDOW, FALSE);
        SetWindowPos(hwnd, nullptr, 0, 0, want.right - want.left, want.bottom - want.top, SWP_NOMOVE | SWP_NOZORDER);
    }
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);
    layout_children();

    if (start_urls.empty()) start_urls.push_back(g_home_url);
    for (size_t i = 0; i < start_urls.size(); i++) new_tab(start_urls[i], i == 0);
    activate_tab(0);
    if (g_script_mode) SetTimer(hwnd, TIMER_SCRIPT, 30, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
