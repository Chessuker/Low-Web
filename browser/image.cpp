// image.cpp — see image.h.
#include "image.h"
#include "inflate.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace image {

namespace {

struct DecodeError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

void check_size(uint64_t w, uint64_t h) {
    if (w == 0 || h == 0) throw DecodeError("image has no pixels");
    if (w > 65535 || h > 65535 || w * h > kMaxPixels) throw DecodeError("image is too large");
}

// =====================================================================================
// PNG
// =====================================================================================

bool is_png(const uint8_t *d, size_t n) {
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    return n >= 8 && std::memcmp(d, sig, 8) == 0;
}

void decode_png(const uint8_t *d, size_t n, Image &img) {
    size_t pos = 8;
    uint32_t w = 0, h = 0;
    int depth = 0, ctype = -1, interlace = 0;
    std::vector<uint8_t> idat, palette, trns;
    bool seen_end = false;
    while (pos + 8 <= n && !seen_end) {
        uint32_t len = be32(d + pos);
        const uint8_t *type = d + pos + 4, *body = d + pos + 8;
        if (len > n - pos - 8) throw DecodeError("PNG chunk is truncated");
        if (!std::memcmp(type, "IHDR", 4)) {
            if (len < 13) throw DecodeError("bad IHDR");
            w = be32(body); h = be32(body + 4);
            depth = body[8]; ctype = body[9]; interlace = body[12];
            if (body[10] || body[11] || interlace > 1) throw DecodeError("unsupported PNG compression or filter");
        } else if (!std::memcmp(type, "PLTE", 4)) {
            palette.assign(body, body + len);
        } else if (!std::memcmp(type, "tRNS", 4)) {
            trns.assign(body, body + len);
        } else if (!std::memcmp(type, "IDAT", 4)) {
            idat.insert(idat.end(), body, body + len);
        } else if (!std::memcmp(type, "IEND", 4)) {
            seen_end = true;
        }
        pos += 12 + (size_t)len;
    }
    if (ctype < 0) throw DecodeError("PNG has no header");
    check_size(w, h);
    int channels;
    switch (ctype) {
    case 0: channels = 1; break;
    case 2: channels = 3; break;
    case 3: channels = 1; break;
    case 4: channels = 2; break;
    case 6: channels = 4; break;
    default: throw DecodeError("bad PNG colour type");
    }
    bool ok_depth = ctype == 0 ? (depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16)
                  : ctype == 3 ? (depth == 1 || depth == 2 || depth == 4 || depth == 8)
                               : (depth == 8 || depth == 16);
    if (!ok_depth) throw DecodeError("bad PNG bit depth");
    if (ctype == 3 && palette.size() < 3) throw DecodeError("PNG palette missing");

    int bits_pp = channels * depth;
    int bpp = bits_pp >= 8 ? bits_pp / 8 : 1;

    struct Pass { int x0, y0, dx, dy; };
    static const Pass adam7[7] = {{0, 0, 8, 8}, {4, 0, 8, 8}, {0, 4, 4, 8}, {2, 0, 4, 4}, {0, 2, 2, 4}, {1, 0, 2, 2}, {0, 1, 1, 2}};
    static const Pass single = {0, 0, 1, 1};
    int npass = interlace ? 7 : 1;

    size_t expected = 0;
    for (int p = 0; p < npass; p++) {
        const Pass &ps = interlace ? adam7[p] : single;
        uint64_t pw = (w - ps.x0 + ps.dx - 1) / ps.dx, ph = (h - ps.y0 + ps.dy - 1) / ps.dy;
        if (w <= (uint32_t)ps.x0 || h <= (uint32_t)ps.y0) continue;
        expected += ph * (1 + (pw * bits_pp + 7) / 8);
    }
    std::vector<uint8_t> raw;
    raw.reserve(expected);
    std::string err;
    if (!deflate::zlib(idat.data(), idat.size(), raw, expected, err)) throw DecodeError("PNG data: " + err);
    if (raw.size() < expected) throw DecodeError("PNG data is truncated");

    img.w = (int)w;
    img.h = (int)h;
    img.rgba.assign((size_t)w * h * 4, 0);

    uint32_t max_val = (1u << depth) - 1;
    auto sample = [&](const uint8_t *row, uint64_t idx) -> uint32_t {  // idx = sample index in row
        if (depth == 8) return row[idx];
        if (depth == 16) return (uint32_t)row[idx * 2] << 8 | row[idx * 2 + 1];
        uint64_t bit = idx * depth;
        return (row[bit / 8] >> (8 - depth - bit % 8)) & max_val;
    };
    auto to8 = [&](uint32_t v) -> uint8_t { return depth == 16 ? (uint8_t)(v >> 8) : depth == 8 ? (uint8_t)v : (uint8_t)(v * 255 / max_val); };

    size_t off = 0;
    std::vector<uint8_t> prev, cur;
    for (int p = 0; p < npass; p++) {
        const Pass &ps = interlace ? adam7[p] : single;
        if (w <= (uint32_t)ps.x0 || h <= (uint32_t)ps.y0) continue;
        uint64_t pw = (w - ps.x0 + ps.dx - 1) / ps.dx, ph = (h - ps.y0 + ps.dy - 1) / ps.dy;
        size_t stride = (size_t)((pw * bits_pp + 7) / 8);
        prev.assign(stride, 0);
        cur.resize(stride);
        for (uint64_t y = 0; y < ph; y++) {
            int filter = raw[off++];
            const uint8_t *src = raw.data() + off;
            off += stride;
            for (size_t i = 0; i < stride; i++) {
                int a = i >= (size_t)bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= (size_t)bpp ? prev[i - bpp] : 0;
                int x = src[i];
                switch (filter) {
                case 0: break;
                case 1: x += a; break;
                case 2: x += b; break;
                case 3: x += (a + b) / 2; break;
                case 4: {
                    int pp = a + b - c, pa = std::abs(pp - a), pb = std::abs(pp - b), pc = std::abs(pp - c);
                    x += (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
                    break;
                }
                default: throw DecodeError("bad PNG filter");
                }
                cur[i] = (uint8_t)x;
            }
            uint64_t oy = ps.y0 + y * ps.dy;
            for (uint64_t x = 0; x < pw; x++) {
                uint64_t ox = ps.x0 + x * ps.dx;
                uint8_t *o = img.rgba.data() + (oy * w + ox) * 4;
                uint8_t r, g, b, a = 255;
                switch (ctype) {
                case 0: {
                    uint32_t v = sample(cur.data(), x);
                    r = g = b = to8(v);
                    if (trns.size() >= 2 && v == be16(trns.data())) a = 0;
                    break;
                }
                case 2: {
                    uint32_t vr = sample(cur.data(), x * 3), vg = sample(cur.data(), x * 3 + 1), vb = sample(cur.data(), x * 3 + 2);
                    r = to8(vr); g = to8(vg); b = to8(vb);
                    if (trns.size() >= 6 && vr == be16(trns.data()) && vg == be16(trns.data() + 2) && vb == be16(trns.data() + 4)) a = 0;
                    break;
                }
                case 3: {
                    uint32_t v = sample(cur.data(), x);
                    if (v * 3 + 2 < palette.size()) { r = palette[v * 3]; g = palette[v * 3 + 1]; b = palette[v * 3 + 2]; }
                    else r = g = b = 0;
                    if (v < trns.size()) a = trns[v];
                    break;
                }
                case 4:
                    r = g = b = to8(sample(cur.data(), x * 2));
                    a = to8(sample(cur.data(), x * 2 + 1));
                    break;
                default:
                    r = to8(sample(cur.data(), x * 4)); g = to8(sample(cur.data(), x * 4 + 1));
                    b = to8(sample(cur.data(), x * 4 + 2)); a = to8(sample(cur.data(), x * 4 + 3));
                }
                o[0] = r; o[1] = g; o[2] = b; o[3] = a;
            }
            prev.swap(cur);
            cur.resize(stride);
        }
    }
}

// =====================================================================================
// JPEG (baseline and progressive, Huffman coded, 1 or 3 components)
// =====================================================================================

const uint8_t kZigzag[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48,
                             41, 34, 27, 20, 13, 6, 7, 14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                             30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

struct JHuff {
    bool present = false;
    uint8_t vals[256];
    int32_t maxcode[18];  // exclusive upper bound of codes of each length
    int32_t delta[17];
    uint16_t fast[512];   // (length << 8) | value for codes up to 9 bits, 0 = slow path

    void build(const uint8_t counts[16], const uint8_t *v, int nv) {
        std::memcpy(vals, v, nv);
        std::memset(fast, 0, sizeof fast);
        int code = 0, k = 0;
        for (int l = 1; l <= 16; l++) {
            delta[l] = k - code;
            for (int i = 0; i < counts[l - 1]; i++, code++, k++) {
                if (l <= 9) {
                    int lo = code << (9 - l), hi = (code + 1) << (9 - l);
                    for (int j = lo; j < hi && j < 512; j++) fast[j] = (uint16_t)(l << 8 | vals[k]);
                }
            }
            maxcode[l] = code;
            if (code > (1 << l)) throw DecodeError("bad JPEG Huffman table");
            code <<= 1;
        }
        maxcode[17] = 0x7FFFFFFF;
        present = true;
    }
};

struct JComp {
    int id = 0, hs = 1, vs = 1, tq = 0;
    int bw = 0, bh = 0;       // blocks per line / column (padded to whole MCUs)
    int td = 0, ta = 0;       // Huffman tables for the current scan
    int dc_pred = 0;
    std::vector<int16_t> coef;  // bw*bh*64, natural order
    std::vector<uint8_t> plane; // samples after IDCT (full resolution after upsampling)
    int stride = 0;
};

struct Jpeg {
    const uint8_t *d, *end, *p;
    uint16_t qt[4][64];  // natural order
    JHuff dc[4], ac[4];
    std::vector<JComp> comps;
    int w = 0, h = 0, hmax = 1, vmax = 1, mcux = 0, mcuy = 0;
    bool progressive = false, frame_seen = false, adobe_rgb = false;
    int restart = 0, orientation = 1;

    // bit reader for entropy-coded data
    uint32_t buf = 0;
    int cnt = 0;
    bool marker_hit = false;
    int eobrun = 0;

    void fill() {
        while (cnt <= 24) {
            uint32_t b = 0;
            if (!marker_hit && p < end) {
                b = *p;
                if (b == 0xFF) {
                    uint8_t nx = p + 1 < end ? p[1] : 0xD9;
                    if (nx == 0x00) p += 2;
                    else { marker_hit = true; b = 0; }
                } else {
                    p++;
                }
            }
            buf |= b << (24 - cnt);
            cnt += 8;
        }
    }
    int bits(int n) {
        if (n == 0) return 0;
        fill();
        int v = (int)(buf >> (32 - n));
        buf <<= n;
        cnt -= n;
        return v;
    }
    int bit() { return bits(1); }
    int decode(const JHuff &t) {
        if (!t.present) throw DecodeError("JPEG uses a missing Huffman table");
        fill();
        uint16_t e = t.fast[buf >> 23];
        if (e) {
            int l = e >> 8;
            buf <<= l;
            cnt -= l;
            return e & 255;
        }
        for (int l = 10; l <= 16; l++) {
            int32_t code = (int32_t)(buf >> (32 - l));
            if (code < t.maxcode[l]) {
                buf <<= l;
                cnt -= l;
                return t.vals[code + t.delta[l]];
            }
        }
        throw DecodeError("bad JPEG Huffman code");
    }
    static int extend(int v, int s) { return s == 0 ? 0 : v < (1 << (s - 1)) ? v - (1 << s) + 1 : v; }

    void reset_bits() {
        buf = 0;
        cnt = 0;
        marker_hit = false;
        eobrun = 0;
        for (auto &c : comps) c.dc_pred = 0;
    }
    // Moves to the data after the next RSTn marker. Stops (without consuming) at any other
    // marker, so a missing RST can never swallow the rest of the file.
    void skip_restart_marker() {
        while (p + 1 < end && !(p[0] == 0xFF && p[1] != 0 && p[1] != 0xFF)) p++;
        if (p + 1 < end && p[1] >= 0xD0 && p[1] <= 0xD7) p += 2;
        reset_bits();
    }

    // ---- block decoders ----
    void block_baseline(JComp &c, int16_t *blk) {
        int t = decode(dc[c.td]);
        if (t > 16) throw DecodeError("bad JPEG DC coefficient");
        c.dc_pred += extend(bits(t), t);
        blk[0] = (int16_t)c.dc_pred;
        for (int k = 1; k < 64;) {
            int rs = decode(ac[c.ta]), r = rs >> 4, s = rs & 15;
            if (s == 0) {
                if (r != 15) break;
                k += 16;
                continue;
            }
            k += r;
            if (k > 63) throw DecodeError("bad JPEG AC coefficient");
            blk[kZigzag[k++]] = (int16_t)extend(bits(s), s);
        }
    }
    void block_dc(JComp &c, int16_t *blk, int ah, int al) {
        if (ah == 0) {
            int t = decode(dc[c.td]);
            if (t > 16) throw DecodeError("bad JPEG DC coefficient");
            c.dc_pred += extend(bits(t), t);
            blk[0] = (int16_t)(c.dc_pred * (1 << al));
        } else if (bit()) {
            blk[0] = (int16_t)(blk[0] | (1 << al));
        }
    }
    void block_ac_first(JComp &c, int16_t *blk, int ss, int se, int al) {
        if (eobrun) { eobrun--; return; }
        for (int k = ss; k <= se;) {
            int rs = decode(ac[c.ta]), r = rs >> 4, s = rs & 15;
            if (s == 0) {
                if (r < 15) {
                    eobrun = (1 << r) - 1;
                    if (r) eobrun += bits(r);
                    break;
                }
                k += 16;
                continue;
            }
            k += r;
            if (k > 63) throw DecodeError("bad JPEG AC coefficient");
            blk[kZigzag[k++]] = (int16_t)(extend(bits(s), s) * (1 << al));
        }
    }
    void block_ac_refine(JComp &c, int16_t *blk, int ss, int se, int al) {
        int16_t pbit = (int16_t)(1 << al);
        auto refine = [&](int16_t *q) {
            if (bit() && (*q & pbit) == 0) *q = (int16_t)(*q > 0 ? *q + pbit : *q - pbit);
        };
        if (eobrun) {
            eobrun--;
            for (int k = ss; k <= se; k++)
                if (blk[kZigzag[k]]) refine(&blk[kZigzag[k]]);
            return;
        }
        int k = ss;
        while (k <= se) {
            int rs = decode(ac[c.ta]), r = rs >> 4, s = rs & 15, val = 0;
            if (s == 0) {
                if (r < 15) {
                    eobrun = (1 << r) - 1;
                    if (r) eobrun += bits(r);
                    r = 64;  // refine the rest of this block, then stop
                }
            } else {
                val = bit() ? pbit : -pbit;
            }
            while (k <= se) {
                int16_t *q = &blk[kZigzag[k++]];
                if (*q) {
                    refine(q);
                } else {
                    if (r == 0) {
                        if (val) *q = (int16_t)val;
                        break;
                    }
                    r--;
                }
            }
        }
    }

    void scan(const uint8_t *hdr, int len) {
        int ns = hdr[0];
        if (ns < 1 || ns > 4 || len < 4 + 2 * ns) throw DecodeError("bad JPEG scan header");
        std::vector<JComp *> sc;
        for (int i = 0; i < ns; i++) {
            int id = hdr[1 + 2 * i], tables = hdr[2 + 2 * i];
            JComp *c = nullptr;
            for (auto &cc : comps) if (cc.id == id) c = &cc;
            if (!c) throw DecodeError("JPEG scan uses an unknown component");
            c->td = (tables >> 4) & 3;
            c->ta = tables & 3;
            sc.push_back(c);
        }
        const uint8_t *q = hdr + 1 + 2 * ns;
        int ss = q[0], se = q[1], ah = q[2] >> 4, al = q[2] & 15;
        if (!progressive) { ss = 0; se = 63; ah = al = 0; }
        if (ss > 63 || se > 63 || ss > se) throw DecodeError("bad JPEG spectral selection");

        reset_bits();
        auto do_block = [&](JComp &c, int bx, int by) {
            int16_t *blk = c.coef.data() + ((size_t)by * c.bw + bx) * 64;
            if (!progressive) block_baseline(c, blk);
            else if (ss == 0) block_dc(c, blk, ah, al);
            else if (ah == 0) block_ac_first(c, blk, ss, se, al);
            else block_ac_refine(c, blk, ss, se, al);
        };
        int mcu_count = 0, total_mcus = 0;
        if (ns == 1) {
            JComp &c = *sc[0];
            int cw = (int)((w * (int64_t)c.hs + hmax - 1) / hmax), ch = (int)((h * (int64_t)c.vs + vmax - 1) / vmax);
            total_mcus = ((cw + 7) / 8) * ((ch + 7) / 8);
        } else {
            total_mcus = mcux * mcuy;
        }
        auto after_mcu = [&]() {  // no RST follows the last MCU of a scan
            if (restart && ++mcu_count % restart == 0 && mcu_count < total_mcus) skip_restart_marker();
        };
        if (ns == 1) {
            JComp &c = *sc[0];
            int cw = (int)((w * (int64_t)c.hs + hmax - 1) / hmax), ch = (int)((h * (int64_t)c.vs + vmax - 1) / vmax);
            int nbx = (cw + 7) / 8, nby = (ch + 7) / 8;
            for (int by = 0; by < nby; by++)
                for (int bx = 0; bx < nbx; bx++) {
                    do_block(c, bx, by);
                    after_mcu();
                }
        } else {
            for (int my = 0; my < mcuy; my++)
                for (int mx = 0; mx < mcux; mx++) {
                    for (JComp *c : sc)
                        for (int v = 0; v < c->vs; v++)
                            for (int u = 0; u < c->hs; u++) do_block(*c, mx * c->hs + u, my * c->vs + v);
                    after_mcu();
                }
        }
        // continue at the next marker
        while (p + 1 < end && !(p[0] == 0xFF && p[1] != 0 && !(p[1] >= 0xD0 && p[1] <= 0xD7))) p++;
    }

    static void idct(const int16_t *in, const uint16_t *qt, uint8_t *out, int stride) {
        static float cosv[8][8];
        static bool ready = false;
        if (!ready) {
            for (int x = 0; x < 8; x++)
                for (int u = 0; u < 8; u++)
                    cosv[x][u] = (float)((u == 0 ? std::sqrt(0.125) : 0.5) * std::cos((2 * x + 1) * u * 3.14159265358979323846 / 16));
            ready = true;
        }
        float tmp[64], deq[64];
        for (int i = 0; i < 64; i++) deq[i] = (float)in[i] * qt[i];
        for (int y = 0; y < 8; y++)  // rows: tmp[v][x] = sum_u C(x,u) F(v,u)
            for (int x = 0; x < 8; x++) {
                float s = 0;
                for (int u = 0; u < 8; u++) s += cosv[x][u] * deq[y * 8 + u];
                tmp[y * 8 + x] = s;
            }
        for (int x = 0; x < 8; x++)
            for (int y = 0; y < 8; y++) {
                float s = 0;
                for (int v = 0; v < 8; v++) s += cosv[y][v] * tmp[v * 8 + x];
                int iv = (int)std::lround(s + 128.0f);
                out[y * stride + x] = (uint8_t)(iv < 0 ? 0 : iv > 255 ? 255 : iv);
            }
    }

    // Subsampled component -> full resolution with a triangle filter (sample centres
    // interpolated bilinearly), the same idea as libjpeg's "fancy upsampling".
    void upsample(JComp &c) {
        int cw = (int)((w * (int64_t)c.hs + hmax - 1) / hmax), ch = (int)((h * (int64_t)c.vs + vmax - 1) / vmax);
        auto taps = [](int n, int valid, float scale, std::vector<int> &i0, std::vector<int> &i1, std::vector<float> &f) {
            i0.resize(n); i1.resize(n); f.resize(n);
            for (int k = 0; k < n; k++) {
                float s = (k + 0.5f) * scale - 0.5f;
                int i = (int)std::floor(s);
                f[k] = s - (float)i;
                i0[k] = std::clamp(i, 0, valid - 1);
                i1[k] = std::clamp(i + 1, 0, valid - 1);
            }
        };
        std::vector<int> x0, x1, y0, y1;
        std::vector<float> fx, fy;
        taps(w, cw, (float)c.hs / hmax, x0, x1, fx);
        taps(h, ch, (float)c.vs / vmax, y0, y1, fy);
        std::vector<uint8_t> full((size_t)w * h);
        for (int y = 0; y < h; y++) {
            const uint8_t *r0 = c.plane.data() + (size_t)y0[y] * c.stride, *r1 = c.plane.data() + (size_t)y1[y] * c.stride;
            for (int x = 0; x < w; x++) {
                float top = r0[x0[x]] + (r0[x1[x]] - r0[x0[x]]) * fx[x];
                float bot = r1[x0[x]] + (r1[x1[x]] - r1[x0[x]]) * fx[x];
                full[(size_t)y * w + x] = (uint8_t)std::lround(top + (bot - top) * fy[y]);
            }
        }
        c.plane.swap(full);
        c.stride = w;
    }

    void read_exif(const uint8_t *s, size_t n) {
        if (n < 14 || std::memcmp(s, "Exif\0\0", 6) != 0) return;
        const uint8_t *t = s + 6;
        size_t tn = n - 6;
        bool le = t[0] == 'I';
        auto u16 = [&](size_t o) -> uint32_t { return o + 2 > tn ? 0 : le ? le16(t + o) : be16(t + o); };
        auto u32 = [&](size_t o) -> uint32_t { return o + 4 > tn ? 0 : le ? le32(t + o) : be32(t + o); };
        uint32_t ifd = u32(4);
        uint32_t cnt_ = u16(ifd);
        for (uint32_t i = 0; i < cnt_ && i < 256; i++) {
            size_t e = ifd + 2 + (size_t)i * 12;
            if (u16(e) == 0x0112) {
                uint32_t o = u16(e + 8);
                if (o >= 1 && o <= 8) orientation = (int)o;
            }
        }
    }

    void run(Image &img) {
        p = d + 2;
        std::memset(qt, 0, sizeof qt);
        for (;;) {
            while (p < end && *p != 0xFF) p++;  // tolerate junk between segments
            while (p < end && *p == 0xFF) p++;
            if (p >= end) throw DecodeError("JPEG ends early");
            uint8_t m = *p++;
            if (m == 0xD9) break;                       // EOI
            if (m >= 0xD0 && m <= 0xD7) continue;       // stray RST
            if (p + 2 > end) throw DecodeError("JPEG ends early");
            int len = be16(p);
            if (len < 2 || p + len > end) throw DecodeError("bad JPEG segment length");
            const uint8_t *s = p + 2;
            int sn = len - 2;
            p += len;
            switch (m) {
            case 0xDB: {  // DQT
                for (int o = 0; o < sn;) {
                    int pq = s[o] >> 4, tq = s[o] & 3;
                    o++;
                    if (o + 64 * (pq + 1) > sn) throw DecodeError("bad JPEG quantization table");
                    for (int k = 0; k < 64; k++) qt[tq][kZigzag[k]] = pq ? be16(s + o + 2 * k) : s[o + k];
                    o += 64 * (pq + 1);
                }
                break;
            }
            case 0xC4: {  // DHT
                for (int o = 0; o < sn;) {
                    if (o + 17 > sn) throw DecodeError("bad JPEG Huffman table");
                    int tc = s[o] >> 4, th = s[o] & 3;
                    const uint8_t *counts = s + o + 1;
                    int total = 0;
                    for (int i = 0; i < 16; i++) total += counts[i];
                    if (total > 256 || o + 17 + total > sn) throw DecodeError("bad JPEG Huffman table");
                    (tc ? ac[th] : dc[th]).build(counts, s + o + 17, total);
                    o += 17 + total;
                }
                break;
            }
            case 0xC0: case 0xC1: case 0xC2: {  // SOF baseline / extended / progressive
                if (frame_seen) throw DecodeError("JPEG has two frames");
                frame_seen = true;
                progressive = m == 0xC2;
                if (sn < 6 || s[0] != 8) throw DecodeError("only 8-bit JPEGs are supported");
                h = be16(s + 1);
                w = be16(s + 3);
                int nc = s[5];
                if (h == 0) throw DecodeError("JPEG with DNL marker is not supported");
                check_size(w, h);
                if ((nc != 1 && nc != 3) || sn < 6 + 3 * nc) throw DecodeError("only grayscale and colour JPEGs are supported");
                for (int i = 0; i < nc; i++) {
                    JComp c;
                    c.id = s[6 + 3 * i];
                    c.hs = s[7 + 3 * i] >> 4;
                    c.vs = s[7 + 3 * i] & 15;
                    c.tq = s[8 + 3 * i] & 3;
                    if (c.hs < 1 || c.hs > 4 || c.vs < 1 || c.vs > 4) throw DecodeError("bad JPEG sampling factors");
                    comps.push_back(c);
                }
                for (auto &c : comps) { hmax = std::max(hmax, c.hs); vmax = std::max(vmax, c.vs); }
                mcux = (w + 8 * hmax - 1) / (8 * hmax);
                mcuy = (h + 8 * vmax - 1) / (8 * vmax);
                for (auto &c : comps) {
                    c.bw = mcux * c.hs;
                    c.bh = mcuy * c.vs;
                    c.coef.assign((size_t)c.bw * c.bh * 64, 0);
                }
                break;
            }
            case 0xC3: case 0xC5: case 0xC6: case 0xC7: case 0xC9: case 0xCA: case 0xCB:
            case 0xCD: case 0xCE: case 0xCF:
                throw DecodeError("this JPEG coding (lossless/arithmetic) is not supported");
            case 0xDD:
                if (sn >= 2) restart = be16(s);
                break;
            case 0xDA:
                if (!frame_seen) throw DecodeError("JPEG scan before frame header");
                scan(s, sn);
                break;
            case 0xE1: read_exif(s, sn); break;
            case 0xEE:
                if (sn >= 12 && std::memcmp(s, "Adobe", 5) == 0 && s[11] == 0 && comps.size() == 3) adobe_rgb = true;
                break;
            default: break;  // APPn, COM, ...
            }
        }
        if (!frame_seen) throw DecodeError("JPEG has no image");

        for (auto &c : comps) {
            int pw = c.bw * 8;
            c.plane.assign((size_t)pw * c.bh * 8, 0);
            for (int by = 0; by < c.bh; by++)
                for (int bx = 0; bx < c.bw; bx++)
                    idct(c.coef.data() + ((size_t)by * c.bw + bx) * 64, qt[c.tq], c.plane.data() + (size_t)by * 8 * pw + bx * 8, pw);
            c.coef.clear();
            c.coef.shrink_to_fit();
            c.stride = pw;
            if (c.hs != hmax || c.vs != vmax) upsample(c);
        }

        // colour conversion + EXIF orientation
        bool swap = orientation >= 5;
        img.w = swap ? h : w;
        img.h = swap ? w : h;
        img.rgba.assign((size_t)w * h * 4, 255);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int v[3];
                for (size_t i = 0; i < comps.size(); i++) {
                    const JComp &c = comps[i];
                    v[i] = c.plane[(size_t)y * c.stride + x];
                }
                int r, g, b;
                if (comps.size() == 1) r = g = b = v[0];
                else if (adobe_rgb) { r = v[0]; g = v[1]; b = v[2]; }
                else {
                    float Y = (float)v[0], cb = v[1] - 128.0f, cr = v[2] - 128.0f;
                    r = (int)std::lround(Y + 1.402f * cr);
                    g = (int)std::lround(Y - 0.344136f * cb - 0.714136f * cr);
                    b = (int)std::lround(Y + 1.772f * cb);
                }
                int ox = x, oy = y;
                switch (orientation) {
                case 2: ox = w - 1 - x; break;
                case 3: ox = w - 1 - x; oy = h - 1 - y; break;
                case 4: oy = h - 1 - y; break;
                case 5: ox = y; oy = x; break;
                case 6: ox = h - 1 - y; oy = x; break;
                case 7: ox = h - 1 - y; oy = w - 1 - x; break;
                case 8: ox = y; oy = w - 1 - x; break;
                }
                uint8_t *o = img.rgba.data() + ((size_t)oy * img.w + ox) * 4;
                o[0] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
                o[1] = (uint8_t)(g < 0 ? 0 : g > 255 ? 255 : g);
                o[2] = (uint8_t)(b < 0 ? 0 : b > 255 ? 255 : b);
            }
    }
};

// =====================================================================================
// GIF (first frame, with transparency and interlacing)
// =====================================================================================

void decode_gif(const uint8_t *d, size_t n, Image &img) {
    if (n < 13) throw DecodeError("GIF is truncated");
    int sw = le16(d + 6), sh = le16(d + 8);
    check_size((uint64_t)sw, (uint64_t)sh);
    uint8_t packed = d[10];
    size_t pos = 13;
    const uint8_t *global = nullptr;
    int global_n = 0;
    if (packed & 0x80) {
        global_n = 2 << (packed & 7);
        if (pos + 3 * (size_t)global_n > n) throw DecodeError("GIF colour table is truncated");
        global = d + pos;
        pos += 3 * (size_t)global_n;
    }
    img.w = sw;
    img.h = sh;
    img.rgba.assign((size_t)sw * sh * 4, 0);  // transparent until drawn
    int transparent = -1;
    auto skip_blocks = [&] {
        while (pos < n) {
            uint8_t len = d[pos++];
            if (!len) return;
            pos += len;
        }
    };
    while (pos < n) {
        uint8_t b = d[pos++];
        if (b == 0x3B) break;  // trailer
        if (b == 0x21) {       // extension
            if (pos >= n) break;
            uint8_t label = d[pos++];
            if (label == 0xF9 && pos + 5 <= n && d[pos] >= 4) {
                if (d[pos + 1] & 1) transparent = d[pos + 4];
            }
            skip_blocks();
            continue;
        }
        if (b != 0x2C) throw DecodeError("bad GIF block");
        if (pos + 9 > n) throw DecodeError("GIF is truncated");
        int fx = le16(d + pos), fy = le16(d + pos + 2), fw = le16(d + pos + 4), fh = le16(d + pos + 6);
        uint8_t ip = d[pos + 8];
        pos += 9;
        const uint8_t *table = global;
        int table_n = global_n;
        if (ip & 0x80) {
            table_n = 2 << (ip & 7);
            if (pos + 3 * (size_t)table_n > n) throw DecodeError("GIF colour table is truncated");
            table = d + pos;
            pos += 3 * (size_t)table_n;
        }
        if (!table) throw DecodeError("GIF has no colour table");
        bool interlaced = ip & 0x40;
        if (pos >= n) throw DecodeError("GIF is truncated");
        int min_size = d[pos++];
        if (min_size < 2 || min_size > 11) throw DecodeError("bad GIF code size");
        std::vector<uint8_t> data;  // concatenated sub-blocks
        while (pos < n) {
            uint8_t len = d[pos++];
            if (!len) break;
            if (pos + len > n) len = (uint8_t)(n - pos);
            data.insert(data.end(), d + pos, d + pos + len);
            pos += len;
        }
        // LZW
        std::vector<uint8_t> pixels;
        pixels.reserve((size_t)fw * fh);
        uint16_t prefix[4096];
        uint8_t suffix[4096], first[4096];
        uint8_t stack[4097];
        int clear = 1 << min_size, end_code = clear + 1;
        int code_size = min_size + 1, next = clear + 2, prev = -1;
        for (int i = 0; i < clear; i++) { prefix[i] = 0xFFFF; suffix[i] = (uint8_t)i; first[i] = (uint8_t)i; }
        uint32_t bitbuf = 0;
        int bits = 0;
        size_t dp = 0;
        size_t want = (size_t)fw * fh;
        while (pixels.size() < want) {
            while (bits < code_size && dp < data.size()) { bitbuf |= (uint32_t)data[dp++] << bits; bits += 8; }
            if (bits < code_size) break;
            int code = (int)(bitbuf & ((1u << code_size) - 1));
            bitbuf >>= code_size;
            bits -= code_size;
            if (code == clear) { code_size = min_size + 1; next = clear + 2; prev = -1; continue; }
            if (code == end_code) break;
            int sp = 0, c = code;
            if (prev < 0) {
                if (code >= clear) throw DecodeError("bad GIF data");
                pixels.push_back((uint8_t)code);
                prev = code;
                continue;
            }
            uint8_t head;
            if (code < next) {
                head = first[code];
            } else if (code == next) {
                head = first[prev];
                stack[sp++] = head;  // KwKwK case: the new entry ends with its own first byte
                c = prev;
            } else {
                throw DecodeError("bad GIF data");
            }
            while (c != 0xFFFF && c >= 0 && sp < 4097) {
                stack[sp++] = suffix[c];
                c = c < clear ? 0xFFFF : prefix[c];
            }
            while (sp) pixels.push_back(stack[--sp]);
            if (next < 4096) {
                prefix[next] = (uint16_t)prev;
                suffix[next] = head;
                first[next] = first[prev];
                next++;
                if (next == (1 << code_size) && code_size < 12) code_size++;
            }
            prev = code;
        }
        // place the frame on the canvas
        static const int start[4] = {0, 4, 2, 1}, step[4] = {8, 8, 4, 2};
        size_t k = 0;
        for (int pass = 0; pass < (interlaced ? 4 : 1); pass++) {
            for (int row = interlaced ? start[pass] : 0; row < fh; row += interlaced ? step[pass] : 1) {
                for (int col = 0; col < fw; col++, k++) {
                    if (k >= pixels.size()) break;
                    int x = fx + col, y = fy + row, v = pixels[k];
                    if (x >= sw || y >= sh || v == transparent || v >= table_n) continue;
                    uint8_t *o = img.rgba.data() + ((size_t)y * sw + x) * 4;
                    o[0] = table[v * 3]; o[1] = table[v * 3 + 1]; o[2] = table[v * 3 + 2]; o[3] = 255;
                }
            }
        }
        return;  // first frame only
    }
    throw DecodeError("GIF has no image");
}

// =====================================================================================
// BMP (uncompressed 8/24/32-bit)
// =====================================================================================

void decode_bmp(const uint8_t *d, size_t n, Image &img) {
    if (n < 54) throw DecodeError("BMP is truncated");
    uint32_t off = le32(d + 10), hsize = le32(d + 14);
    if (hsize < 40) throw DecodeError("unsupported BMP header");
    int32_t w = (int32_t)le32(d + 18), hh = (int32_t)le32(d + 22);
    int bpp = le16(d + 28);
    uint32_t comp = le32(d + 30), colors = le32(d + 46);
    bool top_down = hh < 0;
    int64_t h = top_down ? -(int64_t)hh : hh;
    if (w <= 0) throw DecodeError("bad BMP width");
    check_size((uint64_t)w, (uint64_t)h);
    if (!(comp == 0 || (comp == 3 && bpp == 32))) throw DecodeError("compressed BMPs are not supported");
    if (bpp != 8 && bpp != 24 && bpp != 32) throw DecodeError("unsupported BMP bit depth");
    size_t stride = ((size_t)w * bpp / 8 + 3) & ~(size_t)3;
    if ((uint64_t)off + stride * h > n) throw DecodeError("BMP is truncated");
    const uint8_t *pal = d + 14 + hsize;
    if (bpp == 8) {
        if (colors == 0) colors = 256;
        if (14 + hsize + colors * 4ull > n) throw DecodeError("BMP palette is truncated");
    }
    img.w = w;
    img.h = (int)h;
    img.rgba.assign((size_t)w * h * 4, 255);
    bool any_alpha = false;
    for (int64_t y = 0; y < h; y++) {
        const uint8_t *row = d + off + stride * (top_down ? y : h - 1 - y);
        for (int x = 0; x < w; x++) {
            uint8_t *o = img.rgba.data() + ((size_t)y * w + x) * 4;
            if (bpp == 8) {
                uint32_t i = row[x];
                if (i < colors) { o[0] = pal[i * 4 + 2]; o[1] = pal[i * 4 + 1]; o[2] = pal[i * 4]; }
            } else {
                const uint8_t *s = row + (size_t)x * (bpp / 8);
                o[0] = s[2]; o[1] = s[1]; o[2] = s[0];
                if (bpp == 32) { o[3] = s[3]; any_alpha |= s[3] != 0; }
            }
        }
    }
    if (bpp == 32 && !any_alpha)
        for (size_t i = 3; i < img.rgba.size(); i += 4) img.rgba[i] = 255;
}

}  // namespace

bool is_webp(const uint8_t *d, size_t n);  // webp.cpp
void decode_webp(const uint8_t *d, size_t n, Image &img);
bool is_svg(const uint8_t *d, size_t n);   // svg.cpp
void decode_svg(const uint8_t *d, size_t n, Image &img);

bool decode(const uint8_t *data, size_t size, Image &out, std::string &err) {
    try {
        if (is_png(data, size)) decode_png(data, size, out);
        else if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
            Jpeg j;
            j.d = data;
            j.end = data + size;
            j.run(out);
        } else if (size >= 6 && std::memcmp(data, "GIF8", 4) == 0) decode_gif(data, size, out);
        else if (is_webp(data, size)) decode_webp(data, size, out);
        else if (is_svg(data, size)) decode_svg(data, size, out);
        else if (size >= 2 && data[0] == 'B' && data[1] == 'M') decode_bmp(data, size, out);
        else throw DecodeError("not a PNG, JPEG, GIF, WebP, SVG or BMP image");
        return true;
    } catch (const std::bad_alloc &) {
        err = "out of memory";
    } catch (const std::exception &e) {
        err = e.what();
    }
    out = Image();
    return false;
}

}  // namespace image
