// home.c — the Low-web start page. Draws everything itself, fetches a message
// from its own server with lw_fetch, and links to the other pages.
#include "lowweb.h"

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

#define MAX_W 1920
#define MAX_H 1200
#define RGBA(r, g, b, a) (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(g) << 8) | (u32)(r))
#define RGB(r, g, b) RGBA(r, g, b, 255)

static u32 base[MAX_W * MAX_H];   // the static page, redrawn only when something changes
static u32 fb[MAX_W * MAX_H];     // base + animated bits, presented every frame
static int W = 960, H = 640;
static int need_layout = 1, need_base = 1;

static u32 *target = base;        // what the drawing helpers draw into

// ---------------------------------------------------------------- drawing helpers

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static u32 blend(u32 d, u32 s) {
    u32 a = s >> 24;
    if (a == 255) return s;
    if (a == 0) return d;
    u32 r = ((s & 255) * a + (d & 255) * (255 - a)) / 255;
    u32 g = ((s >> 8 & 255) * a + (d >> 8 & 255) * (255 - a)) / 255;
    u32 b = ((s >> 16 & 255) * a + (d >> 16 & 255) * (255 - a)) / 255;
    return 0xFF000000u | b << 16 | g << 8 | r;
}

// Rounded rectangle with a 1px border (border colour 0 = none).
static void round_rect(int x, int y, int w, int h, int r, u32 fill, u32 border) {
    int x0 = clampi(x, 0, W), x1 = clampi(x + w, 0, W), y0 = clampi(y, 0, H), y1 = clampi(y + h, 0, H);
    for (int j = y0; j < y1; j++) {
        u32 *p = target + j * W;
        for (int i = x0; i < x1; i++) {
            int dx = i < x + r ? x + r - i : i >= x + w - r ? i - (x + w - r - 1) : 0;
            int dy = j < y + r ? y + r - j : j >= y + h - r ? j - (y + h - r - 1) : 0;
            int d2 = dx * dx + dy * dy;
            if (d2 > r * r) continue;
            int edge = i == x || i == x + w - 1 || j == y || j == y + h - 1 || d2 > (r - 1) * (r - 1);
            p[i] = blend(p[i], edge && border ? border : fill);
        }
    }
}

static int text(int x, int y, const char *s, int len, int size, int flags, u32 color) {
    return lw_text(target, W, H, x, y, s, len, size, flags, color);
}
static int tw(const char *s, int len, int size, int flags) { return lw_text_width(s, len, size, flags); }

// Word-wraps UTF-8 text at spaces and newlines. Returns the y after the last line.
static int text_wrapped(int x, int y, int maxw, const char *s, int len, int size, u32 color) {
    int line_h = size * 3 / 2, i = 0;
    while (i < len) {
        int end = i, last_fit = -1;
        while (end < len && s[end] != '\n') {
            int next = end;
            while (next < len && s[next] != ' ' && s[next] != '\n') next++;
            if (tw(s + i, next - i, size, 0) > maxw && last_fit >= 0) break;
            last_fit = next;
            end = next;
            if (end < len && s[end] == ' ') end++;
        }
        int stop = last_fit >= 0 && end < len && s[end] != '\n' ? last_fit : end;
        text(x, y, s + i, stop - i, size, 0, color);
        y += line_h;
        i = stop;
        while (i < len && s[i] == ' ') i++;
        if (i < len && s[i] == '\n') i++;
    }
    return y;
}

// ---------------------------------------------------------------- page state

typedef struct { int x, y, w, h; const char *title, *body, *href; } Card;
static Card cards[] = {
    {0, 0, 0, 0, "Paint", "วาดรูป เปิดไฟล์ภาพจากเครื่อง แล้วเซฟเป็น PNG\nDraw, open images, save as PNG.", "paint/"},
    {0, 0, 0, 0, "Read me", "ข้อความธรรมดา (text/plain) ที่เบราว์เซอร์แสดงให้เอง\nA plain-text file the browser shows on its own.", "about.txt"},
    {0, 0, 0, 0, "The web", "เว็บ HTML ทั่วไปผ่านตัวแปลง (reader view)\nOrdinary HTML sites, through the built-in viewer.", "https://lite.duckduckgo.com/lite/"},
};
#define NCARDS (int)(sizeof cards / sizeof cards[0])
static int hover = -1;

static char motd[2048];
static int motd_len = -1;        // -1 = still loading
static int motd_status;
static double motd_ms, fetch_started;
static int col_x, col_w, motd_y, footer_y;

// ---------------------------------------------------------------- layout and drawing

static void layout(void) {
    col_w = W - 48 < 960 ? W - 48 : 960;
    col_x = (W - col_w) / 2;
    int y = H < 700 ? 40 : 72;
    y += 64 + 12 + 30 * 2 + 36;  // title, subtitle lines
    int gap = 16, cols = col_w >= 840 ? 3 : col_w >= 520 ? 2 : 1;
    int cw = (col_w - gap * (cols - 1)) / cols, bottom = y;
    for (int i = 0; i < NCARDS; i++) {
        Card *c = &cards[i];
        c->w = cw;
        c->h = 150;
        c->x = col_x + (i % cols) * (cw + gap);
        c->y = y + (i / cols) * (c->h + gap);
        if (c->y + c->h > bottom) bottom = c->y + c->h;
    }
    y = bottom + 32;
    motd_y = y;
    footer_y = H - 36;
    need_layout = 0;
}

static void draw_base(void) {
    target = base;
    // background: vertical gradient
    for (int j = 0; j < H; j++) {
        int t = j * 255 / (H ? H : 1);
        u32 c = RGB(15 + (30 - 15) * t / 255, 23 + (41 - 23) * t / 255, 42 + (59 - 42) * t / 255);
        u32 *p = base + j * W;
        for (int i = 0; i < W; i++) p[i] = c;
    }
    int y = H < 700 ? 40 : 72;
    int x = col_x;
    x += text(x, y, LW_STR("Low"), 56, LW_TEXT_BOLD, RGB(241, 245, 249));
    text(x, y, LW_STR("-web"), 56, LW_TEXT_BOLD, RGB(56, 189, 248));
    y += 64 + 12;
    text(col_x, y, LW_STR("เว็บที่ไม่มี HTML, CSS หรือ JavaScript"), 22, 0, RGB(203, 213, 225));
    y += 30;
    text(col_x, y, LW_STR("Every page is a WebAssembly program that draws its own pixels."), 18, 0, RGB(148, 163, 184));

    for (int i = 0; i < NCARDS; i++) {
        Card *c = &cards[i];
        int h = hover == i;
        round_rect(c->x, c->y, c->w, c->h, 12, h ? RGB(39, 52, 73) : RGB(30, 41, 59), h ? RGB(56, 189, 248) : RGB(51, 65, 85));
        int tx = c->x + 20;
        int w = text(tx, c->y + 16, c->title, lw_strlen(c->title), 22, LW_TEXT_BOLD, RGB(241, 245, 249));
        text(tx + w + 8, c->y + 18, LW_STR("→"), 20, 0, h ? RGB(56, 189, 248) : RGB(100, 116, 139));
        text_wrapped(tx, c->y + 54, c->w - 40, c->body, lw_strlen(c->body), 15, RGB(148, 163, 184));
    }

    // message from the server (lw_fetch)
    int my = motd_y;
    round_rect(col_x, my, col_w, 112, 12, RGBA(56, 189, 248, 18), RGBA(56, 189, 248, 90));
    text(col_x + 20, my + 14, LW_STR("ข้อความจากเซิร์ฟเวอร์  ·  motd.txt via lw_fetch"), 13, LW_TEXT_BOLD, RGB(56, 189, 248));
    if (motd_len < 0) {
        text(col_x + 20, my + 40, LW_STR("Loading…"), 16, 0, RGB(148, 163, 184));
    } else if (motd_status != 200) {
        text_wrapped(col_x + 20, my + 40, col_w - 40, motd, motd_len, 15, RGB(248, 113, 113));
    } else {
        text_wrapped(col_x + 20, my + 40, col_w - 40, motd, motd_len, 16, RGB(226, 232, 240));
    }
    need_base = 0;
}

static char num_buf[32];
static int fmt_num(char *o, double v, int decimals) {  // tiny printf("%.Nf")
    int n = 0;
    if (v < 0) { o[n++] = '-'; v = -v; }
    u64 scale = 1;
    for (int i = 0; i < decimals; i++) scale *= 10;
    u64 iv = (u64)(v * scale + 0.5);
    u64 whole = iv / scale, frac = iv % scale;
    char tmp[24];
    int t = 0;
    do { tmp[t++] = (char)('0' + whole % 10); whole /= 10; } while (whole);
    while (t) o[n++] = tmp[--t];
    if (decimals) {
        o[n++] = '.';
        for (u64 d = scale / 10; d; d /= 10) o[n++] = (char)('0' + frac / d % 10);
    }
    return n;
}

static double last_frame, fps = 60;

static void draw_live(double now) {
    target = fb;
    // footer: proof that the page is a running program
    char line[128];
    int n = 0;
    const char *a = "running ";
    while (*a) line[n++] = *a++;
    n += fmt_num(line + n, now / 1000.0, 1);
    a = " s  ·  ";
    while (*a) line[n++] = *a++;
    n += fmt_num(line + n, fps, 0);
    a = " fps";
    while (*a) line[n++] = *a++;
    if (motd_len >= 0 && motd_status == 200) {
        a = "  ·  fetched motd in ";
        while (*a) line[n++] = *a++;
        n += fmt_num(line + n, motd_ms, 0);
        a = " ms";
        while (*a) line[n++] = *a++;
    }
    // pulsing dot
    double ph = now / 1000.0 * 3.14159 * 1.2;
    double s = ph - (int)(ph / 6.28318) * 6.28318;  // cheap sine: parabola approximation
    double sn = s < 3.14159 ? 4 * s * (3.14159 - s) / (3.14159 * 3.14159) : -4 * (s - 3.14159) * (6.28318 - s) / (3.14159 * 3.14159);
    int alpha = 140 + (int)(sn * 110);
    round_rect(col_x, footer_y + 5, 10, 10, 5, RGBA(74, 222, 128, clampi(alpha, 0, 255)), 0);
    text(col_x + 18, footer_y, line, n, 13, 0, RGB(100, 116, 139));
    (void)num_buf;
}

// ---------------------------------------------------------------- memory for the browser

extern u8 __heap_base;
static u32 heap_top;

LW_EXPORT(lw_alloc) u8 *lw_alloc(u32 n) {
    u32 b = ((u32)&__heap_base + 15) & ~15u;
    if (heap_top < b) heap_top = b;
    u64 need = (u64)heap_top + n, have = (u64)__builtin_wasm_memory_size(0) * 65536;
    if (need > 0xFFFF0000ull) return 0;
    if (need > have && __builtin_wasm_memory_grow(0, (u32)((need - have + 65535) / 65536)) == (unsigned long)-1)
        return 0;
    u8 *p = (u8 *)heap_top;
    heap_top = ((u32)need + 15) & ~15u;
    return p;
}

// ---------------------------------------------------------------- Low-web entry points

LW_EXPORT(lw_start) void lw_start(void) {
    lw_set_title(LW_STR("Low-web"));
    fetch_started = lw_now();
    lw_fetch(LW_STR("motd.txt"));
}

LW_EXPORT(lw_resize) void lw_resize(int w, int h) {
    W = clampi(w, 320, MAX_W);
    H = clampi(h, 240, MAX_H);
    need_layout = need_base = 1;
}

LW_EXPORT(lw_frame) void lw_frame(double now) {
    if (last_frame > 0) {
        double dt = now - last_frame;
        if (dt > 0) fps = fps * 0.95 + (1000.0 / dt) * 0.05;
    }
    last_frame = now;
    if (need_layout) layout();
    if (need_base) draw_base();
    __builtin_memcpy(fb, base, (u64)W * H * 4);
    draw_live(now);
    lw_present(fb, W, H);
}

static int card_at(float x, float y) {
    for (int i = 0; i < NCARDS; i++)
        if (x >= cards[i].x && x < cards[i].x + cards[i].w && y >= cards[i].y && y < cards[i].y + cards[i].h) return i;
    return -1;
}

LW_EXPORT(lw_pointer) int lw_pointer(int kind, float x, float y, int button) {
    int h = kind == LW_LEAVE ? -1 : card_at(x, y);
    if (h != hover) { hover = h; need_base = 1; }
    if (kind == LW_UP && button == 0 && h >= 0) lw_navigate(cards[h].href, lw_strlen(cards[h].href));
    return h >= 0 ? LW_CURSOR_HAND : LW_CURSOR_ARROW;
}

LW_EXPORT(lw_key) int lw_key(int key, int mods, int down) {
    (void)mods;
    if (down && key == 'P') { lw_navigate(LW_STR("paint/")); return 1; }
    return 0;
}

LW_EXPORT(lw_on_fetch) void lw_on_fetch(int id, int status, const u8 *data, int len) {
    (void)id;
    motd_status = status;
    motd_ms = lw_now() - fetch_started;
    const char *prefix = status == 200 ? "" : "Could not load motd.txt: ";
    int n = 0;
    while (*prefix && n < (int)sizeof motd) motd[n++] = *prefix++;
    for (int i = 0; i < len && n < (int)sizeof motd; i++)
        if (data[i] != '\r') motd[n++] = (char)data[i];
    while (n > 0 && (motd[n - 1] == '\n' || motd[n - 1] == ' ')) n--;
    motd_len = n;
    heap_top = 0;
    need_base = 1;
}
