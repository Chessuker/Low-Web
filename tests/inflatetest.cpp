// inflatetest COMPRESSED ORIGINAL KIND SEED — feeds COMPRESSED to deflate::Stream in random
// pieces (KIND 0 = gzip, 1 = zlib or raw deflate) and checks that what comes out, piece by
// piece through take(), is exactly ORIGINAL. Run by tests/check_stream.py.
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include "../browser/inflate.h"

static std::vector<uint8_t> read_all(const char *p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

int main(int argc, char **argv) {
    if (argc < 5) { printf("usage: inflatetest COMPRESSED ORIGINAL KIND SEED\n"); return 2; }
    std::vector<uint8_t> in = read_all(argv[1]), want = read_all(argv[2]);
    srand((unsigned)atoi(argv[4]));
    deflate::Stream s(atoi(argv[3]) ? deflate::Stream::Kind::ZlibOrRaw : deflate::Stream::Kind::Gzip, 1u << 30);
    std::vector<uint8_t> got;
    std::string err;
    size_t off = 0;
    int pieces = 0;
    do {
        size_t n = 1 + (size_t)rand() % (rand() % 4 == 0 ? 20 : 20000);  // some tiny pieces, mostly larger
        if (n > in.size() - off) n = in.size() - off;
        bool final = off + n == in.size();
        if (!s.feed(in.data() + off, n, final, err)) { printf("FAIL %s: %s\n", argv[1], err.c_str()); return 1; }
        off += n;
        pieces++;
        const uint8_t *p;
        size_t k = s.take(p);
        got.insert(got.end(), p, p + k);
        for (size_t i = got.size() - k; i < got.size(); i++)
            if (i >= want.size() || got[i] != want[i]) { printf("FAIL %s: wrong byte at %zu\n", argv[1], i); return 1; }
    } while (off < in.size());
    if (got != want || !s.done()) { printf("FAIL %s: %zu of %zu bytes\n", argv[1], got.size(), want.size()); return 1; }
    printf("ok   %-24s %4d pieces  %8zu bytes\n", argv[1], pieces, got.size());
    return 0;
}
