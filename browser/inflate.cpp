// inflate.cpp — DEFLATE decoder (RFC 1951) with zlib (RFC 1950) and gzip (RFC 1952) wrappers.
#include "inflate.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace deflate {

namespace {

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Truncated {};  // streaming: the input ran out; try again with more

// =====================================================================================
// Inflate (RFC 1951)
// =====================================================================================

struct Huffman {
    uint16_t count[16];
    uint16_t symbol[320];
    uint16_t fast[512];  // (symbol << 4) | length for codes of up to 9 bits; 0 = use slow path
};

struct Inflater {
    const uint8_t *p, *end;
    uint32_t bitbuf = 0;
    int bitcnt = 0, overrun = 0;
    bool more_coming = false;  // streaming: running out of input means "wait", not "broken"
    std::vector<uint8_t> &out;
    size_t limit;

    Inflater(const uint8_t *d, size_t n, std::vector<uint8_t> &o, size_t lim) : p(d), end(d + n), out(o), limit(lim) {}

    void need(int n) {
        while (bitcnt < n) {
            uint32_t b = 0;
            if (p < end) b = *p++;
            else if (more_coming) throw Truncated();
            else if (++overrun > 8) throw Error("compressed data is truncated");
            bitbuf |= b << bitcnt;
            bitcnt += 8;
        }
    }
    uint32_t bits(int n) {
        if (n == 0) return 0;
        need(n);
        uint32_t v = bitbuf & ((1u << n) - 1);
        bitbuf >>= n;
        bitcnt -= n;
        return v;
    }

    static void build(Huffman &h, const uint8_t *lengths, int n) {
        std::memset(h.count, 0, sizeof h.count);
        std::memset(h.fast, 0, sizeof h.fast);
        for (int i = 0; i < n; i++) h.count[lengths[i]]++;
        h.count[0] = 0;
        int left = 1;
        for (int len = 1; len < 16; len++) {
            left <<= 1;
            left -= h.count[len];
            if (left < 0) throw Error("bad Huffman code");
        }
        uint16_t offs[16];
        offs[1] = 0;
        for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h.count[len];
        for (int s = 0; s < n; s++)
            if (lengths[s]) h.symbol[offs[lengths[s]]++] = (uint16_t)s;
        int code = 0, idx = 0;
        for (int len = 1; len < 16; len++) {
            for (int k = 0; k < h.count[len]; k++, code++) {
                int sym = h.symbol[idx++];
                if (len > 9) continue;
                int rev = 0;
                for (int b = 0; b < len; b++) rev |= ((code >> b) & 1) << (len - 1 - b);
                for (int r = rev; r < 512; r += 1 << len) h.fast[r] = (uint16_t)(sym << 4 | len);
            }
            code <<= 1;
        }
    }

    int decode(const Huffman &h) {
        need(9);
        uint16_t e = h.fast[bitbuf & 511];
        if (e) {
            bitbuf >>= (e & 15);
            bitcnt -= (e & 15);
            return e >> 4;
        }
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; len++) {
            code |= (int)bits(1);
            int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        throw Error("bad compressed data");
    }

    void put(uint8_t b) {
        if (out.size() >= limit) throw Error("decompressed data is too large");
        out.push_back(b);
    }

    void codes(const Huffman &lit, const Huffman &dist) {
        static const uint16_t lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const uint8_t lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const uint16_t dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const uint8_t dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int sym = decode(lit);
            if (sym < 256) { put((uint8_t)sym); continue; }
            if (sym == 256) return;
            sym -= 257;
            if (sym >= 29) throw Error("bad length code");
            size_t len = lbase[sym] + bits(lext[sym]);
            int ds = decode(dist);
            if (ds >= 30) throw Error("bad distance code");
            size_t d = dbase[ds] + bits(dext[ds]);
            if (d > out.size()) throw Error("distance too far back");
            if (out.size() + len > limit) throw Error("decompressed data is too large");
            size_t from = out.size() - d;
            for (size_t k = 0; k < len; k++) out.push_back(out[from + k]);
        }
    }

    struct Fixed {
        Huffman lit, dist;
        Fixed() {
            uint8_t l[288];
            for (int i = 0; i < 144; i++) l[i] = 8;
            for (int i = 144; i < 256; i++) l[i] = 9;
            for (int i = 256; i < 280; i++) l[i] = 7;
            for (int i = 280; i < 288; i++) l[i] = 8;
            build(lit, l, 288);
            uint8_t d[30];
            for (int i = 0; i < 30; i++) d[i] = 5;
            build(dist, d, 30);
        }
    };

    void run() {
        while (!block()) {}
    }

    // Decodes one block; returns true if it was the last one.
    bool block() {
        static const Fixed fixed;  // thread-safe initialisation (fetches run on several threads)
        const Huffman &fixed_lit = fixed.lit, &fixed_dist = fixed.dist;
        int last;
        {
            last = (int)bits(1);
            int type = (int)bits(2);
            if (type == 0) {
                bits(bitcnt & 7);
                uint32_t len = bits(16), nlen = bits(16);
                if ((len ^ 0xFFFF) != nlen) throw Error("bad stored block");
                for (uint32_t i = 0; i < len; i++) put((uint8_t)bits(8));
            } else if (type == 1) {
                codes(fixed_lit, fixed_dist);
            } else if (type == 2) {
                int nlen = (int)bits(5) + 257, ndist = (int)bits(5) + 1, ncode = (int)bits(4) + 4;
                if (nlen > 286 || ndist > 30) throw Error("bad dynamic block");
                static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
                uint8_t lengths[320] = {0};
                for (int i = 0; i < ncode; i++) lengths[order[i]] = (uint8_t)bits(3);
                Huffman lencode, lit, dist;
                build(lencode, lengths, 19);
                int i = 0;
                while (i < nlen + ndist) {
                    int sym = decode(lencode);
                    if (sym < 16) { lengths[i++] = (uint8_t)sym; continue; }
                    int rep, val = 0;
                    if (sym == 16) {
                        if (i == 0) throw Error("bad repeat");
                        val = lengths[i - 1];
                        rep = 3 + (int)bits(2);
                    } else if (sym == 17) rep = 3 + (int)bits(3);
                    else rep = 11 + (int)bits(7);
                    if (i + rep > nlen + ndist) throw Error("bad repeat");
                    while (rep--) lengths[i++] = (uint8_t)val;
                }
                build(lit, lengths, nlen);
                build(dist, lengths + nlen, ndist);
                codes(lit, dist);
            } else {
                throw Error("bad block type");
            }
        }
        return last != 0;
    }
};

// Parses a gzip header; returns its size, 0 if more bytes are needed, -1 if it is bad.
long gzip_header(const uint8_t *data, size_t size) {
    if (size < 10) return 0;
    if (data[0] != 0x1F || data[1] != 0x8B || data[2] != 8) return -1;
    uint8_t flags = data[3];
    size_t p = 10;
    if (flags & 4) {  // FEXTRA
        if (p + 2 > size) return 0;
        p += 2 + (size_t)(data[p] | data[p + 1] << 8);
    }
    if (flags & 8) { while (p < size && data[p]) p++; p++; }   // FNAME
    if (flags & 16) { while (p < size && data[p]) p++; p++; }  // FCOMMENT
    if (flags & 2) p += 2;                                      // FHCRC
    if (p >= size) return 0;
    return (long)p;
}


bool run(const uint8_t *data, size_t size, std::vector<uint8_t> &out, size_t limit, std::string &err) {
    try {
        Inflater inf(data, size, out, limit);
        inf.run();
        return true;
    } catch (const std::exception &e) {
        err = e.what();
        return false;
    }
}

}  // namespace

bool raw(const uint8_t *data, size_t size, std::vector<uint8_t> &out, size_t limit, std::string &err) {
    return run(data, size, out, limit, err);
}

bool zlib(const uint8_t *data, size_t size, std::vector<uint8_t> &out, size_t limit, std::string &err) {
    if (size < 2 || (data[0] & 15) != 8 || ((data[0] << 8) | data[1]) % 31 != 0) { err = "bad zlib header"; return false; }
    if (data[1] & 0x20) { err = "zlib preset dictionaries are not supported"; return false; }
    return run(data + 2, size - 2, out, limit, err);
}

bool gzip(const uint8_t *data, size_t size, std::vector<uint8_t> &out, size_t limit, std::string &err) {
    long p = size < 18 ? -1 : gzip_header(data, size);
    if (p <= 0) { err = "bad gzip header"; return false; }
    return run(data + p, size - (size_t)p, out, limit, err);
}

// ---- streaming -------------------------------------------------------------------------
// The decoder above can't pause in the middle of a block, so when the input so far ends
// inside a block, that block is decoded again from its start once more input arrives.
// What it produced up to the cut is already right (the same bytes come out again), so
// take() hands it on straight away: some servers send a single block of 300 KB.

bool Stream::feed(const uint8_t *data, size_t size, bool final, std::string &err) {
    in_.insert(in_.end(), data, data + size);
    if (failed_) { err = error_; return false; }
    if (done_) return true;
    auto fail = [&](const std::string &e) {
        failed_ = true;
        error_ = err = e;
        return false;
    };
    if (!started_) {
        if (kind_ == Kind::Gzip) {
            long h = gzip_header(in_.data(), in_.size());
            if (h < 0) return fail("bad gzip header");
            if (h == 0) return final ? fail("bad gzip header") : true;
            pos_ = (size_t)h;
        } else if (kind_ == Kind::ZlibOrRaw) {
            if (in_.size() < 2) return final ? fail("compressed data is truncated") : true;
            bool zlib = (in_[0] & 15) == 8 && ((in_[0] << 8) | in_[1]) % 31 == 0;
            if (zlib && (in_[1] & 0x20)) return fail("zlib preset dictionaries are not supported");
            pos_ = zlib ? 2 : 0;  // servers disagree on whether "deflate" means zlib or raw
        }
        started_ = true;
    }
    for (;;) {
        out.resize(committed_);
        Inflater inf(in_.data() + pos_, in_.size() - pos_, out, limit_);
        inf.bitbuf = bitbuf_;
        inf.bitcnt = bitcnt_;
        inf.more_coming = !final;
        try {
            bool last = inf.block();
            pos_ = (size_t)(inf.p - in_.data());
            bitbuf_ = inf.bitbuf;
            bitcnt_ = inf.bitcnt;
            committed_ = out.size();
            if (last) { done_ = true; return true; }
        } catch (const Truncated &) {
            return true;  // keep the partial output: decoding this block again gives the same prefix
        } catch (const std::exception &e) {
            out.resize(std::max(committed_, emitted_));
            return fail(e.what());
        }
    }
}

size_t Stream::take(const uint8_t *&p) {
    if (out.size() <= emitted_) return 0;
    p = out.data() + emitted_;
    size_t n = out.size() - emitted_;
    emitted_ = out.size();
    return n;
}

}  // namespace deflate
