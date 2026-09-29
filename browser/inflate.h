// inflate.h — DEFLATE decompression, used by the PNG decoder and for gzip'd HTTP bodies.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace deflate {

// Each appends to `out` (at most `limit` bytes) and returns false with `err` set on failure.
bool raw(const uint8_t *data, size_t size, std::vector<uint8_t> &out, size_t limit, std::string &err);
bool zlib(const uint8_t *data, size_t size, std::vector<uint8_t> &out, size_t limit, std::string &err);
bool gzip(const uint8_t *data, size_t size, std::vector<uint8_t> &out, size_t limit, std::string &err);

// Incremental decoding, for bodies that are shown while they download.
class Stream {
public:
    enum class Kind { Gzip, ZlibOrRaw };
    Stream(Kind kind, size_t limit) : kind_(kind), limit_(limit) {}
    // Adds compressed bytes and decodes what it can. `final` = no more input will come.
    // False on bad data.
    bool feed(const uint8_t *data, size_t size, bool final, std::string &err);
    // The output decoded since the last take(); returns its length.
    size_t take(const uint8_t *&p);
    bool done() const { return done_; }
    std::vector<uint8_t> out;  // everything decoded so far

private:
    Kind kind_;
    size_t limit_;
    std::vector<uint8_t> in_;
    size_t pos_ = 0, committed_ = 0, emitted_ = 0;
    uint32_t bitbuf_ = 0;
    int bitcnt_ = 0;
    bool started_ = false, done_ = false, failed_ = false;
    std::string error_;
};

}  // namespace deflate
