// webp.cpp — WebP decoder: lossy (VP8, RFC 6386), lossless (VP8L, RFC 9649), alpha
// (ALPH), the extended format (VP8X) and the first frame of animations. Output matches
// libwebp's default decoding (including its "fancy" chroma upsampling) bit for bit.
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "image.h"
#include "webp_tables.h"

namespace image {

namespace {

using namespace webp_tables;

struct WebpError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
[[noreturn]] void fail(const char *m) { throw WebpError(m); }

uint32_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
uint32_t le24(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16; }
uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// =====================================================================================
// VP8L (lossless)
// =====================================================================================

struct LBits {  // least significant bit first
    const uint8_t *p;
    size_t n, pos = 0;
    uint64_t buf = 0;
    int bits = 0, overrun = 0;

    LBits(const uint8_t *d, size_t len) : p(d), n(len) {}
    void fill() {
        while (bits <= 56) {
            uint64_t b = 0;
            if (pos < n) b = p[pos++];
            else if (++overrun > 16) fail("lossless data is truncated");
            buf |= b << bits;
            bits += 8;
        }
    }
    uint32_t read(int nb) {
        if (!nb) return 0;
        if (bits < nb) fill();
        uint32_t v = (uint32_t)(buf & ((1ull << nb) - 1));
        buf >>= nb;
        bits -= nb;
        return v;
    }
};

struct LHuff {
    int single = -1;  // a code with one symbol takes no bits
    int count[16] = {};
    std::vector<uint16_t> symbols;
    std::vector<uint32_t> fast;  // 256 entries: (symbol << 4) | length, 0 = slow path

    void build(const std::vector<int> &lengths) {
        int n = (int)lengths.size(), nonzero = 0, last = 0;
        for (int i = 0; i < n; i++)
            if (lengths[i]) { nonzero++; last = i; }
        if (nonzero == 0) fail("empty lossless code");
        if (nonzero == 1) { single = last; return; }
        std::memset(count, 0, sizeof count);
        for (int l : lengths) {
            if (l > 15) fail("bad lossless code length");
            count[l]++;
        }
        count[0] = 0;
        int left = 1;
        for (int len = 1; len < 16; len++) {
            left <<= 1;
            left -= count[len];
            if (left < 0) fail("bad lossless code");
        }
        int offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + count[len];
        symbols.assign(nonzero, 0);
        for (int s = 0; s < n; s++)
            if (lengths[s]) symbols[offs[lengths[s]]++] = (uint16_t)s;
        fast.assign(256, 0);
        int code = 0, idx = 0;
        for (int len = 1; len < 16; len++) {
            for (int k = 0; k < count[len]; k++, code++) {
                int sym = symbols[idx++];
                if (len > 8) continue;
                int rev = 0;
                for (int b = 0; b < len; b++) rev |= ((code >> b) & 1) << (len - 1 - b);
                for (int r = rev; r < 256; r += 1 << len) fast[r] = (uint32_t)sym << 4 | (uint32_t)len;
            }
            code <<= 1;
        }
    }
    int read(LBits &br) const {
        if (single >= 0) return single;
        if (br.bits < 15) br.fill();
        uint32_t e = fast[br.buf & 255];
        if (e) {
            br.buf >>= (e & 15);
            br.bits -= (int)(e & 15);
            return (int)(e >> 4);
        }
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; len++) {
            code |= (int)br.read(1);
            int c = count[len];
            if (code - c < first) return symbols[index + (code - first)];
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        fail("bad lossless code");
    }
};

struct LTransform {
    int type, bits, xsize;
    std::vector<uint32_t> data;
};

uint32_t add_pixels(uint32_t a, uint32_t b) {
    uint32_t ag = (a & 0xff00ff00u) + (b & 0xff00ff00u);
    uint32_t rb = (a & 0x00ff00ffu) + (b & 0x00ff00ffu);
    return (ag & 0xff00ff00u) | (rb & 0x00ff00ffu);
}
uint32_t average2(uint32_t a, uint32_t b) { return (((a ^ b) & 0xfefefefeu) >> 1) + (a & b); }
int clip255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

uint32_t select_pred(uint32_t a, uint32_t b, uint32_t c) {  // a = top, b = left, c = top-left
    int pa_minus_pb = 0;
    for (int s = 0; s < 32; s += 8) {
        int ai = (int)(a >> s & 255), bi = (int)(b >> s & 255), ci = (int)(c >> s & 255);
        pa_minus_pb += std::abs(bi - ci) - std::abs(ai - ci);
    }
    return pa_minus_pb <= 0 ? a : b;
}
uint32_t clamp_add_sub_full(uint32_t a, uint32_t b, uint32_t c) {
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8)
        r |= (uint32_t)clip255((int)(a >> s & 255) + (int)(b >> s & 255) - (int)(c >> s & 255)) << s;
    return r;
}
uint32_t clamp_add_sub_half(uint32_t a, uint32_t b) {
    uint32_t r = 0;
    for (int s = 0; s < 32; s += 8) {
        int x = (int)(a >> s & 255), y = (int)(b >> s & 255);
        r |= (uint32_t)clip255(x + (x - y) / 2) << s;
    }
    return r;
}

struct VP8L {
    LBits br;
    int transforms_seen = 0;
    std::vector<LTransform> transforms;

    VP8L(const uint8_t *d, size_t n) : br(d, n) {}

    static int subsample(int size, int bits) { return (size + (1 << bits) - 1) >> bits; }

    void read_code(LHuff &h, int alphabet) {
        std::vector<int> lengths(alphabet, 0);
        if (br.read(1)) {  // simple code
            int num = (int)br.read(1) + 1;
            int first_8bits = (int)br.read(1);
            int s = (int)br.read(first_8bits ? 8 : 1);
            if (s >= alphabet) fail("bad lossless symbol");
            lengths[s] = 1;
            if (num == 2) {
                s = (int)br.read(8);
                if (s >= alphabet) fail("bad lossless symbol");
                lengths[s] = 1;
            }
        } else {
            static const int order[19] = {17, 18, 0, 1, 2, 3, 4, 5, 16, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
            std::vector<int> cl(19, 0);
            int num_codes = (int)br.read(4) + 4;
            for (int i = 0; i < num_codes; i++) cl[order[i]] = (int)br.read(3);
            LHuff lh;
            lh.build(cl);
            int max_symbol = alphabet;
            if (br.read(1)) {
                int nbits = 2 + 2 * (int)br.read(3);
                max_symbol = 2 + (int)br.read(nbits);
                if (max_symbol > alphabet) fail("bad lossless code lengths");
            }
            int prev = 8, sym = 0;
            while (sym < alphabet) {
                if (max_symbol-- == 0) break;
                int c = lh.read(br);
                if (c < 16) {
                    lengths[sym++] = c;
                    if (c) prev = c;
                } else {
                    static const int extra[3] = {2, 3, 7}, offset[3] = {3, 3, 11};
                    int slot = c - 16;
                    int rep = (int)br.read(extra[slot]) + offset[slot];
                    if (sym + rep > alphabet) fail("bad lossless code lengths");
                    int v = c == 16 ? prev : 0;
                    while (rep--) lengths[sym++] = v;
                }
            }
        }
        h.build(lengths);
    }

    static int copy_distance(int sym, LBits &br) {
        if (sym < 4) return sym + 1;
        int extra = (sym - 2) >> 1;
        int offset = (2 + (sym & 1)) << extra;
        return offset + (int)br.read(extra) + 1;
    }

    static int plane_to_distance(int xsize, int code) {
        if (code > 120) return code - 120;
        int dc = kCodeToPlane[code - 1];
        int yoff = dc >> 4, xoff = 8 - (dc & 15);
        int d = yoff * xsize + xoff;
        return d >= 1 ? d : 1;
    }

    // Decodes one entropy-coded image; at level 0 also its transforms.
    std::vector<uint32_t> decode_image(int xsize, int ysize, bool level0) {
        int tx = xsize;
        size_t first_transform = transforms.size();
        if (level0) {
            while (br.read(1)) {
                int type = (int)br.read(2);
                if (transforms_seen & (1 << type)) fail("repeated lossless transform");
                transforms_seen |= 1 << type;
                LTransform t;
                t.type = type;
                t.xsize = tx;
                t.bits = 0;
                if (type == 0 || type == 1) {
                    t.bits = (int)br.read(3) + 2;
                    t.data = decode_image(subsample(tx, t.bits), subsample(ysize, t.bits), false);
                } else if (type == 3) {
                    int num_colors = (int)br.read(8) + 1;
                    int bits = num_colors > 16 ? 0 : num_colors > 4 ? 1 : num_colors > 2 ? 2 : 3;
                    t.bits = bits;
                    std::vector<uint32_t> pal = decode_image(num_colors, 1, false);
                    t.data.assign((size_t)1 << (8 >> bits), 0);
                    for (int i = 0; i < num_colors; i++) t.data[i] = i ? add_pixels(pal[i], t.data[i - 1]) : pal[0];
                    tx = subsample(tx, bits);
                }
                transforms.push_back(std::move(t));
            }
        }
        int cache_bits = 0;
        if (br.read(1)) {
            cache_bits = (int)br.read(4);
            if (cache_bits < 1 || cache_bits > 11) fail("bad lossless color cache");
        }
        int huff_bits = 0, huff_xsize = 0, groups = 1;
        std::vector<uint32_t> huff_image;
        if (level0 && br.read(1)) {
            huff_bits = (int)br.read(3) + 2;
            huff_xsize = subsample(tx, huff_bits);
            huff_image = decode_image(huff_xsize, subsample(ysize, huff_bits), false);
            for (uint32_t &p : huff_image) {
                p = (p >> 8) & 0xffff;
                groups = std::max(groups, (int)p + 1);
            }
        }
        if (groups > 65536) fail("bad lossless meta codes");
        int cache_size = cache_bits ? 1 << cache_bits : 0;
        std::vector<LHuff> codes((size_t)groups * 5);
        static const int alphabet[5] = {256 + 24, 256, 256, 256, 40};
        for (int g = 0; g < groups; g++)
            for (int k = 0; k < 5; k++) read_code(codes[(size_t)g * 5 + k], alphabet[k] + (k == 0 ? cache_size : 0));

        if ((uint64_t)tx * ysize > kMaxPixels) fail("image is too large");
        std::vector<uint32_t> data((size_t)tx * ysize);
        std::vector<uint32_t> cache(cache_size ? cache_size : 1);
        size_t total = data.size(), pos = 0, cached = 0;
        int x = 0, y = 0;
        auto insert_cache = [&](size_t upto) {
            if (!cache_bits) return;
            for (; cached < upto; cached++) cache[(data[cached] * 0x1e35a7bdu) >> (32 - cache_bits)] = data[cached];
        };
        while (pos < total) {
            const LHuff *h = &codes[huff_image.empty() ? 0 : (size_t)huff_image[(size_t)(y >> huff_bits) * huff_xsize + (x >> huff_bits)] * 5];
            int code = h[0].read(br);
            if (code < 256) {
                uint32_t r = (uint32_t)h[1].read(br), b = (uint32_t)h[2].read(br), a = (uint32_t)h[3].read(br);
                data[pos++] = a << 24 | r << 16 | (uint32_t)code << 8 | b;
                if (++x >= tx) { x = 0; y++; }
            } else if (code < 256 + 24) {
                int length = copy_distance(code - 256, br);
                int dsym = h[4].read(br);
                int dist = plane_to_distance(tx, copy_distance(dsym, br));
                if ((size_t)dist > pos || total - pos < (size_t)length) fail("bad lossless back-reference");
                for (int k = 0; k < length; k++, pos++) data[pos] = data[pos - dist];
                x += length;
                while (x >= tx) { x -= tx; y++; }
            } else {
                int key = code - 256 - 24;
                if (key >= cache_size) fail("bad lossless cache index");
                insert_cache(pos);
                data[pos++] = cache[key];
                if (++x >= tx) { x = 0; y++; }
            }
            insert_cache(pos);
            if (br.overrun > 8) fail("lossless data is truncated");
        }
        if (level0) {
            for (size_t i = transforms.size(); i-- > first_transform;) inverse(transforms[i], data, ysize);
            transforms.resize(first_transform);
        }
        return data;
    }

    static void inverse(const LTransform &t, std::vector<uint32_t> &data, int ysize) {
        int w = t.xsize;
        switch (t.type) {
        case 0: {  // predictor
            int bw = subsample(w, t.bits);
            for (int y = 0; y < ysize; y++)
                for (int x = 0; x < w; x++) {
                    size_t i = (size_t)y * w + x;
                    uint32_t pred;
                    if (y == 0) pred = x == 0 ? 0xff000000u : data[i - 1];
                    else if (x == 0) pred = data[i - w];
                    else {
                        int mode = (int)(t.data[(size_t)(y >> t.bits) * bw + (x >> t.bits)] >> 8) & 15;
                        uint32_t L = data[i - 1], T = data[i - w], TL = data[i - w - 1], TR = data[i - w + 1];
                        switch (mode) {
                        case 0: pred = 0xff000000u; break;
                        case 1: pred = L; break;
                        case 2: pred = T; break;
                        case 3: pred = TR; break;
                        case 4: pred = TL; break;
                        case 5: pred = average2(average2(L, TR), T); break;
                        case 6: pred = average2(L, TL); break;
                        case 7: pred = average2(L, T); break;
                        case 8: pred = average2(TL, T); break;
                        case 9: pred = average2(T, TR); break;
                        case 10: pred = average2(average2(L, TL), average2(T, TR)); break;
                        case 11: pred = select_pred(T, L, TL); break;
                        case 12: pred = clamp_add_sub_full(L, T, TL); break;
                        case 13: pred = clamp_add_sub_half(average2(L, T), TL); break;
                        default: pred = 0xff000000u;  // 14, 15: as mode 0
                        }
                    }
                    data[i] = add_pixels(data[i], pred);
                }
            break;
        }
        case 1: {  // cross colour
            int bw = subsample(w, t.bits);
            for (int y = 0; y < ysize; y++)
                for (int x = 0; x < w; x++) {
                    uint32_t m = t.data[(size_t)(y >> t.bits) * bw + (x >> t.bits)];
                    int8_t g2r = (int8_t)(m & 255), g2b = (int8_t)(m >> 8 & 255), r2b = (int8_t)(m >> 16 & 255);
                    uint32_t &p = data[(size_t)y * w + x];
                    int8_t green = (int8_t)(p >> 8);
                    int red = (int)(p >> 16 & 255), blue = (int)(p & 255);
                    red = (red + ((g2r * green) >> 5)) & 255;
                    blue += (g2b * green) >> 5;
                    blue += (r2b * (int8_t)red) >> 5;
                    blue &= 255;
                    p = (p & 0xff00ff00u) | (uint32_t)red << 16 | (uint32_t)blue;
                }
            break;
        }
        case 2:  // subtract green
            for (uint32_t &p : data) {
                uint32_t g = p >> 8 & 255;
                uint32_t rb = ((p & 0x00ff00ffu) + (g << 16 | g)) & 0x00ff00ffu;
                p = (p & 0xff00ff00u) | rb;
            }
            break;
        case 3: {  // colour indexing: expands packed pixels to the full width
            int pw = subsample(w, t.bits);
            std::vector<uint32_t> out((size_t)w * ysize);
            int bpp = 8 >> t.bits, per = 1 << t.bits;
            uint32_t mask = (1u << bpp) - 1;
            for (int y = 0; y < ysize; y++)
                for (int x = 0; x < w; x++) {
                    uint32_t packed = data[(size_t)y * pw + x / per] >> 8 & 255;
                    uint32_t idx = t.bits ? (packed >> ((x % per) * bpp)) & mask : packed;
                    out[(size_t)y * w + x] = idx < t.data.size() ? t.data[idx] : 0;
                }
            data.swap(out);
            break;
        }
        }
    }
};

void decode_vp8l(const uint8_t *d, size_t n, Image &img) {
    if (n < 5 || d[0] != 0x2f) fail("bad lossless header");
    uint32_t bits = le32(d + 1);
    int w = (int)(bits & 0x3fff) + 1, h = (int)(bits >> 14 & 0x3fff) + 1;
    if ((bits >> 29) != 0) fail("unsupported lossless version");
    if ((uint64_t)w * h > kMaxPixels) fail("image is too large");
    VP8L dec(d + 5, n - 5);
    std::vector<uint32_t> argb = dec.decode_image(w, h, true);
    img.w = w;
    img.h = h;
    img.rgba.resize((size_t)w * h * 4);
    for (size_t i = 0; i < argb.size(); i++) {
        uint32_t p = argb[i];
        uint8_t *o = img.rgba.data() + i * 4;
        o[0] = (uint8_t)(p >> 16); o[1] = (uint8_t)(p >> 8); o[2] = (uint8_t)p; o[3] = (uint8_t)(p >> 24);
    }
}

// =====================================================================================
// VP8 (lossy)
// =====================================================================================

struct BoolDec {
    const uint8_t *p = nullptr, *end = nullptr;
    uint32_t value = 0;
    uint32_t range = 255;
    int bit_count = 0;

    uint32_t next() { return p < end ? *p++ : 0; }
    void init(const uint8_t *d, size_t n) {
        p = d;
        end = d + n;
        value = next() << 8;
        value |= next();
        range = 255;
        bit_count = 0;
    }
    int get(int prob) {
        uint32_t split = 1 + (((range - 1) * (uint32_t)prob) >> 8);
        uint32_t big = split << 8;
        int r;
        if (value >= big) { r = 1; range -= split; value -= big; }
        else { r = 0; range = split; }
        while (range < 128) {
            value <<= 1;
            range <<= 1;
            if (++bit_count == 8) { bit_count = 0; value |= next(); }
        }
        return r;
    }
    int literal(int n) { int v = 0; while (n--) v = (v << 1) | get(128); return v; }
    int signed_value(int n) { int v = literal(n); return get(128) ? -v : v; }
};

const uint8_t kZigzag[16] = {0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
const uint8_t kBands[17] = {0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0};
const uint8_t kCat3[] = {173, 148, 140, 0}, kCat4[] = {176, 155, 140, 135, 0},
              kCat5[] = {180, 157, 141, 134, 130, 0}, kCat6[] = {254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129, 0};
const uint8_t *const kCat3456[4] = {kCat3, kCat4, kCat5, kCat6};

enum { B_DC, B_TM, B_VE, B_HE, B_RD, B_VR, B_LD, B_VL, B_HD, B_HU };

constexpr int BPS = 32;

inline uint8_t clip8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }
inline uint8_t avg3(int a, int b, int c) { return (uint8_t)((a + 2 * b + c + 2) >> 2); }
inline uint8_t avg2(int a, int b) { return (uint8_t)((a + b + 1) >> 1); }

// ---- intra prediction (dst points into a buffer with the row above and column left) ----

void pred_true_motion(uint8_t *dst, int size) {
    const uint8_t *top = dst - BPS;
    int tl = top[-1];
    for (int y = 0; y < size; y++) {
        int left = dst[-1 + y * BPS];
        for (int x = 0; x < size; x++) dst[x + y * BPS] = clip8(top[x] + left - tl);
    }
}
void pred_vertical(uint8_t *dst, int size) {
    for (int y = 0; y < size; y++) std::memcpy(dst + y * BPS, dst - BPS, size);
}
void pred_horizontal(uint8_t *dst, int size) {
    for (int y = 0; y < size; y++) std::memset(dst + y * BPS, dst[-1 + y * BPS], size);
}
// DC with the available edges (have_top, have_left); size 16 or 8
void pred_dc(uint8_t *dst, int size, bool have_top, bool have_left) {
    int shift = size == 16 ? 4 : 3, dc;
    if (have_top && have_left) {
        int s = 0;
        for (int i = 0; i < size; i++) s += dst[i - BPS] + dst[-1 + i * BPS];
        dc = (s + size) >> (shift + 1);
    } else if (have_top) {
        int s = 0;
        for (int i = 0; i < size; i++) s += dst[i - BPS];
        dc = (s + size / 2) >> shift;
    } else if (have_left) {
        int s = 0;
        for (int i = 0; i < size; i++) s += dst[-1 + i * BPS];
        dc = (s + size / 2) >> shift;
    } else {
        dc = 0x80;
    }
    for (int y = 0; y < size; y++) std::memset(dst + y * BPS, dc, size);
}

#define DST(x, y) dst[(x) + (y) * BPS]
void pred4(uint8_t *dst, int mode) {
    const uint8_t *top = dst - BPS;
    int X = top[-1], A = top[0], B = top[1], C = top[2], D = top[3], E = top[4], F = top[5], G = top[6], Hh = top[7];
    int I = dst[-1], J = dst[-1 + BPS], K = dst[-1 + 2 * BPS], L = dst[-1 + 3 * BPS];
    switch (mode) {
    case B_DC: {
        int dc = 4;
        for (int i = 0; i < 4; i++) dc += dst[i - BPS] + dst[-1 + i * BPS];
        dc >>= 3;
        for (int y = 0; y < 4; y++) std::memset(dst + y * BPS, dc, 4);
        break;
    }
    case B_TM: pred_true_motion(dst, 4); break;
    case B_VE: {
        uint8_t v[4] = {avg3(X, A, B), avg3(A, B, C), avg3(B, C, D), avg3(C, D, E)};
        for (int y = 0; y < 4; y++) std::memcpy(dst + y * BPS, v, 4);
        break;
    }
    case B_HE: {
        uint8_t v[4] = {avg3(X, I, J), avg3(I, J, K), avg3(J, K, L), avg3(K, L, L)};
        for (int y = 0; y < 4; y++) std::memset(dst + y * BPS, v[y], 4);
        break;
    }
    case B_RD:
        DST(0, 3) = avg3(J, K, L);
        DST(1, 3) = DST(0, 2) = avg3(I, J, K);
        DST(2, 3) = DST(1, 2) = DST(0, 1) = avg3(X, I, J);
        DST(3, 3) = DST(2, 2) = DST(1, 1) = DST(0, 0) = avg3(A, X, I);
        DST(3, 2) = DST(2, 1) = DST(1, 0) = avg3(B, A, X);
        DST(3, 1) = DST(2, 0) = avg3(C, B, A);
        DST(3, 0) = avg3(D, C, B);
        break;
    case B_VR:
        DST(0, 0) = DST(1, 2) = avg2(X, A);
        DST(1, 0) = DST(2, 2) = avg2(A, B);
        DST(2, 0) = DST(3, 2) = avg2(B, C);
        DST(3, 0) = avg2(C, D);
        DST(0, 3) = avg3(K, J, I);
        DST(0, 2) = avg3(J, I, X);
        DST(0, 1) = DST(1, 3) = avg3(I, X, A);
        DST(1, 1) = DST(2, 3) = avg3(X, A, B);
        DST(2, 1) = DST(3, 3) = avg3(A, B, C);
        DST(3, 1) = avg3(B, C, D);
        break;
    case B_LD:
        DST(0, 0) = avg3(A, B, C);
        DST(1, 0) = DST(0, 1) = avg3(B, C, D);
        DST(2, 0) = DST(1, 1) = DST(0, 2) = avg3(C, D, E);
        DST(3, 0) = DST(2, 1) = DST(1, 2) = DST(0, 3) = avg3(D, E, F);
        DST(3, 1) = DST(2, 2) = DST(1, 3) = avg3(E, F, G);
        DST(3, 2) = DST(2, 3) = avg3(F, G, Hh);
        DST(3, 3) = avg3(G, Hh, Hh);
        break;
    case B_VL:
        DST(0, 0) = avg2(A, B);
        DST(1, 0) = DST(0, 2) = avg2(B, C);
        DST(2, 0) = DST(1, 2) = avg2(C, D);
        DST(3, 0) = DST(2, 2) = avg2(D, E);
        DST(0, 1) = avg3(A, B, C);
        DST(1, 1) = DST(0, 3) = avg3(B, C, D);
        DST(2, 1) = DST(1, 3) = avg3(C, D, E);
        DST(3, 1) = DST(2, 3) = avg3(D, E, F);
        DST(3, 2) = avg3(E, F, G);
        DST(3, 3) = avg3(F, G, Hh);
        break;
    case B_HD:
        DST(0, 0) = DST(2, 1) = avg2(I, X);
        DST(0, 1) = DST(2, 2) = avg2(J, I);
        DST(0, 2) = DST(2, 3) = avg2(K, J);
        DST(0, 3) = avg2(L, K);
        DST(3, 0) = avg3(A, B, C);
        DST(2, 0) = avg3(X, A, B);
        DST(1, 0) = DST(3, 1) = avg3(I, X, A);
        DST(1, 1) = DST(3, 2) = avg3(J, I, X);
        DST(1, 2) = DST(3, 3) = avg3(K, J, I);
        DST(1, 3) = avg3(L, K, J);
        break;
    case B_HU:
        DST(0, 0) = avg2(I, J);
        DST(2, 0) = DST(0, 1) = avg2(J, K);
        DST(2, 1) = DST(0, 2) = avg2(K, L);
        DST(1, 0) = avg3(I, J, K);
        DST(3, 0) = DST(1, 1) = avg3(J, K, L);
        DST(3, 1) = DST(1, 2) = avg3(K, L, L);
        DST(3, 2) = DST(2, 2) = DST(0, 3) = DST(1, 3) = DST(2, 3) = DST(3, 3) = (uint8_t)L;
        break;
    }
}
#undef DST

// ---- transforms ----

inline int mul1(int a) { return ((a * 20091) >> 16) + a; }
inline int mul2(int a) { return (a * 35468) >> 16; }

void idct_add(const int16_t *in, uint8_t *dst) {
    int C[16], *tmp = C;
    for (int i = 0; i < 4; i++) {  // vertical pass
        int a = in[0] + in[8], b = in[0] - in[8];
        int c = mul2(in[4]) - mul1(in[12]), d = mul1(in[4]) + mul2(in[12]);
        tmp[0] = a + d; tmp[1] = b + c; tmp[2] = b - c; tmp[3] = a - d;
        tmp += 4;
        in++;
    }
    tmp = C;
    for (int i = 0; i < 4; i++) {  // horizontal pass
        int dc = tmp[0] + 4;
        int a = dc + tmp[8], b = dc - tmp[8];
        int c = mul2(tmp[4]) - mul1(tmp[12]), d = mul1(tmp[4]) + mul2(tmp[12]);
        dst[0] = clip8(dst[0] + ((a + d) >> 3));
        dst[1] = clip8(dst[1] + ((b + c) >> 3));
        dst[2] = clip8(dst[2] + ((b - c) >> 3));
        dst[3] = clip8(dst[3] + ((a - d) >> 3));
        tmp++;
        dst += BPS;
    }
}

void inverse_wht(const int16_t *in, int16_t *out) {  // out: DC of 16 blocks, 16 apart
    int tmp[16];
    for (int i = 0; i < 4; i++) {
        int a0 = in[0 + i] + in[12 + i], a1 = in[4 + i] + in[8 + i];
        int a2 = in[4 + i] - in[8 + i], a3 = in[0 + i] - in[12 + i];
        tmp[0 + i] = a0 + a1; tmp[8 + i] = a0 - a1;
        tmp[4 + i] = a3 + a2; tmp[12 + i] = a3 - a2;
    }
    for (int i = 0; i < 4; i++) {
        int dc = tmp[0 + i * 4] + 3;
        int a0 = dc + tmp[3 + i * 4], a1 = tmp[1 + i * 4] + tmp[2 + i * 4];
        int a2 = tmp[1 + i * 4] - tmp[2 + i * 4], a3 = dc - tmp[3 + i * 4];
        out[0] = (int16_t)((a0 + a1) >> 3);
        out[16] = (int16_t)((a3 + a2) >> 3);
        out[32] = (int16_t)((a0 - a1) >> 3);
        out[48] = (int16_t)((a3 - a2) >> 3);
        out += 64;
    }
}

// ---- loop filter ----

inline int sclip1(int v) { return v < -128 ? -128 : v > 127 ? 127 : v; }   // [-1020,1020] -> [-128,127]
inline int sclip2(int v) { return v < -16 ? -16 : v > 15 ? 15 : v; }       // [-112,112] -> [-16,15]

void filter2(uint8_t *p, int step) {
    int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
    int a = 3 * (q0 - p0) + sclip1(p1 - q1);
    int a1 = sclip2((a + 4) >> 3), a2 = sclip2((a + 3) >> 3);
    p[-step] = clip8(p0 + a2);
    p[0] = clip8(q0 - a1);
}
void filter4(uint8_t *p, int step) {
    int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
    int a = 3 * (q0 - p0);
    int a1 = sclip2((a + 4) >> 3), a2 = sclip2((a + 3) >> 3), a3 = (a1 + 1) >> 1;
    p[-2 * step] = clip8(p1 + a3);
    p[-step] = clip8(p0 + a2);
    p[0] = clip8(q0 - a1);
    p[step] = clip8(q1 - a3);
}
void filter6(uint8_t *p, int step) {
    int p2 = p[-3 * step], p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step], q2 = p[2 * step];
    int a = sclip1(3 * (q0 - p0) + sclip1(p1 - q1));
    int a1 = (27 * a + 63) >> 7, a2 = (18 * a + 63) >> 7, a3 = (9 * a + 63) >> 7;
    p[-3 * step] = clip8(p2 + a3);
    p[-2 * step] = clip8(p1 + a2);
    p[-step] = clip8(p0 + a1);
    p[0] = clip8(q0 - a1);
    p[step] = clip8(q1 - a2);
    p[2 * step] = clip8(q2 - a3);
}
bool hev(const uint8_t *p, int step, int thresh) {
    int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
    return std::abs(p1 - p0) > thresh || std::abs(q1 - q0) > thresh;
}
bool needs_filter(const uint8_t *p, int step, int t) {
    int p1 = p[-2 * step], p0 = p[-step], q0 = p[0], q1 = p[step];
    return 4 * std::abs(p0 - q0) + std::abs(p1 - q1) <= t;
}
bool needs_filter2(const uint8_t *p, int step, int t, int it) {
    int p3 = p[-4 * step], p2 = p[-3 * step], p1 = p[-2 * step], p0 = p[-step];
    int q0 = p[0], q1 = p[step], q2 = p[2 * step], q3 = p[3 * step];
    if (4 * std::abs(p0 - q0) + std::abs(p1 - q1) > t) return false;
    return std::abs(p3 - p2) <= it && std::abs(p2 - p1) <= it && std::abs(p1 - p0) <= it &&
           std::abs(q3 - q2) <= it && std::abs(q2 - q1) <= it && std::abs(q1 - q0) <= it;
}
// hstride: across the edge; vstride: along it
void filter_loop26(uint8_t *p, int hstride, int vstride, int size, int thresh, int ithresh, int hev_t) {
    int t2 = 2 * thresh + 1;
    while (size-- > 0) {
        if (needs_filter2(p, hstride, t2, ithresh)) {
            if (hev(p, hstride, hev_t)) filter2(p, hstride);
            else filter6(p, hstride);
        }
        p += vstride;
    }
}
void filter_loop24(uint8_t *p, int hstride, int vstride, int size, int thresh, int ithresh, int hev_t) {
    int t2 = 2 * thresh + 1;
    while (size-- > 0) {
        if (needs_filter2(p, hstride, t2, ithresh)) {
            if (hev(p, hstride, hev_t)) filter2(p, hstride);
            else filter4(p, hstride);
        }
        p += vstride;
    }
}
void simple_filter(uint8_t *p, int hstride, int vstride, int size, int thresh) {
    int t2 = 2 * thresh + 1;
    for (int i = 0; i < size; i++, p += vstride)
        if (needs_filter(p, hstride, t2)) filter2(p, hstride);
}

// ---- YUV -> RGB, libwebp's fixed-point formulas ----

inline int mult_hi(int v, int c) { return (v * c) >> 8; }
inline uint8_t yuv_clip(int v) { return (v & ~16383) == 0 ? (uint8_t)(v >> 6) : v < 0 ? 0 : 255; }
inline void yuv_to_rgb(int y, int u, int v, uint8_t *o) {
    o[0] = yuv_clip(mult_hi(y, 19077) + mult_hi(v, 26149) - 14234);
    o[1] = yuv_clip(mult_hi(y, 19077) - mult_hi(u, 6419) - mult_hi(v, 13320) + 8708);
    o[2] = yuv_clip(mult_hi(y, 19077) + mult_hi(u, 33050) - 17685);
}

// One output row with "fancy" upsampling. For a top row (as in libwebp's line pairs) `a`
// is the nearer chroma row; for a bottom row `b` is.
void upsample_row(const uint8_t *yrow, const uint8_t *au, const uint8_t *av, const uint8_t *bu, const uint8_t *bv,
                  uint8_t *out, int len, bool top) {
    auto load = [](const uint8_t *u, const uint8_t *v, int x) { return (uint32_t)u[x] | (uint32_t)v[x] << 16; };
    uint32_t tl = load(au, av, 0), l = load(bu, bv, 0);
    {
        uint32_t uv0 = top ? (3 * tl + l + 0x00020002u) >> 2 : (3 * l + tl + 0x00020002u) >> 2;
        yuv_to_rgb(yrow[0], uv0 & 0xff, (uv0 >> 16) & 0xff, out);
    }
    int last_pair = (len - 1) >> 1;
    for (int x = 1; x <= last_pair; x++) {
        uint32_t t = load(au, av, x), uv = load(bu, bv, x);
        uint32_t avg = tl + t + l + uv + 0x00080008u;
        uint32_t diag_12 = (avg + 2 * (t + l)) >> 3;
        uint32_t diag_03 = (avg + 2 * (tl + uv)) >> 3;
        uint32_t uv0, uv1;
        if (top) { uv0 = (diag_12 + tl) >> 1; uv1 = (diag_03 + t) >> 1; }
        else { uv0 = (diag_03 + l) >> 1; uv1 = (diag_12 + uv) >> 1; }
        yuv_to_rgb(yrow[2 * x - 1], uv0 & 0xff, (uv0 >> 16) & 0xff, out + (2 * x - 1) * 4);
        yuv_to_rgb(yrow[2 * x], uv1 & 0xff, (uv1 >> 16) & 0xff, out + (2 * x) * 4);
        tl = t;
        l = uv;
    }
    if (!(len & 1)) {
        uint32_t uv0 = top ? (3 * tl + l + 0x00020002u) >> 2 : (3 * l + tl + 0x00020002u) >> 2;
        yuv_to_rgb(yrow[len - 1], uv0 & 0xff, (uv0 >> 16) & 0xff, out + (len - 1) * 4);
    }
}

struct FilterInfo {
    uint8_t limit, ilevel, hev_thresh, inner;
};

void decode_vp8(const uint8_t *d, size_t n, Image &img, const std::vector<uint8_t> *alpha) {
    if (n < 10) fail("VP8 data is truncated");
    uint32_t tag = le24(d);
    bool key_frame = !(tag & 1);
    uint32_t first_part = tag >> 5;
    if (!key_frame) fail("not a VP8 key frame");
    if (d[3] != 0x9d || d[4] != 0x01 || d[5] != 0x2a) fail("bad VP8 signature");
    int width = (int)(le16(d + 6) & 0x3fff), height = (int)(le16(d + 8) & 0x3fff);
    if (!width || !height || (uint64_t)width * height > kMaxPixels) fail("bad VP8 size");
    const uint8_t *part0 = d + 10;
    if (first_part > n - 10) fail("VP8 data is truncated");
    BoolDec br;
    br.init(part0, first_part);
    br.get(128);  // colour space
    br.get(128);  // clamping type

    // segment header
    bool use_segment = br.get(128), update_map = false, absolute = false;
    int seg_quant[4] = {0, 0, 0, 0}, seg_filter[4] = {0, 0, 0, 0};
    uint8_t seg_prob[3] = {255, 255, 255};
    if (use_segment) {
        update_map = br.get(128);
        if (br.get(128)) {
            absolute = br.get(128);
            for (int s = 0; s < 4; s++) seg_quant[s] = br.get(128) ? br.signed_value(7) : 0;
            for (int s = 0; s < 4; s++) seg_filter[s] = br.get(128) ? br.signed_value(6) : 0;
        }
        if (update_map)
            for (int s = 0; s < 3; s++) seg_prob[s] = (uint8_t)(br.get(128) ? br.literal(8) : 255);
    }
    // filter header
    bool simple = br.get(128);
    int level = br.literal(6), sharpness = br.literal(3);
    bool use_lf_delta = br.get(128);
    int ref_delta0 = 0, mode_delta0 = 0;
    if (use_lf_delta && br.get(128)) {
        for (int i = 0; i < 4; i++)
            if (br.get(128)) { int v = br.signed_value(6); if (i == 0) ref_delta0 = v; }
        for (int i = 0; i < 4; i++)
            if (br.get(128)) { int v = br.signed_value(6); if (i == 0) mode_delta0 = v; }
    }
    int filter_type = level == 0 ? 0 : simple ? 1 : 2;
    // partitions
    int num_parts = 1 << br.literal(2);
    const uint8_t *sizes = part0 + first_part;
    size_t size_left = n - 10 - first_part;
    if (size_left < (size_t)(num_parts - 1) * 3) fail("VP8 partitions are truncated");
    const uint8_t *part_start = sizes + (num_parts - 1) * 3;
    size_t avail = size_left - (num_parts - 1) * 3;
    std::vector<BoolDec> parts(num_parts);
    for (int p = 0; p < num_parts; p++) {
        size_t psize = p < num_parts - 1 ? le24(sizes + 3 * p) : avail;
        if (psize > avail) psize = avail;
        parts[p].init(part_start, psize);
        part_start += psize;
        avail -= psize;
    }
    // quantizers
    int base_q = br.literal(7);
    int dq_y1_dc = br.get(128) ? br.signed_value(4) : 0;
    int dq_y2_dc = br.get(128) ? br.signed_value(4) : 0;
    int dq_y2_ac = br.get(128) ? br.signed_value(4) : 0;
    int dq_uv_dc = br.get(128) ? br.signed_value(4) : 0;
    int dq_uv_ac = br.get(128) ? br.signed_value(4) : 0;
    struct Quant { int y1[2], y2[2], uv[2]; } quant[4];
    auto clipq = [](int v, int m) { return v < 0 ? 0 : v > m ? m : v; };
    for (int s = 0; s < 4; s++) {
        int q = use_segment ? seg_quant[s] + (absolute ? 0 : base_q) : base_q;
        Quant &m = quant[s];
        m.y1[0] = kDcTable[clipq(q + dq_y1_dc, 127)];
        m.y1[1] = kAcTable[clipq(q, 127)];
        m.y2[0] = kDcTable[clipq(q + dq_y2_dc, 127)] * 2;
        m.y2[1] = (kAcTable[clipq(q + dq_y2_ac, 127)] * 101581) >> 16;
        if (m.y2[1] < 8) m.y2[1] = 8;
        m.uv[0] = kDcTable[clipq(q + dq_uv_dc, 117)];
        m.uv[1] = kAcTable[clipq(q + dq_uv_ac, 127)];
    }
    br.get(128);  // refresh entropy probabilities: ignored for a still image
    // token probabilities
    uint8_t proba[4][8][3][11];
    for (int t = 0; t < 4; t++)
        for (int b = 0; b < 8; b++)
            for (int c = 0; c < 3; c++)
                for (int p = 0; p < 11; p++) {
                    int idx = ((t * 8 + b) * 3 + c) * 11 + p;
                    proba[t][b][c][p] = (uint8_t)(br.get(kCoeffsUpdateProba[idx]) ? br.literal(8) : kCoeffsProba0[idx]);
                }
    bool use_skip = br.get(128);
    int skip_prob = use_skip ? br.literal(8) : 0;

    // filter strengths per segment and mode
    FilterInfo fstr[4][2];
    for (int s = 0; s < 4; s++) {
        int base = use_segment ? seg_filter[s] + (absolute ? 0 : level) : level;
        for (int i4 = 0; i4 <= 1; i4++) {
            FilterInfo &f = fstr[s][i4];
            int lv = base;
            if (use_lf_delta) {
                lv += ref_delta0;
                if (i4) lv += mode_delta0;
            }
            lv = lv < 0 ? 0 : lv > 63 ? 63 : lv;
            if (lv > 0) {
                int il = lv;
                if (sharpness > 0) {
                    il >>= sharpness > 4 ? 2 : 1;
                    if (il > 9 - sharpness) il = 9 - sharpness;
                }
                if (il < 1) il = 1;
                f.ilevel = (uint8_t)il;
                f.limit = (uint8_t)(2 * lv + il);
                f.hev_thresh = (uint8_t)(lv >= 40 ? 2 : lv >= 15 ? 1 : 0);
            } else {
                f.limit = 0;
                f.ilevel = 0;
                f.hev_thresh = 0;
            }
            f.inner = (uint8_t)i4;
        }
    }

    int mb_w = (width + 15) >> 4, mb_h = (height + 15) >> 4;
    int ys = mb_w * 16, uvs = mb_w * 8;
    std::vector<uint8_t> Y((size_t)ys * mb_h * 16), U((size_t)uvs * mb_h * 8), V((size_t)uvs * mb_h * 8);
    std::vector<FilterInfo> finfo((size_t)mb_w * mb_h);
    std::vector<uint8_t> top_modes((size_t)mb_w * 4, B_DC);
    struct Nz { uint8_t y, u, v, dc; };
    std::vector<Nz> top_nz(mb_w, Nz{0, 0, 0, 0});

    uint8_t work_y[(1 + 16) * BPS + 32], work_u[(1 + 8) * BPS + 16], work_v[(1 + 8) * BPS + 16];
    uint8_t *yb = work_y + BPS + 8, *ub = work_u + BPS + 8, *vb = work_v + BPS + 8;
    int16_t coeffs[24 * 16];

    auto get_coeffs = [&](BoolDec &tb, int type, int ctx, const int *dq, int nstart, int16_t *out) -> int {
        int n = nstart;
        const uint8_t *p = proba[type][kBands[n]][ctx];
        for (; n < 16; n++) {
            if (!tb.get(p[0])) return n;
            while (!tb.get(p[1])) {
                p = proba[type][kBands[++n]][0];
                if (n == 16) return 16;
            }
            int v;
            if (!tb.get(p[2])) {
                v = 1;
                p = proba[type][kBands[n + 1]][1];
            } else {
                if (!tb.get(p[3])) {
                    if (!tb.get(p[4])) v = 2;
                    else v = 3 + tb.get(p[5]);
                } else if (!tb.get(p[6])) {
                    if (!tb.get(p[7])) v = 5 + tb.get(159);
                    else { v = 7 + 2 * tb.get(165); v += tb.get(145); }
                } else {
                    int bit1 = tb.get(p[8]);
                    int bit0 = tb.get(p[9 + bit1]);
                    int cat = 2 * bit1 + bit0;
                    v = 0;
                    for (const uint8_t *tab = kCat3456[cat]; *tab; ++tab) v += v + tb.get(*tab);
                    v += 3 + (8 << cat);
                }
                p = proba[type][kBands[n + 1]][2];
            }
            out[kZigzag[n]] = (int16_t)((tb.get(128) ? -v : v) * dq[n > 0]);
        }
        return 16;
    };

    for (int my = 0; my < mb_h; my++) {
        BoolDec &tb = parts[my & (num_parts - 1)];
        uint8_t left_modes[4] = {B_DC, B_DC, B_DC, B_DC};
        Nz left_nz{0, 0, 0, 0};
        for (int mx = 0; mx < mb_w; mx++) {
            // ---- modes (first partition) ----
            int segment = 0;
            if (update_map) segment = !br.get(seg_prob[0]) ? br.get(seg_prob[1]) : br.get(seg_prob[2]) + 2;
            bool skip = use_skip ? br.get(skip_prob) : false;
            bool is_i4 = !br.get(145);
            uint8_t imodes[16];
            uint8_t *top = &top_modes[(size_t)mx * 4];
            int ymode = B_DC;
            if (!is_i4) {
                ymode = br.get(156) ? (br.get(128) ? B_TM : B_HE) : (br.get(163) ? B_VE : B_DC);
                std::memset(top, ymode, 4);
                std::memset(left_modes, ymode, 4);
            } else {
                for (int y = 0; y < 4; y++) {
                    int m = left_modes[y];
                    for (int x = 0; x < 4; x++) {
                        const uint8_t *prob = &kBModesProba[(top[x] * 10 + m) * 9];
                        m = !br.get(prob[0]) ? B_DC
                          : !br.get(prob[1]) ? B_TM
                          : !br.get(prob[2]) ? B_VE
                          : !br.get(prob[3]) ? (!br.get(prob[4]) ? B_HE : (!br.get(prob[5]) ? B_RD : B_VR))
                          : (!br.get(prob[6]) ? B_LD : (!br.get(prob[7]) ? B_VL : (!br.get(prob[8]) ? B_HD : B_HU)));
                        top[x] = (uint8_t)m;
                        imodes[y * 4 + x] = (uint8_t)m;
                    }
                    left_modes[y] = (uint8_t)m;
                }
            }
            int uvmode = !br.get(142) ? B_DC : !br.get(114) ? B_VE : br.get(183) ? B_TM : B_HE;

            // ---- residuals (token partition) ----
            std::memset(coeffs, 0, sizeof coeffs);
            Nz &tnz = top_nz[mx];
            const Quant &q = quant[segment];
            bool any = false;
            if (!skip) {
                int first, type;
                if (!is_i4) {
                    int16_t dc[16] = {0};
                    int nz = get_coeffs(tb, 1, tnz.dc + left_nz.dc, q.y2, 0, dc);
                    tnz.dc = left_nz.dc = nz > 0;
                    inverse_wht(dc, coeffs);
                    first = 1;
                    type = 0;
                } else {
                    first = 0;
                    type = 3;
                }
                for (int y = 0; y < 4; y++) {
                    int l = (left_nz.y >> y) & 1;
                    for (int x = 0; x < 4; x++) {
                        int t = (tnz.y >> x) & 1;
                        int nz = get_coeffs(tb, type, l + t, q.y1, first, coeffs + (y * 4 + x) * 16);
                        l = nz > first;
                        tnz.y = (uint8_t)((tnz.y & ~(1 << x)) | (l << x));
                    }
                    left_nz.y = (uint8_t)((left_nz.y & ~(1 << y)) | (l << y));
                }
                for (int ch = 0; ch < 2; ch++) {
                    uint8_t &tbits = ch ? tnz.v : tnz.u;
                    uint8_t &lbits = ch ? left_nz.v : left_nz.u;
                    for (int y = 0; y < 2; y++) {
                        int l = (lbits >> y) & 1;
                        for (int x = 0; x < 2; x++) {
                            int t = (tbits >> x) & 1;
                            int nz = get_coeffs(tb, 2, l + t, q.uv, 0, coeffs + (16 + ch * 4 + y * 2 + x) * 16);
                            l = nz > 0;
                            tbits = (uint8_t)((tbits & ~(1 << x)) | (l << x));
                        }
                        lbits = (uint8_t)((lbits & ~(1 << y)) | (l << y));
                    }
                }
                for (int i = 0; i < 24 * 16 && !any; i++) any = coeffs[i] != 0;
            } else {
                tnz.y = tnz.u = tnz.v = 0;
                left_nz.y = left_nz.u = left_nz.v = 0;
                if (!is_i4) tnz.dc = left_nz.dc = 0;
            }
            if (filter_type) {
                FilterInfo f = fstr[segment][is_i4];
                f.inner |= (uint8_t)any;
                finfo[(size_t)my * mb_w + mx] = f;
            }

            // ---- reconstruction: fill the work buffers' edges from the (unfiltered) planes ----
            uint8_t *py = &Y[(size_t)my * 16 * ys + mx * 16];
            uint8_t *pu = &U[(size_t)my * 8 * uvs + mx * 8], *pv = &V[(size_t)my * 8 * uvs + mx * 8];
            if (my > 0) {
                std::memcpy(yb - BPS, py - ys, 16);
                std::memcpy(ub - BPS, pu - uvs, 8);
                std::memcpy(vb - BPS, pv - uvs, 8);
                yb[-BPS - 1] = mx > 0 ? py[-ys - 1] : 129;
                ub[-BPS - 1] = mx > 0 ? pu[-uvs - 1] : 129;
                vb[-BPS - 1] = mx > 0 ? pv[-uvs - 1] : 129;
            } else {
                std::memset(yb - BPS - 1, 127, 16 + 4 + 1);
                std::memset(ub - BPS - 1, 127, 8 + 1);
                std::memset(vb - BPS - 1, 127, 8 + 1);
            }
            for (int j = 0; j < 16; j++) yb[j * BPS - 1] = mx > 0 ? py[j * ys - 1] : 129;
            for (int j = 0; j < 8; j++) {
                ub[j * BPS - 1] = mx > 0 ? pu[j * uvs - 1] : 129;
                vb[j * BPS - 1] = mx > 0 ? pv[j * uvs - 1] : 129;
            }
            if (is_i4) {
                uint8_t *top_right = yb - BPS + 16;
                if (my > 0) {
                    if (mx >= mb_w - 1) std::memset(top_right, py[-ys + 15], 4);
                    else std::memcpy(top_right, py - ys + 16, 4);
                }
                for (int r = 1; r <= 3; r++) std::memcpy(top_right + r * 4 * BPS, top_right, 4);
                for (int b = 0; b < 16; b++) {
                    uint8_t *dst = yb + (b >> 2) * 4 * BPS + (b & 3) * 4;
                    pred4(dst, imodes[b]);
                    idct_add(coeffs + b * 16, dst);
                }
            } else {
                switch (ymode) {
                case B_DC: pred_dc(yb, 16, my > 0, mx > 0); break;
                case B_TM: pred_true_motion(yb, 16); break;
                case B_VE: pred_vertical(yb, 16); break;
                default: pred_horizontal(yb, 16); break;
                }
                for (int b = 0; b < 16; b++) idct_add(coeffs + b * 16, yb + (b >> 2) * 4 * BPS + (b & 3) * 4);
            }
            for (int ch = 0; ch < 2; ch++) {
                uint8_t *cb = ch ? vb : ub;
                switch (uvmode) {
                case B_DC: pred_dc(cb, 8, my > 0, mx > 0); break;
                case B_TM: pred_true_motion(cb, 8); break;
                case B_VE: pred_vertical(cb, 8); break;
                default: pred_horizontal(cb, 8); break;
                }
                for (int b = 0; b < 4; b++) idct_add(coeffs + (16 + ch * 4 + b) * 16, cb + (b >> 1) * 4 * BPS + (b & 1) * 4);
            }
            for (int j = 0; j < 16; j++) std::memcpy(py + j * ys, yb + j * BPS, 16);
            for (int j = 0; j < 8; j++) {
                std::memcpy(pu + j * uvs, ub + j * BPS, 8);
                std::memcpy(pv + j * uvs, vb + j * BPS, 8);
            }
        }
    }

    // ---- loop filter, macroblock by macroblock in raster order ----
    if (filter_type) {
        for (int my = 0; my < mb_h; my++)
            for (int mx = 0; mx < mb_w; mx++) {
                const FilterInfo &f = finfo[(size_t)my * mb_w + mx];
                int limit = f.limit;
                if (!limit) continue;
                uint8_t *py = &Y[(size_t)my * 16 * ys + mx * 16];
                if (filter_type == 1) {
                    if (mx > 0) simple_filter(py, 1, ys, 16, limit + 4);
                    if (f.inner)
                        for (int k = 4; k < 16; k += 4) simple_filter(py + k, 1, ys, 16, limit);
                    if (my > 0) simple_filter(py, ys, 1, 16, limit + 4);
                    if (f.inner)
                        for (int k = 4; k < 16; k += 4) simple_filter(py + k * ys, ys, 1, 16, limit);
                } else {
                    uint8_t *pu = &U[(size_t)my * 8 * uvs + mx * 8], *pv = &V[(size_t)my * 8 * uvs + mx * 8];
                    int il = f.ilevel, ht = f.hev_thresh;
                    if (mx > 0) {
                        filter_loop26(py, 1, ys, 16, limit + 4, il, ht);
                        filter_loop26(pu, 1, uvs, 8, limit + 4, il, ht);
                        filter_loop26(pv, 1, uvs, 8, limit + 4, il, ht);
                    }
                    if (f.inner) {
                        for (int k = 4; k < 16; k += 4) filter_loop24(py + k, 1, ys, 16, limit, il, ht);
                        filter_loop24(pu + 4, 1, uvs, 8, limit, il, ht);
                        filter_loop24(pv + 4, 1, uvs, 8, limit, il, ht);
                    }
                    if (my > 0) {
                        filter_loop26(py, ys, 1, 16, limit + 4, il, ht);
                        filter_loop26(pu, uvs, 1, 8, limit + 4, il, ht);
                        filter_loop26(pv, uvs, 1, 8, limit + 4, il, ht);
                    }
                    if (f.inner) {
                        for (int k = 4; k < 16; k += 4) filter_loop24(py + k * ys, ys, 1, 16, limit, il, ht);
                        filter_loop24(pu + 4 * uvs, uvs, 1, 8, limit, il, ht);
                        filter_loop24(pv + 4 * uvs, uvs, 1, 8, limit, il, ht);
                    }
                }
            }
    }

    // ---- colour conversion ----
    img.w = width;
    img.h = height;
    img.rgba.assign((size_t)width * height * 4, 255);
    int ch_rows = (height + 1) >> 1;
    for (int y = 0; y < height; y++) {
        const uint8_t *yrow = &Y[(size_t)y * ys];
        int a_row, b_row;
        bool top;
        if (y == 0) { a_row = b_row = 0; top = true; }
        else if (y & 1) { a_row = (y - 1) >> 1; b_row = std::min((y + 1) >> 1, ch_rows - 1); top = true; }
        else { a_row = (y >> 1) - 1; b_row = y >> 1; top = false; }
        upsample_row(yrow, &U[(size_t)a_row * uvs], &V[(size_t)a_row * uvs], &U[(size_t)b_row * uvs], &V[(size_t)b_row * uvs],
                     img.rgba.data() + (size_t)y * width * 4, width, top);
        if (!alpha)
            for (int x = 0; x < width; x++) img.rgba[((size_t)y * width + x) * 4 + 3] = 255;
    }
    if (alpha)
        for (size_t i = 0; i < (size_t)width * height; i++) img.rgba[i * 4 + 3] = (*alpha)[i];
}

// ALPH chunk -> width*height alpha values
std::vector<uint8_t> decode_alpha(const uint8_t *d, size_t n, int w, int h) {
    if (n < 1) fail("empty alpha chunk");
    int method = d[0] & 3, filter = (d[0] >> 2) & 3;
    std::vector<uint8_t> a((size_t)w * h);
    if (method == 0) {
        if (n - 1 < a.size()) fail("alpha data is truncated");
        std::memcpy(a.data(), d + 1, a.size());
    } else if (method == 1) {
        VP8L dec(d + 1, n - 1);
        std::vector<uint32_t> argb = dec.decode_image(w, h, true);
        for (size_t i = 0; i < a.size(); i++) a[i] = (uint8_t)(argb[i] >> 8);
    } else {
        fail("unknown alpha compression");
    }
    for (int y = 0; y < h; y++) {  // undo the filter, row by row
        uint8_t *row = &a[(size_t)y * w];
        const uint8_t *prev = y ? row - w : nullptr;
        if (filter == 0) continue;
        if (!prev || filter == 1) {
            int pred = prev ? prev[0] : 0;
            for (int x = 0; x < w; x++) { row[x] = (uint8_t)(row[x] + pred); pred = row[x]; }
        } else if (filter == 2) {
            for (int x = 0; x < w; x++) row[x] = (uint8_t)(row[x] + prev[x]);
        } else {
            int top = prev[0], top_left = top, left = top;
            for (int x = 0; x < w; x++) {
                top = prev[x];
                int g = left + top - top_left;
                left = (uint8_t)(row[x] + (g < 0 ? 0 : g > 255 ? 255 : g));
                top_left = top;
                row[x] = (uint8_t)left;
            }
        }
    }
    return a;
}

// Decodes the image in a VP8 / VP8L (+ optional ALPH) chunk sequence.
void decode_frame(const uint8_t *d, size_t n, Image &img) {
    const uint8_t *alph = nullptr;
    size_t alph_n = 0;
    size_t pos = 0;
    while (pos + 8 <= n) {
        const uint8_t *tag = d + pos;
        uint32_t size = le32(d + pos + 4);
        if (size > n - pos - 8) size = (uint32_t)(n - pos - 8);
        const uint8_t *body = d + pos + 8;
        if (!std::memcmp(tag, "ALPH", 4)) { alph = body; alph_n = size; }
        else if (!std::memcmp(tag, "VP8L", 4)) { decode_vp8l(body, size, img); return; }
        else if (!std::memcmp(tag, "VP8 ", 4)) {
            if (size < 10) fail("VP8 data is truncated");
            int w = (int)(le16(body + 6) & 0x3fff), h = (int)(le16(body + 8) & 0x3fff);
            if (alph) {
                std::vector<uint8_t> a = decode_alpha(alph, alph_n, w, h);
                decode_vp8(body, size, img, &a);
            } else {
                decode_vp8(body, size, img, nullptr);
            }
            return;
        }
        pos += 8 + size + (size & 1);
    }
    fail("WebP has no image data");
}

}  // namespace

bool is_webp(const uint8_t *d, size_t n) { return n >= 12 && !std::memcmp(d, "RIFF", 4) && !std::memcmp(d + 8, "WEBP", 4); }

void decode_webp(const uint8_t *d, size_t n, Image &img) {
    size_t riff = le32(d + 4);
    if (riff + 8 < n) n = riff + 8;
    const uint8_t *chunks = d + 12;
    size_t len = n - 12;
    if (len >= 8 && !std::memcmp(chunks, "VP8X", 4)) {
        uint32_t xsize = le32(chunks + 4);
        if (xsize < 10 || xsize > len - 8) fail("bad VP8X chunk");
        int cw = (int)le24(chunks + 8 + 4) + 1, chh = (int)le24(chunks + 8 + 7) + 1;
        if ((uint64_t)cw * chh > kMaxPixels) fail("image is too large");
        bool animated = chunks[8] & 2;
        size_t pos = 8 + xsize + (xsize & 1);
        if (animated) {  // first frame on a transparent canvas
            while (pos + 8 <= len) {
                uint32_t size = le32(chunks + pos + 4);
                if (size > len - pos - 8) size = (uint32_t)(len - pos - 8);
                if (!std::memcmp(chunks + pos, "ANMF", 4) && size >= 16) {
                    const uint8_t *f = chunks + pos + 8;
                    int fx = (int)le24(f) * 2, fy = (int)le24(f + 3) * 2;
                    Image frame;
                    decode_frame(f + 16, size - 16, frame);
                    img.w = cw;
                    img.h = chh;
                    img.rgba.assign((size_t)cw * chh * 4, 0);
                    for (int y = 0; y < frame.h && fy + y < chh; y++)
                        for (int x = 0; x < frame.w && fx + x < cw; x++)
                            std::memcpy(&img.rgba[((size_t)(fy + y) * cw + fx + x) * 4], &frame.rgba[((size_t)y * frame.w + x) * 4], 4);
                    return;
                }
                pos += 8 + size + (size & 1);
            }
            fail("animated WebP has no frames");
        }
        decode_frame(chunks + pos, len - pos, img);
        return;
    }
    decode_frame(chunks, len, img);
}

}  // namespace image
