// paint.c — a tiny paint program for Low-web. All logic lives here: UI, drawing,
// flood fill, image scaling and PNG encoding. No libc. The browser only shows our
// frames, forwards input, decodes images the user opens and saves the PNG we make.

#include "lowweb.h"

typedef unsigned char u8;
typedef unsigned int u32;
typedef unsigned long long u64;

#define W 1024          // whole framebuffer
#define H 640
#define TB 56           // toolbar height
#define CW W            // paint area
#define CH (H - TB)
#define UNDO_N 8
#define NSIZES 5
#define NCOLORS 16
#define FILL_TOL 24     // per-channel tolerance for the bucket

// Pixels are RGBA bytes in memory => 0xAABBGGRR as a little-endian u32.
#define RGB(r, g, b) (0xFF000000u | ((u32)(b) << 16) | ((u32)(g) << 8) | (u32)(r))
#define C_WHITE RGB(255, 255, 255)
#define C_BAR   RGB(243, 243, 241)
#define C_LINE  RGB(200, 200, 196)
#define C_HOVER RGB(228, 228, 224)
#define C_DARK  RGB(40, 40, 44)
#define C_TEXT  RGB(30, 30, 34)
#define C_MUTED RGB(170, 170, 166)

static u32 fb[W * H];
static u32 canvas[CW * CH];
static u32 undo_buf[UNDO_N][CW * CH];
static int undo_head, undo_count;

enum { T_PEN, T_ERASER, T_FILL };
enum {
    B_PEN, B_ERASE, B_FILL,
    B_SWATCH0,
    B_SIZE0 = B_SWATCH0 + NCOLORS,
    B_UNDO = B_SIZE0 + NSIZES, B_CLEAR, B_OPEN, B_SAVE,
    B_COUNT
};

typedef struct { int x, y, w, h; } Rect;
static Rect btn[B_COUNT], cur_box;
static const char *const label[B_COUNT] = {
    [B_PEN] = "PEN", [B_ERASE] = "ERASE", [B_FILL] = "FILL",
    [B_UNDO] = "UNDO", [B_CLEAR] = "CLEAR", [B_OPEN] = "OPEN", [B_SAVE] = "SAVE",
};
static const u32 palette[NCOLORS] = {
    RGB(0, 0, 0),       RGB(90, 90, 90),    RGB(170, 170, 170), RGB(255, 255, 255),
    RGB(230, 40, 40),   RGB(245, 140, 30),  RGB(250, 215, 40),  RGB(120, 200, 60),
    RGB(20, 120, 50),   RGB(40, 200, 220),  RGB(40, 100, 230),  RGB(20, 30, 110),
    RGB(130, 60, 200),  RGB(230, 60, 170),  RGB(250, 180, 200), RGB(120, 75, 40),
};
static const float brush_r[NSIZES] = { 1.0f, 2.5f, 5.0f, 10.0f, 20.0f };
static const int   icon_r[NSIZES]  = { 2, 3, 5, 8, 12 };

static int tool = T_PEN, size = 1, hover = -1, drawing, in_view, dirty = 1;
static u32 color = RGB(0, 0, 0);
static float mx, my, lx, ly;   // mouse (framebuffer coords), last stroke point (canvas coords)

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int inside(Rect r, int x, int y) { return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h; }

// ---------------------------------------------------------------- framebuffer drawing

static void fill_rect(int x, int y, int w, int h, u32 c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int j = 0; j < h; j++) {
        u32 *p = fb + (y + j) * W + x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
}

static void outline(Rect r, int t, u32 c) {
    fill_rect(r.x, r.y, r.w, t, c);
    fill_rect(r.x, r.y + r.h - t, r.w, t, c);
    fill_rect(r.x, r.y, t, r.h, c);
    fill_rect(r.x + r.w - t, r.y, t, r.h, c);
}

// Circle (filled when inner < 0, otherwise a ring between inner and outer radius).
static void fb_circle(float cx, float cy, float inner, float outer, int ymin, u32 c) {
    int x0 = (int)(cx - outer - 1), x1 = (int)(cx + outer + 1);
    int y0 = (int)(cy - outer - 1), y1 = (int)(cy + outer + 1);
    x0 = clampi(x0, 0, W - 1); x1 = clampi(x1, 0, W - 1);
    y0 = clampi(y0, ymin, H - 1); y1 = clampi(y1, ymin, H - 1);
    float in2 = inner < 0 ? -1 : inner * inner, out2 = outer * outer;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            float dx = x + 0.5f - cx, dy = y + 0.5f - cy, d2 = dx * dx + dy * dy;
            if (d2 <= out2 && d2 >= in2) fb[y * W + x] = c;
        }
}

// 5x7 font, only the letters the toolbar uses. Drawn at 2x.
static const u8 font[26][7] = {
    ['A' - 'A'] = {14, 17, 17, 31, 17, 17, 17},
    ['C' - 'A'] = {14, 17, 16, 16, 16, 17, 14},
    ['D' - 'A'] = {30, 17, 17, 17, 17, 17, 30},
    ['E' - 'A'] = {31, 16, 16, 30, 16, 16, 31},
    ['F' - 'A'] = {31, 16, 16, 30, 16, 16, 16},
    ['I' - 'A'] = {14, 4, 4, 4, 4, 4, 14},
    ['L' - 'A'] = {16, 16, 16, 16, 16, 16, 31},
    ['N' - 'A'] = {17, 25, 21, 19, 17, 17, 17},
    ['O' - 'A'] = {14, 17, 17, 17, 17, 17, 14},
    ['P' - 'A'] = {30, 17, 17, 30, 16, 16, 16},
    ['R' - 'A'] = {30, 17, 17, 30, 20, 18, 17},
    ['S' - 'A'] = {15, 16, 16, 14, 1, 1, 30},
    ['U' - 'A'] = {17, 17, 17, 17, 17, 17, 14},
    ['V' - 'A'] = {17, 17, 17, 17, 17, 10, 4},
};

static int text_w(const char *s) { int n = 0; while (s[n]) n++; return n ? n * 12 - 2 : 0; }

static void draw_text(int x, int y, const char *s, u32 c) {
    for (; *s; s++, x += 12) {
        if (*s < 'A' || *s > 'Z') continue;
        const u8 *g = font[*s - 'A'];
        for (int r = 0; r < 7; r++)
            for (int k = 0; k < 5; k++)
                if (g[r] >> (4 - k) & 1) fill_rect(x + k * 2, y + r * 2, 2, 2, c);
    }
}

// ---------------------------------------------------------------- canvas operations

static void push_undo(void) {
    __builtin_memcpy(undo_buf[undo_head], canvas, sizeof canvas);
    undo_head = (undo_head + 1) % UNDO_N;
    if (undo_count < UNDO_N) undo_count++;
}

static void undo(void) {
    if (!undo_count) return;
    undo_head = (undo_head + UNDO_N - 1) % UNDO_N;
    undo_count--;
    __builtin_memcpy(canvas, undo_buf[undo_head], sizeof canvas);
}

static void clear_canvas(void) {
    for (int i = 0; i < CW * CH; i++) canvas[i] = C_WHITE;
}

// Stroke = capsule: every pixel within r of the segment (x0,y0)-(x1,y1). No gaps at any speed.
static void segment(float x0, float y0, float x1, float y1, float r, u32 c) {
    int bx0 = clampi((int)((x0 < x1 ? x0 : x1) - r - 1), 0, CW - 1);
    int bx1 = clampi((int)((x0 > x1 ? x0 : x1) + r + 1), 0, CW - 1);
    int by0 = clampi((int)((y0 < y1 ? y0 : y1) - r - 1), 0, CH - 1);
    int by1 = clampi((int)((y0 > y1 ? y0 : y1) + r + 1), 0, CH - 1);
    float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy, r2 = r * r;
    for (int y = by0; y <= by1; y++)
        for (int x = bx0; x <= bx1; x++) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float t = len2 > 0 ? (px * dx + py * dy) / len2 : 0;
            t = t < 0 ? 0 : t > 1 ? 1 : t;
            float ex = px - t * dx, ey = py - t * dy;
            if (ex * ex + ey * ey <= r2) canvas[y * CW + x] = c;
        }
}

static int similar(u32 a, u32 b) {
    for (int s = 0; s < 24; s += 8) {
        int d = (int)(a >> s & 255) - (int)(b >> s & 255);
        if (d > FILL_TOL || d < -FILL_TOL) return 0;
    }
    return 1;
}

// BFS flood fill; each pixel enters the queue at most once, so the queue never overflows.
static u32 queue[CW * CH];
static u8 seen[CW * CH];

static void flood(int x, int y, u32 c) {
    u32 target = canvas[y * CW + x];
    if (target == c) return;
    __builtin_memset(seen, 0, sizeof seen);
    int qh = 0, qt = 0, start = y * CW + x;
    queue[qt++] = start; seen[start] = 1;
    while (qh < qt) {
        int i = queue[qh++], px = i % CW, py = i / CW;
        canvas[i] = c;
        int nb[4] = { px > 0 ? i - 1 : -1, px < CW - 1 ? i + 1 : -1,
                      py > 0 ? i - CW : -1, py < CH - 1 ? i + CW : -1 };
        for (int k = 0; k < 4; k++) {
            int j = nb[k];
            if (j >= 0 && !seen[j] && similar(canvas[j], target)) { seen[j] = 1; queue[qt++] = j; }
        }
    }
}

// ---------------------------------------------------------------- PNG encoder (zlib, fixed-Huffman deflate)

#define RAW_SIZE (CH * (1 + CW * 3))
static u8 raw[RAW_SIZE];
static u8 png[RAW_SIZE + RAW_SIZE / 8 + 1024];
static u32 crc_table[256];
static int hash_head[1 << 15], hash_prev[1 << 15];

static u8 *op;
static u32 bitbuf;
static int bitcnt;

static void put_bits(u32 v, int n) {
    bitbuf |= v << bitcnt;
    bitcnt += n;
    while (bitcnt >= 8) { *op++ = (u8)bitbuf; bitbuf >>= 8; bitcnt -= 8; }
}

// Huffman codes are defined MSB-first but deflate packs bits LSB-first.
static void put_code(u32 code, int len) {
    u32 r = 0;
    for (int i = 0; i < len; i++) r = (r << 1) | (code >> i & 1);
    put_bits(r, len);
}

static void put_lit(int v) {
    if (v < 144)      put_code(0x30 + v, 8);
    else if (v < 256) put_code(0x190 + v - 144, 9);
    else if (v < 280) put_code(v - 256, 7);
    else              put_code(0xC0 + v - 280, 8);
}

static const unsigned short len_base[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const u8 len_extra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const unsigned short dist_base[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const u8 dist_extra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static void put_match(int len, int dist) {
    int i = 28;
    while (len_base[i] > len) i--;
    put_lit(257 + i);
    put_bits(len - len_base[i], len_extra[i]);
    i = 29;
    while (dist_base[i] > dist) i--;
    put_code(i, 5);
    put_bits(dist - dist_base[i], dist_extra[i]);
}

static u32 hash3(const u8 *p) { return ((p[0] | p[1] << 8 | (u32)p[2] << 16) * 2654435761u) >> 17; }

static void hash_insert(const u8 *src, int pos, int n) {
    if (pos + 3 > n) return;
    u32 h = hash3(src + pos);
    hash_prev[pos & 32767] = hash_head[h];
    hash_head[h] = pos;
}

static void deflate(const u8 *src, int n) {
    for (int i = 0; i < (1 << 15); i++) hash_head[i] = -1;
    bitbuf = 0; bitcnt = 0;
    put_bits(1, 1);  // BFINAL
    put_bits(1, 2);  // BTYPE = fixed Huffman
    int i = 0;
    while (i < n) {
        int best = 0, dist = 0;
        if (i + 3 <= n) {
            int max = n - i < 258 ? n - i : 258;
            int cand = hash_head[hash3(src + i)];
            for (int chain = 16; cand >= 0 && i - cand <= 32768 && chain--; ) {
                int l = 0;
                while (l < max && src[cand + l] == src[i + l]) l++;
                if (l > best) { best = l; dist = i - cand; if (l == max) break; }
                int next = hash_prev[cand & 32767];
                if (next >= cand) break;
                cand = next;
            }
        }
        if (best >= 3) {
            put_match(best, dist);
            for (int k = 0; k < best; k++) hash_insert(src, i + k, n);
            i += best;
        } else {
            put_lit(src[i]);
            hash_insert(src, i, n);
            i++;
        }
    }
    put_lit(256);
    if (bitcnt) { *op++ = (u8)bitbuf; bitbuf = 0; bitcnt = 0; }
}

static void be32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static u32 crc32(const u8 *p, u32 n) {
    u32 c = 0xFFFFFFFFu;
    while (n--) c = crc_table[(c ^ *p++) & 255] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static u8 *chunk_begin(u8 *p, const char *type) {
    for (int i = 0; i < 4; i++) p[4 + i] = type[i];
    return p + 8;
}

static u8 *chunk_end(u8 *start, u8 *end) {
    u32 len = (u32)(end - start - 8);
    be32(start, len);
    be32(end, crc32(start + 4, len + 4));
    return end + 4;
}

static u32 encode_png(void) {
    // Scanlines: filter byte 0 + RGB.
    u8 *r = raw;
    for (int y = 0; y < CH; y++) {
        *r++ = 0;
        for (int x = 0; x < CW; x++) {
            u32 p = canvas[y * CW + x];
            *r++ = p; *r++ = p >> 8; *r++ = p >> 16;
        }
    }
    u32 a = 1, b = 0;
    for (int i = 0; i < RAW_SIZE; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }

    static const u8 sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    u8 *p = png, *c;
    for (int i = 0; i < 8; i++) *p++ = sig[i];

    c = p; p = chunk_begin(p, "IHDR");
    be32(p, CW); be32(p + 4, CH);
    p[8] = 8; p[9] = 2; p[10] = 0; p[11] = 0; p[12] = 0;   // 8-bit RGB
    p = chunk_end(c, p + 13);

    c = p; op = chunk_begin(p, "IDAT");
    *op++ = 0x78; *op++ = 0x01;                            // zlib header
    deflate(raw, RAW_SIZE);
    be32(op, (b << 16) | a); op += 4;                      // adler32
    p = chunk_end(c, op);

    c = p; p = chunk_end(c, chunk_begin(p, "IEND"));
    return (u32)(p - png);
}

// ---------------------------------------------------------------- UI

static int text_button(int id, int x) {
    int w = text_w(label[id]) + 20;
    btn[id] = (Rect){ x, 10, w, 36 };
    return x + w + 6;
}

static void layout(void) {
    int x = 8;
    x = text_button(B_PEN, x);
    x = text_button(B_ERASE, x);
    x = text_button(B_FILL, x) + 10;
    for (int i = 0; i < NCOLORS; i++)
        btn[B_SWATCH0 + i] = (Rect){ x + (i % 8) * 26, 5 + (i / 8) * 24, 22, 22 };
    x += 8 * 26 + 6;
    cur_box = (Rect){ x, 8, 40, 40 };
    x += 40 + 16;
    for (int i = 0; i < NSIZES; i++)
        btn[B_SIZE0 + i] = (Rect){ x + i * 40, 10, 36, 36 };
    x += NSIZES * 40 + 10;
    x = text_button(B_UNDO, x);
    x = text_button(B_CLEAR, x);
    x = text_button(B_OPEN, x);
    text_button(B_SAVE, x);
}

static int hit(int x, int y) {
    for (int b = 0; b < B_COUNT; b++)
        if (inside(btn[b], x, y)) return b;
    return -1;
}

static void draw_toolbar(void) {
    fill_rect(0, 0, W, TB, C_BAR);
    fill_rect(0, TB - 1, W, 1, C_LINE);
    for (int b = 0; b < B_COUNT; b++) {
        Rect r = btn[b];
        if (label[b]) {
            int active = b <= B_FILL && tool == b - B_PEN;
            int disabled = b == B_UNDO && !undo_count;
            u32 bg = active ? C_DARK : (hover == b && !disabled) ? C_HOVER : C_WHITE;
            u32 fg = active ? C_WHITE : disabled ? C_MUTED : C_TEXT;
            fill_rect(r.x, r.y, r.w, r.h, bg);
            outline(r, 1, active ? C_DARK : C_LINE);
            draw_text(r.x + 10, r.y + 11, label[b], fg);
        } else if (b < B_SIZE0) {
            u32 pc = palette[b - B_SWATCH0];
            if (pc == color) outline((Rect){ r.x - 2, r.y - 2, r.w + 4, r.h + 4 }, 2, C_DARK);
            fill_rect(r.x, r.y, r.w, r.h, pc);
            outline(r, 1, hover == b ? C_DARK : C_LINE);
        } else {
            int i = b - B_SIZE0;
            fill_rect(r.x, r.y, r.w, r.h, hover == b ? C_HOVER : C_WHITE);
            outline(r, size == i ? 2 : 1, size == i ? C_DARK : C_LINE);
            fb_circle(r.x + r.w / 2.0f, r.y + r.h / 2.0f, -1, icon_r[i], 0, C_TEXT);
        }
    }
    fill_rect(cur_box.x, cur_box.y, cur_box.w, cur_box.h, color);
    outline(cur_box, 2, C_DARK);
}

static void draw_toast(void);

static void render_all(void) {
    draw_toolbar();
    __builtin_memcpy(fb + TB * W, canvas, sizeof canvas);
    if (in_view && my >= TB && tool != T_FILL) {   // fill uses the system crosshair
        float r = brush_r[size];
        fb_circle(mx, my, r - 0.25f, r + 0.75f, TB, RGB(0, 0, 0));
        fb_circle(mx, my, r + 0.75f, r + 1.75f, TB, C_WHITE);
    }
    draw_toast();
}

static void open_file(void) { lw_open_file(LW_STR("png;jpg;jpeg;bmp")); }

static void save(void) { lw_save_file(LW_STR("drawing.png"), png, (int)encode_png()); }

static void do_button(int b) {
    if (b >= B_PEN && b <= B_FILL) tool = b - B_PEN;
    else if (b >= B_SWATCH0 && b < B_SIZE0) { color = palette[b - B_SWATCH0]; if (tool == T_ERASER) tool = T_PEN; }
    else if (b >= B_SIZE0 && b < B_UNDO) size = b - B_SIZE0;
    else if (b == B_UNDO)  undo();
    else if (b == B_CLEAR) { push_undo(); clear_canvas(); }
    else if (b == B_OPEN)  open_file();
    else if (b == B_SAVE)  save();
}

// RGBA image of any size -> scaled down to fit (box filter), centered, composited over white.
static void import_image(const u8 *src, int w, int h) {
    if (w <= 0 || h <= 0) return;
    push_undo();
    clear_canvas();
    int dw = w, dh = h;
    if (dw > CW || dh > CH) {
        if ((u64)w * CH > (u64)h * CW) { dw = CW; dh = (int)((u64)h * CW / w); }
        else                           { dh = CH; dw = (int)((u64)w * CH / h); }
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    }
    int ox = (CW - dw) / 2, oy = (CH - dh) / 2;
    for (int dy = 0; dy < dh; dy++) {
        int sy0 = (int)((u64)dy * h / dh), sy1 = (int)((u64)(dy + 1) * h / dh);
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (int dx = 0; dx < dw; dx++) {
            int sx0 = (int)((u64)dx * w / dw), sx1 = (int)((u64)(dx + 1) * w / dw);
            if (sx1 <= sx0) sx1 = sx0 + 1;
            u64 acc[3] = {0, 0, 0}, n = 0;
            for (int sy = sy0; sy < sy1; sy++) {
                const u8 *s = src + ((u64)sy * w + sx0) * 4;
                for (int sx = sx0; sx < sx1; sx++, s += 4, n++) {
                    u32 al = s[3], bg = 255 * (255 - al);   // over white, scaled by 255
                    acc[0] += s[0] * al + bg;
                    acc[1] += s[1] * al + bg;
                    acc[2] += s[2] * al + bg;
                }
            }
            n *= 255;
            canvas[(oy + dy) * CW + ox + dx] =
                RGB((acc[0] + n / 2) / n, (acc[1] + n / 2) / n, (acc[2] + n / 2) / n);
        }
    }
    dirty = 1;
}

// ---------------------------------------------------------------- toast messages

static char toast_msg[160];
static int toast_len;
static double toast_until;

static void toast(const char *a, const char *b, int blen) {
    toast_len = 0;
    for (; *a && toast_len < 100; a++) toast_msg[toast_len++] = *a;
    for (int i = 0; i < blen && toast_len < (int)sizeof toast_msg; i++) toast_msg[toast_len++] = b[i];
    toast_until = lw_now() + 2500;
    dirty = 1;
}

static void draw_toast(void) {
    if (!toast_len) return;
    int tw = lw_text_width(toast_msg, toast_len, 16, 0);
    int bw = tw + 32, bh = 36, bx = (W - bw) / 2, by = H - bh - 20;
    fill_rect(bx, by, bw, bh, RGB(40, 40, 44));
    lw_text(fb, W, H, bx + 16, by + 8, toast_msg, toast_len, 16, 0, C_WHITE);
}

// ---------------------------------------------------------------- memory for the browser

// A bump allocator: the browser asks for memory to hand us a file, we use it and
// release everything at once.
extern u8 __heap_base;
static u32 heap_top;

LW_EXPORT(lw_alloc) u8 *lw_alloc(u32 n) {
    u32 base = ((u32)&__heap_base + 15) & ~15u;
    if (heap_top < base) heap_top = base;
    u64 need = (u64)heap_top + n, have = (u64)__builtin_wasm_memory_size(0) * 65536;
    if (need > 0xFFFF0000ull) return 0;
    if (need > have && __builtin_wasm_memory_grow(0, (u32)((need - have + 65535) / 65536)) == (unsigned long)-1)
        return 0;
    u8 *p = (u8 *)heap_top;
    heap_top = ((u32)need + 15) & ~15u;
    return p;
}

static void heap_reset(void) { heap_top = 0; }

// ---------------------------------------------------------------- Low-web entry points

LW_EXPORT(lw_start) void lw_start(void) {
    for (u32 n = 0; n < 256; n++) {
        u32 c = n;
        for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    layout();
    clear_canvas();
    lw_set_title(LW_STR("Paint"));
}

LW_EXPORT(lw_frame) void lw_frame(double now) {
    if (toast_len && now > toast_until) { toast_len = 0; dirty = 1; }
    if (!dirty) return;
    dirty = 0;
    render_all();
    lw_present(fb, W, H);
}

static void pointer_down(float x, float y, int button) {
    if (y < TB) {
        if (button == 0) { int b = hit((int)x, (int)y); if (b >= 0) do_button(b); }
        return;
    }
    float cx = x, cy = y - TB;
    int px = clampi((int)cx, 0, CW - 1), py = clampi((int)cy, 0, CH - 1);
    if (button == 2) { color = canvas[py * CW + px]; if (tool == T_ERASER) tool = T_PEN; return; }
    if (button != 0) return;
    push_undo();
    if (tool == T_FILL) { flood(px, py, color); return; }
    drawing = 1;
    lx = cx; ly = cy;
    segment(cx, cy, cx, cy, brush_r[size], tool == T_ERASER ? C_WHITE : color);
}

static int pointer_move(float x, float y) {
    mx = x; my = y; in_view = 1;
    if (drawing) {
        float cx = x, cy = y - TB;
        segment(lx, ly, cx, cy, brush_r[size], tool == T_ERASER ? C_WHITE : color);
        lx = cx; ly = cy;
        return LW_CURSOR_NONE;
    }
    hover = y < TB ? hit((int)x, (int)y) : -1;
    if (y < TB) return hover >= 0 ? LW_CURSOR_HAND : LW_CURSOR_ARROW;
    return tool == T_FILL ? LW_CURSOR_CROSS : LW_CURSOR_NONE;
}

LW_EXPORT(lw_pointer) int lw_pointer(int kind, float x, float y, int button) {
    dirty = 1;
    switch (kind) {
    case LW_DOWN: pointer_down(x, y, button); break;
    case LW_UP:   drawing = 0; break;
    case LW_LEAVE: in_view = 0; hover = -1; return LW_CURSOR_ARROW;
    case LW_WHEEL:
        if (button > 0 && size < NSIZES - 1) size++;
        if (button < 0 && size > 0) size--;
        break;
    }
    return pointer_move(x, y);
}

LW_EXPORT(lw_key) int lw_key(int key, int mods, int down) {
    if (!down || !(mods & LW_CTRL)) return 0;
    dirty = 1;
    if (key == 'Z') { undo(); return 1; }
    if (key == 'S') { save(); return 1; }
    if (key == 'O') { open_file(); return 1; }
    return 0;
}

LW_EXPORT(lw_char) void lw_char(int ch) {
    if (ch >= 'A' && ch <= 'Z') ch += 32;
    dirty = 1;
    switch (ch) {
    case 'p': case 'b': tool = T_PEN; return;
    case 'e': tool = T_ERASER; return;
    case 'f': case 'g': tool = T_FILL; return;
    case '[': if (size > 0) size--; return;
    case ']': if (size < NSIZES - 1) size++; return;
    }
    if (ch >= '1' && ch <= '0' + NSIZES) size = ch - '1';
}

// A file the user picked with OPEN or dropped on the page.
LW_EXPORT(lw_on_file) void lw_on_file(const u8 *data, int len, const char *name, int name_len) {
    int w = 0, h = 0;
    int img = lw_image_decode(data, len, &w, &h);
    if (!img) {
        toast("Not an image I can open: ", name, name_len);
    } else {
        u8 *px = lw_alloc((u32)w * (u32)h * 4);
        if (!px) {
            lw_image_free(img);
            toast("Image is too large: ", name, name_len);
        } else {
            lw_image_read(img, px);
            import_image(px, w, h);
            toast("Opened ", name, name_len);
        }
    }
    heap_reset();
}
