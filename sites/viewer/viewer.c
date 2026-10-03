// viewer.c — Low-web's HTML viewer, itself an ordinary Low-web page.
//
// The browser does not know HTML. When a navigation answers with an HTML document, the
// browser starts this page and hands it the document (fetch id 0 via lw_on_fetch_ex).
// Everything else happens here, in C: charset decoding, HTML tokenizing and tree
// building, a "reader mode" layout (blocks, inline text, lists, tables, images, simple
// GET forms), drawing, scrolling and links. CSS stylesheets and JavaScript are ignored;
// a few presentational attributes and inline style properties are honoured.
#include "lowweb.h"

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;

#define RGBA(r, g, b, a) (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(g) << 8) | (u32)(r))
#define RGB(r, g, b) RGBA(r, g, b, 255)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static const u32 C_TEXT = RGB(32, 33, 36), C_LINK = RGB(26, 13, 171), C_MUTED = RGB(112, 117, 122);

// =====================================================================================
// memory: a size-class allocator on top of memory.grow, plus an arena for the DOM
// =====================================================================================

extern u8 __heap_base;
static u32 brk_top;
static void *free_list[32];

static void fail(const char *msg) {
    lw_log(msg, lw_strlen(msg));
    __builtin_trap();
}

static void *sys_alloc(u32 n) {
    u32 base = ((u32)&__heap_base + 15) & ~15u;
    if (brk_top < base) brk_top = base;
    u64 need = (u64)brk_top + n, have = (u64)__builtin_wasm_memory_size(0) * 65536;
    if (need > 0xFFF00000ull) return 0;
    if (need > have && __builtin_wasm_memory_grow(0, (u32)((need - have + 65535) / 65536)) == (unsigned long)-1) return 0;
    void *p = (void *)brk_top;
    brk_top = (u32)((need + 15) & ~15ull);
    return p;
}

// Small blocks come in power-of-two classes. Large ones (over 64 KB) would waste up to half
// their size that way (a 1 MB arena chunk plus its header took a 2 MB block), so they are
// sized in 64 KB steps. A free large block is reused best-fit and split if it is bigger
// than asked; a freed one is merged with a free block right after it, and free space at the
// top of the heap goes back to it. (The heap is one row of blocks, each starting with its
// class: the next block's header is at blk + size.)
#define LARGE_CLS 0xFFu
#define LARGE_FREE 0xFEu
#define LARGE_STEP 65536u
static u8 *large_free;  // free large blocks: [u32 LARGE_FREE][u32 size][next pointer]...

static void large_unlink(u8 *blk) {
    for (u8 **pp = &large_free; *pp; pp = (u8 **)(*pp + 8))
        if (*pp == blk) {
            *pp = *(u8 **)(blk + 8);
            return;
        }
}

static void large_push(u8 *blk, u32 size) {
    *(u32 *)blk = LARGE_FREE;
    *(u32 *)(blk + 4) = size;
    *(u8 **)(blk + 8) = large_free;
    large_free = blk;
}

static void *mem_alloc(u32 n) {
    if ((u64)n + 8 > LARGE_STEP) {
        u64 want = ((u64)n + 8 + LARGE_STEP - 1) & ~(u64)(LARGE_STEP - 1);
        if (want > 0xF0000000ull) return 0;
        u8 *blk = 0;
        for (u8 *b = large_free; b; b = *(u8 **)(b + 8)) {
            u32 size = *(u32 *)(b + 4);
            if (size >= want && (!blk || size < *(u32 *)(blk + 4))) blk = b;
        }
        if (blk) {
            large_unlink(blk);
            u32 size = *(u32 *)(blk + 4);
            if (size > want) {  // keep the rest
                large_push(blk + want, size - (u32)want);
                *(u32 *)(blk + 4) = (u32)want;
            }
        } else {
            if (!(blk = (u8 *)sys_alloc((u32)want))) return 0;
            *(u32 *)(blk + 4) = (u32)want;
        }
        *(u32 *)blk = LARGE_CLS;
        return blk + 8;
    }
    u32 cls = 4;
    while (((u64)1 << cls) < (u64)n + 8) cls++;
    u8 *blk = (u8 *)free_list[cls];
    if (blk) free_list[cls] = *(void **)(blk + 8);
    else if (!(blk = (u8 *)sys_alloc(1u << cls))) return 0;
    *(u32 *)blk = cls;
    return blk + 8;
}

static void mem_free(void *p) {
    if (!p) return;
    u8 *blk = (u8 *)p - 8;
    u32 cls = *(u32 *)blk;
    if (cls == LARGE_CLS) {
        u32 size = *(u32 *)(blk + 4);
        for (;;) {  // merge with free blocks that follow
            u8 *next = blk + size;
            if ((u32)(unsigned long)next >= brk_top || *(u32 *)next != LARGE_FREE) break;
            large_unlink(next);
            size += *(u32 *)(next + 4);
        }
        if ((u32)(unsigned long)(blk + size) != brk_top) {
            large_push(blk, size);
            return;
        }
        brk_top = (u32)(unsigned long)blk;  // the last block: give it back to the heap, and
        for (u8 *b = large_free; b;) {      // any free block that is now the last one too
            if ((u32)(unsigned long)(b + *(u32 *)(b + 4)) == brk_top) {
                large_unlink(b);
                brk_top = (u32)(unsigned long)b;
                b = large_free;
            } else {
                b = *(u8 **)(b + 8);
            }
        }
        return;
    }
    *(void **)(blk + 8) = free_list[cls];
    free_list[cls] = blk;
}

static void *must_alloc(u32 n) {
    void *p = mem_alloc(n);
    if (!p) fail("viewer: out of memory");
    return p;
}

static void *grow_array(void *p, int count, int *cap, int elem) {
    if (count < *cap) return p;
    int ncap = *cap ? *cap * 2 : 256;
    void *q = must_alloc((u32)ncap * (u32)elem);
    if (p) {
        __builtin_memcpy(q, p, (u32)count * (u32)elem);
        mem_free(p);
    }
    *cap = ncap;
    return q;
}

static u8 *arena_cur, *arena_end;
static void *arena(u32 n) {
    n = (n + 7) & ~7u;
    if (!arena_cur || arena_cur + n > arena_end) {
        u32 size = n > (1u << 20) - 8 ? n : (1u << 20) - 8;  // (whole 64 KB steps with the block header)
        arena_cur = (u8 *)must_alloc(size);
        arena_end = arena_cur + size;
    }
    void *p = arena_cur;
    arena_cur += n;
    return p;
}

static char *arena_str(const char *s, int len) {
    char *d = (char *)arena((u32)len + 1);
    __builtin_memcpy(d, s, (u32)len);
    d[len] = 0;
    return d;
}

LW_EXPORT(lw_alloc) void *lw_alloc(int n) { return n < 0 ? 0 : mem_alloc((u32)n); }

// =====================================================================================
// small string helpers
// =====================================================================================

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static int is_space(u8 c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int is_alpha(u8 c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
static int is_digit(u8 c) { return c >= '0' && c <= '9'; }
static int is_alnum(u8 c) { return is_alpha(c) || is_digit(c); }

static int ieq(const char *a, const char *b) {  // case-insensitive, both NUL-terminated
    if (!a || !b) return 0;
    while (*a && *b)
        if (lower(*a++) != lower(*b++)) return 0;
    return *a == *b;
}
static int iprefix(const char *s, int len, const char *pre) {
    int i = 0;
    for (; pre[i]; i++)
        if (i >= len || lower(s[i]) != pre[i]) return 0;
    return 1;
}

// parses a leading integer; *pct is set if a '%' follows. Returns -1 if there is none.
static int parse_int(const char *s, int *pct) {
    if (pct) *pct = 0;
    if (!s) return -1;
    while (is_space((u8)*s)) s++;
    if (!is_digit((u8)*s)) return -1;
    int v = 0;
    while (is_digit((u8)*s) && v < 100000000) v = v * 10 + (*s++ - '0');
    while (*s == '.' || is_digit((u8)*s)) s++;
    if (pct && *s == '%') *pct = 1;
    return v;
}

static int utf8_put(u8 *o, u32 cp) {
    if (cp < 0x80) { o[0] = (u8)cp; return 1; }
    if (cp < 0x800) { o[0] = (u8)(0xC0 | cp >> 6); o[1] = (u8)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) { o[0] = (u8)(0xE0 | cp >> 12); o[1] = (u8)(0x80 | (cp >> 6 & 63)); o[2] = (u8)(0x80 | (cp & 63)); return 3; }
    o[0] = (u8)(0xF0 | cp >> 18); o[1] = (u8)(0x80 | (cp >> 12 & 63)); o[2] = (u8)(0x80 | (cp >> 6 & 63)); o[3] = (u8)(0x80 | (cp & 63));
    return 4;
}

static int utf8_len(u8 c) { return c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4; }

// =====================================================================================
// charsets: everything becomes UTF-8
// =====================================================================================

enum { CS_UTF8, CS_1252, CS_THAI };

static const u16 cp1252_hi[32] = {0x20AC, 0xFFFD, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
                                  0x2039, 0x0152, 0xFFFD, 0x017D, 0xFFFD, 0xFFFD, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
                                  0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0xFFFD, 0x017E, 0x0178};

static int charset_from_name(const char *s, int len) {
    char n[32];
    int k = 0;
    while (len > 0 && (is_space((u8)*s) || *s == '"' || *s == '\'')) { s++; len--; }
    for (int i = 0; i < len && k < 31; i++) {
        char c = lower(s[i]);
        if (!(is_alnum((u8)c) || c == '-' || c == '_')) break;
        n[k++] = c;
    }
    n[k] = 0;
    if (!k) return -1;
    if (ieq(n, "utf-8") || ieq(n, "utf8")) return CS_UTF8;
    if (ieq(n, "windows-874") || ieq(n, "tis-620") || ieq(n, "iso-8859-11") || ieq(n, "x-windows-874") || ieq(n, "cp874") || ieq(n, "tis620"))
        return CS_THAI;
    if (ieq(n, "iso-8859-1") || ieq(n, "latin1") || ieq(n, "windows-1252") || ieq(n, "us-ascii") || ieq(n, "ascii") || ieq(n, "cp1252"))
        return CS_1252;
    return -1;
}

static int find_charset_param(const char *s, int len) {
    for (int i = 0; i + 8 <= len; i++)
        if (iprefix(s + i, len - i, "charset")) {
            int j = i + 7;
            while (j < len && is_space((u8)s[j])) j++;
            if (j < len && s[j] == '=') return charset_from_name(s + j + 1, len - j - 1);
        }
    return -1;
}

static int valid_utf8(const u8 *d, int n) {
    for (int i = 0; i < n;) {
        u8 c = d[i];
        if (c < 0x80) { i++; continue; }
        int k = c >= 0xF0 && c < 0xF5 ? 4 : c >= 0xE0 ? 3 : c >= 0xC2 && c < 0xE0 ? 2 : 0;
        if (!k || (c >= 0xF5)) return 0;
        if (i + k > n) return 1;  // truncated at the end: fine
        for (int j = 1; j < k; j++)
            if ((d[i + j] & 0xC0) != 0x80) return 0;
        i += k;
    }
    return 1;
}

// The document arrives in pieces (lw_on_fetch_data) and is parsed as it comes. UTF-8 is
// parsed where it lies; other charsets are converted into doc_buf first.
static u8 *raw_buf, *doc_buf;
static int raw_len, raw_cap, raw_done, raw_used, raw_skip;  // raw_used: converted; raw_skip: BOM
static int doc_len, doc_cap;
static int doc_cs = -1;  // CS_*, -1 = not decided yet
static char doc_ctype[200];
static int doc_ctype_len;

static void buf_append(u8 **b, int *len, int *cap, const u8 *d, int n) {
    if (*len + n > *cap) {
        int nc = MAX(*cap * 2, *len + n + (64 << 10));
        u8 *nb = (u8 *)must_alloc((u32)nc);
        if (*len) __builtin_memcpy(nb, *b, (u32)*len);
        mem_free(*b);
        *b = nb;
        *cap = nc;
    }
    __builtin_memcpy(*b + *len, d, (u32)n);
    *len += n;
}

// Decides the charset once enough has arrived: a BOM, the Content-Type header, or a
// <meta charset> in the first 4 KB. An undeclared document is judged as a whole (valid
// UTF-8, else Thai or Western legacy text), so that waits for all of it.
static int decide_charset(void) {
    if (doc_cs >= 0) return 1;
    const u8 *d = raw_buf;
    int n = raw_len;
    if (n < 3 && !raw_done) return 0;
    if (n >= 3 && d[0] == 0xEF && d[1] == 0xBB && d[2] == 0xBF) {
        raw_skip = raw_used = 3;
        doc_cs = CS_UTF8;
        return 1;
    }
    int cs = find_charset_param(doc_ctype, doc_ctype_len);
    if (cs < 0) {
        if (n < 4096 && !raw_done) return 0;
        cs = find_charset_param((const char *)d, MIN(n, 4096));
    }
    if (cs < 0) {
        if (!raw_done) return 0;
        if (valid_utf8(d, n)) cs = CS_UTF8;
        else {
            int thai = 0, high = 0;  // undeclared legacy text: Thai if most high bytes are Thai letters
            for (int i = 0; i < n; i++)
                if (d[i] >= 0x80) { high++; thai += d[i] >= 0xA1 && d[i] <= 0xFB; }
            cs = high && thai * 10 > high * 7 ? CS_THAI : CS_1252;
        }
    }
    doc_cs = cs;
    return 1;
}

static void convert_more(void) {  // single-byte charsets -> doc_buf
    int n = raw_len - raw_used;
    if (n <= 0 || doc_cs == CS_UTF8) return;
    if (doc_len + n * 3 > doc_cap) {
        int nc = MAX(doc_cap * 2, doc_len + n * 3 + (64 << 10));
        u8 *nb = (u8 *)must_alloc((u32)nc);
        if (doc_len) __builtin_memcpy(nb, doc_buf, (u32)doc_len);
        mem_free(doc_buf);
        doc_buf = nb;
        doc_cap = nc;
    }
    const u8 *d = raw_buf + raw_used;
    for (int i = 0; i < n; i++) {
        u8 c = d[i];
        u32 cp = c;
        if (c >= 0x80) {
            if (doc_cs == CS_THAI && c >= 0xA1 && c <= 0xFB) cp = 0x0E00 + (c - 0xA0);
            else if (c < 0xA0) cp = cp1252_hi[c - 0x80];
        }
        doc_len += utf8_put(doc_buf + doc_len, cp);
    }
    raw_used = raw_len;
}

// =====================================================================================
// character references
// =====================================================================================

typedef struct { const char *name; u16 cp; } Entity;
static const Entity entities[] = {
    {"AElig", 198}, {"Aacute", 193}, {"Agrave", 192}, {"Auml", 196}, {"Dagger", 8225}, {"Delta", 916}, {"Eacute", 201},
    {"Omega", 937}, {"Ouml", 214}, {"Prime", 8243}, {"Uuml", 220}, {"aacute", 225}, {"acute", 180}, {"aelig", 230},
    {"agrave", 224}, {"alpha", 945}, {"amp", 38}, {"apos", 39}, {"aring", 229}, {"asymp", 8776}, {"auml", 228},
    {"bdquo", 8222}, {"beta", 946}, {"brvbar", 166}, {"bull", 8226}, {"ccedil", 231}, {"cedil", 184}, {"cent", 162},
    {"check", 10003}, {"clubs", 9827}, {"copy", 169}, {"dagger", 8224}, {"darr", 8595}, {"deg", 176}, {"delta", 948},
    {"diams", 9830}, {"divide", 247}, {"eacute", 233}, {"egrave", 232}, {"emsp", 8195}, {"ensp", 8194}, {"euro", 8364},
    {"frac12", 189}, {"frac14", 188}, {"frac34", 190}, {"gamma", 947}, {"ge", 8805}, {"gt", 62}, {"hArr", 8660},
    {"harr", 8596}, {"hearts", 9829}, {"hellip", 8230}, {"iacute", 237}, {"iexcl", 161}, {"infin", 8734}, {"iquest", 191},
    {"lArr", 8656}, {"lambda", 955}, {"laquo", 171}, {"larr", 8592}, {"ldquo", 8220}, {"le", 8804}, {"loz", 9674},
    {"lrm", 8206}, {"lsaquo", 8249}, {"lsquo", 8216}, {"lt", 60}, {"macr", 175}, {"mdash", 8212}, {"micro", 181},
    {"middot", 183}, {"minus", 8722}, {"mu", 956}, {"nbsp", 160}, {"ndash", 8211}, {"ne", 8800}, {"not", 172},
    {"ntilde", 241}, {"oacute", 243}, {"omega", 969}, {"ordf", 170}, {"ordm", 186}, {"oslash", 248}, {"ouml", 246},
    {"para", 182}, {"permil", 8240}, {"pi", 960}, {"plusmn", 177}, {"pound", 163}, {"prime", 8242}, {"quot", 34},
    {"rArr", 8658}, {"raquo", 187}, {"rarr", 8594}, {"rdquo", 8221}, {"reg", 174}, {"rlm", 8207}, {"rsaquo", 8250},
    {"rsquo", 8217}, {"sbquo", 8218}, {"sect", 167}, {"shy", 173}, {"sigma", 963}, {"spades", 9824}, {"star", 9734},
    {"sup1", 185}, {"sup2", 178}, {"sup3", 179}, {"szlig", 223}, {"theta", 952}, {"thinsp", 8201}, {"times", 215},
    {"trade", 8482}, {"uacute", 250}, {"uarr", 8593}, {"uml", 168}, {"uuml", 252}, {"yen", 165}, {"zwj", 8205},
    {"zwnj", 8204},
};

static int entity_lookup(const char *s, int len) {  // binary search (table is sorted by strcmp order)
    int lo = 0, hi = (int)(sizeof entities / sizeof entities[0]) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char *e = entities[mid].name;
        int c = 0, i = 0;
        for (; i < len && e[i]; i++)
            if (s[i] != e[i]) { c = (u8)s[i] - (u8)e[i]; break; }
        if (!c) c = i < len ? 1 : e[i] ? -1 : 0;
        if (!c) return entities[mid].cp;
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return -1;
}

// Decodes character references from s[0..len) into o (which must hold len bytes: the
// output never grows). Returns the output length.
static int decode_entities(const u8 *s, int len, u8 *o) {
    int k = 0;
    for (int i = 0; i < len;) {
        if (s[i] != '&') {  // most text has no references: copy up to the next '&' at once
            int j = i + 1;
            while (j < len && s[j] != '&') j++;
            __builtin_memcpy(o + k, s + i, (u32)(j - i));
            k += j - i;
            i = j;
            continue;
        }
        int j = i + 1;
        u32 cp = 0;
        int ok = 0;
        if (j < len && s[j] == '#') {
            j++;
            int hex = j < len && (s[j] | 32) == 'x';
            if (hex) j++;
            int start = j;
            while (j < len && (hex ? (is_digit(s[j]) || ((s[j] | 32) >= 'a' && (s[j] | 32) <= 'f')) : is_digit(s[j]))) {
                u32 d = is_digit(s[j]) ? (u32)(s[j] - '0') : (u32)((s[j] | 32) - 'a' + 10);
                if (cp < 0x110000) cp = cp * (hex ? 16 : 10) + d;
                j++;
            }
            if (j > start) {
                ok = 1;
                if (j < len && s[j] == ';') j++;
                if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) cp = 0xFFFD;
                else if (cp >= 0x80 && cp < 0xA0) cp = cp1252_hi[cp - 0x80];
            }
        } else {
            int start = j;
            while (j < len && j - start < 10 && is_alnum(s[j])) j++;
            int v = entity_lookup((const char *)s + start, j - start);
            if (v >= 0 && j < len && s[j] == ';') { cp = (u32)v; j++; ok = 1; }
            else if (v >= 0 && (v == 38 || v == 60 || v == 62 || v == 34 || v == 160 || v == 169)) { cp = (u32)v; ok = 1; }
        }
        if (!ok) { o[k++] = s[i++]; continue; }
        u8 tmp[4];
        int n = utf8_put(tmp, cp);
        if (k + n > j) { o[k++] = s[i++]; continue; }  // cannot happen for valid references; be safe
        for (int t = 0; t < n; t++) o[k++] = tmp[t];
        i = j;
    }
    return k;
}

// =====================================================================================
// elements
// =====================================================================================

enum { FB = 1, FV = 2, FS = 4, FR = 8, FC = 16, FP = 32 };  // block, void, skip, raw text, RCDATA, closes <p>

#define TAGS(X)                                                                                                      \
    X(A, "a", 0) X(ABBR, "abbr", 0) X(ADDRESS, "address", FB | FP) X(AREA, "area", FV | FS) X(ARTICLE, "article", FB | FP) \
    X(ASIDE, "aside", FB | FP) X(AUDIO, "audio", FS) X(B, "b", 0) X(BASE, "base", FV | FS) X(BDI, "bdi", 0)                 \
    X(BDO, "bdo", 0) X(BIG, "big", 0) X(BLOCKQUOTE, "blockquote", FB | FP) X(BODY, "body", FB) X(BR, "br", FV)             \
    X(BUTTON, "button", 0) X(CANVAS, "canvas", FS) X(CAPTION, "caption", FB) X(CENTER, "center", FB | FP)                 \
    X(CITE, "cite", 0) X(CODE, "code", 0) X(COL, "col", FV | FS) X(COLGROUP, "colgroup", FS) X(DATA, "data", 0)           \
    X(DATALIST, "datalist", FS) X(DD, "dd", FB | FP) X(DEL, "del", 0) X(DETAILS, "details", FB | FP) X(DFN, "dfn", 0)     \
    X(DIALOG, "dialog", FB | FS) X(DIR, "dir", FB | FP) X(DIV, "div", FB | FP) X(DL, "dl", FB | FP) X(DT, "dt", FB | FP)  \
    X(EM, "em", 0) X(EMBED, "embed", FV | FS) X(FIELDSET, "fieldset", FB | FP) X(FIGCAPTION, "figcaption", FB | FP)       \
    X(FIGURE, "figure", FB | FP) X(FONT, "font", 0) X(FOOTER, "footer", FB | FP) X(FORM, "form", FB | FP)                 \
    X(FRAME, "frame", FV | FS) X(FRAMESET, "frameset", FS) X(H1, "h1", FB | FP) X(H2, "h2", FB | FP) X(H3, "h3", FB | FP) \
    X(H4, "h4", FB | FP) X(H5, "h5", FB | FP) X(H6, "h6", FB | FP) X(HEAD, "head", FS) X(HEADER, "header", FB | FP)      \
    X(HGROUP, "hgroup", FB | FP) X(HR, "hr", FB | FV | FP) X(HTML, "html", FB) X(I, "i", 0) X(IFRAME, "iframe", FS | FR)  \
    X(IMAGE, "image", FV) X(IMG, "img", FV) X(INPUT, "input", FV) X(INS, "ins", 0) X(KBD, "kbd", 0)                       \
    X(LABEL, "label", 0) X(LEGEND, "legend", FB) X(LI, "li", FB | FP) X(LINK, "link", FV | FS) X(LISTING, "listing", FB | FP) \
    X(MAIN, "main", FB | FP) X(MAP, "map", FS) X(MARK, "mark", 0) X(MARQUEE, "marquee", FB) X(MATH, "math", FS)          \
    X(MENU, "menu", FB | FP) X(META, "meta", FV | FS) X(METER, "meter", FS) X(NAV, "nav", FB | FP) X(NOBR, "nobr", 0)   \
    X(NOEMBED, "noembed", FS | FR) X(NOFRAMES, "noframes", FS | FR) X(NOSCRIPT, "noscript", 0) X(OBJECT, "object", FS)  \
    X(OL, "ol", FB | FP) X(OPTGROUP, "optgroup", 0) X(OPTION, "option", 0) X(OUTPUT, "output", 0) X(P, "p", FB | FP)     \
    X(PARAM, "param", FV | FS) X(PICTURE, "picture", 0) X(PLAINTEXT, "plaintext", FB | FP) X(PRE, "pre", FB | FP)         \
    X(PROGRESS, "progress", FS) X(Q, "q", 0) X(RP, "rp", FS) X(RT, "rt", 0) X(RUBY, "ruby", 0) X(S, "s", 0)              \
    X(SAMP, "samp", 0) X(SCRIPT, "script", FS | FR) X(SEARCH, "search", FB | FP) X(SECTION, "section", FB | FP)           \
    X(SELECT, "select", 0) X(SLOT, "slot", 0) X(SMALL, "small", 0) X(SOURCE, "source", FV | FS) X(SPAN, "span", 0)        \
    X(STRIKE, "strike", 0) X(STRONG, "strong", 0) X(STYLE, "style", FS | FR) X(SUB, "sub", 0) X(SUMMARY, "summary", FB)   \
    X(SUP, "sup", 0) X(SVG, "svg", FS) X(TABLE, "table", FB | FP) X(TBODY, "tbody", FB) X(TD, "td", FB)                   \
    X(TEMPLATE, "template", FS) X(TEXTAREA, "textarea", FC) X(TFOOT, "tfoot", FB) X(TH, "th", FB) X(THEAD, "thead", FB)  \
    X(TIME, "time", 0) X(TITLE, "title", FS | FC) X(TR, "tr", FB) X(TRACK, "track", FV | FS) X(TT, "tt", 0)               \
    X(U, "u", 0) X(UL, "ul", FB | FP) X(VAR, "var", 0) X(VIDEO, "video", FS) X(WBR, "wbr", FV) X(XMP, "xmp", FB | FP | FR)

#define X_ENUM(id, name, flags) T_##id,
enum { T_UNKNOWN, TAGS(X_ENUM) T_COUNT };
#define X_NAME(id, name, flags) name,
static const char *const tag_names[] = {"", TAGS(X_NAME)};
#define X_FLAGS(id, name, flags) flags,
static const u8 tag_flags[] = {0, TAGS(X_FLAGS)};

static int tag_lookup(const char *s, int len) {  // tag_names[1..] are in sorted order
    int lo = 1, hi = T_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const char *e = tag_names[mid];
        int c = 0, i = 0;
        for (; i < len && e[i]; i++)
            if (s[i] != e[i]) { c = (u8)s[i] - (u8)e[i]; break; }
        if (!c) c = i < len ? 1 : e[i] ? -1 : 0;
        if (!c) return mid;
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return T_UNKNOWN;
}

// =====================================================================================
// the document tree
// =====================================================================================

enum { N_ELEM, N_TEXT, N_ROOT };

typedef struct Node Node;
struct Node {
    Node *parent, *first, *last, *next;
    const char *text;   // text: UTF-8 (references decoded, whitespace collapsed outside <pre>)
    u32 len;
    const char **attr;  // element: name, value, name, value...
    const char *name;   // element: tag name (for unknown tags)
    u8 *brk;            // text: cached line-break opportunities (non-ASCII text only)
    u64 *anc;           // element: Bloom filter of the ancestors (css.c), made when first needed
    u16 nattr;
    u16 tag;
    u8 type;
    u8 css_hidden;      // display:none by a stylesheet (css.c; valid when css_ep is current)
    u8 css_ep;          // css.c epoch css_hidden was computed in
    u8 lpass;           // layout pass that last reached this node (y is valid for that pass)
    u8 open;            // element: the parser may still add children (the document is streaming in)
    int ref;            // image or control index, -1
    int y;              // layout: top of the element (for #fragment links)
    struct Lay *lay;    // element: its CSS layout properties (css.c), 0 if none; valid with css_ep
};

static Node *root, *head_node;
static Node *stack[512];
static int sp, in_pre, head_open;
static u32 body_bg, body_text, body_link;
static int has_body_bg;
static int script_count;

static const char *attr(const Node *n, const char *name) {  // name: lower case
    for (int i = 0; i < n->nattr; i++) {
        const char *a = n->attr[2 * i], *b = name;
        if (*a != *b) continue;
        while (*a && *a == *b) a++, b++;
        if (*a == *b) return n->attr[2 * i + 1];
    }
    return 0;
}

static Node *new_node(int type, int tag) {
    Node *n = (Node *)arena(sizeof(Node));
    __builtin_memset(n, 0, sizeof(Node));
    n->type = (u8)type;
    n->tag = (u16)tag;
    n->ref = -1;
    return n;
}

static void append(Node *parent, Node *n) {
    n->parent = parent;
    if (parent->last) parent->last->next = n;
    else parent->first = n;
    parent->last = n;
}

static Node *cur(void) { return stack[sp - 1]; }

// The parser tells the rest of the viewer about elements as they are made and finished
// (form controls, stylesheets, the title...; defined with the entry points).
static void node_opened(Node *n);
static void node_closed(Node *n);
static int body_seen;  // the document's body has begun

static void push(Node *n) {
    if (sp < (int)(sizeof stack / sizeof stack[0])) {
        stack[sp++] = n;
        n->open = 1;
        if (n->tag == T_PRE || n->tag == T_LISTING || n->tag == T_TEXTAREA || n->tag == T_PLAINTEXT) in_pre++;
    }
}

static void pop(void) {
    if (sp <= 1) return;
    Node *n = stack[--sp];
    if (n->tag == T_PRE || n->tag == T_LISTING || n->tag == T_TEXTAREA || n->tag == T_PLAINTEXT) in_pre--;
    if (n == head_node) head_open = 0;
    n->open = 0;
    node_closed(n);
}

static void pop_through(int index) {  // pops stack[index] and everything above it
    while (sp > index && sp > 1) pop();
}

// Index of the nearest open element with this tag, not looking past a scope boundary.
enum { SCOPE_DEFAULT, SCOPE_BUTTON, SCOPE_LIST, SCOPE_TABLE };
static int in_scope(int tag, int scope, const char *name) {
    for (int i = sp - 1; i >= 1; i--) {
        int t = stack[i]->tag;
        if (t == tag && (tag != T_UNKNOWN || ieq(stack[i]->name, name))) return i;
        if (scope == SCOPE_TABLE) {
            if (t == T_TABLE || t == T_TEMPLATE) return -1;
            continue;
        }
        if (t == T_TABLE || t == T_TD || t == T_TH || t == T_CAPTION || t == T_TEMPLATE || t == T_OBJECT || t == T_MARQUEE)
            return -1;
        if (scope == SCOPE_BUTTON && t == T_BUTTON) return -1;
        if (scope == SCOPE_LIST && (t == T_OL || t == T_UL)) return -1;
    }
    return -1;
}

static void close_p(void) {
    int i = in_scope(T_P, SCOPE_BUTTON, 0);
    if (i >= 0) pop_through(i);
}

static void close_head(void) {
    if (!head_open) return;
    for (int i = sp - 1; i >= 1; i--)
        if (stack[i] == head_node) { pop_through(i); break; }
    head_open = 0;
}

static int head_allowed(int tag) {
    return tag == T_TITLE || tag == T_META || tag == T_LINK || tag == T_STYLE || tag == T_SCRIPT || tag == T_BASE ||
           tag == T_NOSCRIPT || tag == T_TEMPLATE;
}

static int is_table_section(int t) { return t == T_TABLE || t == T_TBODY || t == T_THEAD || t == T_TFOOT; }

static void apply_body_attrs(const char **a, int n);

static Node *start_tag(int tag, const char *name, const char **attrs, int nattr, int self_closing) {
    if (tag == T_HTML) return 0;
    if (tag == T_BODY || (tag != T_HEAD && !head_allowed(tag))) body_seen = 1;
    if (tag == T_BODY) {
        close_head();
        apply_body_attrs(attrs, nattr);
        return 0;
    }
    if (tag == T_HEAD) {
        if (!head_node) {
            head_node = new_node(N_ELEM, T_HEAD);
            append(cur(), head_node);
            push(head_node);
            head_open = 1;
        }
        return 0;
    }
    if (head_open && !head_allowed(tag)) close_head();
    if (tag == T_IMAGE) tag = T_IMG;
    int fl = tag_flags[tag];
    if (fl & FP) close_p();
    switch (tag) {
    case T_LI: {
        int i = in_scope(T_LI, SCOPE_LIST, 0);
        if (i >= 0) pop_through(i);
        break;
    }
    case T_DT:
    case T_DD: {
        for (int i = sp - 1; i >= 1; i--) {
            int t = stack[i]->tag;
            if (t == T_DT || t == T_DD) { pop_through(i); break; }
            if (t == T_DL || t == T_TABLE || t == T_TD || t == T_TH) break;
        }
        break;
    }
    case T_OPTION: if (cur()->tag == T_OPTION) pop(); break;
    case T_OPTGROUP:
        if (cur()->tag == T_OPTION) pop();
        if (cur()->tag == T_OPTGROUP) pop();
        break;
    case T_A: {
        int i = in_scope(T_A, SCOPE_DEFAULT, 0);
        if (i >= 0) pop_through(i);
        break;
    }
    case T_BUTTON: {
        int i = in_scope(T_BUTTON, SCOPE_DEFAULT, 0);
        if (i >= 0) pop_through(i);
        break;
    }
    case T_SELECT: {
        int i = in_scope(T_SELECT, SCOPE_DEFAULT, 0);
        if (i >= 0) pop_through(i);
        break;
    }
    case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6:
        if (cur()->tag >= T_H1 && cur()->tag <= T_H6) pop();
        break;
    case T_TR: {
        int t = in_scope(T_TABLE, SCOPE_TABLE, 0);
        if (t >= 0)
            while (sp - 1 > t && !is_table_section(cur()->tag)) pop();
        break;
    }
    case T_TD:
    case T_TH: {
        int t = in_scope(T_TABLE, SCOPE_TABLE, 0);
        if (t >= 0) {
            int tr = -1;
            for (int i = sp - 1; i > t; i--)
                if (stack[i]->tag == T_TR) { tr = i; break; }
            if (tr >= 0) pop_through(tr + 1);
            else {
                while (sp - 1 > t && !is_table_section(cur()->tag)) pop();
                Node *row = new_node(N_ELEM, T_TR);
                append(cur(), row);
                push(row);
            }
        }
        break;
    }
    case T_THEAD: case T_TBODY: case T_TFOOT: case T_CAPTION: case T_COLGROUP: {
        int t = in_scope(T_TABLE, SCOPE_TABLE, 0);
        if (t >= 0) pop_through(t + 1);
        break;
    }
    }
    Node *n = new_node(N_ELEM, tag);
    n->name = tag == T_UNKNOWN ? name : tag_names[tag];
    if (nattr) {
        n->attr = (const char **)arena((u32)nattr * 2 * sizeof(char *));
        __builtin_memcpy(n->attr, attrs, (u32)nattr * 2 * sizeof(char *));
        n->nattr = (u16)nattr;
    }
    append(cur(), n);
    int is_void = (fl & FV) || (self_closing && (fl & FS || tag == T_UNKNOWN));
    if (!is_void) push(n);
    node_opened(n);
    if (!n->open) node_closed(n);  // void (or too deep to keep open): finished already
    return n;
}

static void end_tag(int tag, const char *name) {
    if (tag == T_HTML || tag == T_BODY) return;
    if (tag == T_HEAD) { close_head(); return; }
    if (tag == T_BR) { start_tag(T_BR, "br", 0, 0, 0); return; }
    if (tag == T_P) { close_p(); return; }
    if (tag == T_IMAGE) tag = T_IMG;
    int scope = SCOPE_DEFAULT;
    if (tag == T_TABLE || tag == T_TR || tag == T_TD || tag == T_TH || is_table_section(tag) || tag == T_CAPTION)
        scope = SCOPE_TABLE;
    if (tag == T_LI) scope = SCOPE_LIST;
    int i = scope == SCOPE_TABLE && tag != T_TABLE ? -1 : in_scope(tag, scope, name);
    if (scope == SCOPE_TABLE && tag != T_TABLE) {
        for (int k = sp - 1; k >= 1; k--) {
            if (stack[k]->tag == tag) { i = k; break; }
            if (stack[k]->tag == T_TABLE) break;
        }
    } else if (tag == T_TABLE) {
        for (int k = sp - 1; k >= 1; k--)
            if (stack[k]->tag == T_TABLE) { i = k; break; }
    }
    if (i >= 0) pop_through(i);
}

static u8 *scratch;
static int scratch_cap;
static u8 *scratch_buf(int n) {
    if (n > scratch_cap) {
        mem_free(scratch);
        scratch_cap = MAX(n, 4096);
        scratch = (u8 *)must_alloc((u32)scratch_cap);
    }
    return scratch;
}

static void add_text(const u8 *s, int len, int raw) {
    if (head_open && cur() == head_node) {  // text directly in <head> means the body has begun
        int ws = 1;
        for (int i = 0; i < len && ws; i++) ws = is_space(s[i]);
        if (ws) return;
        close_head();
    }
    u8 *buf = scratch_buf(len + 1);
    int n = raw ? (__builtin_memcpy(buf, s, (u32)len), len) : decode_entities(s, len, buf);
    int m = 0;  // line ends as HTML reads them: CR LF and a lone CR are LF
    for (int i = 0; i < n; i++) {
        if (buf[i] != 0x0D) buf[m++] = buf[i];
        else if (i + 1 >= n || buf[i + 1] != 0x0A) buf[m++] = 0x0A;
    }
    n = m;
    if (!in_pre) {  // collapse whitespace
        int k = 0, sp_ = 0;
        for (int i = 0; i < n; i++) {
            if (is_space(buf[i])) { sp_ = 1; continue; }
            if (sp_) { buf[k++] = ' '; sp_ = 0; }
            buf[k++] = buf[i];
        }
        if (sp_) buf[k++] = ' ';
        n = k;
    }
    if (!n) return;
    Node *parent = cur();
    Node *prev = parent->last;
    if (prev && prev->type == N_TEXT && !in_pre && n == 1 && buf[0] == ' ' && prev->len && prev->text[prev->len - 1] == ' ')
        return;  // a lone space after text that already ends in one
    Node *t = new_node(N_TEXT, 0);
    t->text = arena_str((const char *)buf, n);
    t->len = (u32)n;
    append(parent, t);
    if (!head_open && !(n == 1 && buf[0] == ' ')) body_seen = 1;  // (not the newline after <!doctype>)
}

// Finds "</name" (case-insensitive) from i; returns its index or n.
static int find_end_tag(const u8 *d, int n, int i, const char *name) {
    int len = lw_strlen(name);
    for (; i + 2 + len <= n; i++) {
        if (d[i] != '<' || d[i + 1] != '/') continue;
        int k = 0;
        while (k < len && lower((char)d[i + 2 + k]) == name[k]) k++;
        if (k == len && (i + 2 + len == n || !is_alnum(d[i + 2 + len]))) return i;
    }
    return n;
}

static int parse_pos, parse_done, pre_nl_pending, plaintext_mode;
static int parse_dropped;  // source bytes parsed and thrown away while the rest downloaded

static void parse_begin(void) {
    root = new_node(N_ROOT, 0);
    root->open = 1;
    sp = 0;
    stack[sp++] = root;
}

// Tokenizes d[parse_pos..n). Unless `final`, it stops in front of anything that may go on
// in bytes still to come (a tag, comment, text run or <script> body cut off at the end),
// so every token is seen whole, just as if the document had come in one piece. It also
// stops (between tokens) at `deadline`. Returns 0 if it stopped for the deadline.
static int parse_some(const u8 *d, int n, int final, double deadline) {
    const char *attrs[2 * 64];
    int i = parse_pos, count = 0, timed_out = 0;
    while (i < n) {
        if ((++count & 31) == 0 && lw_now() > deadline) { timed_out = 1; break; }
        if (plaintext_mode) {  // <plaintext>: everything after it is text
            add_text(d + i, n - i, 1);
            i = n;
            break;
        }
        if (pre_nl_pending) {  // a newline right after <pre> is ignored
            pre_nl_pending = 0;
            if (d[i] == '\n') { i++; continue; }
        }
        if (d[i] == '<' && i + 1 >= n && !final) break;
        if (d[i] == '<' && i + 1 < n) {
            u8 c1 = d[i + 1];
            if (!final && ((c1 == '!' && i + 3 >= n) || (c1 == '/' && i + 2 >= n))) break;
            if (c1 == '!' && i + 3 < n && d[i + 2] == '-' && d[i + 3] == '-') {  // comment
                int j = i + 4;
                while (j + 2 < n && !(d[j] == '-' && d[j + 1] == '-' && d[j + 2] == '>')) j++;
                if (j + 2 >= n && !final) break;
                i = j + 3 > n ? n : j + 3;
                continue;
            }
            if (c1 == '!' || c1 == '?' || (c1 == '/' && i + 2 < n && !is_alpha(d[i + 2]) && d[i + 2] != '>')) {
                int j = i + 2;
                while (j < n && d[j] != '>') j++;
                if (j >= n && !final) break;
                i = j + 1;
                continue;
            }
            int end = c1 == '/';
            if (i + 1 + end < n && is_alpha(d[i + 1 + end])) {
                int j = i + 1 + end;
                char name[32];
                int nl = 0;
                while (j < n && !is_space(d[j]) && d[j] != '/' && d[j] != '>') {
                    if (nl < 31) name[nl++] = lower((char)d[j]);
                    j++;
                }
                name[nl] = 0;
                int tag = tag_lookup(name, nl);
                const char *uname = tag == T_UNKNOWN ? arena_str(name, nl) : 0;
                int na = 0, self_closing = 0;
                for (;;) {  // attributes
                    while (j < n && (is_space(d[j]) || d[j] == '/')) {
                        self_closing = d[j] == '/';
                        j++;
                    }
                    if (j >= n || d[j] == '>') break;
                    int ns = j;
                    while (j < n && !is_space(d[j]) && d[j] != '/' && d[j] != '>' && d[j] != '=') j++;
                    int ne = j;
                    while (j < n && is_space(d[j])) j++;
                    int vs = j, ve = j;
                    if (j < n && d[j] == '=') {
                        j++;
                        while (j < n && is_space(d[j])) j++;
                        if (j < n && (d[j] == '"' || d[j] == '\'')) {
                            u8 q = d[j++];
                            vs = j;
                            while (j < n && d[j] != q) j++;
                            ve = j;
                            if (j < n) j++;
                        } else {
                            vs = j;
                            while (j < n && !is_space(d[j]) && d[j] != '>') j++;
                            ve = j;
                        }
                    }
                    self_closing = 0;
                    if (ne == ns) { j++; continue; }
                    if (end || na >= 64) continue;
                    char *an = arena_str((const char *)d + ns, ne - ns);
                    for (char *p = an; *p; p++) *p = lower(*p);
                    char *av = (char *)arena((u32)(ve - vs) + 1);
                    int vl = decode_entities(d + vs, ve - vs, (u8 *)av);
                    av[vl] = 0;
                    attrs[2 * na] = an;
                    attrs[2 * na + 1] = av;
                    na++;
                }
                if (j >= n && !final) break;  // the tag isn't complete yet
                if (!end && (tag_flags[tag] & (FR | FC)) && !self_closing && !final) {  // nor, maybe, its raw text
                    int e = find_end_tag(d, n, j + 1, name);
                    while (e < n && d[e] != '>') e++;
                    if (e >= n) break;
                }
                i = j < n ? j + 1 : n;
                if (end) {
                    end_tag(tag, name);
                    continue;
                }
                if (tag == T_SCRIPT) script_count++;
                Node *el = start_tag(tag, uname, attrs, na, self_closing);
                int fl = tag_flags[tag];
                if ((fl & (FR | FC)) && el && !self_closing) {  // raw text / RCDATA content
                    int e = find_end_tag(d, n, i, name);
                    if (fl & FC) add_text(d + i, e - i, 0);
                    else if (tag == T_XMP || tag == T_STYLE) add_text(d + i, e - i, 1);  // <style> is read by css.c
                    // script, iframe, noembed, noframes: content ignored
                    while (e < n && d[e] != '>') e++;
                    i = e < n ? e + 1 : n;
                    if (sp > 1 && cur() == el) pop();
                } else if (tag == T_PRE || tag == T_LISTING || tag == T_TEXTAREA) {
                    if (i < n) { if (d[i] == '\n') i++; }  // a newline right after <pre> is ignored
                    else if (!final) pre_nl_pending = 1;
                } else if (tag == T_PLAINTEXT) {
                    plaintext_mode = 1;
                }
                continue;
            }
        }
        int j = i + 1;
        while (j < n && d[j] != '<') j++;
        if (j >= n && !final) break;  // the text may go on
        add_text(d + i, j - i, 0);
        i = j;
    }
    parse_pos = i;
    return !timed_out;
}

// =====================================================================================
// colours and inline styles
// =====================================================================================

static int hexv(char c) {
    c = lower(c);
    return is_digit((u8)c) ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}

// Returns 1 and sets *out for a recognised colour.
static int parse_color(const char *s, int len, u32 *out) {
    while (len && is_space((u8)*s)) { s++; len--; }
    while (len && is_space((u8)s[len - 1])) len--;
    if (!len) return 0;
    if (s[0] == '#' || (len == 6 && hexv(s[0]) >= 0 && hexv(s[1]) >= 0 && hexv(s[5]) >= 0)) {
        if (s[0] == '#') { s++; len--; }
        int v[6];
        for (int i = 0; i < len && i < 6; i++) if ((v[i] = hexv(s[i])) < 0) return 0;
        if (len == 3) { *out = RGB(v[0] * 17, v[1] * 17, v[2] * 17); return 1; }
        if (len >= 6) { *out = RGB(v[0] * 16 + v[1], v[2] * 16 + v[3], v[4] * 16 + v[5]); return 1; }
        return 0;
    }
    if (iprefix(s, len, "rgb")) {
        int c[3] = {0, 0, 0}, k = 0, i = 0;
        while (i < len && s[i] != '(') i++;
        for (i++; i < len && k < 3; i++) {
            if (is_digit((u8)s[i])) {
                int v = 0;
                while (i < len && is_digit((u8)s[i])) v = v * 10 + (s[i++] - '0');
                while (i < len && (s[i] == '.' || is_digit((u8)s[i]))) i++;
                if (i < len && s[i] == '%') v = v * 255 / 100;
                c[k++] = v > 255 ? 255 : v;
            }
        }
        if (k < 3) return 0;
        *out = RGB(c[0], c[1], c[2]);
        return 1;
    }
    static const struct { const char *name; u32 c; } names[] = {
        {"black", RGB(0, 0, 0)}, {"white", RGB(255, 255, 255)}, {"red", RGB(255, 0, 0)}, {"green", RGB(0, 128, 0)},
        {"blue", RGB(0, 0, 255)}, {"gray", RGB(128, 128, 128)}, {"grey", RGB(128, 128, 128)}, {"silver", RGB(192, 192, 192)},
        {"maroon", RGB(128, 0, 0)}, {"navy", RGB(0, 0, 128)}, {"purple", RGB(128, 0, 128)}, {"teal", RGB(0, 128, 128)},
        {"olive", RGB(128, 128, 0)}, {"lime", RGB(0, 255, 0)}, {"aqua", RGB(0, 255, 255)}, {"fuchsia", RGB(255, 0, 255)},
        {"yellow", RGB(255, 255, 0)}, {"orange", RGB(255, 165, 0)}, {"darkgray", RGB(169, 169, 169)},
        {"lightgray", RGB(211, 211, 211)}, {"darkred", RGB(139, 0, 0)}, {"darkblue", RGB(0, 0, 139)},
        {"darkgreen", RGB(0, 100, 0)}, {"brown", RGB(165, 42, 42)}, {"pink", RGB(255, 192, 203)},
    };
    char nm[16];
    if (len >= 16) return 0;
    for (int i = 0; i < len; i++) nm[i] = lower(s[i]);
    nm[len] = 0;
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (ieq(nm, names[i].name)) { *out = names[i].c; return 1; }
    return 0;
}

static int attr_color(const Node *n, const char *name, u32 *out) {
    const char *v = attr(n, name);
    return v && parse_color(v, lw_strlen(v), out);
}

static void apply_body_attrs(const char **a, int n) {
    for (int i = 0; i < n; i++) {
        const char *k = a[2 * i], *v = a[2 * i + 1];
        u32 c;
        if (ieq(k, "bgcolor") && parse_color(v, lw_strlen(v), &c)) { body_bg = c; has_body_bg = 1; }
        if (ieq(k, "text") && parse_color(v, lw_strlen(v), &c)) body_text = c;
        if (ieq(k, "link") && parse_color(v, lw_strlen(v), &c)) body_link = c;
    }
}

// =====================================================================================
// layout
// =====================================================================================

static float S = 1;          // device pixels per CSS pixel
static int W = 800, H = 600; // view
static u32 *fb;
static int fb_cap;

typedef struct {
    short size;
    u8 flags;       // LW_TEXT_*
    u8 align;       // 0 left, 1 center, 2 right
    u8 pre, nowrap, deco;  // deco: 1 underline, 2 strike
    u32 color;
    int link;
} Style;

enum { IT_TEXT, IT_RECT, IT_IMAGE, IT_CONTROL, IT_VIDEO };
typedef struct {
    u8 kind, flags, deco;
    u8 layer;      // 1: in a float (painted after the rest, as CSS paints floats above blocks)
    short size;
    int x, y, w, h, asc;
    u32 color;
    const char *s;
    int len;
    int link, ref;
} Item;

static Item *items;
static int nitems, items_cap;

typedef struct { const char *href, *target; } Link;
static Link *links;
static int nlinks, links_cap;

static int doc_h;
static int content_x, content_w;

typedef struct {
    int x, w, y;
    int margin;             // pending (collapsed) vertical margin
    int measure;
    int max_w, min_w;       // measure results
    int lx;                 // x within the current line
    int line_start, line_items;
    int space, brk;
    u8 align;
    int strut;              // empty-line height (for <br><br>)
    int bfc;                // the float scope (block formatting context) its lines avoid floats of
    int bx, bw;             // x and w outside the current line, while a float narrows it (adj)
    u8 adj;
} Ctx;

// ---- floats -------------------------------------------------------------------------------
// A float is a box at the left or right of its scope (bfc: the document, a table cell, a
// flex or grid item, a float's own content); lines in that scope get shorter beside it.

typedef struct { int x0, x1, y0, y1, bfc; u8 right; } FloatBox;  // x0..x1 includes the gap to the text
static FloatBox *floats;
static int nfloats, floats_cap, next_bfc;

// The room [*l, *r) inside [l0, r0) beside floats of scope bfc in rows y..y+h; returns
// the lowest bottom of those floats (where more room may be), or -1 if none is in the way.
static int float_band(int bfc, int y, int h, int l0, int r0, int *l, int *r) {
    int next = -1;
    *l = l0;
    *r = r0;
    for (int i = 0; i < nfloats; i++) {
        FloatBox *f = &floats[i];
        if (f->bfc != bfc || f->y0 >= y + MAX(h, 1) || f->y1 <= y) continue;
        if (f->right) { if (f->x0 < *r) *r = f->x0; }
        else if (f->x1 > *l) *l = f->x1;
        if (next < 0 || f->y1 < next) next = f->y1;
    }
    return next;
}

static int floats_bottom(int bfc, int side) {  // side: 1 left, 2 right, 3 both
    int b = 0;
    for (int i = 0; i < nfloats; i++)
        if (floats[i].bfc == bfc && (side & (floats[i].right ? 2 : 1)) && floats[i].y1 > b) b = floats[i].y1;
    return b;
}

// Before the first thing on a line: make the line fit beside the floats (or move it below
// them if too little room is left).
static void line_begin(Ctx *c, int h) {
    if (c->measure || c->line_items || !nfloats || c->adj) return;
    int y = c->y + c->margin, l, r;
    for (int tries = 0; tries < 64; tries++) {
        int next = float_band(c->bfc, y, h, c->x, c->x + c->w, &l, &r);
        if (next < 0 || r - l >= MIN(c->w, (int)(60 * S))) break;
        y = next;  // too narrow: below the float that ends first
        c->y = y;
        c->margin = 0;
    }
    if (l == c->x && r == c->x + c->w) return;
    c->bx = c->x;
    c->bw = c->w;
    c->adj = 1;
    c->x = l;
    c->w = MAX(r - l, 1);
}

static void line_end(Ctx *c) {
    if (!c->adj) return;
    c->x = c->bx;
    c->w = c->bw;
    c->adj = 0;
}

static void clear_floats(Ctx *c, int side) {
    if (c->measure || !nfloats) return;
    int b = floats_bottom(c->bfc, side);
    if (b > c->y + c->margin) {
        c->y = b;
        c->margin = 0;
    }
}

// list markers: emitted on the first line inside the <li>
static char marker_text[24];
static int marker_len, marker_right, marker_pending;
static Style marker_style;

static Item *new_item(int kind) {
    items = (Item *)grow_array(items, nitems, &items_cap, sizeof(Item));
    Item *it = &items[nitems++];
    __builtin_memset(it, 0, sizeof *it);
    it->kind = (u8)kind;
    it->link = -1;
    it->ref = -1;
    return it;
}

static int ascent(int size) { return (size * 108 + 50) / 100; }
static int descent(int size) { return (size * 27 + 50) / 100; }

// ---- measuring text (cached: the same text is measured again on every relayout) ----

typedef struct { const char *s; int len; int key; int w; } WCache;
#define WCACHE (1 << 16)
static WCache *wcache;

static int text_w(const char *s, int len, const Style *st) {
    if (!wcache) {
        wcache = (WCache *)must_alloc(WCACHE * sizeof(WCache));
        __builtin_memset(wcache, 0, WCACHE * sizeof(WCache));
    }
    int key = st->size * 8 + (st->flags & 7);
    u32 h = ((u32)(unsigned long)s * 2654435761u) ^ ((u32)len * 40503u) ^ ((u32)key * 97u);
    WCache *e = &wcache[h & (WCACHE - 1)];
    if (e->s == s && e->len == len && e->key == key) return e->w;
    int w = lw_text_width(s, len, st->size, st->flags);
    e->s = s; e->len = len; e->key = key; e->w = w;
    return w;
}

static int space_w(const Style *st) { return (st->flags & LW_TEXT_MONO) ? st->size * 55 / 100 : st->size * 27 / 100; }

static void place_marker(int base) {
    if (!marker_pending) return;
    marker_pending = 0;
    Item *m = new_item(IT_TEXT);
    m->s = arena_str(marker_text, marker_len);
    m->len = marker_len;
    m->size = marker_style.size;
    m->flags = marker_style.flags;
    m->color = marker_style.color;
    m->w = text_w(m->s, m->len, &marker_style);
    m->x = marker_right - m->w;
    m->asc = ascent(m->size);
    m->h = m->asc + descent(m->size);
    m->y = base - m->asc;
}

static void flush_line(Ctx *c) {
    if (c->measure) {
        if (c->lx > c->max_w) c->max_w = c->lx;
        c->lx = 0; c->line_items = 0; c->space = 0; c->brk = 0;
        return;
    }
    if (!c->line_items) { c->lx = 0; c->space = 0; c->brk = 0; c->line_start = nitems; line_end(c); return; }
    c->y += c->margin;
    c->margin = 0;
    int asc = 0, desc = 0;
    for (int i = c->line_start; i < nitems; i++) {
        Item *it = &items[i];
        asc = MAX(asc, it->asc);
        desc = MAX(desc, it->h - it->asc);
    }
    int base = c->y + asc;
    int dx = c->align == 1 ? (c->w - c->lx) / 2 : c->align == 2 ? c->w - c->lx : 0;
    if (dx < 0) dx = 0;
    for (int i = c->line_start; i < nitems; i++) {
        items[i].y = base - items[i].asc;
        items[i].x += dx;
    }
    place_marker(base);
    c->y = base + desc;
    if (c->lx > c->max_w) c->max_w = c->lx;
    c->lx = 0; c->line_items = 0; c->space = 0; c->brk = 0;
    c->line_start = nitems;
    line_end(c);
}

static void emit_text(Ctx *c, const char *s, int len, int x, int w, const Style *st) {
    c->lx = x + w;
    c->line_items++;
    c->space = 0;
    c->brk = 0;
    if (w > c->min_w) c->min_w = w;
    if (c->measure) return;
    Item *it = new_item(IT_TEXT);
    it->s = s; it->len = len;
    it->size = st->size; it->flags = st->flags; it->deco = st->deco;
    it->color = st->color; it->link = st->link;
    it->x = c->x + x; it->w = w;
    it->asc = ascent(st->size);
    it->h = it->asc + descent(st->size);
}

// Places one unbreakable piece of text; may wrap before it.
static void place_segment(Ctx *c, const char *s, int len, const Style *st, int can_break) {
    int lh = ascent(st->size) + descent(st->size);
    if (!c->line_items) line_begin(c, lh);
    int w = text_w(s, len, st);
    int sw = c->space && c->line_items ? space_w(st) : 0;
    if (c->line_items && (can_break || c->space) && !st->nowrap && c->lx + sw + w > c->w) {
        flush_line(c);
        sw = 0;
    }
    if (!c->line_items) { sw = 0; line_begin(c, lh); }
    if (w > c->w && !st->nowrap && !c->measure && len > 1) {
        // a single word wider than the line (long URLs): split it between characters
        while (len > 0) {
            if (!c->line_items) line_begin(c, lh);
            int lo = 1, hi = len, fit = 0;
            while (lo <= hi) {
                int mid = (lo + hi) / 2;
                int m = mid;
                while (m < len && (s[m] & 0xC0) == 0x80) m++;
                if (text_w(s, m, st) <= c->w - c->lx - sw) { fit = m; lo = mid + 1; }
                else hi = mid - 1;
            }
            if (!fit) {
                if (c->line_items) { flush_line(c); sw = 0; continue; }
                fit = utf8_len((u8)s[0]);
                if (fit > len) fit = len;
            }
            emit_text(c, s, fit, c->lx + sw, text_w(s, fit, st), st);
            s += fit;
            len -= fit;
            sw = 0;
            if (len) flush_line(c);
        }
        return;
    }
    emit_text(c, s, len, c->lx + sw, w, st);
}

static void flow_text(Ctx *c, Node *t, const Style *st) {
    const char *s = t->text;
    int len = (int)t->len;
    if (st->pre) {
        int i = 0;
        while (i <= len) {
            int j = i;
            while (j < len && s[j] != '\n') j++;
            if (j > i) {
                // tabs render as spaces through the font; good enough for code
                if (!c->line_items) line_begin(c, ascent(st->size) + descent(st->size));
                int w = text_w(s + i, j - i, st);
                emit_text(c, s + i, j - i, c->lx, w, st);
            }
            if (j < len) {
                if (!c->line_items && !c->measure) {  // an empty line keeps its height
                    c->y += c->margin + st->size * 134 / 100;
                    c->margin = 0;
                }
                flush_line(c);
            }
            i = j + 1;
        }
        return;
    }
    int ascii = 1;
    for (int i = 0; i < len; i++)
        if ((u8)s[i] >= 0x80) { ascii = 0; break; }
    if (!ascii && !t->brk) {
        t->brk = (u8 *)arena((u32)len);
        lw_line_breaks(s, len, t->brk);
    }
    int i = 0;
    while (i < len) {
        if (s[i] == ' ') { c->space = 1; i++; continue; }
        int j = i + 1;
        while (j < len && s[j] != ' ' && !(t->brk && t->brk[j])) j++;
        // Break opportunities come per text node; where a node starts with Thai/CJK (no
        // spaces between words), the node boundary is almost always a word boundary too.
        int can_break = c->brk || (t->brk && t->brk[i]) || (i == 0 && (u8)s[0] >= 0xE0);
        place_segment(c, s + i, j - i, st, can_break);
        i = j;
    }
}

// Inline box (image, form control).
static Item *place_box(Ctx *c, int w, int h, int kind, const Style *st) {
    if (!c->line_items) line_begin(c, h);
    int sw = c->space && c->line_items ? space_w(st) : 0;
    if (c->line_items && c->lx + sw + w > c->w) { flush_line(c); sw = 0; }
    if (!c->line_items) { sw = 0; line_begin(c, h); }
    int x = c->lx + sw;
    c->lx = x + w;
    c->line_items++;
    c->space = 0;
    if (w > c->min_w) c->min_w = w;
    if (c->measure) return 0;
    Item *it = new_item(kind);
    it->x = c->x + x;
    it->w = w;
    it->h = h;
    it->asc = h;
    it->link = st->link;
    return it;
}

// ---- images --------------------------------------------------------------------------

enum { IMG_NEW, IMG_QUEUED, IMG_LOADING, IMG_READY, IMG_FAILED };
typedef struct {
    const char *url;
    int state, fetch_id;
    int nat_w, nat_h;  // natural size (CSS px)
    int pw, ph;        // stored pixels (may be downscaled)
    u32 *px;
} Img;
static Img *imgs;
static int nimgs, imgs_cap, images_changed;

static int b64v(u8 c) {
    return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : is_digit(c) ? c - '0' + 52 : c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1;
}

static void image_from_bytes(Img *im, const u8 *data, int len) {
    int w = 0, h = 0;
    int handle = lw_image_decode(data, len, &w, &h);
    if (!handle) { im->state = IMG_FAILED; return; }
    u32 *px = (u32 *)mem_alloc((u32)w * (u32)h * 4);
    if (!px) { lw_image_free(handle); im->state = IMG_FAILED; return; }
    lw_image_read(handle, px);
    im->nat_w = w;
    im->nat_h = h;
    int k = 1;  // keep at most ~2 Mpx per image
    while ((u64)(w / k) * (u64)(h / k) > 2000000ull) k++;
    if (k > 1) {
        int pw = w / k, ph = h / k;
        u32 *small = (u32 *)mem_alloc((u32)pw * (u32)ph * 4);
        u32 *to = small ? small : px;
        for (int y = 0; y < ph; y++)
            for (int x = 0; x < pw; x++) to[y * pw + x] = px[(y * k) * w + x * k];
        if (small) {
            mem_free(px);
            px = small;
        }
        im->pw = pw;
        im->ph = ph;
    } else {
        im->pw = w;
        im->ph = h;
    }
    im->px = px;
    im->state = IMG_READY;
}

static int media_matches(const char *s, const char *e);  // css.c
static int hexv(char c);

// Image formats the browser cannot decode (it reads PNG, JPEG, GIF, WebP, SVG and BMP).
static int undecodable_url(const char *u, int len) {
    int q = 0;
    while (q < len && u[q] != '?' && u[q] != '#') q++;
    return (q >= 5 && iprefix(u + q - 5, 5, ".avif")) || (q >= 4 && iprefix(u + q - 4, 4, ".jxl"));
}

// Picks a URL from a srcset: the largest width descriptor up to ~800px (or 1x for
// density descriptors), avoiding formats we cannot decode.
static const char *pick_srcset(const char *set) {
    const char *best = 0;
    int best_len = 0, best_score = -1000000;
    const char *p = set;
    while (*p) {
        while (*p && (is_space((u8)*p) || *p == ',')) p++;
        const char *u = p;
        while (*p && !is_space((u8)*p) && !(*p == ',' && (p[1] == ' ' || !p[1]))) p++;
        int ul = (int)(p - u);
        while (*p && is_space((u8)*p)) p++;
        int w = 0, x10 = 10;
        if (is_digit((u8)*p)) {
            int v = parse_int(p, 0);
            while (is_digit((u8)*p) || *p == '.') p++;
            if (*p == 'w') w = v;
            else if (*p == 'x') x10 = v * 10;
        }
        while (*p && *p != ',') p++;
        if (!ul) continue;
        int score = w ? (w <= 800 ? w : 800 - (w - 800)) : 1000 - (x10 > 10 ? x10 : 0);
        if (undecodable_url(u, ul)) score -= 100000;
        if (score > best_score) { best = u; best_len = ul; best_score = score; }
    }
    return best ? arena_str(best, best_len) : 0;
}

static int decodable_type(const char *t) {
    return !t || !*t || ieq(t, "image/jpeg") || ieq(t, "image/jpg") || ieq(t, "image/png") || ieq(t, "image/gif") ||
           ieq(t, "image/bmp") || ieq(t, "image/webp") || ieq(t, "image/svg+xml");
}

// The image at this URL (in imgs), made (queued for fetching) if there is none yet.
static int img_for_url(const char *src) {
    if (src && *src)  // the same picture used again (icons, bullets): fetch and decode it once
        for (int i = 0; i < nimgs; i++) {
            const char *a = imgs[i].url, *b = src;
            if (!a) continue;
            while (*a && *a == *b) a++, b++;
            if (*a == *b) return i;
        }
    imgs = (Img *)grow_array(imgs, nimgs, &imgs_cap, sizeof(Img));
    Img *im = &imgs[nimgs];
    __builtin_memset(im, 0, sizeof *im);
    im->url = src;
    im->state = IMG_QUEUED;
    int len = src ? lw_strlen(src) : 0;
    if (!len) im->state = IMG_FAILED;
    else if (iprefix(src, len, "data:")) {  // data: URI, decoded right here
        const char *comma = src;
        while (*comma && *comma != ',') comma++;
        int b64 = 0;
        for (const char *p = src; p < comma; p++)
            if (iprefix(p, (int)(comma - p), ";base64")) b64 = 1;
        im->state = IMG_FAILED;
        if (*comma && !b64) {  // data:image/svg+xml,%3Csvg... (percent-encoded)
            const char *d = comma + 1;
            int dl = lw_strlen(d), k = 0;
            u8 *buf = (u8 *)mem_alloc((u32)dl + 1);
            if (buf) {
                for (int i = 0; i < dl; i++) {
                    if (d[i] == '%' && i + 2 < dl && hexv(d[i + 1]) >= 0 && hexv(d[i + 2]) >= 0) {
                        buf[k++] = (u8)(hexv(d[i + 1]) * 16 + hexv(d[i + 2]));
                        i += 2;
                    } else {
                        buf[k++] = (u8)d[i];
                    }
                }
                image_from_bytes(im, buf, k);
                mem_free(buf);
            }
        }
        if (*comma && b64) {
            const char *d = comma + 1;
            int dl = lw_strlen(d);
            u8 *buf = (u8 *)mem_alloc((u32)dl * 3 / 4 + 4);
            if (buf) {
                int k = 0, bits = 0;
                u32 acc = 0;
                for (int i = 0; i < dl; i++) {
                    int v = b64v((u8)d[i]);
                    if (v < 0) continue;
                    acc = acc << 6 | (u32)v;
                    bits += 6;
                    if (bits >= 8) { bits -= 8; buf[k++] = (u8)(acc >> bits); }
                }
                image_from_bytes(im, buf, k);
                mem_free(buf);
            }
        }
    } else if (undecodable_url(src, len)) {
        im->state = IMG_FAILED;
    }
    return nimgs++;
}

static int image_for(Node *n) {
    if (n->ref >= 0) return n->ref;
    const char *src = 0;
    if (n->parent && n->parent->tag == T_PICTURE) {  // <picture>: first <source> we can decode
        for (Node *c = n->parent->first; c && c != n && !src; c = c->next) {
            if (c->type != N_ELEM || c->tag != T_SOURCE || !decodable_type(attr(c, "type"))) continue;
            const char *media = attr(c, "media");
            if (media && !media_matches(media, media + lw_strlen(media))) continue;
            const char *set = attr(c, "srcset");
            if (set && *set) src = pick_srcset(set);
            if (src && undecodable_url(src, lw_strlen(src))) src = 0;
        }
    }
    if (!src) src = attr(n, "src");
    const char *lazy = attr(n, "data-src");
    if (lazy && *lazy && (!src || !*src || iprefix(src, lw_strlen(src), "data:"))) src = lazy;
    const char *set = attr(n, "srcset");
    if (set && *set && (!src || !*src || undecodable_url(src, lw_strlen(src)))) {
        const char *alt = pick_srcset(set);
        if (alt && (!src || !*src || !undecodable_url(alt, lw_strlen(alt)))) src = alt;
    }
    return n->ref = img_for_url(src);
}


static int in_flight;
static void pump_images(void) {
    for (int i = 0; i < nimgs && in_flight < 6; i++) {
        if (imgs[i].state != IMG_QUEUED) continue;
        imgs[i].state = IMG_LOADING;
        imgs[i].fetch_id = lw_fetch(imgs[i].url, lw_strlen(imgs[i].url));
        in_flight++;
    }
}

// Writes a subtree back out as XML (inline <svg> becomes an SVG image).
typedef struct { char *buf; int len, cap; } Sbuf;
static void sb_put(Sbuf *b, const char *s, int n) {
    if (b->len + n + 1 > b->cap) {
        int cap = MAX(b->cap * 2, b->len + n + 1024);
        char *nb = (char *)must_alloc((u32)cap);
        if (b->buf) { __builtin_memcpy(nb, b->buf, (u32)b->len); mem_free(b->buf); }
        b->buf = nb;
        b->cap = cap;
    }
    __builtin_memcpy(b->buf + b->len, s, (u32)n);
    b->len += n;
}
static void sb_escaped(Sbuf *b, const char *s, int n) {
    for (int i = 0; i < n; i++) {
        char ch = s[i];
        if (ch == '&') sb_put(b, "&amp;", 5);
        else if (ch == '<') sb_put(b, "&lt;", 4);
        else if (ch == '"') sb_put(b, "&quot;", 6);
        else sb_put(b, &ch, 1);
    }
}
static void serialize(Node *n, Sbuf *b, int depth) {
    if (n->type == N_TEXT) { sb_escaped(b, n->text, (int)n->len); return; }
    if (n->type != N_ELEM || depth > 200) return;
    const char *name = n->tag == T_UNKNOWN ? n->name : tag_names[n->tag];
    if (n->tag == T_IMG) name = "image";
    sb_put(b, "<", 1);
    sb_put(b, name, lw_strlen(name));
    for (int i = 0; i < n->nattr; i++) {
        sb_put(b, " ", 1);
        sb_put(b, n->attr[2 * i], lw_strlen(n->attr[2 * i]));
        sb_put(b, "=\"", 2);
        sb_escaped(b, n->attr[2 * i + 1], lw_strlen(n->attr[2 * i + 1]));
        sb_put(b, "\"", 1);
    }
    sb_put(b, ">", 1);
    for (Node *c = n->first; c; c = c->next) serialize(c, b, depth + 1);
    sb_put(b, "</", 2);
    sb_put(b, name, lw_strlen(name));
    sb_put(b, ">", 1);
}

static void flow_inline_svg(Ctx *c, Node *n, const Style *st) {
    if (n->ref < 0) {
        Sbuf b = {0, 0, 0};
        serialize(n, &b, 0);
        imgs = (Img *)grow_array(imgs, nimgs, &imgs_cap, sizeof(Img));
        Img *im = &imgs[nimgs];
        __builtin_memset(im, 0, sizeof *im);
        im->state = IMG_FAILED;
        if (b.buf) image_from_bytes(im, (const u8 *)b.buf, b.len);
        mem_free(b.buf);
        n->ref = nimgs++;
    }
    Img *im = &imgs[n->ref];
    if (im->state != IMG_READY) return;
    int pct = 0;
    int aw = parse_int(attr(n, "width"), &pct);
    if (pct) aw = -1;
    int ah = parse_int(attr(n, "height"), &pct);
    if (pct) ah = -1;
    int w, h;
    if (aw > 0 && ah > 0) { w = aw; h = ah; }
    else if (aw > 0) { w = aw; h = aw * im->nat_h / MAX(im->nat_w, 1); }
    else if (ah > 0) { h = ah; w = ah * im->nat_w / MAX(im->nat_h, 1); }
    else {  // sized by CSS we don't run: keep icons icon-sized (at most two lines tall)
        w = im->nat_w;
        h = im->nat_h;
        int max_h = (int)(st->size * 2 / S);
        if (h > max_h) { w = w * max_h / MAX(h, 1); h = max_h; }
    }
    w = (int)(w * S + 0.5f);
    h = (int)(h * S + 0.5f);
    if (w > c->w && c->w > 0) { h = (int)((u64)h * (u64)c->w / (u64)w); w = c->w; }
    if (w <= 0 || h <= 0) return;
    Item *it = place_box(c, w, h, IT_IMAGE, st);
    if (it) it->ref = n->ref;
}

static int pct_max_width(Node *n);  // (after css.c)

static void flow_image(Ctx *c, Node *n, const Style *st) {
    Img *im = &imgs[image_for(n)];
    int pct = 0;
    int aw = parse_int(attr(n, "width"), &pct);
    if (pct) aw = aw * c->w / 100 * 100 / (int)(S * 100);  // percent of the line, back to CSS px
    int ah = parse_int(attr(n, "height"), &pct);
    if (pct) ah = -1;
    int w, h;
    int ready = im->state == IMG_READY;
    if (aw > 0 && ah > 0) { w = aw; h = ah; }
    else if (aw > 0) { w = aw; h = ready ? aw * im->nat_h / MAX(im->nat_w, 1) : 0; }
    else if (ah > 0) { h = ah; w = ready ? ah * im->nat_w / MAX(im->nat_h, 1) : 0; }
    else if (ready) { w = im->nat_w; h = im->nat_h; }
    else w = h = 0;
    w = (int)(w * S + 0.5f);
    h = (int)(h * S + 0.5f);
    if (w > c->w && c->w > 0) { h = (int)((u64)h * (u64)c->w / (u64)w); w = c->w; }
    if (!w || !h) {
        if (im->state == IMG_FAILED) {  // show the alt text instead
            const char *alt = attr(n, "alt");
            if (alt && *alt) {
                Style s2 = *st;
                s2.flags |= LW_TEXT_ITALIC;
                if (s2.link < 0) s2.color = C_MUTED;
                Node tmp;
                __builtin_memset(&tmp, 0, sizeof tmp);
                tmp.type = N_TEXT;
                tmp.text = alt;
                tmp.len = (u32)lw_strlen(alt);
                c->space = 1;
                flow_text(c, &tmp, &s2);
                c->space = 1;
            }
        }
        return;
    }
    int min_before = c->min_w;
    Item *it = place_box(c, w, h, IT_IMAGE, st);
    if (it) it->ref = n->ref;
    if (c->measure && pct_max_width(n)) c->min_w = min_before;  // it can be as narrow as the room: it needs none
}

// ---- video and audio ------------------------------------------------------------------
// <video>, and <audio controls>: the picture (the poster until it plays) with a bar of
// controls under it. Nothing is downloaded until the user clicks play (no autoplay: it
// costs data and memory); then the browser plays it (lw_video_*) and draws its pictures
// into our frame itself, where draw_video_item says (lw_video_place). The bar is under the
// picture, not on it, since the browser's pictures cover whatever we draw there.

typedef struct {
    Node *node;
    int audio;           // <audio>: the bar alone
    int poster;          // imgs index, -1 = none
    int handle;          // lw_video_open's; 0 = not opened yet
    int state, waiting, muted;
    double t, dur, buf;  // seconds: where it is, how long, how far the data goes on from t
    int vw, vh;          // the picture's size, once known
    int laid_vw;         // the vw the layout used (it lays out again when it changes)
    int shown;           // what the bar shows, in short: it is drawn again when this changes
    int visible;         // drawn in the last frame: item `item` at `screen_y`
    int item, screen_y;
    const char *error;   // FAILED: why
} Video;
static Video *videos;
static int nvideos, videos_cap, video_focus = -1, video_drag = -1, bars_dirty;
// Fullscreen: one video fills the view (and the browser the screen), drawn as full_item.
static int video_full = -1;
static Item full_item;

static int video_for(Node *n) {
    if (n->ref >= 0) return n->ref;
    videos = (Video *)grow_array(videos, nvideos, &videos_cap, sizeof(Video));
    Video *v = &videos[nvideos];
    __builtin_memset(v, 0, sizeof *v);
    v->node = n;
    v->audio = n->tag == T_AUDIO;
    v->poster = -1;
    v->state = LW_VIDEO_PAUSED;
    const char *p = attr(n, "poster");
    if (p && *p && !v->audio) v->poster = img_for_url(p);  // (fetched with the page's images)
    return n->ref = nvideos++;
}

// Kinds Windows plays as it comes (MP4/H.264, AAC, MP3, WAV).
static int common_media(const char *t) {
    static const char *const ok[] = {"video/mp4", "audio/mp4", "audio/mpeg", "audio/mp3", "audio/aac", "audio/wav", "audio/x-wav",
                                     "audio/wave", "video/quicktime", "audio/x-m4a", "video/x-m4v"};
    int len = lw_strlen(t);
    for (int i = 0; i < (int)(sizeof ok / sizeof ok[0]); i++)
        if (iprefix(t, len, ok[i])) return 1;
    return 0;
}

// What to play: src, else the first <source> of a common kind, else any <source> but a
// streaming playlist (HLS, DASH: those need JavaScript); 0 if there is nothing.
static const char *video_src(Node *n) {
    const char *s = attr(n, "src");
    if (s && *s) return s;
    const char *other = 0;
    for (Node *c = n->first; c; c = c->next) {
        if (c->type != N_ELEM || c->tag != T_SOURCE) continue;
        const char *u = attr(c, "src"), *t = attr(c, "type");
        if (!u || !*u) continue;
        if (!t || !*t || common_media(t)) return u;
        if (!other && !iprefix(t, lw_strlen(t), "application/")) other = u;
    }
    return other;
}

#define VIDEO_BAR ((int)(36 * S))
static int video_css_width(Node *n, int of, int em);  // (after css.c)

static void flow_video(Ctx *c, Node *n, const Style *st) {
    int vi = video_for(n);
    Video *v = &videos[vi];
    if (v->audio && !attr(n, "controls")) return;  // an <audio> without controls shows nothing
    int avail = c->measure ? -1 : c->w;
    int cw = video_css_width(n, avail, st->size), w, ph = 0;
    if (v->audio) {
        w = cw > 0 ? cw : (int)(300 * S);
    } else {
        int nw = v->vw, nh = v->vh;  // its shape: the video's, else the poster's, else 16:9
        Img *po = v->poster >= 0 ? &imgs[v->poster] : 0;
        if ((!nw || !nh) && po && po->state == IMG_READY) { nw = po->nat_w; nh = po->nat_h; }
        if (!nw || !nh) { nw = 640; nh = 360; }
        v->laid_vw = v->vw;
        int pct = 0;
        int aw = parse_int(attr(n, "width"), &pct);
        if (pct) aw = avail > 0 ? (int)(aw * avail / 100 / S) : -1;  // (CSS px)
        int ah = parse_int(attr(n, "height"), &pct);
        if (pct) ah = -1;
        w = cw > 0 ? cw : aw > 0 ? (int)(aw * S) : ah > 0 ? (int)((long long)ah * nw / nh * S) : (int)(nw * S);
        ph = aw > 0 && ah > 0 ? (int)((long long)w * ah / aw) : (int)((long long)w * nh / nw);
    }
    if (avail > 0 && w > avail) { ph = (int)((long long)ph * avail / w); w = avail; }
    if (w < 1) w = 1;
    int min_before = c->min_w;
    Item *it = place_box(c, w, ph + VIDEO_BAR, IT_VIDEO, st);
    if (it) { it->ref = vi; it->link = -1; }
    if (c->measure) c->min_w = MAX(min_before, MIN(w, (int)(200 * S)));  // it can shrink
}

// ---- form controls --------------------------------------------------------------------

enum { K_TEXT, K_PASSWORD, K_SUBMIT, K_RESET, K_BUTTON, K_CHECKBOX, K_RADIO, K_HIDDEN, K_SELECT, K_TEXTAREA, K_IMAGE, K_OTHER };
typedef struct {
    Node *node, *form;
    int kind, checked, disabled;
    const char *name;
    char *value;        // editable, UTF-8
    int vlen, vcap;
    const char *label;  // buttons, selects: text shown
    int label_len;
    int option;         // select: chosen option index
} Control;
static Control *controls;
static int ncontrols, controls_cap, focus = -1;
#define C_SELECTION RGB(179, 215, 255)

static int text_content(Node *n, char *out, int cap, int k) {
    for (Node *c = n->first; c; c = c->next) {
        if (c->type == N_TEXT)
            for (u32 i = 0; i < c->len && k < cap; i++) out[k++] = c->text[i];
        else if (c->type == N_ELEM) k = text_content(c, out, cap, k);
    }
    return k;
}

static Node *nth_option(Node *sel, int want, int *count) {
    Node *found = 0;
    int k = 0;
    for (Node *c = sel->first; c; c = c->next) {
        if (c->type != N_ELEM) continue;
        if (c->tag == T_OPTGROUP) {
            for (Node *o = c->first; o; o = o->next)
                if (o->type == N_ELEM && o->tag == T_OPTION) { if (k == want) found = o; k++; }
        } else if (c->tag == T_OPTION) {
            if (k == want) found = c;
            k++;
        }
    }
    if (count) *count = k;
    return found;
}

static void select_label(Control *k) {
    Node *o = nth_option(k->node, k->option, 0);
    char buf[256];
    int n = o ? text_content(o, buf, sizeof buf, 0) : 0;
    while (n && buf[n - 1] == ' ') n--;
    int s = 0;
    while (s < n && buf[s] == ' ') s++;
    k->label = arena_str(buf + s, n - s);
    k->label_len = n - s;
}

static void set_value(Control *k, const char *v, int len) {
    if (len + 1 > k->vcap) {
        int cap = MAX(len + 1, 64);
        char *nv = (char *)must_alloc((u32)cap);
        mem_free(k->value);
        k->value = nv;
        k->vcap = cap;
    }
    __builtin_memcpy(k->value, v, (u32)len);
    k->vlen = len;
}

// Makes the form control for a finished <input>, <button>, <select> or <textarea>.
static void make_control(Node *c) {
    Node *form = 0;
    for (Node *p = c->parent; p; p = p->parent) {
        if (p->type != N_ELEM) continue;
        if (p->tag == T_SELECT || p->tag == T_TEXTAREA || p->tag == T_BUTTON) return;  // not inside another control
        if (!form && p->tag == T_FORM) form = p;
    }
    {
        int kind = -1;
        if (c->tag == T_INPUT) {
            const char *t = attr(c, "type");
            kind = !t || ieq(t, "text") || ieq(t, "search") || ieq(t, "email") || ieq(t, "url") || ieq(t, "tel") || ieq(t, "number") ? K_TEXT
                 : ieq(t, "password") ? K_PASSWORD : ieq(t, "submit") ? K_SUBMIT : ieq(t, "reset") ? K_RESET
                 : ieq(t, "button") ? K_BUTTON : ieq(t, "checkbox") ? K_CHECKBOX : ieq(t, "radio") ? K_RADIO
                 : ieq(t, "hidden") ? K_HIDDEN : ieq(t, "image") ? K_IMAGE : K_OTHER;
        } else if (c->tag == T_BUTTON) {
            const char *t = attr(c, "type");
            kind = !t || ieq(t, "submit") ? K_SUBMIT : ieq(t, "reset") ? K_RESET : K_BUTTON;
        } else if (c->tag == T_SELECT) kind = K_SELECT;
        else if (c->tag == T_TEXTAREA) kind = K_TEXTAREA;
        if (kind >= 0) {
            controls = (Control *)grow_array(controls, ncontrols, &controls_cap, sizeof(Control));
            Control *k = &controls[ncontrols];
            __builtin_memset(k, 0, sizeof *k);
            k->node = c;
            k->form = form;
            k->kind = kind;
            k->name = attr(c, "name");
            k->disabled = attr(c, "disabled") != 0;
            k->checked = attr(c, "checked") != 0;
            const char *v = attr(c, "value");
            if (kind == K_TEXTAREA) {
                char buf[4096];
                int n2 = text_content(c, buf, sizeof buf, 0);
                set_value(k, buf, n2);
            } else if (v) set_value(k, v, lw_strlen(v));
            else set_value(k, "", 0);
            if (kind == K_SUBMIT || kind == K_RESET || kind == K_BUTTON) {
                if (c->tag == T_BUTTON) {
                    char buf[256];
                    int n2 = text_content(c, buf, sizeof buf, 0);
                    int s = 0;
                    while (s < n2 && buf[s] == ' ') s++;
                    while (n2 > s && buf[n2 - 1] == ' ') n2--;
                    k->label = arena_str(buf + s, n2 - s);
                    k->label_len = n2 - s;
                } else {
                    k->label = k->vlen ? k->value : kind == K_RESET ? "Reset" : "Submit";
                    k->label_len = k->vlen ? k->vlen : lw_strlen(k->label);
                    if (k->vlen) k->label = arena_str(k->value, k->vlen);
                }
                if (!k->label_len) { k->label = "Submit"; k->label_len = 6; }
            }
            if (kind == K_SELECT) {
                int count = 0;
                nth_option(c, 0, &count);
                for (int i = 0; i < count; i++)
                    if (attr(nth_option(c, i, 0), "selected")) k->option = i;
                select_label(k);
            }
            c->ref = ncontrols++;
        }
    }
}

static void flow_control(Ctx *c, Node *n, const Style *st) {
    if (n->ref < 0) return;
    Control *k = &controls[n->ref];
    Style cs = *st;
    cs.size = (short)(14 * S);
    cs.flags = 0;
    int w, h = (int)(26 * S);
    switch (k->kind) {
    case K_HIDDEN: case K_OTHER: return;
    case K_CHECKBOX: case K_RADIO: w = h = (int)(15 * S); break;
    case K_SUBMIT: case K_RESET: case K_BUTTON: case K_IMAGE:
        w = text_w(k->label ? k->label : "Go", k->label ? k->label_len : 2, &cs) + (int)(24 * S);
        break;
    case K_SELECT: w = text_w(k->label, k->label_len, &cs) + (int)(34 * S); break;
    case K_TEXTAREA: {
        int cols = parse_int(attr(n, "cols"), 0), rows = parse_int(attr(n, "rows"), 0);
        w = (int)((cols > 0 ? cols : 40) * 8 * S);
        h = (int)((rows > 0 ? rows : 3) * 18 * S + 8 * S);
        break;
    }
    default: {
        int size = parse_int(attr(n, "size"), 0);
        w = (int)((size > 0 ? MIN(size * 8 + 16, 600) : 200) * S);
    }
    }
    if (w > c->w) w = MAX(c->w, (int)(20 * S));
    Item *it = place_box(c, w, h, IT_CONTROL, st);
    if (it) {
        it->ref = n->ref;
        it->asc = h - (int)(6 * S);  // sit slightly below the baseline, like text
        if (it->asc < 0) it->asc = h;
    }
}

#include "css.c"

// ---- styles from tags, attributes and inline style ----------------------------------

static int is_hidden(Node *n) {
    if (css_hidden(n)) return 1;
    if (attr(n, "hidden")) return 1;
    if (n->tag == T_DIALOG && !attr(n, "open")) return 1;
    const char *s = attr(n, "style");
    if (!s) return 0;
    for (const char *p = s; *p; p++) {
        if (iprefix(p, lw_strlen(p), "display")) {
            const char *q = p + 7;
            while (*q == ' ' || *q == ':') q++;
            if (iprefix(q, lw_strlen(q), "none")) return 1;
        }
        if (iprefix(p, lw_strlen(p), "visibility")) {
            const char *q = p + 10;
            while (*q == ' ' || *q == ':') q++;
            if (iprefix(q, lw_strlen(q), "hidden")) return 1;
        }
    }
    return 0;
}

// Applies the few inline style properties we understand. *bg receives a background.
static void inline_style(Node *n, Style *st, u32 *bg) {
    const char *s = attr(n, "style");
    if (!s) return;
    while (*s) {
        while (*s == ' ' || *s == ';') s++;
        const char *k = s;
        while (*s && *s != ':' && *s != ';') s++;
        int kl = (int)(s - k);
        while (kl && k[kl - 1] == ' ') kl--;
        if (*s != ':') continue;
        s++;
        while (*s == ' ') s++;
        const char *v = s;
        while (*s && *s != ';') s++;
        int vl = (int)(s - v);
        while (vl && v[vl - 1] == ' ') vl--;
        u32 col;
        if (kl == 5 && iprefix(k, kl, "color")) { if (parse_color(v, vl, &col)) st->color = col; }
        else if ((kl == 16 && iprefix(k, kl, "background-color")) || (kl == 10 && iprefix(k, kl, "background"))) {
            if (bg && parse_color(v, vl, &col)) *bg = col;
        } else if (kl == 11 && iprefix(k, kl, "font-weight")) {
            if (iprefix(v, vl, "bold") || (is_digit((u8)v[0]) && v[0] >= '6')) st->flags |= LW_TEXT_BOLD;
            else if (iprefix(v, vl, "normal") || iprefix(v, vl, "400")) st->flags &= (u8)~LW_TEXT_BOLD;
        } else if (kl == 10 && iprefix(k, kl, "font-style")) {
            if (iprefix(v, vl, "italic") || iprefix(v, vl, "oblique")) st->flags |= LW_TEXT_ITALIC;
        } else if (kl == 10 && iprefix(k, kl, "text-align")) {
            st->align = iprefix(v, vl, "center") ? 1 : iprefix(v, vl, "right") ? 2 : 0;
        } else if (kl == 9 && iprefix(k, kl, "font-size")) {
            int px = parse_int(v, 0);
            int i = 0;
            while (i < vl && (is_digit((u8)v[i]) || v[i] == '.')) i++;
            if (px > 0 && iprefix(v + i, vl - i, "px")) st->size = (short)MIN(px * S, 96 * S);
            else if (px > 0 && iprefix(v + i, vl - i, "pt")) st->size = (short)MIN(px * 4 / 3 * S, 96 * S);
            else if (px > 0 && (iprefix(v + i, vl - i, "em") || iprefix(v + i, vl - i, "rem"))) st->size = (short)MIN(st->size * px, 96 * S);
            else if (px > 0 && iprefix(v + i, vl - i, "%")) st->size = (short)MIN(st->size * px / 100, 96 * S);
        } else if (kl == 11 && iprefix(k, kl, "font-family")) {
            for (int i = 0; i + 9 <= vl; i++)
                if (iprefix(v + i, vl - i, "monospace") || iprefix(v + i, vl - i, "courier")) st->flags |= LW_TEXT_MONO;
        } else if (kl == 11 && iprefix(k, kl, "white-space")) {
            if (iprefix(v, vl, "nowrap")) st->nowrap = 1;
            else if (iprefix(v, vl, "pre")) st->pre = 1;
        } else if (kl == 15 && iprefix(k, kl, "text-decoration")) {
            if (iprefix(v, vl, "underline")) st->deco |= 1;
            if (iprefix(v, vl, "line-through")) st->deco |= 2;
        }
    }
}

static void align_attr(Node *n, Style *st) {
    const char *a = attr(n, "align");
    if (!a) return;
    if (ieq(a, "center") || ieq(a, "middle")) st->align = 1;
    else if (ieq(a, "right")) st->align = 2;
    else if (ieq(a, "left") || ieq(a, "justify")) st->align = 0;
}

// ---- lists ------------------------------------------------------------------------------

static int list_depth;
static int list_kind[32], list_count[32];  // kind: 0 bullet, 1 decimal, 2 lower-alpha, 3 upper-alpha, 4 none

static void set_marker(int right, const Style *st) {
    int d = list_depth - 1;
    if (d < 0 || list_kind[d] == 4) return;
    int n = 0;
    if (list_kind[d] == 0) {
        static const char *bullets[3] = {"\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA"};  // • ◦ ▪
        const char *b = bullets[d % 3];
        while (b[n]) { marker_text[n] = b[n]; n++; }
    } else {
        int v = list_count[d];
        if (list_kind[d] == 1 || v <= 0 || v > 26) {
            char tmp[12];
            int t = 0, neg = v < 0;
            unsigned uv = (unsigned)(neg ? -v : v);
            do { tmp[t++] = (char)('0' + uv % 10); uv /= 10; } while (uv);
            if (neg) marker_text[n++] = '-';
            while (t) marker_text[n++] = tmp[--t];
        } else {
            marker_text[n++] = (char)((list_kind[d] == 2 ? 'a' : 'A') + v - 1);
        }
        marker_text[n++] = '.';
    }
    marker_len = n;
    marker_right = right;
    marker_style = *st;
    marker_style.link = -1;
    marker_style.deco = 0;
    marker_pending = 1;
}

// ---- blocks ----------------------------------------------------------------------------

static void flow(Node *n, Ctx *c, const Style *parent);

static void flow_children(Node *n, Ctx *c, const Style *st) {
    for (Node *k = n->first; k; k = k->next) flow(k, c, st);
}

static void begin_block(Ctx *c) {
    flush_line(c);
}

// A block whose children are being laid out. Usually that happens right away, by
// recursion; the incremental layout (layout_step) keeps the outer blocks on this stack
// instead, so it can stop between two children and carry on in the next frame.
typedef struct {
    Node *n;               // the block (the root frame: the document root, or none)
    Node *last;            // its last child laid out so far
    Node *single;          // root frame in reader view: lay out just this node
    Ctx *c;                // the parent's context
    Ctx sub;
    Style st;
    u32 bg;
    int mb, ml, mr, pad, bg_item, post;
} Frame;
#define MAX_FRAMES 96
static Frame frames[MAX_FRAMES];
static int nframes;
static int defer_block;  // set by layout_step: the next block() goes on the frame stack

enum { POST_NONE, POST_LIST, POST_LI };  // what a block does when it ends

static void close_block(Frame *f) {
    Ctx *c = f->c, *sub = &f->sub;
    flush_line(sub);
    if (f->bg >> 24) {
        sub->y += sub->margin + f->pad;
        sub->margin = 0;
        if (f->bg_item >= 0) items[f->bg_item].h = sub->y - items[f->bg_item].y;
    }
    c->y = sub->y;
    c->margin = MAX(sub->margin, f->mb);
    int extra = f->ml + f->mr + ((f->bg >> 24) ? 2 * f->pad : 0);
    c->max_w = MAX(c->max_w, sub->max_w + extra);
    c->min_w = MAX(c->min_w, sub->min_w + extra);
    c->line_start = nitems;
    c->lx = 0; c->line_items = 0; c->space = 0; c->brk = 0;
    if (f->post == POST_LIST) list_depth--;
    else if (f->post == POST_LI) marker_pending = 0;
}

// Lays out n's children as a block. Margins in device px.
static void block(Node *n, Ctx *c, const Style *st, int mt, int mb, int ml, int mr, u32 bg, int pad, int post) {
    int defer = defer_block && nframes < MAX_FRAMES;
    defer_block = 0;
    Frame local;
    Frame *f = defer ? &frames[nframes++] : &local;
    __builtin_memset(f, 0, sizeof *f);
    f->n = n; f->c = c; f->st = *st; f->bg = bg;
    f->mb = mb; f->ml = ml; f->mr = mr; f->pad = pad; f->post = post;
    f->bg_item = -1;
    begin_block(c);
    c->margin = MAX(c->margin, mt);
    Ctx *sub = &f->sub;
    sub->x = c->x + ml;
    sub->w = MAX(c->w - ml - mr, (int)(8 * S));
    sub->y = c->y;
    sub->margin = c->margin;
    sub->measure = c->measure;
    sub->align = st->align;
    sub->line_start = nitems;
    sub->bfc = c->bfc;
    c->margin = 0;
    if (bg >> 24) {
        if (!c->measure) {
            sub->y += sub->margin;
            sub->margin = 0;
            Item *r = new_item(IT_RECT);
            r->x = sub->x; r->y = sub->y; r->w = sub->w; r->color = bg;
            f->bg_item = nitems - 1;
            sub->line_start = nitems;
        }
        sub->y += pad;
        sub->x += pad;
        sub->w = MAX(sub->w - 2 * pad, (int)(8 * S));
    }
    n->y = sub->y + sub->margin;
    if (defer) return;  // layout_step lays out the children
    flow_children(n, sub, st);
    close_block(f);
}

// ---- tables ------------------------------------------------------------------------------

#define MAX_COLS 64
typedef struct { Node *cell; int col, span; } Cell;

static int table_rows(Node *t, Node **rows, int max) {
    int n = 0;
    for (Node *c = t->first; c && n < max; c = c->next) {
        if (c->type != N_ELEM) continue;
        if (c->tag == T_TR) rows[n++] = c;
        else if (c->tag == T_THEAD || c->tag == T_TBODY || c->tag == T_TFOOT || c->tag == T_FORM)
            for (Node *r = c->first; r && n < max; r = r->next)
                if (r->type == N_ELEM && r->tag == T_TR) rows[n++] = r;
    }
    return n;
}

static int span_of(Node *cell) {
    int s = parse_int(attr(cell, "colspan"), 0);
    return s < 1 ? 1 : s > MAX_COLS ? MAX_COLS : s;
}

static Style cell_style(Node *cell, Node *row, const Style *st) {
    Style cs = *st;
    if (cell->tag == T_TH) { cs.flags |= LW_TEXT_BOLD; cs.align = 1; }
    align_attr(row, &cs);
    align_attr(cell, &cs);
    const char *nw = attr(cell, "nowrap");
    if (nw) cs.nowrap = 1;
    return cs;
}

static void measure_cell(Node *cell, Node *row, const Style *st, int *mn, int *mx) {
    Style cs = cell_style(cell, row, st);
    inline_style(cell, &cs, 0);
    Ctx m;
    __builtin_memset(&m, 0, sizeof m);
    m.w = 1 << 24;
    m.measure = 1;
    flow_children(cell, &m, &cs);
    flush_line(&m);
    *mn = m.min_w;
    *mx = m.max_w;
}

static void flow_table_at(Node *t, Ctx *c, const Style *st);

// A table beside floats takes the room left there if that is at least half the width,
// else it goes below them.
static void flow_table(Node *t, Ctx *c, const Style *st) {
    begin_block(c);
    if (c->measure || !nfloats) { flow_table_at(t, c, st); return; }
    int l, r, x = c->x, w = c->w;
    if (float_band(c->bfc, c->y + c->margin, (int)(20 * S), c->x, c->x + c->w, &l, &r) >= 0) {
        if (r - l >= c->w / 2) { c->x = l; c->w = r - l; }
        else clear_floats(c, 3);
    }
    flow_table_at(t, c, st);
    c->x = x;
    c->w = w;
}

static void flow_table_at(Node *t, Ctx *c, const Style *st) {
    static Node *rows_buf[4096];
    // rows_buf is shared by nested tables; copy what we need first
    int nrows = table_rows(t, rows_buf, 4096);
    Node **rows = (Node **)must_alloc((u32)(nrows ? nrows : 1) * sizeof(Node *));
    __builtin_memcpy(rows, rows_buf, (u32)nrows * sizeof(Node *));

    int border = parse_int(attr(t, "border"), 0);
    if (border < 0) border = attr(t, "border") ? 1 : 0;
    int spacing = parse_int(attr(t, "cellspacing"), 0);
    if (spacing < 0) spacing = 2;
    int padding = parse_int(attr(t, "cellpadding"), 0);
    if (padding < 0) padding = 1;
    spacing = (int)(spacing * S);
    padding = (int)(padding * S);
    int bw = border > 0 ? MAX(1, (int)S) : 0;

    int ncols = 0;
    for (int r = 0; r < nrows; r++) {
        int k = 0;
        for (Node *cell = rows[r]->first; cell; cell = cell->next)
            if (cell->type == N_ELEM && (cell->tag == T_TD || cell->tag == T_TH)) k += span_of(cell);
        ncols = MAX(ncols, MIN(k, MAX_COLS));
    }

    // caption
    for (Node *k = t->first; k; k = k->next)
        if (k->type == N_ELEM && k->tag == T_CAPTION) {
            Style cs = *st;
            cs.align = 1;
            block(k, c, &cs, 0, 0, 0, 0, 0, 0, POST_NONE);
        }
    if (!ncols) { mem_free(rows); return; }

    int col_min[MAX_COLS], col_max[MAX_COLS], col_fix[MAX_COLS], col_w[MAX_COLS];
    for (int i = 0; i < ncols; i++) col_min[i] = col_max[i] = 0, col_fix[i] = -1;
    int cell_extra = 2 * padding + 2 * bw;

    int pct = 0;
    int avail = c->w;
    int tw_attr = parse_int(attr(t, "width"), &pct);
    int want_w = -1;
    if (tw_attr > 0) want_w = pct ? avail * MIN(tw_attr, 100) / 100 : MIN((int)(tw_attr * S), avail);
    int inner = (want_w > 0 ? want_w : avail) - spacing * (ncols + 1) - 2 * bw;

    for (int r = 0; r < nrows; r++) {
        int col = 0;
        for (Node *cell = rows[r]->first; cell && col < ncols; cell = cell->next) {
            if (cell->type != N_ELEM || (cell->tag != T_TD && cell->tag != T_TH)) continue;
            int span = MIN(span_of(cell), ncols - col);
            int mn, mx;
            measure_cell(cell, rows[r], st, &mn, &mx);
            mn += cell_extra;
            mx += cell_extra;
            int cw = parse_int(attr(cell, "width"), &pct);
            if (span == 1) {
                col_min[col] = MAX(col_min[col], mn);
                col_max[col] = MAX(col_max[col], mx);
                if (cw > 0) col_fix[col] = MAX(col_fix[col], pct ? inner * MIN(cw, 100) / 100 : (int)(cw * S));
            } else {  // spread what the spanned columns lack
                int have_min = 0, have_max = 0;
                for (int k = 0; k < span; k++) have_min += col_min[col + k], have_max += col_max[col + k];
                if (mn > have_min) for (int k = 0; k < span; k++) col_min[col + k] += (mn - have_min) / span;
                if (mx > have_max) for (int k = 0; k < span; k++) col_max[col + k] += (mx - have_max) / span;
            }
            col += span;
        }
    }
    int sum_min = 0, sum_max = 0;
    for (int i = 0; i < ncols; i++) {
        if (col_fix[i] >= 0) { col_max[i] = MAX(col_fix[i], col_min[i]); }
        if (col_max[i] < col_min[i]) col_max[i] = col_min[i];
        sum_min += col_min[i];
        sum_max += col_max[i];
    }
    if (c->measure) {  // a table inside a cell being measured
        int fixed = spacing * (ncols + 1) + 2 * bw;
        c->min_w = MAX(c->min_w, sum_min + fixed);
        c->max_w = MAX(c->max_w, (want_w > 0 && !pct ? want_w : sum_max + fixed));
        mem_free(rows);
        return;
    }
    if (inner < 1) inner = 1;
    if (sum_max <= inner) {
        for (int i = 0; i < ncols; i++) col_w[i] = col_max[i];
        if (want_w > 0 && sum_max < inner) {  // stretch to the requested width
            int extra = inner - sum_max;
            for (int i = 0; i < ncols; i++) col_w[i] += sum_max ? (int)((u64)extra * (u64)col_max[i] / (u64)sum_max) : extra / ncols;
        }
    } else if (sum_min >= inner) {
        for (int i = 0; i < ncols; i++) col_w[i] = col_min[i];
    } else {
        int extra = inner - sum_min, span_ = sum_max - sum_min;
        for (int i = 0; i < ncols; i++) col_w[i] = col_min[i] + (int)((u64)extra * (u64)(col_max[i] - col_min[i]) / (u64)MAX(span_, 1));
    }
    int table_w = spacing * (ncols + 1) + 2 * bw;
    for (int i = 0; i < ncols; i++) table_w += col_w[i];

    int tx = c->x;
    Style ts = *st;
    align_attr(t, &ts);
    if (ts.align == 1 || st->align == 1) tx += MAX(0, (c->w - table_w) / 2);
    else if (ts.align == 2) tx += MAX(0, c->w - table_w);

    c->y += c->margin;
    c->margin = 0;
    int y0 = c->y;
    t->y = y0;
    u32 tbg = 0;
    attr_color(t, "bgcolor", &tbg);
    inline_style(t, &ts, &tbg);
    int t_item = -1;
    if (tbg >> 24) {
        Item *r = new_item(IT_RECT);
        r->x = tx; r->y = y0; r->w = table_w; r->color = tbg;
        t_item = nitems - 1;
    }
    int y = y0 + bw + spacing;
    for (int r = 0; r < nrows; r++) {
        Node *row = rows[r];
        u32 rbg = 0;
        attr_color(row, "bgcolor", &rbg);
        Style rs = ts;
        inline_style(row, &rs, &rbg);
        int r_item = -1;
        if (rbg >> 24) {
            Item *it = new_item(IT_RECT);
            it->x = tx + bw; it->y = y; it->w = table_w - 2 * bw; it->color = rbg;
            r_item = nitems - 1;
        }
        int col = 0, x = tx + bw + spacing, row_h = 0;
        int first_item = nitems;
        int cell_items[MAX_COLS], cell_count = 0;
        for (Node *cell = row->first; cell && col < ncols; cell = cell->next) {
            if (cell->type != N_ELEM || (cell->tag != T_TD && cell->tag != T_TH)) continue;
            int span = MIN(span_of(cell), ncols - col);
            int cw = spacing * (span - 1);
            for (int k = 0; k < span; k++) cw += col_w[col + k];
            Style cs = cell_style(cell, row, &rs);
            u32 cbg = 0;
            attr_color(cell, "bgcolor", &cbg);
            inline_style(cell, &cs, &cbg);
            int c_item = -1;
            if ((cbg >> 24) || bw) {
                Item *it = new_item(IT_RECT);
                it->x = x; it->y = y; it->w = cw; it->color = cbg;
                it->ref = bw ? 1 : 0;  // 1 = draw a border
                c_item = nitems - 1;
            }
            Ctx sub;
            __builtin_memset(&sub, 0, sizeof sub);
            sub.x = x + bw + padding;
            sub.w = MAX(cw - 2 * padding - 2 * bw, 1);
            sub.y = y + bw + padding;
            sub.align = cs.align;
            sub.line_start = nitems;
            sub.bfc = ++next_bfc;
            cell->y = sub.y;
            flow_children(cell, &sub, &cs);
            flush_line(&sub);
            if (nfloats) sub.y = MAX(sub.y, floats_bottom(sub.bfc, 3));
            int h = sub.y - y + padding + bw;
            row_h = MAX(row_h, h);
            if (c_item >= 0 && cell_count < MAX_COLS) cell_items[cell_count++] = c_item;
            x += cw + spacing;
            col += span;
        }
        (void)first_item;
        for (int k = 0; k < cell_count; k++) items[cell_items[k]].h = row_h;
        if (r_item >= 0) items[r_item].h = row_h;
        y += row_h + spacing;
    }
    y += bw;
    if (t_item >= 0) items[t_item].h = y - y0;
    if (bw) {  // outer border
        Item *it = new_item(IT_RECT);
        it->x = tx; it->y = y0; it->w = table_w; it->h = y - y0; it->ref = 1;
    }
    c->y = y;
    c->max_w = MAX(c->max_w, table_w);
    c->line_start = nitems;
    c->lx = 0; c->line_items = 0; c->space = 0;
    mem_free(rows);
}

// ---- the main flow ------------------------------------------------------------------------

static const short heading_pct[6] = {200, 150, 117, 100, 83, 67};
static const short heading_margin[6] = {67, 83, 100, 133, 167, 233};

static u8 layout_pass;  // stamps the nodes each layout reaches (Node.lpass)
static int flow_wait;   // flow() met an element the parser hasn't finished; try again later

// A table still being parsed would hold up everything after it (the incremental layout
// only waits at a block, and a table is laid out as a whole: its columns depend on all its
// rows). So the layout lays it out "provisionally" with what has arrived, shows that, and
// later goes back to the state before it (prov_*) to lay it out again with more rows, or
// for good once it is closed. Each redo costs as much as the table so far, so redos come
// at most every 250 ms and no more often than 4x their own cost apart.
static int provisional;       // flow(): lay out elements that are still open as they are now
static int want_provisional;  // flow() met an open table that could be laid out provisionally
static int prov_active;       // the last thing laid out is provisional
static int prov_items, prov_links, prov_list, prov_marker, prov_bytes, prov_floats;
static Ctx prov_sub;
static double prov_t, prov_cost;

// ---- CSS boxes: floats, flex rows, grids ---------------------------------------------------

static Node *no_float;  // the element whose own float is being laid out (its content isn't floated again)
static Node *main_node;  // (below: what reader view shows)

static int len_px(const Lay *l, int prop, int em, int of, int def) {
    if (!l || !l->has[prop]) return def;
    long long v = l->val[prop];
    switch (l->unit[prop]) {
    case U_PX: case U_NUM: return (int)(v * S / 100);
    case U_EM: return (int)(v * em / 100);
    case U_REM: return (int)(v * 16 * S / 100);
    case U_PCT: return of < 0 ? def : (int)(of * v / 10000);  // of < 0: measuring (a percentage of nothing yet)
    case U_VW: return (int)(W * v / 10000);
    }
    return def;
}

// What box n makes, from its CSS (and the old align attribute of images and tables).
enum { BOX_NORMAL, BOX_FLOAT_LEFT, BOX_FLOAT_RIGHT, BOX_FLEX_ROW, BOX_GRID };
static int grid_tracks(const char *t, int w, int gap, int em, int *px, int *fr, int max);

static int box_kind(Node *n, const Ctx *c, int em) {
    const Lay *l = n->lay;  // (display, float: lay_items only for flex and grid)
    int fl = l && l->has[LP_FLOAT] ? l->val[LP_FLOAT] : 0;
    if (!fl && (n->tag == T_IMG || n->tag == T_TABLE)) {
        const char *a = attr(n, "align");
        if (a) fl = ieq(a, "left") ? 2 : ieq(a, "right") ? 3 : 0;
    }
    if ((fl == 2 || fl == 3) && n != no_float && n != main_node && n->tag != T_BODY && n->tag != T_HTML) return fl == 2 ? BOX_FLOAT_LEFT : BOX_FLOAT_RIGHT;
    if (!l) return BOX_NORMAL;
    if (l->disp == LD_FLEX || l->disp == LD_GRID) l = lay_items(n);
    if (l->disp == LD_FLEX && !(l->has[LP_DIR] && l->val[LP_DIR] == 2)) return BOX_FLEX_ROW;
    if (l->disp == LD_GRID && l->has[LP_COLS]) {
        int px[24], fr[24];
        int gap = len_px(l, LP_CGAP, em, c->w, 0);
        if (grid_tracks((const char *)(unsigned long)l->val[LP_COLS], c->w, gap, em, px, fr, 24) > 1) return BOX_GRID;
    }
    return BOX_NORMAL;
}

static int video_css_width(Node *n, int of, int em) {  // the CSS width in device pixels, -1 = none
    return len_px(lay_items(n), LP_WIDTH, em, of, -1);
}

static int pct_max_width(Node *n) {  // width or max-width in %: its min-content size counts as 0
    const Lay *l = lay_items(n);
    return l && ((l->has[LP_MAXW] && l->unit[LP_MAXW] == U_PCT) || (l->has[LP_WIDTH] && l->unit[LP_WIDTH] == U_PCT));
}

static void measure_node(Node *k, const Style *st, int *mn, int *mx) {
    Ctx m;
    __builtin_memset(&m, 0, sizeof m);
    m.w = 1 << 24;
    m.measure = 1;
    flow(k, &m, st);
    flush_line(&m);
    *mn = m.min_w;
    *mx = m.max_w;
}

// Lays out k alone in a box at (x, y), w wide, as its own float scope. Returns its height;
// *from is where its items start.
static int flow_box(Node *k, int x, int y, int w, const Style *st, int *from) {
    Ctx sub;
    __builtin_memset(&sub, 0, sizeof sub);
    sub.x = x;
    sub.w = MAX(w, 1);
    sub.y = y;
    sub.align = st->align;
    sub.line_start = nitems;
    sub.bfc = ++next_bfc;
    *from = nitems;
    Node *nf = no_float;
    no_float = k;
    flow(k, &sub, st);
    no_float = nf;
    flush_line(&sub);
    int bottom = sub.y + sub.margin;
    if (nfloats) bottom = MAX(bottom, floats_bottom(sub.bfc, 3));
    return bottom - y;
}

static void flow_float(Node *n, Ctx *c, const Style *st, int right, int min_y) {
    if (c->line_items) flush_line(c);  // (a float in the middle of a line goes below it)
    int avail = c->w, em = st->size;
    const Lay *l = lay_items(n);
    int w = len_px(l, LP_WIDTH, em, avail, -1);
    if (w <= 0) {  // as wide as its content wants, within the room there is
        int mn, mx;
        measure_node(n, st, &mn, &mx);
        w = MAX(MIN(mx, avail), mn);
    }
    int maxw = len_px(l, LP_MAXW, em, avail, -1);
    if (maxw > 0 && w > maxw) w = maxw;
    if (w > avail) w = avail;
    if (w < 1) w = 1;
    int gap = (int)(12 * S), y = MAX(c->y + c->margin, min_y), lft, rgt;
    for (int tries = 0; tries < 64; tries++) {  // as high as it fits beside earlier floats
        int next = float_band(c->bfc, y, 1, c->x, c->x + c->w, &lft, &rgt);
        if (next < 0 || rgt - lft >= w) break;
        y = next;
    }
    int x = right ? rgt - w : lft, from;
    int h = flow_box(n, x, y, w, st, &from);
    for (int i = from; i < nitems; i++) items[i].layer = 1;
    floats = (FloatBox *)grow_array(floats, nfloats, &floats_cap, sizeof(FloatBox));
    FloatBox *f = &floats[nfloats++];
    f->x0 = right ? x - gap : x;
    f->x1 = right ? x + w : x + w + gap;
    f->y0 = y;
    f->y1 = y + h + (int)(6 * S);
    f->bfc = c->bfc;
    f->right = (u8)right;
    n->y = y;
    c->line_start = nitems;  // the float's items are not part of the line that comes next
}

// The boxes of a flex container or grid: element children, and runs of text (anonymous items).
static int box_children(Node *n, Node **out, int max) {
    int k = 0;
    for (Node *ch = n->first; ch && k < max; ch = ch->next) {
        if (ch->type == N_TEXT) {
            int ws = 1;
            for (u32 i = 0; i < ch->len && ws; i++) ws = ch->text[i] == ' ';
            if (!ws) out[k++] = ch;
        } else if (ch->type == N_ELEM && !((tag_flags[ch->tag] & FS) && ch->tag != T_SVG && ch->tag != T_VIDEO && ch->tag != T_AUDIO) &&
                   !is_hidden(ch)) {
            out[k++] = ch;
        }
    }
    return k;
}

static void stretch_and_align(int from, int to, int top, int line_h, int h, int align) {
    int dy = align == 3 ? (line_h - h) / 2 : align == 4 ? line_h - h : 0;
    if (dy > 0)
        for (int i = from; i < to; i++) items[i].y += dy;
    else if (align <= 1 && from < to && items[from].kind == IT_RECT && items[from].y >= top && items[from].y < top + line_h)
        items[from].h = top + line_h - items[from].y;  // stretch: its background reaches the bottom of the line
}

#define MAX_BOXES 256

// A row whose box (or the one or two around it) clips what sticks out sideways (overflow:
// hidden, auto, scroll): a carousel or a strip that scrolls. Only what fits is shown.
static int clips_x(Node *n) {
    for (int up = 0; n && n->type == N_ELEM && up < 3; up++, n = n->parent) {
        const Lay *l = lay_items(n);
        if (l && l->has[LP_OVER] && l->val[LP_OVER] == 2) return 1;
    }
    return 0;
}

// display: flex, in a row (flex-direction: row; columns are laid out as blocks).
static void flow_flex_row(Node *n, Ctx *c, const Style *st) {
    begin_block(c);
    const Lay *l = lay_items(n);
    int em = st->size, W = c->measure ? -1 : c->w;  // (measuring: percentages count as auto)
    static Node *buf[MAX_BOXES];
    Node **kids = (Node **)must_alloc(MAX_BOXES * sizeof(Node *));
    int nk = box_children(n, buf, MAX_BOXES);
    __builtin_memcpy(kids, buf, (u32)nk * sizeof(Node *));
    int gap = len_px(l, LP_CGAP, em, W, 0), rgap = len_px(l, LP_RGAP, em, W, 0);
    int wrap = l && l->has[LP_WRAP] && l->val[LP_WRAP] == 2;
    int justify = l && l->has[LP_JUSTIFY] ? l->val[LP_JUSTIFY] : 1, align = l && l->has[LP_ALIGN] ? l->val[LP_ALIGN] : 1;
    int *basis = (int *)must_alloc((u32)(nk + 1) * 4 * 8), *mins = basis + nk + 1, *grow = mins + nk + 1, *shrink = grow + nk + 1,
        *width = shrink + nk + 1, *from = width + nk + 1, *hs = from + nk + 1, *to = hs + nk + 1;  // (not on the stack: flex nests)
    for (int i = 0; i < nk; i++) {
        const Lay *kl = lay_items(kids[i]);
        int mn, mx;
        measure_node(kids[i], st, &mn, &mx);
        int b = len_px(kl, LP_BASIS, em, W, -1);
        if (b < 0) b = len_px(kl, LP_WIDTH, em, W, -1);
        if (b < 0) b = mx;
        int maxw = len_px(kl, LP_MAXW, em, W, -1);
        if (maxw > 0 && b > maxw) b = maxw;
        basis[i] = b;
        mins[i] = MAX(MIN(mn, b > 0 && kl && kl->has[LP_WIDTH] ? b : mn), len_px(kl, LP_MINW, em, W, 0));
        grow[i] = kl && kl->has[LP_GROW] ? kl->val[LP_GROW] : 0;
        shrink[i] = kl && kl->has[LP_SHRINK] ? kl->val[LP_SHRINK] : 100;
    }
    if (c->measure) {
        int sum = 0, summin = 0, maxmin = 0;
        for (int i = 0; i < nk; i++) { sum += basis[i]; summin += mins[i]; maxmin = MAX(maxmin, mins[i]); }
        if (nk) { sum += gap * (nk - 1); summin += gap * (nk - 1); }
        c->max_w = MAX(c->max_w, sum);
        c->min_w = MAX(c->min_w, wrap || clips_x(n) ? maxmin : summin);  // (clipped: one item at a time)
        mem_free(basis);
        mem_free(kids);
        return;
    }
    int clip = !wrap && !c->measure && clips_x(n);
    if (!wrap && !clip) {  // a row with too little room for its items wraps (rather than squeeze them to slivers)
        int need = gap * (nk > 0 ? nk - 1 : 0);
        for (int k = 0; k < nk; k++) need += MAX(mins[k], MIN(basis[k], (int)(100 * S)));
        if (need > W) wrap = 1;
    }
    c->y += c->margin;
    c->margin = 0;
    n->y = c->y;
    int y = c->y, i = 0;
    while (i < nk) {
        int j = i, sum = 0;  // one line: i..j
        while (j < nk) {
            int add = basis[j] + (j > i ? gap : 0);
            if (wrap && j > i && sum + add > W) break;
            sum += add;
            j++;
        }
        int free = W - sum, tgrow = 0;
        long long tshrink = 0;
        for (int k = i; k < j; k++) { tgrow += grow[k]; tshrink += (long long)shrink[k] * basis[k]; }
        for (int k = i; k < j; k++) {
            int w = basis[k];
            if (free > 0 && tgrow > 0) w += (int)((long long)free * grow[k] / tgrow);
            else if (free < 0 && tshrink > 0 && !clip) w -= (int)((long long)(-free) * shrink[k] * basis[k] / tshrink);
            width[k] = MAX(w, mins[k]);
            if (clip && width[k] > W) width[k] = W;
        }
        int used = gap * (j - i - 1), extra = 0;
        for (int k = i; k < j; k++) used += width[k];
        int left = W - used, x = c->x, between = gap;
        if (left > 0 && !(free > 0 && tgrow > 0)) {
            int cnt = j - i;
            if (justify == 2) x += left / 2;
            else if (justify == 3) x += left;
            else if (justify == 4 && cnt > 1) between += left / (cnt - 1);
            else if (justify == 5) { extra = left / cnt; x += extra / 2; between += extra; }
            else if (justify == 6) { extra = left / (cnt + 1); x += extra; between += extra; }
        }
        int line_h = 0;
        for (int k = i; k < j; k++) {
            if (clip && k > i && x + width[k] > c->x + W) { j = k; break; }  // clipped away: the rest isn't shown
            hs[k - i] = flow_box(kids[k], x, y, width[k], st, &from[k - i]);
            to[k - i] = nitems;
            line_h = MAX(line_h, hs[k - i]);
            x += width[k] + between;
        }
        for (int k = i; k < j; k++) stretch_and_align(from[k - i], to[k - i], y, line_h, hs[k - i], align);
        y += line_h + (j < nk ? rgap : 0);
        i = clip ? nk : j;
    }
    c->y = y;
    c->max_w = MAX(c->max_w, W);
    c->line_start = nitems;
    c->lx = 0; c->line_items = 0; c->space = 0; c->brk = 0;
    mem_free(basis);
    mem_free(kids);
}

// grid-template-columns, for a grid `w` wide: each track's fixed size (px) or fr share (x100).
// repeat(n, ...), repeat(auto-fill | auto-fit, ...), minmax(), fr, px/em/rem/%, auto.
static int grid_tracks(const char *t, int w, int gap, int em, int *px, int *fr, int max) {
    int n = 0;
    while (*t && n < max) {
        while (*t == ' ' || *t == ',') t++;
        if (!*t) break;
        if (*t == '[') { while (*t && *t != ']') t++; if (*t) t++; continue; }  // line names
        const char *e = t;
        int depth = 0;
        while (*e && !(depth == 0 && *e == ' ')) { if (*e == '(') depth++; if (*e == ')') depth--; e++; }
        if (iprefix(t, (int)(e - t), "repeat(")) {
            const char *a = t + 7, *comma = a;
            while (comma < e && *comma != ',') comma++;
            const char *inner = comma + 1, *ie = e - 1;  // without the final ")"
            int auto_fill = iprefix(a, (int)(comma - a), "auto-f");
            int count = auto_fill ? 0 : parse_int(a, 0);
            char one[160];
            int ol = MIN((int)(ie - inner), 159);
            __builtin_memcpy(one, inner, (u32)ol);
            one[ol] = 0;
            int tpx[24], tfr[24];
            int tn = grid_tracks(one, w, gap, em, tpx, tfr, 24);
            if (tn <= 0) { t = e; continue; }
            if (auto_fill) {  // as many as fit, each at its minimum (the px part)
                int size = 0;
                for (int i = 0; i < tn; i++) size += MAX(tpx[i], (int)(20 * S)) + gap;
                count = MAX(1, (w + gap) / MAX(size, 1));
            }
            for (int r = 0; r < MAX(count, 1) && n < max; r++)
                for (int i = 0; i < tn && n < max; i++) { px[n] = tpx[i]; fr[n] = tfr[i]; n++; }
            t = e;
            continue;
        }
        Lay one;
        __builtin_memset(&one, 0, sizeof one);
        int val;
        u8 unit;
        const char *q = t;
        if (iprefix(t, (int)(e - t), "minmax(")) {  // minmax(a, b): b if it is a share, else fixed b
            q = t + 7;
            read_len(&q, e, &val, &unit);
            one.has[0] = 1; one.val[0] = val; one.unit[0] = unit;
            int lo = unit == U_AUTO ? 0 : len_px(&one, 0, em, w, 0);
            while (q < e && (*q == ',' || *q == ' ')) q++;
            const char *b = q;
            if (read_len(&q, e, &val, &unit) && (unit == U_NUM || iprefix(b, (int)(e - b), "auto"))) {
                px[n] = lo; fr[n] = unit == U_NUM ? MAX(val, 1) : 100;
            } else {
                one.val[0] = val; one.unit[0] = unit;
                px[n] = MAX(lo, len_px(&one, 0, em, w, lo)); fr[n] = 0;
            }
        } else if (read_len(&q, e, &val, &unit) && unit != U_AUTO) {
            if (unit == U_NUM && val > 0 && iprefix(q - 2, 2, "fr")) { px[n] = 0; fr[n] = val; }
            else { one.has[0] = 1; one.val[0] = val; one.unit[0] = unit; px[n] = len_px(&one, 0, em, w, 0); fr[n] = 0; }
        } else {  // auto, min-content, fit-content(): a share of what is left
            px[n] = 0; fr[n] = 100;
        }
        n++;
        t = e;
    }
    return n;
}

static void flow_grid(Node *n, Ctx *c, const Style *st) {
    begin_block(c);
    const Lay *l = lay_items(n);
    int em = st->size, W = c->measure ? -1 : c->w;
    int cgap = len_px(l, LP_CGAP, em, W, 0), rgap = len_px(l, LP_RGAP, em, W, 0);
    int px[24], fr[24], cw[24];
    int nc = grid_tracks((const char *)(unsigned long)l->val[LP_COLS], W, cgap, em, px, fr, 24);
    static Node *buf[MAX_BOXES];
    Node **kids = (Node **)must_alloc(MAX_BOXES * sizeof(Node *));
    int nk = box_children(n, buf, MAX_BOXES);
    __builtin_memcpy(kids, buf, (u32)nk * sizeof(Node *));
    if (c->measure) {
        int mxw = 0, mnw = 0;
        for (int i = 0; i < nk; i++) {
            int mn, mx;
            measure_node(kids[i], st, &mn, &mx);
            mxw = MAX(mxw, mx);
            mnw = MAX(mnw, mn);
        }
        c->max_w = MAX(c->max_w, nc * mxw + (nc - 1) * cgap);
        c->min_w = MAX(c->min_w, mnw);
        mem_free(kids);
        return;
    }
    int fixed = cgap * (nc - 1), tfr = 0;
    for (int i = 0; i < nc; i++) { fixed += px[i]; tfr += fr[i]; }
    int rest = W - fixed;
    for (int i = 0; i < nc; i++) cw[i] = px[i] + (fr[i] && tfr && rest > 0 ? (int)((long long)rest * fr[i] / tfr) : 0);
    c->y += c->margin;
    c->margin = 0;
    n->y = c->y;
    int y = c->y, k = 0;
    while (k < nk) {
        int col = 0, line_h = 0, cnt = 0, from[24], hs[24], to[24];
        while (k < nk && col < nc) {
            const Lay *kl = lay_items(kids[k]);
            int span = kl && kl->has[LP_SPAN] ? kl->val[LP_SPAN] : 1;
            if (span < 0 || span > nc) span = nc;
            if (col > 0 && col + span > nc) break;  // doesn't fit in this row
            int x = c->x, w = cgap * (span - 1);
            for (int i = 0; i < col; i++) x += cw[i] + cgap;
            for (int i = col; i < col + span && i < nc; i++) w += cw[i];
            hs[cnt] = flow_box(kids[k], x, y, w, st, &from[cnt]);
            to[cnt] = nitems;
            line_h = MAX(line_h, hs[cnt]);
            cnt++;
            col += span;
            k++;
        }
        int align = l->has[LP_ALIGN] ? l->val[LP_ALIGN] : 1;
        for (int i = 0; i < cnt; i++) stretch_and_align(from[i], to[i], y, line_h, hs[i], align);
        y += line_h + (k < nk ? rgap : 0);
    }
    c->y = y;
    c->max_w = MAX(c->max_w, W);
    c->line_start = nitems;
    c->lx = 0; c->line_items = 0; c->space = 0; c->brk = 0;
    mem_free(kids);
}

static void flow(Node *n, Ctx *c, const Style *parent) {
    int defer = defer_block;  // only a block that is n itself may be deferred, not its descendants
    defer_block = 0;
    if (n->type == N_TEXT) { flow_text(c, n, parent); return; }
    if (n->type != N_ELEM) return;
    int tag = n->tag;
    if ((tag == T_VIDEO || tag == T_AUDIO) && !is_hidden(n)) {
        n->y = c->y + c->margin;
        n->lpass = layout_pass;
        flow_video(c, n, parent);
        return;
    }
    if ((tag_flags[tag] & FS) && tag != T_SVG) return;
    if (is_hidden(n)) return;
    Style st = *parent;
    u32 bg = 0;
    int em = st.size;
    n->y = c->y + c->margin;
    n->lpass = layout_pass;
    int kind = box_kind(n, c, st.size);
    if (n->open && !provisional) {  // still being parsed: only a block can start now (its children follow later)
        int blocky = (tag_flags[tag] & FB) && tag != T_HR && tag != T_TABLE && tag != T_IMG && tag != T_SVG &&
                     tag != T_INPUT && tag != T_BUTTON && tag != T_SELECT && tag != T_TEXTAREA && tag != T_BR && kind == BOX_NORMAL;
        if (!defer || !blocky || nframes >= MAX_FRAMES) {
            if (defer && (tag == T_TABLE || kind != BOX_NORMAL) && !c->measure) want_provisional = 1;
            flow_wait = 1;
            return;
        }
    }
    const Lay *lay = n->lay;
    int floating = kind == BOX_FLOAT_LEFT || kind == BOX_FLOAT_RIGHT, clear_y = 0;
    if (lay && lay->has[LP_CLEAR] && lay->val[LP_CLEAR] > 1 && floating && !c->measure && nfloats) {
        // a float that clears goes below those floats itself; what follows it stays where it is
        int v = lay->val[LP_CLEAR];
        clear_y = floats_bottom(c->bfc, v == 2 ? 1 : v == 3 ? 2 : 3);
    } else if (lay && lay->has[LP_CLEAR] && lay->val[LP_CLEAR] > 1) {
        begin_block(c);
        clear_floats(c, lay->val[LP_CLEAR] == 2 ? 1 : lay->val[LP_CLEAR] == 3 ? 2 : 3);
    }
    if (floating) {
        if (c->measure) {  // measured as a block of its own
            Node *nf = no_float;
            no_float = n;
            begin_block(c);
            flow(n, c, &st);
            begin_block(c);
            no_float = nf;
            return;
        }
        flow_float(n, c, &st, kind == BOX_FLOAT_RIGHT, clear_y);
        return;
    }
    if (kind == BOX_FLEX_ROW) { inline_style(n, &st, 0); flow_flex_row(n, c, &st); return; }
    if (kind == BOX_GRID) { inline_style(n, &st, 0); flow_grid(n, c, &st); return; }

    switch (tag) {
    case T_A: {
        const char *href = attr(n, "href");
        if (href && *href && !iprefix(href, lw_strlen(href), "javascript:")) {
            if (!c->measure) {
                links = (Link *)grow_array(links, nlinks, &links_cap, sizeof(Link));
                links[nlinks].href = href;
                links[nlinks].target = attr(n, "target");
                st.link = nlinks++;
            } else st.link = 0;
            st.color = body_link ? body_link : C_LINK;
        }
        break;
    }
    case T_B: case T_STRONG: st.flags |= LW_TEXT_BOLD; break;
    case T_I: case T_EM: case T_CITE: case T_VAR: case T_DFN: case T_ADDRESS: st.flags |= LW_TEXT_ITALIC; break;
    case T_CODE: case T_TT: case T_KBD: case T_SAMP:
        st.flags |= LW_TEXT_MONO;
        st.size = (short)(st.size * 88 / 100);
        break;
    case T_U: case T_INS: st.deco |= 1; break;
    case T_S: case T_STRIKE: case T_DEL: st.deco |= 2; break;
    case T_SMALL: st.size = (short)(st.size * 83 / 100); break;
    case T_BIG: st.size = (short)(st.size * 120 / 100); break;
    case T_SUB: case T_SUP: st.size = (short)(st.size * 75 / 100); break;
    case T_NOBR: st.nowrap = 1; break;
    case T_FONT: {
        u32 col;
        if (attr_color(n, "color", &col)) st.color = col;
        const char *sz = attr(n, "size");
        if (sz) {
            static const short px[7] = {10, 13, 16, 18, 24, 32, 48};
            int v = parse_int(sz[0] == '+' || sz[0] == '-' ? sz + 1 : sz, 0);
            if (v >= 0) {
                if (sz[0] == '+') v = 3 + v;
                else if (sz[0] == '-') v = 3 - v;
                v = v < 1 ? 1 : v > 7 ? 7 : v;
                st.size = (short)(px[v - 1] * S);
            }
        }
        const char *face = attr(n, "face");
        if (face && (iprefix(face, lw_strlen(face), "courier") || iprefix(face, lw_strlen(face), "monospace"))) st.flags |= LW_TEXT_MONO;
        break;
    }
    case T_BR:
        if (attr(n, "clear")) { flush_line(c); clear_floats(c, 3); }
        if (c->line_items) flush_line(c);
        else if (!c->measure) { c->y += c->margin + st.size * 134 / 100; c->margin = 0; }
        return;
    case T_WBR: c->brk = 1; return;
    case T_IMG: flow_image(c, n, &st); return;
    case T_SVG: flow_inline_svg(c, n, &st); return;
    case T_INPUT: case T_BUTTON: case T_SELECT: case T_TEXTAREA: flow_control(c, n, &st); return;
    case T_HR: {
        begin_block(c);
        int m = (int)(8 * S);
        c->y += MAX(c->margin, m);
        c->margin = 0;
        n->y = c->y;
        if (!c->measure) {
            Item *r = new_item(IT_RECT);
            r->x = c->x; r->y = c->y; r->w = c->w; r->h = MAX(1, (int)S);
            r->color = RGB(200, 200, 200);
        }
        c->y += MAX(1, (int)S);
        c->margin = m;
        c->line_start = nitems;
        return;
    }
    case T_TABLE: {
        Style ts = st;
        ts.align = 0;
        u32 col;
        if (attr_color(n, "text", &col)) ts.color = col;
        flow_table(n, c, &ts);
        return;
    }
    }

    int block_like = (tag_flags[tag] & FB) != 0;
    if (lay && (lay->disp == LD_INLINE || lay->disp == LD_INLINE_BLOCK) && tag != T_TD && tag != T_TH && tag != T_TR) block_like = 0;
    else if (lay && (lay->disp == LD_BLOCK || lay->disp == LD_FLEX || lay->disp == LD_GRID)) block_like = 1;
    if (lay && lay->disp == LD_FLEX && block_like && (lay = lay_items(n))->has[LP_ALIGN] && lay->val[LP_ALIGN] == 3) st.align = 1;  // a centred column
    if (!block_like) {  // inline element
        inline_style(n, &st, 0);
        if (tag == T_Q) {
            static const char open_q[] = "\xE2\x80\x9C", close_q[] = "\xE2\x80\x9D";
            place_segment(c, open_q, 3, &st, 0);
            flow_children(n, c, &st);
            place_segment(c, close_q, 3, &st, 0);
            return;
        }
        if (tag == T_SUMMARY) st.flags |= LW_TEXT_BOLD;
        if (tag >= T_H1 && tag <= T_H6) {  // a heading made inline still looks like one: its size is
            st.size = (short)(st.size * heading_pct[tag - T_H1] / 100);  // then set on the box around it,
            st.flags |= LW_TEXT_BOLD;  // with font-size, which we don't read (Wikipedia's .mw-heading)
        }
        flow_children(n, c, &st);
        return;
    }

    // block elements
    int mt = 0, mb = 0, ml = 0, mr = 0, pad = 0;
    align_attr(n, &st);
    switch (tag) {
    case T_P: mt = mb = em; break;
    case T_H1: case T_H2: case T_H3: case T_H4: case T_H5: case T_H6: {
        int k = tag - T_H1;
        st.size = (short)(st.size * heading_pct[k] / 100);
        st.flags |= LW_TEXT_BOLD;
        mt = mb = em * heading_margin[k] / 100 * heading_pct[k] / 100;  // margins are in the heading's em
        break;
    }
    case T_PRE: case T_LISTING: case T_XMP: case T_PLAINTEXT:
        st.pre = 1;
        st.flags |= LW_TEXT_MONO;
        st.size = (short)(st.size * 88 / 100);
        mt = mb = em;
        bg = RGB(246, 248, 250);
        pad = (int)(8 * S);
        break;
    case T_BLOCKQUOTE: mt = mb = em; ml = mr = (int)(40 * S); break;
    case T_FIGURE: mt = mb = em; ml = mr = (int)(40 * S); break;
    case T_DD: ml = (int)(40 * S); break;
    case T_DL: mt = mb = list_depth ? 0 : em; break;
    case T_CENTER: st.align = 1; break;
    case T_CAPTION: st.align = 1; break;
    case T_FIELDSET: mt = mb = (int)(8 * S); ml = mr = (int)(2 * S); break;
    case T_LEGEND: st.flags |= LW_TEXT_BOLD; break;
    case T_UL: case T_OL: case T_MENU: case T_DIR: {
        mt = mb = list_depth ? 0 : em;
        ml = (int)(40 * S);
        if (list_depth < 32) {
            int kind = tag == T_OL ? 1 : 0;
            const char *type = attr(n, "type");
            if (type && tag == T_OL) kind = type[0] == 'a' ? 2 : type[0] == 'A' ? 3 : 1;
            const char *style = attr(n, "style");
            if (style) {
                for (const char *p = style; *p; p++)
                    if (iprefix(p, lw_strlen(p), "list-style") ) {
                        const char *q = p;
                        while (*q && *q != ';') {
                            if (iprefix(q, lw_strlen(q), "none")) kind = 4;
                            q++;
                        }
                        break;
                    }
            }
            list_kind[list_depth] = kind;
            int start = parse_int(attr(n, "start"), 0);
            list_count[list_depth] = start > 0 ? start - 1 : 0;
        }
        list_depth++;
        inline_style(n, &st, &bg);
        defer_block = defer;
        block(n, c, &st, mt, mb, ml, mr, bg, pad, POST_LIST);  // ends with list_depth--
        return;
    }
    case T_LI: {
        begin_block(c);
        if (list_depth > 0 && list_depth <= 32) {
            int d = list_depth - 1;
            int v = parse_int(attr(n, "value"), 0);
            list_count[d] = v > 0 ? v : list_count[d] + 1;
            if (!c->measure) set_marker(c->x - (int)(6 * S), &st);
        }
        inline_style(n, &st, &bg);
        defer_block = defer;
        block(n, c, &st, 0, 0, 0, 0, bg, pad, POST_LI);  // ends with marker_pending = 0
        return;
    }
    case T_DETAILS: mt = mb = (int)(4 * S); break;
    }
    inline_style(n, &st, &bg);
    u32 col;
    if (tag == T_TD || tag == T_TH || tag == T_TR) { if (attr_color(n, "bgcolor", &col)) bg = col; }
    defer_block = defer;
    block(n, c, &st, mt, mb, ml, mr, bg, pad, POST_NONE);
}

static Node *title_node;

// Reader view: when the page marks its main content (<main>, role="main", or a single
// <article>), show just that; menus and sidebars are usually hidden by CSS we don't run.
static Node *main_node;
static int full_page;

static Node *find_main(Node *n, int *articles, Node **article) {
    for (Node *c = n->first; c; c = c->next) {
        if (c->type != N_ELEM || (tag_flags[c->tag] & FS)) continue;
        const char *role = attr(c, "role");
        if (c->tag == T_MAIN || (role && ieq(role, "main"))) return c;
        if (c->tag == T_ARTICLE) {
            if (!*article) *article = c;
            (*articles)++;
            continue;  // articles inside an article don't count separately
        }
        Node *m = find_main(c, articles, article);
        if (m) return m;
    }
    return 0;
}

// ---- incremental layout --------------------------------------------------------------------
// Layout runs top to bottom in slices of a few milliseconds, one slice per frame, so what
// is on screen shows as soon as it is laid out and the window stays responsive while the
// rest of a long page follows. Only whole children of blocks on the frame stack are
// units of work; tables and inline content are laid out in one go.

static int layout_running;  // a layout is in progress
static int draw_limit;      // items[0..draw_limit) have their final place
static int laid_y;          // how far down the layout has got
static int prev_doc_h;      // doc_h of the previous layout, kept as the scroll range meanwhile
static int layout_slices;
static double layout_cpu;   // ms spent in layout_step for this layout

static int css_stale;  // rules were added while this layout ran: lay out again after it

static void layout_start(void) {
    css_stale = 0;
    nitems = 0;
    nlinks = 0;
    list_depth = 0;
    marker_pending = 0;
    layout_pass = (u8)(layout_pass % 255 + 1);
    prov_active = want_provisional = 0;
    nfloats = 0;
    next_bfc = 1;
    int pad = (int)(16 * S);
    content_w = MIN(W - 2 * pad - (int)(10 * S), (int)(1100 * S));
    if (content_w < (int)(100 * S)) content_w = MAX(W - 2 * pad, 50);
    content_x = MAX(pad, (W - content_w) / 2);
    Frame *f = &frames[0];
    __builtin_memset(f, 0, sizeof *f);
    f->sub.x = content_x;
    f->sub.w = content_w;
    f->sub.y = pad;
    f->st.size = (short)(16 * S);
    f->st.color = body_text ? body_text : C_TEXT;
    f->st.link = -1;
    f->bg_item = -1;
    if (main_node && !full_page) f->single = main_node;
    else f->n = root;
    nframes = 1;
    prev_doc_h = layout_running ? prev_doc_h : doc_h;
    layout_running = 1;
    draw_limit = 0;
    laid_y = 0;
    layout_slices = 0;
    layout_cpu = 0;
}

// Lays out until `deadline`, but at least down to `need_y`. Returns 1 when the layout is done.
static int layout_step(double deadline, int need_y) {
    if (!layout_running) return 1;
    double t0 = lw_now();
    int pad = (int)(16 * S);
    while (nframes) {
        Frame *f = &frames[nframes - 1];
        Node *k = f->single ? (f->last ? 0 : f->single) : f->last ? f->last->next : f->n ? f->n->first : 0;
        if (k) {
            if (prov_active) {  // k was laid out provisionally: again, once it has grown (or closed)
                int bytes = parse_dropped + parse_pos;
                if (k->open && (bytes == prov_bytes || lw_now() - prov_t < MAX(250.0, 4 * prov_cost))) break;
                nitems = prov_items;
                nlinks = prov_links;
                list_depth = prov_list;
                marker_pending = prov_marker;
                nfloats = prov_floats;
                f->sub = prov_sub;
                prov_active = 0;
            }
            defer_block = 1;
            flow(k, &f->sub, &f->st);
            defer_block = 0;
            if (flow_wait) {  // wait for the parser
                flow_wait = 0;
                if (want_provisional) {  // an open table: lay out what there is of it meanwhile
                    want_provisional = 0;
                    prov_items = nitems;
                    prov_links = nlinks;
                    prov_list = list_depth;
                    prov_marker = marker_pending;
                    prov_floats = nfloats;
                    prov_sub = f->sub;
                    prov_bytes = parse_dropped + parse_pos;
                    double t1 = lw_now();
                    provisional = 1;
                    flow(k, &f->sub, &f->st);
                    provisional = 0;
                    flow_wait = 0;
                    prov_t = lw_now();
                    prov_cost = prov_t - t1;
                    prov_active = 1;
                }
                break;
            }
            f->last = k;
            if (frames[nframes - 1].sub.y >= need_y && lw_now() >= deadline) break;
        } else if (f->n && f->n->open) {
            break;  // more children may still arrive
        } else if (nframes == 1) {
            flush_line(&f->sub);
            doc_h = MAX(f->sub.y + f->sub.margin, nfloats ? floats_bottom(0, 3) : 0) + pad;
            nframes = 0;
        } else {
            nframes--;
            close_block(f);
        }
    }
    layout_slices++;
    layout_cpu += lw_now() - t0;
    if (!nframes) {
        layout_running = 0;
        draw_limit = nitems;
        laid_y = doc_h;
    } else {
        // show what is placed: everything but the line still being filled, with the
        // backgrounds of the open blocks stretched down to where the layout is
        Frame *top = &frames[nframes - 1];
        draw_limit = top->sub.line_items ? top->sub.line_start : nitems;
        laid_y = top->sub.y;
        for (int i = 1; i < nframes; i++)
            if (frames[i].bg_item >= 0) items[frames[i].bg_item].h = frames[i].sub.y - items[frames[i].bg_item].y;
        doc_h = MAX(prev_doc_h, laid_y + pad);
    }
    pump_images();
    return !layout_running;
}

// =====================================================================================
// drawing
// =====================================================================================

static int scroll_y, target_y, hover_link = -1, hover_control = -1;
static int dirty = 1, need_layout, has_doc;
static double last_layout, caret_t;
static char doc_url[1024];
static int doc_url_len;
static int drag_scroll = -1, drag_start_y, drag_start_scroll;
static char toast_msg[200];
static int toast_len;
static double toast_until;
static int js_notice;

static void show_toast(const char *m) {
    toast_len = 0;
    while (m[toast_len] && toast_len < (int)sizeof toast_msg) { toast_msg[toast_len] = m[toast_len]; toast_len++; }
    toast_until = lw_now() + 3000;
    dirty = 1;
}

static int badge_x = -1, badge_y, badge_w, badge_h;

static u32 blend(u32 d, u32 s) {
    u32 a = s >> 24;
    if (a == 255) return s;
    if (!a) return d;
    u32 r = ((s & 255) * a + (d & 255) * (255 - a)) / 255;
    u32 g = ((s >> 8 & 255) * a + (d >> 8 & 255) * (255 - a)) / 255;
    u32 b = ((s >> 16 & 255) * a + (d >> 16 & 255) * (255 - a)) / 255;
    return 0xFF000000u | b << 16 | g << 8 | r;
}

// Sets n pixels: the first few one by one, the rest by copying what is done (memory.copy,
// which the browser runs natively: far faster than a loop of stores in the interpreter).
static void fill_span(u32 *p, int n, u32 col) {
    int k = n < 16 ? n : 16;
    for (int i = 0; i < k; i++) p[i] = col;
    while (k < n) {
        int c = k < n - k ? k : n - k;
        __builtin_memcpy(p + k, p, (u32)c * 4);
        k += c;
    }
}

static void fill(int x, int y, int w, int h, u32 col) {
    int x0 = MAX(x, 0), y0 = MAX(y, 0), x1 = MIN(x + w, W), y1 = MIN(y + h, H);
    if (x0 >= x1 || y0 >= y1 || !(col >> 24)) return;
    if (col >> 24 == 255) {  // opaque: one row, then copies of it
        u32 *first = fb + y0 * W + x0;
        fill_span(first, x1 - x0, col);
        for (int j = y0 + 1; j < y1; j++) __builtin_memcpy(fb + j * W + x0, first, (u32)(x1 - x0) * 4);
        return;
    }
    for (int j = y0; j < y1; j++) {
        u32 *p = fb + j * W;
        for (int i = x0; i < x1; i++) p[i] = blend(p[i], col);
    }
}

static void frame_rect(int x, int y, int w, int h, int t, u32 col) {
    fill(x, y, w, t, col);
    fill(x, y + h - t, w, t, col);
    fill(x, y, t, h, col);
    fill(x + w - t, y, t, h, col);
}

static void draw_image(Item *it, int y) {
    Img *im = &imgs[it->ref];
    if (im->state != IMG_READY) {
        if (im->state == IMG_FAILED) frame_rect(it->x, y, it->w, it->h, 1, RGB(220, 220, 220));
        return;
    }
    int x0 = MAX(it->x, 0), x1 = MIN(it->x + it->w, W), y0 = MAX(y, 0), y1 = MIN(y + it->h, H);
    if (x0 >= x1 || y0 >= y1) return;
    for (int j = y0; j < y1; j++) {
        int sy = (int)((u64)(j - y) * (u64)im->ph / (u64)it->h);
        const u32 *src = im->px + sy * im->pw;
        u32 *dst = fb + j * W;
        for (int i = x0; i < x1; i++) {
            u32 s = src[(u64)(i - it->x) * (u64)im->pw / (u64)it->w];
            dst[i] = (s >> 24) == 255 ? s : blend(dst[i], s);
        }
    }
}

static void fill_circle(int cx, int cy, int r, u32 col) {
    for (int dy = -r, dx = 0; dy <= r; dy++) {
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r) dx++;
        while (dx > 0 && dx * dx + dy * dy > r * r) dx--;
        fill(cx - dx, cy + dy, 2 * dx + 1, 1, col);
    }
}

static void fill_play(int x, int cy, int h, u32 col) {  // a play triangle h tall, its left side at x
    int half = h / 2;
    for (int dy = -half; dy <= half; dy++) fill(x, cy + dy, (half - (dy < 0 ? -dy : dy)) * 173 / 100 + 1, 1, col);
}

static int fmt_time(char *o, double sec) {  // m:ss or h:mm:ss
    int t = sec > 0 ? (int)sec : 0, h = t / 3600, m = t / 60 % 60, k = 0;
    char tmp[12];
    int v = h ? h : m, n = 0;
    do { tmp[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) o[k++] = tmp[--n];
    if (h) { o[k++] = ':'; o[k++] = (char)('0' + m / 10); o[k++] = (char)('0' + m % 10); }
    o[k++] = ':';
    o[k++] = (char)('0' + t % 60 / 10);
    o[k++] = (char)('0' + t % 10);
    return k;
}

// Where the parts of a video's box are (y: its top on the screen).
typedef struct {
    int px, py, pw, ph;  // the picture
    int by;              // the bar's top
    int bx1;             // the play button: [it->x, bx1)
    int tx0, tx1;        // the seek track
    int time_x;          // the time text
    int fx0;             // the fullscreen button: [fx0, mx0) (none for <audio>: fx0 == mx0)
    int mx0;             // the mute button: [mx0, right edge)
} VGeo;

static void video_geo(const Item *it, int y, VGeo *g) {
    const Video *v = &videos[it->ref];
    int bar = VIDEO_BAR;
    g->px = it->x;
    g->py = y;
    g->pw = it->w;
    g->ph = it->h - bar;
    g->by = y + g->ph;
    g->bx1 = it->x + bar;
    g->time_x = g->bx1;
    const char *sample = v->dur >= 3600 ? "0:00:00 / 0:00:00" : "00:00 / 00:00";
    g->tx0 = g->time_x + lw_text_width(sample, lw_strlen(sample), (int)(12 * S), 0) + (int)(12 * S);
    g->mx0 = it->x + it->w - bar;
    g->fx0 = v->audio ? g->mx0 : g->mx0 - bar;
    g->tx1 = g->fx0 - (int)(8 * S);
    if (g->tx1 - g->tx0 < (int)(30 * S)) g->tx0 = g->tx1;  // too narrow for a track
}

static void draw_video_bar(Item *it, int y);

static void draw_video_item(Item *it, int y) {
    Video *v = &videos[it->ref];
    v->visible = 1;
    v->item = it == &full_item ? -2 : (int)(it - items);
    v->screen_y = y;
    VGeo g;
    video_geo(it, y, &g);
    int live = v->handle && v->state != LW_VIDEO_FAILED;
    if (!v->audio) {
        fill(g.px, g.py, g.pw, g.ph, RGB(0, 0, 0));
        Img *po = v->poster >= 0 ? &imgs[v->poster] : 0;
        if (po && po->state == IMG_READY && po->nat_w > 0 && po->nat_h > 0) {  // fitted, centred
            int fw = g.pw, fh = (int)((long long)g.pw * po->nat_h / po->nat_w);
            if (fh > g.ph) { fh = g.ph; fw = (int)((long long)g.ph * po->nat_w / po->nat_h); }
            Item tmp = *it;
            tmp.x = g.px + (g.pw - fw) / 2;
            tmp.w = fw;
            tmp.h = fh;
            tmp.ref = v->poster;
            if (fw > 0 && fh > 0) draw_image(&tmp, g.py + (g.ph - fh) / 2);
        }
        int cx = g.px + g.pw / 2, cy = g.py + g.ph / 2;
        if (v->state == LW_VIDEO_FAILED) {
            int size = (int)(13 * S);
            const char *m = "Can't play this video";
            lw_text(fb, W, H, g.px + (int)(12 * S), g.py + (int)(10 * S), m, lw_strlen(m), size, LW_TEXT_BOLD, RGB(255, 255, 255));
            if (v->error) lw_text(fb, W, H, g.px + (int)(12 * S), g.py + (int)(10 * S) + ascent(size) + descent(size), v->error,
                                  lw_strlen(v->error), size, 0, RGB(200, 200, 200));
        } else if (!live) {  // not started: a big play button
            int r = MIN((int)(34 * S), MIN(g.pw, g.ph) / 4);
            fill_circle(cx, cy, r, RGBA(0, 0, 0, 150));
            fill_play(cx - r * 3 / 10, cy, r, RGB(255, 255, 255));
        }
        if (live) lw_video_place(v->handle, g.px, g.py, g.pw, g.ph, 0, 0, W, H);  // the browser draws it there
    }
    draw_video_bar(it, y);
}

// The bar: play/pause, time, seek track, mute.
static void draw_video_bar(Item *it, int y) {
    Video *v = &videos[it->ref];
    VGeo g;
    video_geo(it, y, &g);
    int bar = VIDEO_BAR, cy = g.by + bar / 2;
    u32 white = RGB(255, 255, 255);
    fill(it->x, g.by, it->w, bar, RGB(28, 28, 31));
    if (video_focus == it->ref) fill(it->x, g.by, it->w, MAX(1, (int)S), RGB(14, 165, 233));
    int ih = (int)(14 * S), bx = it->x + (bar - ih) / 2;
    if (v->state == LW_VIDEO_PLAYING) {
        fill(bx + ih / 6, cy - ih / 2, ih / 4, ih, white);
        fill(bx + ih * 7 / 12, cy - ih / 2, ih / 4, ih, white);
    } else {
        fill_play(bx + ih / 6, cy, ih, white);
    }
    char tb[48];
    int k;
    int size = (int)(12 * S);
    if (v->handle && (v->state == LW_VIDEO_LOADING || v->waiting)) {
        const char *m = "Loading\xE2\x80\xA6";
        k = lw_strlen(m);
        __builtin_memcpy(tb, m, (u32)k);
    } else {
        k = fmt_time(tb, v->t);
        tb[k++] = ' ';
        tb[k++] = '/';
        tb[k++] = ' ';
        if (v->dur > 0) k += fmt_time(tb + k, v->dur);
        else { tb[k++] = '-'; tb[k++] = '-'; }
    }
    lw_text(fb, W, H, g.time_x, cy - (ascent(size) + descent(size)) / 2, tb, k, size, 0, white);
    if (g.tx1 > g.tx0) {
        int th = MAX(3, (int)(4 * S)), ty = cy - th / 2, tw = g.tx1 - g.tx0;
        fill(g.tx0, ty, tw, th, RGBA(255, 255, 255, 70));
        if (v->dur > 0) {
            int pos = (int)(tw * MIN(1.0, v->t / v->dur)), got = (int)(tw * MIN(1.0, (v->t + v->buf) / v->dur));
            if (got > pos) fill(g.tx0 + pos, ty, got - pos, th, RGBA(255, 255, 255, 120));
            fill(g.tx0, ty, pos, th, RGB(14, 165, 233));
            fill_circle(g.tx0 + pos, cy, (int)(6 * S), white);
        }
    }
    if (g.fx0 < g.mx0) {  // fullscreen: four corners (in fullscreen, pointing in)
        int fx = g.fx0 + (bar - ih) / 2, fy = cy - ih / 2, t = MAX(2, (int)(2 * S)), l = ih * 3 / 8, in = video_full == it->ref;
        for (int c = 0; c < 4; c++) {  // an L in each corner, its arms meeting at the outer (inner) corner
            int x0 = c & 1 ? fx + ih - l : fx, y0 = c & 2 ? fy + ih - l : fy;
            int right = (c & 1) ? !in : in, bottom = (c & 2) ? !in : in;
            fill(x0, bottom ? y0 + l - t : y0, l, t, white);
            fill(right ? x0 + l - t : x0, y0, t, l, white);
        }
    }
    int mx = g.mx0 + (bar - ih) / 2;  // a speaker; muted: with a cross
    fill(mx, cy - ih / 5, ih / 4, ih * 2 / 5, white);
    for (int d = 0; d < ih / 3; d++) fill(mx + ih / 4 + d, cy - ih / 5 - d, 1, ih * 2 / 5 + 2 * d, white);
    if (v->muted) {
        for (int d = 0; d < ih / 3; d++) {
            fill(mx + ih * 2 / 3 + d, cy - ih / 6 + d, MAX(1, (int)(1.5f * S)), MAX(1, (int)(1.5f * S)), white);
            fill(mx + ih * 2 / 3 + d, cy + ih / 6 - d, MAX(1, (int)(1.5f * S)), MAX(1, (int)(1.5f * S)), white);
        }
    } else {
        fill(mx + ih * 3 / 4, cy - ih / 4, MAX(1, (int)(1.5f * S)), ih / 2, white);
    }
}

static void draw_text_item(Item *it, int y, int hovered) {
    lw_text(fb, W, H, it->x, y, it->s, it->len, it->size, it->flags, it->color);
    if ((it->deco & 1) || hovered) fill(it->x, y + it->asc + (int)(2 * S), it->w, MAX(1, (int)S), it->color);
    if (it->deco & 2) fill(it->x, y + it->asc - it->size * 30 / 100, it->w, MAX(1, (int)S), it->color);
}

// ---- editing text fields ------------------------------------------------------------
// The focused field has a caret and a selection: byte offsets into its value, from fanchor
// to fcaret. A one-line field scrolls sideways to keep the caret in view; a textarea wraps
// its lines and scrolls down. Keys: arrows (by character, Ctrl: by word; Shift selects),
// Home/End, Backspace/Delete, Ctrl+A/C/X/V, Enter (a new line in a textarea), Tab to the
// next field. The mouse places the caret, drags a selection, double-clicks a word.

static int fcaret, fanchor;  // in the focused control's value
static int fsx, fsy;         // its scroll: pixels sideways (one line), lines down (textarea)
static int fwant_x = -1;     // textarea: the x that Up/Down keep to
static int field_drag;       // the mouse is selecting in the focused field
static double field_click_t;
static int field_clicks, field_click_x, field_click_y;

static int is_mark(const u8 *p);
static int fsize(void) { return (int)(14 * S); }
static int fw(const char *s, int len) { return len > 0 ? lw_text_width(s, len, fsize(), 0) : 0; }
static int is_text_field(const Control *k) { return k->kind == K_TEXT || k->kind == K_PASSWORD || k->kind == K_TEXTAREA; }

// What a field shows: its value, or a dot for each character of a password.
static char fdots[12288];
static const char *field_shown(const Control *k, int *len) {
    if (k->kind != K_PASSWORD) { *len = k->vlen; return k->value; }
    int n = 0;
    for (int i = 0; i < k->vlen && n + 3 <= (int)sizeof fdots; i++)
        if ((k->value[i] & 0xC0) != 0x80) { fdots[n++] = (char)0xE2; fdots[n++] = (char)0x80; fdots[n++] = (char)0xA2; }
    *len = n;
    return fdots;
}

static int shown_off(const Control *k, int off) {  // an offset in the value -> in what is shown
    if (k->kind != K_PASSWORD) return off;
    int n = 0;
    for (int i = 0; i < off && i < k->vlen; i++) n += (k->value[i] & 0xC0) != 0x80;
    return n * 3;
}

static int value_off(const Control *k, int shown) {  // and back
    if (k->kind != K_PASSWORD) return shown;
    int cps = shown / 3, i = 0;
    while (i < k->vlen && cps > 0) {
        i++;
        while (i < k->vlen && (k->value[i] & 0xC0) == 0x80) i++;
        cps--;
    }
    return i;
}

// Character boundaries (a Thai vowel or tone mark goes with the letter before it).
static int next_char(const char *v, int len, int i) {
    if (i >= len) return len;
    i += utf8_len((u8)v[i]);
    while (i < len && len - i >= 2 && is_mark((const u8 *)v + i)) i += utf8_len((u8)v[i]);
    return i > len ? len : i;
}

static int prev_char(const char *v, int i) {
    if (i <= 0) return 0;
    do {
        i--;
        while (i > 0 && (v[i] & 0xC0) == 0x80) i--;
    } while (i > 0 && is_mark((const u8 *)v + i));
    return i;
}

static int prev_cp(const char *v, int i) {  // Backspace takes one code point (a mark on its own)
    if (i <= 0) return 0;
    i--;
    while (i > 0 && (v[i] & 0xC0) == 0x80) i--;
    return i;
}

static int word_byte(char c) { return c != ' ' && c != '\n' && c != '\t'; }
static int next_word(const char *v, int len, int i) {
    while (i < len && !word_byte(v[i])) i++;
    while (i < len && word_byte(v[i])) i++;
    return i;
}
static int prev_word(const char *v, int i) {
    while (i > 0 && !word_byte(v[i - 1])) i--;
    while (i > 0 && word_byte(v[i - 1])) i--;
    return i;
}

// The last character boundary b of s[0..len] whose text s[0..b] is at most x wide.
static int fit_boundary(const char *s, int len, int x) {
    if (x < 0) return 0;
    int lo = 0, hi = len;
    while (lo < hi) {
        int mid = lo + (hi - lo + 1) / 2;
        while (mid < hi && ((s[mid] & 0xC0) == 0x80 || is_mark((const u8 *)s + mid))) mid++;
        if (fw(s, mid) <= x) {
            lo = mid;
        } else {
            hi = mid - 1;
            while (hi > lo && ((s[hi] & 0xC0) == 0x80 || is_mark((const u8 *)s + hi))) hi--;
        }
    }
    return lo;
}

static int nearest_boundary(const char *s, int len, int x) {
    if (x <= 0) return 0;
    int b = fit_boundary(s, len, x), nb = next_char(s, len, b);
    if (nb > b && x - fw(s, b) > fw(s, nb) - x) b = nb;
    return b;
}

// A textarea's lines: paragraphs wrapped at spaces (or anywhere in a long word).
#define MAX_FLINES 800
static int fl_start[MAX_FLINES], fl_len[MAX_FLINES], fl_n;

static void field_lines(const Control *k, int w) {
    const char *v = k->value;
    int p = 0;
    fl_n = 0;
    for (;;) {
        int e = p;
        while (e < k->vlen && v[e] != '\n') e++;
        int q = p;
        do {
            int take = e - q;
            if (take && fw(v + q, take) > w) {
                int fit = fit_boundary(v + q, take, w), sp = 0;
                for (int i = 1; i <= fit; i++)
                    if (v[q + i - 1] == ' ') sp = i;
                take = sp ? sp : fit ? fit : next_char(v + q, take, 0);
            }
            if (fl_n < MAX_FLINES) {
                fl_start[fl_n] = q;
                fl_len[fl_n] = take;
                fl_n++;
            }
            q += take;
        } while (q < e);
        if (e >= k->vlen) break;
        p = e + 1;
    }
}

static int caret_line(int off) {
    int l = 0;
    while (l + 1 < fl_n && fl_start[l + 1] <= off) l++;
    return l;
}

static Item *control_item(int ref) {
    for (int i = 0; i < draw_limit; i++)
        if (items[i].kind == IT_CONTROL && items[i].ref == ref) return &items[i];
    return 0;
}

static int field_inner_w(const Item *it) { return it ? it->w - (int)(12 * S) : (int)(300 * S); }
static int field_line_h(void) { return (int)(18 * S); }

// The value offset under view point (x, y) in the field (laid out as `it`).
static int field_pos_at(Control *k, Item *it, int x, int y) {
    int tx = it->x + (int)(6 * S);
    if (k->kind == K_TEXTAREA) {
        field_lines(k, field_inner_w(it));
        int top = it->y - scroll_y + (int)(4 * S), dy = y - top;
        int l = (it->ref == focus ? fsy : 0) + (dy < 0 ? -1 : dy / field_line_h());
        if (l < 0) l = 0;
        if (l >= fl_n) l = fl_n - 1;
        return fl_start[l] + nearest_boundary(k->value + fl_start[l], fl_len[l], x - tx);
    }
    int len;
    const char *sh = field_shown(k, &len);
    return value_off(k, nearest_boundary(sh, len, x - tx + (it->ref == focus ? fsx : 0)));
}

static void draw_field(Item *it, int y, Control *k) {
    int focused = it->ref == focus, b = MAX(1, (int)S), size = fsize();
    u32 border = focused ? RGB(26, 115, 232) : RGB(118, 118, 118);
    fill(it->x, y, it->w, it->h, RGB(255, 255, 255));
    frame_rect(it->x, y, it->w, it->h, focused ? 2 * b : b, border);
    int tx = it->x + (int)(6 * S), iw = field_inner_w(it), th = ascent(size) + descent(size);
    if (focused) {
        if (fcaret > k->vlen) fcaret = k->vlen;
        if (fanchor > k->vlen) fanchor = k->vlen;
    }
    int s0 = focused ? MIN(fcaret, fanchor) : 0, s1 = focused ? MAX(fcaret, fanchor) : 0;
    int blink = focused && ((int)((lw_now() - caret_t) / 530) & 1) == 0;
    if (!k->vlen && !focused) {
        const char *ph = attr(k->node, "placeholder");
        int ty = k->kind == K_TEXTAREA ? y + (int)(4 * S) : y + (it->h - th) / 2;
        if (ph) lw_text(fb, W, H, tx, ty, ph, lw_strlen(ph), size, 0, RGB(150, 150, 150));
        return;
    }
    if (k->kind == K_TEXTAREA) {
        field_lines(k, iw);
        int lh = field_line_h(), rows = MAX(1, (it->h - (int)(8 * S)) / lh);
        if (focused) {
            int cl = caret_line(fcaret);
            if (cl < fsy) fsy = cl;
            if (cl >= fsy + rows) fsy = cl - rows + 1;
        }
        int first = focused ? fsy : 0;
        for (int l = first; l < fl_n && l < first + rows; l++) {
            int ly = y + (int)(4 * S) + (l - first) * lh, st = fl_start[l], len = fl_len[l];
            const char *v = k->value + st;
            int a = MAX(s0, st), e = MIN(s1, st + len);
            if (s1 > s0 && (a < e || (s1 > st + len && s0 <= st + len))) {
                int x0 = fw(v, a - st), x1 = fw(v, e - st) + (s1 > st + len ? (int)(4 * S) : 0);
                fill(tx + x0, ly, MIN(x1, iw) - x0, th, C_SELECTION);
            }
            int shown = len;
            while (shown > 0 && v[shown - 1] == '\n') shown--;
            lw_text(fb, W, H, tx, ly, v, shown, size, 0, C_TEXT);
            if (blink && l == caret_line(fcaret)) fill(tx + fw(v, fcaret - st), ly, MAX(1, (int)S), th, C_TEXT);
        }
        return;
    }
    int len, ty = y + (it->h - th) / 2;
    const char *sh = field_shown(k, &len);
    int cx = focused ? fw(sh, shown_off(k, fcaret)) : 0;
    if (focused) {  // keep the caret in view
        int total = fw(sh, len);
        if (cx - fsx > iw) fsx = cx - iw;
        if (cx < fsx) fsx = cx;
        if (fsx > 0 && total - fsx < iw) fsx = MAX(0, total - iw);
    }
    int sx = focused ? fsx : 0;
    int a = fit_boundary(sh, len, sx);
    if (fw(sh, a) < sx) a = next_char(sh, len, a);
    int e = fit_boundary(sh, len, sx + iw);
    if (s1 > s0) {
        int x0 = fw(sh, MAX(shown_off(k, s0), a)) - sx, x1 = fw(sh, MIN(shown_off(k, s1), e)) - sx;
        if (x1 > x0) fill(tx + x0, ty, x1 - x0, th, C_SELECTION);
    }
    if (e > a) lw_text(fb, W, H, tx + fw(sh, a) - sx, ty, sh + a, e - a, size, 0, C_TEXT);
    if (blink) fill(tx + cx - sx, ty, MAX(1, (int)S), th, C_TEXT);
}

static void draw_control(Item *it, int y) {
    Control *k = &controls[it->ref];
    Style cs;
    __builtin_memset(&cs, 0, sizeof cs);
    cs.size = (short)(14 * S);
    int ty = y + (it->h - ascent(cs.size) - descent(cs.size)) / 2;
    u32 border = k->node == 0 ? 0 : (it->ref == focus ? RGB(26, 115, 232) : RGB(118, 118, 118));
    int b = MAX(1, (int)S);
    switch (k->kind) {
    case K_CHECKBOX: case K_RADIO:
        fill(it->x, y, it->w, it->h, RGB(255, 255, 255));
        frame_rect(it->x, y, it->w, it->h, b, border);
        if (k->checked) fill(it->x + it->w / 4, y + it->h / 4, it->w / 2, it->h / 2, RGB(26, 115, 232));
        return;
    case K_SUBMIT: case K_RESET: case K_BUTTON: case K_IMAGE: {
        fill(it->x, y, it->w, it->h, hover_control == it->ref ? RGB(229, 229, 229) : RGB(240, 240, 240));
        frame_rect(it->x, y, it->w, it->h, b, RGB(160, 160, 160));
        const char *l = k->label ? k->label : "Go";
        int ll = k->label ? k->label_len : 2;
        int tw = text_w(l, ll, &cs);
        lw_text(fb, W, H, it->x + (it->w - tw) / 2, ty, l, ll, cs.size, 0, C_TEXT);
        return;
    }
    case K_SELECT: {
        fill(it->x, y, it->w, it->h, RGB(255, 255, 255));
        frame_rect(it->x, y, it->w, it->h, b, border);
        lw_text(fb, W, H, it->x + (int)(8 * S), ty, k->label, k->label_len, cs.size, 0, C_TEXT);
        lw_text(fb, W, H, it->x + it->w - (int)(18 * S), ty, "\xE2\x96\xBE", 3, cs.size, 0, C_TEXT);
        return;
    }
    default: draw_field(it, y, k); return;
    }
}

static void draw_pill(int x, int y, const char *s, int len, int size, u32 bg, u32 fg) {
    Style st;
    __builtin_memset(&st, 0, sizeof st);
    st.size = (short)size;
    int tw = lw_text_width(s, len, size, 0);
    int ph = ascent(size) + descent(size) + (int)(6 * S);
    fill(x, y, tw + (int)(16 * S), ph, bg);
    lw_text(fb, W, H, x + (int)(8 * S), y + (int)(3 * S), s, len, size, 0, fg);
}

// The absolute form of a link, for the status bubble (the browser resolves the real
// navigation itself).
static int display_url(const char *href, char *out, int cap) {
    int hl = lw_strlen(href), k = 0;
    int has_scheme = 0;
    for (int i = 0; i < hl && i < 12; i++) {
        if (href[i] == ':') { has_scheme = i > 1; break; }
        if (!is_alnum((u8)href[i]) && href[i] != '+' && href[i] != '-' && href[i] != '.') break;
    }
    int start = 0;  // how much of doc_url to keep
    if (has_scheme || !doc_url_len) start = 0;
    else if (hl >= 2 && href[0] == '/' && href[1] == '/') {  // scheme-relative
        while (start < doc_url_len && doc_url[start] != ':') start++;
        start++;
    } else if (href[0] == '/') {  // origin + path
        int slashes = 0;
        while (start < doc_url_len && !(doc_url[start] == '/' && ++slashes == 3)) start++;
    } else if (href[0] == '#' || href[0] == '?') {
        while (start < doc_url_len && doc_url[start] != (href[0] == '#' ? '#' : '?')) start++;
    } else {  // relative to the directory
        int q = 0;
        while (q < doc_url_len && doc_url[q] != '?' && doc_url[q] != '#') q++;
        for (int i = 0; i < q; i++)
            if (doc_url[i] == '/') start = i + 1;
    }
    for (int i = 0; i < start && k < cap - 1; i++) out[k++] = doc_url[i];
    for (int i = 0; i < hl && k < cap - 1; i++) out[k++] = href[i];
    return k;
}

// ---- selecting text ------------------------------------------------------------------
// The two ends of the selection are places in the text items' strings: pointers into the
// document's text, which stays where it is when the page is laid out again (the items
// don't), so the selection survives relayouts. Item indexes are looked up when needed.

static const char *sel_a, *sel_b;  // where the selection started, where it ends now; 0 = none
static int sel_drag;               // the left button is down and the mouse has moved: selecting
static int press_down, press_x, press_y;
static const char *press_pos;      // the text place where the button went down
static double last_press_t;
static int press_count;            // 1 click, 2 double click (a word), 3 triple click (a paragraph)
static int drag_x, drag_y;         // the mouse, while selecting (it may be outside the view)

static int item_w(const Item *it, int len) {
    Style st;
    __builtin_memset(&st, 0, sizeof st);
    st.size = it->size;
    st.flags = it->flags;
    return len <= 0 ? 0 : len >= it->len ? it->w : text_w(it->s, len, &st);
}

static int is_mark(const u8 *p) {  // a combining mark: no caret before it (Thai vowels and tones above and below)
    u32 cp = p[0] == 0xE0 && p[1] == 0xB8 ? 0x0E00 + (p[2] & 0x3F) : p[0] == 0xE0 && p[1] == 0xB9 ? 0x0E40 + (p[2] & 0x3F)
           : p[0] == 0xCC || (p[0] == 0xCD && p[1] < 0xB0) ? 0x300 : 0;
    return cp == 0x300 || cp == 0x0E31 || (cp >= 0x0E34 && cp <= 0x0E3A) || (cp >= 0x0E47 && cp <= 0x0E4E);
}

static int next_boundary(const Item *it, int k) {
    k += utf8_len((u8)it->s[k]);
    while (k < it->len && it->len - k >= 2 && is_mark((const u8 *)it->s + k)) k += utf8_len((u8)it->s[k]);
    return k > it->len ? it->len : k;
}

static int item_of(const char *p) {
    if (!p) return -1;
    for (int i = 0; i < draw_limit; i++)
        if (items[i].kind == IT_TEXT && p >= items[i].s && p <= items[i].s + items[i].len) return i;
    return -1;
}

// The text place nearest to (x, y) in the view: on the line under it the closest character
// boundary; left or right of a line its start or end.
static const char *text_pos_at(int x, int y) {
    int dy = y + scroll_y, best = -1;
    long long best_d = 1LL << 62;
    for (int i = 0; i < draw_limit; i++) {
        Item *it = &items[i];
        if (it->kind != IT_TEXT || !it->len) continue;
        long long d = dy < it->y ? (long long)(it->y - dy) << 16 : dy >= it->y + it->h ? (long long)(dy - it->y - it->h + 1) << 16 : 0;
        d += x < it->x ? it->x - x : x >= it->x + it->w ? x - it->x - it->w + 1 : 0;
        if (d < best_d) { best_d = d; best = i; }
    }
    if (best < 0) return 0;
    Item *it = &items[best];
    if (x <= it->x) return it->s;
    if (x >= it->x + it->w) return it->s + it->len;
    int k = 0, kw = 0;
    while (k < it->len) {
        int n = next_boundary(it, k), nw = item_w(it, n);
        if (x - it->x < (kw + nw) / 2) break;
        k = n;
        kw = nw;
    }
    return it->s + k;
}

// The selection in document order: items i0..i1, from p0 in i0 to p1 in i1. 0 if none.
static int sel_range(int *i0, const char **p0, int *i1, const char **p1) {
    if (!sel_a || !sel_b || sel_a == sel_b) return 0;
    int ia = item_of(sel_a), ib = item_of(sel_b);
    if (ia < 0 || ib < 0) return 0;
    if (ia < ib || (ia == ib && sel_a < sel_b)) { *i0 = ia; *p0 = sel_a; *i1 = ib; *p1 = sel_b; }
    else { *i0 = ib; *p0 = sel_b; *i1 = ia; *p1 = sel_a; }
    return 1;
}

static int same_line(const Item *a, const Item *b) { return a->y + a->asc == b->y + b->asc; }

// What goes between two neighbouring text items when they are copied.
static int item_gap(const Item *prev, const Item *it, char *out) {
    const char *pe = prev->s + prev->len;
    if (it->s >= pe && it->s - pe < 64) {  // the same text node: copy what is between (spaces, newlines in <pre>)
        int ws = 1;
        for (const char *q = pe; q < it->s && ws; q++) ws = *q == ' ' || *q == '\n' || *q == '\t';
        if (ws) {
            int n = (int)(it->s - pe);
            if (out) __builtin_memcpy(out, pe, (u32)n);
            return n;
        }
    }
    const char *g = same_line(prev, it) ? (it->x > prev->x + prev->w ? " " : "")
                  : it->y - (prev->y + prev->h) > prev->h / 3 ? "\n\n" : "\n";
    int n = lw_strlen(g);
    if (out) __builtin_memcpy(out, g, (u32)n);
    return n;
}

// The selected text as UTF-8 (out = 0: just its length).
static int selection_text(char *out) {
    int i0, i1, k = 0;
    const char *p0, *p1;
    if (!sel_range(&i0, &p0, &i1, &p1)) return 0;
    Item *prev = 0;
    for (int i = i0; i <= i1; i++) {
        Item *it = &items[i];
        if (it->kind != IT_TEXT) continue;
        const char *a = i == i0 ? p0 : it->s, *b = i == i1 ? p1 : it->s + it->len;
        if (prev) k += item_gap(prev, it, out ? out + k : 0);
        if (out) __builtin_memcpy(out + k, a, (u32)(b - a));
        k += (int)(b - a);
        prev = it;
    }
    return k;
}

static void copy_selection(void) {
    int n = selection_text(0);
    if (n <= 0) return;
    char *buf = (char *)must_alloc((u32)n);
    selection_text(buf);
    lw_clipboard_set(buf, n);
    mem_free(buf);
}

static void select_all_text(void) {
    sel_a = sel_b = 0;
    for (int i = 0; i < draw_limit; i++)
        if (items[i].kind == IT_TEXT && items[i].len) {
            if (!sel_a) sel_a = items[i].s;
            sel_b = items[i].s + items[i].len;
        }
    dirty = 1;
}

// A double click selects the word (the text item: words are laid out one by one), a triple
// click the run of text it belongs to.
static void select_around(const char *p, int what) {
    int i = item_of(p);
    if (i < 0) return;
    int a = i, b = i;
    if (what == 3) {
        while (a > 0 && items[a - 1].kind == IT_TEXT && items[a].s > items[a - 1].s &&
               items[a].s - (items[a - 1].s + items[a - 1].len) == 1 && items[a].s[-1] == ' ') a--;
        while (b + 1 < draw_limit && items[b + 1].kind == IT_TEXT && items[b + 1].s > items[b].s &&
               items[b + 1].s - (items[b].s + items[b].len) == 1 && items[b + 1].s[-1] == ' ') b++;
    }
    sel_a = items[a].s;
    sel_b = items[b].s + items[b].len;
    dirty = 1;
}

// ---- find in page (lw_find) ------------------------------------------------------------
// The browser's find bar (Ctrl+F) asks. Matches are looked for in the laid-out text as
// copying would give it (so "quick brown" is found across a wrapped line), ignoring case,
// and kept as item positions, in document order; when the layout changes they are looked
// for again.

typedef struct { int i0, o0, i1, o1; } Match;  // from item i0 at byte o0 to item i1 at byte o1
static Match *matches;
static int nmatches, matches_cap, match_cur = -1;
static char find_text[256];
static int find_len;
static u8 find_pass;
static int find_limit;  // draw_limit at the last search
static double find_t;   // and when it was

static void scroll_to(int y);
static u8 fold(u8 c) { return c >= 'A' && c <= 'Z' ? (u8)(c + 32) : c; }

static void find_run(void) {
    nmatches = 0;
    find_pass = layout_pass;
    find_limit = draw_limit;
    find_t = lw_now();
    if (!find_len) return;
    int total = 0, nt = 0;
    Item *prev = 0;
    for (int i = 0; i < draw_limit; i++) {
        Item *it = &items[i];
        if (it->kind != IT_TEXT) continue;
        if (prev) total += item_gap(prev, it, 0);
        total += it->len;
        nt++;
        prev = it;
    }
    if (total < find_len) return;
    char *t = (char *)must_alloc((u32)total);
    int *ti = (int *)must_alloc((u32)nt * 4), *tp = (int *)must_alloc((u32)nt * 4);  // each text item and where it starts in t
    int k = 0, n = 0;
    prev = 0;
    for (int i = 0; i < draw_limit; i++) {
        Item *it = &items[i];
        if (it->kind != IT_TEXT) continue;
        if (prev) k += item_gap(prev, it, t + k);
        ti[n] = i;
        tp[n++] = k;
        __builtin_memcpy(t + k, it->s, (u32)it->len);
        k += it->len;
        prev = it;
    }
    int j = 0;  // the text item the search is in
    for (int pos = 0; pos + find_len <= k && nmatches < 0xFFFF; pos++) {
        int q = 0;
        while (q < find_len && fold((u8)t[pos + q]) == fold((u8)find_text[q])) q++;
        if (q < find_len) continue;
        Match m;
        while (j + 1 < n && tp[j + 1] <= pos) j++;
        if (pos >= tp[j] + items[ti[j]].len && j + 1 < n) { m.i0 = ti[j + 1]; m.o0 = 0; }  // starts in a gap
        else { m.i0 = ti[j]; m.o0 = pos - tp[j]; }
        int e = pos + find_len, je = j;
        while (je + 1 < n && tp[je + 1] < e) je++;
        m.i1 = ti[je];
        m.o1 = MIN(e - tp[je], items[ti[je]].len);
        matches = (Match *)grow_array(matches, nmatches, &matches_cap, sizeof(Match));
        matches[nmatches++] = m;
        pos = e - 1;
    }
    mem_free(t);
    mem_free(ti);
    mem_free(tp);
}

static void find_reveal(void) {
    if (match_cur < 0 || match_cur >= nmatches) return;
    Item *it = &items[matches[match_cur].i0];
    if (it->y < scroll_y + (int)(50 * S) || it->y + it->h > scroll_y + H - (int)(60 * S)) scroll_to(it->y - H / 3);
    dirty = 1;
}

// how: 0 = look for text[0..len) (memory from lw_alloc, freed here), starting from what is
// in view; 1 / -1 = the next / previous match; 2 = stop (no highlights); 3 = just tell.
// Returns (the current match, from 1) << 16 | the number of matches, or 0 for none.
LW_EXPORT(lw_find) int lw_find(const char *text, int len, int how) {
    if (how == 0) {
        find_len = MIN(MAX(len, 0), (int)sizeof find_text);
        while (find_len > 0 && find_len < len && (text[find_len] & 0xC0) == 0x80) find_len--;
        if (find_len) __builtin_memcpy(find_text, text, (u32)find_len);
        if (text) mem_free((void *)text);
        find_run();
        match_cur = -1;
        for (int m = 0; m < nmatches && match_cur < 0; m++)
            if (items[matches[m].i0].y >= scroll_y) match_cur = m;
        if (match_cur < 0 && nmatches) match_cur = 0;
        find_reveal();
        dirty = 1;
    } else if ((how == 1 || how == -1) && nmatches) {
        match_cur = (match_cur + how + nmatches) % nmatches;
        find_reveal();
    } else if (how == 2) {
        find_len = nmatches = 0;
        match_cur = -1;
        dirty = 1;
    }
    return nmatches ? (match_cur + 1) << 16 | nmatches : 0;
}

static void draw_matches(int i, Item *it, int y) {  // behind item i's text
    int lo = 0, hi = nmatches;  // the first match that doesn't end before item i
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (matches[mid].i1 < i) lo = mid + 1;
        else hi = mid;
    }
    for (int m = lo; m < nmatches && matches[m].i0 <= i; m++) {
        Match *mt = &matches[m];
        int x0 = it->x + (i == mt->i0 ? item_w(it, mt->o0) : 0);
        int x1 = it->x + (i == mt->i1 ? item_w(it, mt->o1) : it->w);
        if (i < mt->i1 && i + 1 < draw_limit && items[i + 1].kind == IT_TEXT && same_line(it, &items[i + 1]) && items[i + 1].x > x1)
            x1 = items[i + 1].x;
        if (x1 > x0) fill(x0, y, x1 - x0, it->h, m == match_cur ? RGB(255, 150, 50) : RGB(255, 236, 120));
    }
}

static void redraw(void) {
    u32 page_bg = has_body_bg ? body_bg : RGB(255, 255, 255);
    fill_span(fb, W * H, page_bg);
    int top = scroll_y, bottom = scroll_y + H;
    int s0 = -1, s1 = -1;
    const char *q0 = 0, *q1 = 0;
    if (!sel_range(&s0, &q0, &s1, &q1)) s0 = s1 = -1;
    for (int i = 0; i < nvideos; i++) videos[i].visible = 0;
    if (video_full >= 0) {  // just the video, over all of the view
        fill(0, 0, W, H, RGB(0, 0, 0));
        __builtin_memset(&full_item, 0, sizeof full_item);
        full_item.kind = IT_VIDEO;
        full_item.w = W;
        full_item.h = H;
        full_item.ref = video_full;
        full_item.link = -1;
        draw_video_item(&full_item, 0);
        lw_present(fb, W, H);
        return;
    }
    for (int pass = 0; pass < (nfloats ? 2 : 1); pass++)
    for (int i = 0; i < draw_limit; i++) {
        Item *it = &items[i];
        if (it->y > bottom || it->y + it->h < top || it->layer != pass) continue;
        int y = it->y - scroll_y;
        if (it->kind == IT_TEXT && i >= s0 && i <= s1) {  // selected: a background behind the text
            int x0 = it->x + (i == s0 ? item_w(it, (int)(q0 - it->s)) : 0);
            int x1 = it->x + (i == s1 ? item_w(it, (int)(q1 - it->s)) : it->w);
            if (i < s1 && i + 1 < draw_limit && items[i + 1].kind == IT_TEXT && same_line(it, &items[i + 1]) && items[i + 1].x > x1)
                x1 = items[i + 1].x;  // and the space to the next word
            if (x1 > x0) fill(x0, y, x1 - x0, it->h, C_SELECTION);
        }
        if (it->kind == IT_TEXT && nmatches) draw_matches(i, it, y);
        switch (it->kind) {
        case IT_TEXT: draw_text_item(it, y, it->link >= 0 && it->link == hover_link); break;
        case IT_RECT:
            fill(it->x, y, it->w, it->h, it->color);
            if (it->ref == 1) frame_rect(it->x, y, it->w, it->h, MAX(1, (int)S), RGB(190, 190, 190));
            break;
        case IT_IMAGE: draw_image(it, y); break;
        case IT_CONTROL: draw_control(it, y); break;
        case IT_VIDEO: draw_video_item(it, y); break;
        }
    }
    if (js_notice) {
        const char *m = "This page shows little without JavaScript, which Low-web does not run.";
        draw_pill((int)(16 * S), H - (int)(80 * S), m, lw_strlen(m), (int)(14 * S), RGB(254, 243, 199), RGB(120, 53, 15));
    }
    // scrollbar
    if (doc_h > H) {
        int tw_ = (int)(8 * S), tx = W - tw_ - (int)(2 * S);
        int th = MAX((int)((u64)H * (u64)H / (u64)doc_h), (int)(30 * S));
        int ty = (int)((u64)scroll_y * (u64)(H - th) / (u64)MAX(doc_h - H, 1));
        fill(tx, 0, tw_, H, RGBA(0, 0, 0, 12));
        fill(tx, ty, tw_, th, drag_scroll >= 0 ? RGBA(0, 0, 0, 140) : RGBA(0, 0, 0, 90));
    }
    // status bubble for the hovered link
    if (hover_link >= 0 && hover_link < nlinks) {
        char full[1200];
        int len = display_url(links[hover_link].href, full, (int)sizeof full);
        const char *h = full;
        len = MIN(len, 200);
        int size = (int)(13 * S);
        draw_pill(0, H - ascent(size) - descent(size) - (int)(6 * S), h, len, size, RGB(241, 243, 244), RGB(60, 64, 67));
    } else {
        const char *badge = !main_node ? "HTML \xC2\xB7 full page"
                          : full_page ? "Full page \xC2\xB7 click for reader view"
                                      : "Reader view \xC2\xB7 click for the full page";
        int size = (int)(11 * S);
        int tw = lw_text_width(badge, lw_strlen(badge), size, 0) + (int)(16 * S);
        badge_x = W - tw - (int)(16 * S);
        badge_y = H - ascent(size) - descent(size) - (int)(10 * S);
        badge_w = tw;
        badge_h = ascent(size) + descent(size) + (int)(6 * S);
        draw_pill(badge_x, badge_y, badge, lw_strlen(badge), size,
                  RGBA(60, 64, 67, 200), RGB(255, 255, 255));
    }
    if (toast_len) {
        int size = (int)(15 * S);
        int tw = lw_text_width(toast_msg, toast_len, size, 0) + (int)(16 * S);
        draw_pill((W - tw) / 2, H - (int)(60 * S), toast_msg, toast_len, size, RGB(40, 40, 44), RGB(255, 255, 255));
    }
    lw_present(fb, W, H);
}

// =====================================================================================
// navigation, forms, input
// =====================================================================================

static int clamp_scroll(int y) {
    int max = doc_h - H;
    if (y > max) y = max;
    if (y < 0) y = 0;
    return y;
}

static void scroll_to(int y) { target_y = clamp_scroll(y); dirty = 1; }

// Whether an element's id (or name) is the #fragment `f`: as it is, or percent-decoded,
// as browsers try it (a Thai heading's id comes as "#%E0%B8%9B..." in links and addresses).
static int anchor_is(const char *a, const char *f, int len) {
    int i = 0, j = 0;
    while (i < len && a[j] && a[j] == f[i]) i++, j++;
    if (i == len && !a[j]) return 1;
    for (i = 0, j = 0; i < len; j++) {
        int c = (u8)f[i];
        if (c == '%' && i + 2 < len && hexv(f[i + 1]) >= 0 && hexv(f[i + 2]) >= 0) {
            c = hexv(f[i + 1]) * 16 + hexv(f[i + 2]);
            i += 3;
        } else {
            i++;
        }
        if ((u8)a[j] != c || !a[j]) return 0;
    }
    return !a[j];
}

static Node *find_anchor(Node *n, const char *id, int len) {
    for (Node *c = n->first; c; c = c->next) {
        if (c->type != N_ELEM) continue;
        const char *a = attr(c, "id");
        if (!a) a = attr(c, "name");
        if (a && anchor_is(a, id, len)) return c;
        Node *f = find_anchor(c, id, len);
        if (f) return f;
    }
    return 0;
}

// A #fragment whose target the (incremental) layout hasn't reached yet.
static Node *pending_anchor;

static int restore_y = -1;  // a reading position to go back to (lw_restore), once laid out that far

// A page the browser unloads (a tab put to sleep, back/forward, reload) is asked where the
// reader was, and gets it back when it is loaded again: the scroll position, and whether
// the full page was shown instead of reader view.
LW_EXPORT(lw_state) int lw_state(void) { return (target_y < 0 ? 0 : target_y) << 1 | (full_page & 1); }

LW_EXPORT(lw_restore) void lw_restore(int state) {
    if (state <= 0) return;
    full_page = state & 1;
    restore_y = state >> 1;  // (and it wins over the URL's #fragment: doc_begin)
}

static void check_restore(void) {
    if (restore_y < 0 || !has_doc) return;
    if (pending_anchor) restore_y = -1;  // the reader followed a link meanwhile
    else if (layout_running && laid_y < restore_y + H) return;
    else scroll_y = target_y = clamp_scroll(restore_y);
    restore_y = -1;
    dirty = 1;
}

static void check_anchor(void) {
    Node *a = pending_anchor;
    if (!a || !has_doc) return;
    int reached = a->lpass == layout_pass && (!layout_running || laid_y >= a->y + H);
    if (reached) scroll_to(a->y - (int)(8 * S));
    if (reached || !layout_running) pending_anchor = 0;  // done, or it never showed up (hidden)
}

// how: 0 = this tab, 1 = a new tab in front, 2 = a new tab in the background
static void follow(const char *href, int how) {
    int len = lw_strlen(href);
    if (len && href[0] == '#') {
        Node *a = root ? find_anchor(root, href + 1, len - 1) : 0;
        pending_anchor = a;
        if (a) check_anchor();
        else if (len == 1) scroll_to(0);
        return;
    }
    if (iprefix(href, len, "mailto:") || iprefix(href, len, "tel:")) {
        show_toast("Low-web can't open mail or phone links.");
        return;
    }
    if (how) lw_open_tab(href, len, how == 1);
    else lw_navigate(href, len);
}

// application/x-www-form-urlencoded
static int url_encode(const char *s, int len, char *o, int k, int cap) {
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < len && k + 3 < cap; i++) {
        u8 c = (u8)s[i];
        if (is_alnum(c) || c == '-' || c == '_' || c == '.' || c == '*') o[k++] = (char)c;
        else if (c == ' ') o[k++] = '+';
        else { o[k++] = '%'; o[k++] = hex[c >> 4]; o[k++] = hex[c & 15]; }
    }
    return k;
}

static void submit(Node *form, Control *by) {
    if (!form) return;
    const char *method = attr(form, "method");
    int post = method && ieq(method, "post");
    static char url[8192];
    int k = 0;
    const char *action = attr(form, "action");
    if (action)  // GET replaces the action's query; POST keeps the action as it is
        for (int i = 0; action[i] && (post || action[i] != '?') && action[i] != '#' && k < 4000; i++) url[k++] = action[i];
    int url_len = k;
    if (!post) url[k++] = '?';
    int first = 1;
    for (int i = 0; i < ncontrols; i++) {
        Control *c = &controls[i];
        if (c->form != form || !c->name || !*c->name || c->disabled) continue;
        const char *v = c->value;
        int vl = c->vlen;
        switch (c->kind) {
        case K_CHECKBOX: case K_RADIO:
            if (!c->checked) continue;
            if (!attr(c->node, "value")) { v = "on"; vl = 2; }
            break;
        case K_SUBMIT: case K_IMAGE:
            if (c != by) continue;
            break;
        case K_RESET: case K_BUTTON: case K_OTHER: continue;
        case K_SELECT: {
            Node *o = nth_option(c->node, c->option, 0);
            if (!o) continue;
            const char *ov = attr(o, "value");
            if (ov) { v = ov; vl = lw_strlen(ov); }
            else { v = c->label; vl = c->label_len; }
            break;
        }
        }
        if (!first && k < (int)sizeof url - 1) url[k++] = '&';
        first = 0;
        k = url_encode(c->name, lw_strlen(c->name), url, k, (int)sizeof url);
        if (k < (int)sizeof url - 1) url[k++] = '=';
        k = url_encode(v, vl, url, k, (int)sizeof url);
    }
    if (post) lw_navigate_post(url, url_len, url + url_len, k - url_len);
    else lw_navigate(url, k);
}

static void activate_control(int idx) {
    Control *k = &controls[idx];
    if (k->disabled) return;
    focus = -1;
    switch (k->kind) {
    case K_TEXT: case K_PASSWORD: case K_TEXTAREA:
        focus = idx;
        fcaret = fanchor = k->vlen;
        fsx = fsy = 0;
        fwant_x = -1;
        sel_a = sel_b = 0;  // typing goes to the field now, and so does Ctrl+C
        caret_t = lw_now();
        break;
    case K_CHECKBOX: k->checked = !k->checked; break;
    case K_RADIO:
        for (int i = 0; i < ncontrols; i++)
            if (controls[i].kind == K_RADIO && controls[i].form == k->form && controls[i].name && k->name && ieq(controls[i].name, k->name))
                controls[i].checked = 0;
        k->checked = 1;
        break;
    case K_SELECT: {
        int count = 0;
        nth_option(k->node, 0, &count);
        if (count) { k->option = (k->option + 1) % count; select_label(k); need_layout = 1; }
        break;
    }
    case K_SUBMIT: case K_IMAGE: submit(k->form, k); break;
    case K_RESET:
        for (int i = 0; i < ncontrols; i++)
            if (controls[i].form == k->form && (controls[i].kind == K_TEXT || controls[i].kind == K_PASSWORD)) {
                const char *v = attr(controls[i].node, "value");
                set_value(&controls[i], v ? v : "", v ? lw_strlen(v) : 0);
            }
        break;
    }
    dirty = 1;
}

static int item_at(int x, int y, int kind) {
    int dy = y + scroll_y;
    for (int i = draw_limit - 1; i >= 0; i--) {
        Item *it = &items[i];
        if (kind == IT_CONTROL || kind == IT_VIDEO ? it->kind != kind : it->link < 0) continue;
        if (x >= it->x && x < it->x + it->w && dy >= it->y && dy < it->y + it->h) return i;
    }
    return -1;
}

// Play, or pause; the first time, opens it.
static void video_toggle(int vi) {
    Video *v = &videos[vi];
    dirty = 1;
    if (!v->handle) {
        const char *src = video_src(v->node);
        if (!src) {
            v->state = LW_VIDEO_FAILED;
            v->error = "it names no file to play (the site may play it with JavaScript)";
            return;
        }
        v->handle = lw_video_open(src, lw_strlen(src));
        if (!v->handle) {
            v->state = LW_VIDEO_FAILED;
            v->error = "too many videos on this page";
            return;
        }
        v->state = LW_VIDEO_LOADING;
        if (v->muted) lw_video_volume(v->handle, 1, 1);
        lw_video_play(v->handle);
    } else if (v->state == LW_VIDEO_PLAYING) {
        lw_video_pause(v->handle);
    } else if (v->state != LW_VIDEO_FAILED) {
        if (v->state == LW_VIDEO_ENDED) lw_video_seek(v->handle, 0);
        lw_video_play(v->handle);
    }
}

static void video_fullscreen(int vi, int on) {
    if (on) {
        lw_fullscreen(1);  // (the browser says no unless the user clicked or pressed a key)
        video_full = vi;
        video_focus = vi;
    } else {
        lw_fullscreen(0);
        video_full = -1;
    }
    dirty = 1;
}

static void video_seek_to(int vi, double t) {
    Video *v = &videos[vi];
    if (!v->handle || v->dur <= 0) return;
    v->t = t < 0 ? 0 : t > v->dur ? v->dur : t;  // (shown at once)
    lw_video_seek(v->handle, v->t);
    dirty = 1;
}

static Item *video_item(int vi) {
    if (vi == video_full) return &full_item;
    for (int i = 0; i < draw_limit; i++)
        if (items[i].kind == IT_VIDEO && items[i].ref == vi) return &items[i];
    return 0;
}

static int item_screen_y(const Item *it) { return it == &full_item ? 0 : it->y - scroll_y; }

static double pic_click_t;
static int pic_click_vi = -1;

// A press on a video's picture or bar, or a drag along its track. 1 if it was one.
static int video_pointer(int kind, int x, int y, int button) {
    if (video_drag >= 0) {
        Item *it = video_item(video_drag);
        if (it && (kind == LW_MOVE || kind == LW_UP)) {
            VGeo g;
            video_geo(it, item_screen_y(it), &g);
            video_seek_to(video_drag, videos[video_drag].dur * (x - g.tx0) / MAX(g.tx1 - g.tx0, 1));
        }
        if (kind == LW_UP || !it) video_drag = -1;
        return 1;
    }
    int ii = video_full >= 0 ? -1 : item_at(x, y, IT_VIDEO);
    if (ii < 0 && video_full < 0) {
        if (kind == LW_DOWN && video_focus >= 0) { video_focus = -1; dirty = 1; }
        return 0;
    }
    if (kind != LW_DOWN || button != 0) return 1;
    Item *it = video_full >= 0 ? &full_item : &items[ii];
    int vi = it->ref;
    Video *v = &videos[vi];
    if (video_focus != vi) { video_focus = vi; dirty = 1; }
    VGeo g;
    video_geo(it, item_screen_y(it), &g);
    if (y < g.by) {  // the picture: play or pause; twice quickly: fullscreen (and as it was)
        double now = lw_now();
        int twice = pic_click_vi == vi && now - pic_click_t < 400;
        video_toggle(vi);
        if (twice && !v->audio) video_fullscreen(vi, video_full != vi);
        pic_click_t = twice ? 0 : now;
        pic_click_vi = vi;
    } else if (x < g.bx1) video_toggle(vi);  // the play button
    else if (x >= g.fx0 && x < g.mx0) video_fullscreen(vi, video_full != vi);
    else if (x >= g.mx0) {
        v->muted = !v->muted;
        if (v->handle) lw_video_volume(v->handle, 1, v->muted);
        dirty = 1;
    } else if (g.tx1 > g.tx0 && x >= g.tx0 - (int)(8 * S)) {
        video_drag = vi;
        video_seek_to(vi, v->dur * (x - g.tx0) / MAX(g.tx1 - g.tx0, 1));
    }
    return 1;
}

// Keys for the video clicked last: Space or K plays/pauses, arrows go 5 s back/on, M mutes.
static int video_key(int key) {
    if (video_focus < 0) return 0;
    Video *v = &videos[video_focus];
    if (key == LW_KEY_SPACE || key == 'K') video_toggle(video_focus);
    else if (key == 'F' && !v->audio) video_fullscreen(video_focus, video_full != video_focus);
    else if (key == LW_KEY_ESCAPE && video_full >= 0) video_fullscreen(video_full, 0);
    else if ((key == LW_KEY_LEFT || key == LW_KEY_RIGHT) && v->handle) video_seek_to(video_focus, v->t + (key == LW_KEY_LEFT ? -5 : 5));
    else if (key == 'M') {
        v->muted = !v->muted;
        if (v->handle) lw_video_volume(v->handle, 1, v->muted);
        dirty = 1;
    } else return 0;
    return 1;
}

// ---- the right-click menu ---------------------------------------------------------------
// Its items are for what is under the mouse: a link, an image, a video, the selection. The
// browser shows them above its own (Back, Forward, Reload) and says which one was picked.

enum { MA_NONE, MA_OPEN_LINK, MA_COPY_LINK, MA_OPEN_IMAGE, MA_SAVE_IMAGE, MA_COPY_IMAGE, MA_PLAY, MA_MUTE, MA_COPY_VIDEO,
       MA_COPY, MA_SELECT_ALL, MA_FULL_PAGE };
#define MAX_MENU 24
static u8 menu_acts[MAX_MENU];
static int nmenu, menu_video = -1;
static const char *menu_href, *menu_img;  // (strings of the document: they stay put while the menu is open)
static int save_fetch = -1;               // the image being fetched to be saved
static char save_name[128];
static int save_name_len;

static int image_item_at(int x, int y) {
    int dy = y + scroll_y;
    for (int i = draw_limit - 1; i >= 0; i--) {
        Item *it = &items[i];
        if (it->kind == IT_IMAGE && it->ref >= 0 && x >= it->x && x < it->x + it->w && dy >= it->y && dy < it->y + it->h) return i;
    }
    return -1;
}

static void menu_item(char *buf, int *k, int cap, const char *label, int act) {
    int n = lw_strlen(label);
    if (nmenu >= MAX_MENU || *k + n + 1 >= cap) return;
    if (nmenu) buf[(*k)++] = '\n';
    __builtin_memcpy(buf + *k, label, (u32)n);
    *k += n;
    menu_acts[nmenu++] = (u8)act;
}

static void open_menu(int x, int y) {
    static char buf[512];
    int k = 0;
    nmenu = 0;
    menu_href = menu_img = 0;
    menu_video = video_full;
    if (video_full < 0) {
        int vi = item_at(x, y, IT_VIDEO);
        if (vi >= 0) menu_video = items[vi].ref;
        int li = item_at(x, y, IT_TEXT);  // (anything that is a link)
        if (li >= 0 && items[li].link >= 0 && items[li].link < nlinks) menu_href = links[items[li].link].href;
        int ii = image_item_at(x, y);
        if (ii >= 0 && items[ii].ref < nimgs) menu_img = imgs[items[ii].ref].url;
    }
    if (menu_href) {
        menu_item(buf, &k, (int)sizeof buf, "Open link in new &tab", MA_OPEN_LINK);
        menu_item(buf, &k, (int)sizeof buf, "Copy &link address", MA_COPY_LINK);
        menu_item(buf, &k, (int)sizeof buf, "-", MA_NONE);
    }
    if (menu_img) {
        int data = iprefix(menu_img, lw_strlen(menu_img), "data:");  // (in the page itself: no address to give)
        menu_item(buf, &k, (int)sizeof buf, data ? "~Open image in new tab" : "Open &image in new tab", MA_OPEN_IMAGE);
        menu_item(buf, &k, (int)sizeof buf, data ? "~Save image as..." : "Sa&ve image as...", MA_SAVE_IMAGE);
        menu_item(buf, &k, (int)sizeof buf, data ? "~Copy image address" : "C&opy image address", MA_COPY_IMAGE);
        menu_item(buf, &k, (int)sizeof buf, "-", MA_NONE);
    }
    if (menu_video >= 0) {
        Video *v = &videos[menu_video];
        menu_item(buf, &k, (int)sizeof buf, v->state == LW_VIDEO_PLAYING ? "&Pause" : "&Play", MA_PLAY);
        menu_item(buf, &k, (int)sizeof buf, v->muted ? "Un&mute" : "&Mute", MA_MUTE);
        menu_item(buf, &k, (int)sizeof buf, video_src(v->node) ? "Copy vi&deo address" : "~Copy video address", MA_COPY_VIDEO);
        menu_item(buf, &k, (int)sizeof buf, "-", MA_NONE);
    }
    menu_item(buf, &k, (int)sizeof buf, sel_a && sel_b && sel_a != sel_b ? "&Copy\tCtrl+C" : "~&Copy\tCtrl+C", MA_COPY);
    menu_item(buf, &k, (int)sizeof buf, "Select &all\tCtrl+A", MA_SELECT_ALL);
    if (main_node) menu_item(buf, &k, (int)sizeof buf, full_page ? "Show reader vie&w" : "Show full pa&ge", MA_FULL_PAGE);
    lw_menu(buf, k);
}

// The file name an address ends with ("image" if none).
static int url_file_name(const char *u, char *out, int cap) {
    int n = 0, start = 0;
    while (u[n] && u[n] != '?' && u[n] != '#') n++;
    for (int i = 0; i < n; i++)
        if (u[i] == '/') start = i + 1;
    int k = 0;
    for (int i = start; i < n && k < cap - 1; i++) out[k++] = u[i];
    if (!k)
        for (const char *d = "image"; *d && k < cap - 1; d++) out[k++] = *d;
    return k;
}

LW_EXPORT(lw_on_menu) void lw_on_menu(int i) {
    static char u[4096];
    if (i < 0 || i >= nmenu) return;
    switch (menu_acts[i]) {
    case MA_OPEN_LINK:
        if (!menu_href) break;
        if (iprefix(menu_href, lw_strlen(menu_href), "mailto:") || iprefix(menu_href, lw_strlen(menu_href), "tel:")) follow(menu_href, 1);
        else lw_open_tab(u, display_url(menu_href, u, (int)sizeof u), 1);
        break;
    case MA_COPY_LINK: if (menu_href) lw_clipboard_set(u, display_url(menu_href, u, (int)sizeof u)); break;
    case MA_OPEN_IMAGE: if (menu_img) lw_open_tab(u, display_url(menu_img, u, (int)sizeof u), 1); break;
    case MA_COPY_IMAGE: if (menu_img) lw_clipboard_set(u, display_url(menu_img, u, (int)sizeof u)); break;
    case MA_SAVE_IMAGE:  // fetched again (from the cache, usually), then handed to the browser to save
        if (!menu_img) break;
        save_name_len = url_file_name(menu_img, save_name, (int)sizeof save_name);
        save_fetch = lw_fetch(u, display_url(menu_img, u, (int)sizeof u));
        break;
    case MA_PLAY: if (menu_video >= 0 && menu_video < nvideos) video_toggle(menu_video); break;
    case MA_MUTE:
        if (menu_video >= 0 && menu_video < nvideos) {
            Video *v = &videos[menu_video];
            v->muted = !v->muted;
            if (v->handle) lw_video_volume(v->handle, 1, v->muted);
            dirty = 1;
        }
        break;
    case MA_COPY_VIDEO:
        if (menu_video >= 0 && menu_video < nvideos) {
            const char *src = video_src(videos[menu_video].node);
            if (src) lw_clipboard_set(u, display_url(src, u, (int)sizeof u));
        }
        break;
    case MA_COPY: copy_selection(); break;
    case MA_SELECT_ALL: select_all_text(); dirty = 1; break;
    case MA_FULL_PAGE:
        full_page = !full_page;
        need_layout = 1;
        scroll_y = target_y = 0;
        break;
    }
}

// Each frame: what the playing videos are doing. Their bars are drawn again when what they
// show changes (the browser draws the pictures by itself).
static void videos_tick(void) {
    for (int i = 0; i < nvideos; i++) {
        Video *v = &videos[i];
        if (!v->handle) continue;
        double inf[6];
        int st = lw_video_info(v->handle, inf);
        if (st == LW_VIDEO_FAILED && v->state != LW_VIDEO_FAILED) {
            char e[300];
            int n = lw_video_error(v->handle, e, (int)sizeof e);
            v->error = arena_str(e, MAX(0, MIN(n, (int)sizeof e)));
        }
        if (st != v->state && v->visible) dirty = 1;  // the picture's part changes too (the play button, an error)
        v->state = st;
        if (video_drag != i) v->t = inf[0];
        v->dur = inf[1];
        v->buf = inf[2];
        v->vw = (int)inf[3];
        v->vh = (int)inf[4];
        v->waiting = inf[5] != 0;
        const char *aw = attr(v->node, "width"), *ah = attr(v->node, "height");
        if (!v->audio && v->vw && v->vw != v->laid_vw && !(aw && ah)) need_layout = 1;  // now its shape is known
        int shown = st * 2 + v->waiting + (int)v->t * 16 + (int)v->dur * 7919;
        if (v->dur > 0) shown += (int)(v->t * 400 / v->dur) * 104729 + (int)((v->t + v->buf) * 100 / v->dur) * 1299709;
        if (shown != v->shown) {
            v->shown = shown;
            if (v->visible) bars_dirty = 1;
        }
    }
}

// Only the bars changed (time, progress): draw them again over the last frame, place the
// pictures again (the browser draws only the videos placed for each frame) and show it.
// Far cheaper than drawing the whole page several times a second while a video plays.
static void redraw_bars(void) {
    for (int i = 0; i < nvideos; i++) {
        Video *v = &videos[i];
        if (!v->visible || v->item < -2 || v->item == -1 || v->item >= nitems) continue;
        Item *it = v->item == -2 ? &full_item : &items[v->item];
        draw_video_bar(it, v->screen_y);
        if (v->handle && v->state != LW_VIDEO_FAILED && !v->audio) {
            VGeo g;
            video_geo(it, v->screen_y, &g);
            lw_video_place(v->handle, g.px, g.py, g.pw, g.ph, 0, 0, W, H);
        }
    }
    lw_present(fb, W, H);
}

static int pressed_link = -1;
static int mouse_x, mouse_y, mouse_in, hover_scroll;  // where the mouse is, to update the hover when the page scrolls

LW_EXPORT(lw_pointer) int lw_pointer(int kind, float fx, float fy, int button) {
    int x = (int)fx, y = (int)fy;
    mouse_x = x;
    mouse_y = y;
    mouse_in = kind != LW_LEAVE;
    hover_scroll = scroll_y;
    int bar_x = W - (int)(12 * S);
    if (button == 2 && (kind == LW_DOWN || kind == LW_UP)) {  // the right button: the menu
        if (kind == LW_UP) open_menu(x, y);
        return LW_CURSOR_ARROW;
    }
    if (video_full >= 0) {
        if (kind != LW_WHEEL && kind != LW_LEAVE) video_pointer(kind, x, y, button);
        return LW_CURSOR_ARROW;
    }
    if (kind == LW_WHEEL) {
        pending_anchor = 0;  // the reader took over
        restore_y = -1;
        scroll_to(target_y - button * (int)(100 * S) / 120);
        return LW_CURSOR_ARROW;
    }
    if (kind == LW_LEAVE) {
        if (hover_link >= 0 || hover_control >= 0) dirty = 1;
        hover_link = hover_control = -1;
        return LW_CURSOR_ARROW;
    }
    if (drag_scroll >= 0) {
        if (kind == LW_UP) { drag_scroll = -1; dirty = 1; }
        else if (doc_h > H) {
            int th = MAX((int)((u64)H * (u64)H / (u64)doc_h), (int)(30 * S));
            int dy = y - drag_start_y;
            scroll_y = target_y = clamp_scroll(drag_start_scroll + (int)((long long)dy * (doc_h - H) / MAX(H - th, 1)));
            dirty = 1;
        }
        return LW_CURSOR_ARROW;
    }
    if (kind == LW_DOWN && button == 0 && x >= bar_x && doc_h > H) {
        drag_scroll = 1;
        drag_start_y = y;
        int th = MAX((int)((u64)H * (u64)H / (u64)doc_h), (int)(30 * S));
        int ty = (int)((u64)scroll_y * (u64)(H - th) / (u64)MAX(doc_h - H, 1));
        if (y < ty || y > ty + th) {  // clicked the track: jump there
            scroll_y = target_y = clamp_scroll((int)((long long)(y - th / 2) * (doc_h - H) / MAX(H - th, 1)));
        }
        drag_start_scroll = scroll_y;
        dirty = 1;
        return LW_CURSOR_ARROW;
    }
    if (main_node && hover_link < 0 && x >= badge_x && x < badge_x + badge_w && y >= badge_y && y < badge_y + badge_h) {
        if (kind == LW_UP && button == 0) {
            full_page = !full_page;
            need_layout = 1;
            scroll_y = target_y = 0;
        }
        return LW_CURSOR_HAND;
    }
    if (field_drag) {  // selecting in the focused field
        Item *fi = focus >= 0 ? control_item(focus) : 0;
        if (fi) {
            int p = field_pos_at(&controls[focus], fi, x, y);
            if (p != fcaret) { fcaret = p; dirty = 1; }
        }
        if (kind == LW_UP && button == 0) field_drag = 0;
        return LW_CURSOR_TEXT;
    }
    if (!sel_drag && video_pointer(kind, x, y, button)) {
        if (hover_link >= 0 || hover_control >= 0) { hover_link = hover_control = -1; dirty = 1; }
        return LW_CURSOR_HAND;
    }
    drag_x = x;
    drag_y = y;
    if (press_down && kind == LW_MOVE && !sel_drag && (x - press_x) * (x - press_x) + (y - press_y) * (y - press_y) > 16) {
        sel_drag = 1;  // the press became a drag: select text (and don't follow the link it started on)
        pressed_link = -1;
        if (!(lw_mods() & LW_SHIFT) || !sel_a) sel_a = press_pos;
    }
    if (sel_drag) {
        const char *p = text_pos_at(x, y);
        if (p && p != sel_b) { sel_b = p; dirty = 1; }
        if (kind == LW_UP && button == 0) { sel_drag = 0; press_down = 0; }
        return LW_CURSOR_TEXT;
    }
    int ci = item_at(x, y, IT_CONTROL);
    int li = ci < 0 ? item_at(x, y, IT_TEXT) : -1;
    int link = li >= 0 ? items[li].link : -1;
    int ctl = ci >= 0 ? items[ci].ref : -1;
    if (link != hover_link || ctl != hover_control) { hover_link = link; hover_control = ctl; dirty = 1; }
    if (kind == LW_DOWN && (button == 0 || button == 1)) {
        pressed_link = link;
        if (button == 0 && ctl >= 0 && is_text_field(&controls[ctl]) && !controls[ctl].disabled) {
            Control *k = &controls[ctl];
            int was = focus, p;
            if (was != ctl) activate_control(ctl);
            p = field_pos_at(k, &items[ci], x, y);
            double now = lw_now();
            int near = (x - field_click_x) * (x - field_click_x) + (y - field_click_y) * (y - field_click_y) <= 25;
            field_clicks = now - field_click_t < 500 && near && was == ctl ? field_clicks % 3 + 1 : 1;
            field_click_t = now;
            field_click_x = x;
            field_click_y = y;
            if (field_clicks == 3 || (field_clicks == 2 && k->kind == K_PASSWORD)) {
                fanchor = 0;
                fcaret = k->vlen;
            } else if (field_clicks == 2) {  // a word
                int a = p, e = p;
                while (a > 0 && word_byte(k->value[a - 1])) a--;
                while (e < k->vlen && word_byte(k->value[e])) e++;
                fanchor = a;
                fcaret = e;
            } else {
                fcaret = p;
                if (!(lw_mods() & LW_SHIFT) || was != ctl) fanchor = p;
                field_drag = 1;
            }
            fwant_x = -1;
            caret_t = now;
            dirty = 1;
        } else if (button == 0 && ctl >= 0) {
            activate_control(ctl);
        } else if (button == 0 && focus >= 0) {
            focus = -1;
            dirty = 1;
        }
    }
    if (kind == LW_DOWN && button == 0 && ctl < 0) {
        double now = lw_now();
        int near = (x - press_x) * (x - press_x) + (y - press_y) * (y - press_y) <= 25;
        press_count = now - last_press_t < 500 && near ? press_count % 3 + 1 : 1;
        last_press_t = now;
        press_down = 1;
        press_x = x;
        press_y = y;
        press_pos = text_pos_at(x, y);
        if ((lw_mods() & LW_SHIFT) && sel_a) {  // Shift+click: extend the selection to here
            sel_b = press_pos;
            sel_drag = 1;
            pressed_link = -1;
            dirty = 1;
        } else if (press_count > 1 && link < 0) {
            select_around(press_pos, press_count);
        } else if (sel_a) {
            sel_a = sel_b = 0;  // a click clears the selection
            dirty = 1;
        }
    }
    if (kind == LW_UP && button == 0) press_down = 0;
    if (kind == LW_UP && (button == 0 || button == 1)) {
        if (link >= 0 && link == pressed_link && link < nlinks) {
            const char *target = links[link].target;
            int how = button == 1 || (lw_mods() & LW_CTRL) ? 2 : target && ieq(target, "_blank") ? 1 : 0;
            follow(links[link].href, how);
        }
        pressed_link = -1;
    }
    if (ctl >= 0) {
        int kk = controls[ctl].kind;
        return kk == K_TEXT || kk == K_PASSWORD || kk == K_TEXTAREA ? LW_CURSOR_TEXT : LW_CURSOR_HAND;
    }
    if (link >= 0) return LW_CURSOR_HAND;
    int ti = -1;  // over text: the text cursor, as browsers show
    for (int i = draw_limit - 1, dy = y + scroll_y; i >= 0 && ti < 0; i--)
        if (items[i].kind == IT_TEXT && x >= items[i].x && x < items[i].x + items[i].w && dy >= items[i].y && dy < items[i].y + items[i].h) ti = i;
    return ti >= 0 ? LW_CURSOR_TEXT : LW_CURSOR_ARROW;
}

static int field_sel(int *a, int *b) {
    *a = MIN(fcaret, fanchor);
    *b = MAX(fcaret, fanchor);
    return *b > *a;
}

static void field_set_caret(int off, int extend) {
    fcaret = off;
    if (!extend) fanchor = off;
    caret_t = lw_now();
    dirty = 1;
}

// The selection (or just the caret's place) becomes t[0..n).
static void field_replace(Control *k, const char *t, int n) {
    int a, b;
    field_sel(&a, &b);
    int room = 4000 - (k->vlen - (b - a));
    if (n > room) {
        n = room < 0 ? 0 : room;
        while (n > 0 && (t[n] & 0xC0) == 0x80) n--;  // not in the middle of a character
    }
    int len = k->vlen - (b - a) + n;
    char *v = (char *)must_alloc((u32)len + 1);
    __builtin_memcpy(v, k->value, (u32)a);
    __builtin_memcpy(v + a, t, (u32)n);
    __builtin_memcpy(v + a + n, k->value + b, (u32)(k->vlen - b));
    set_value(k, v, len);
    mem_free(v);
    fcaret = fanchor = a + n;
    fwant_x = -1;
    caret_t = lw_now();
    dirty = 1;
}

// Pastes the clipboard into the focused field, over its selection. One-line fields get
// newlines and tabs as spaces.
static void paste_into_field(Control *k) {
    char buf[4096];
    int n = lw_clipboard_get(buf, (int)sizeof buf);
    if (n <= 0) return;
    if (n > (int)sizeof buf) n = (int)sizeof buf;
    while (n > 0 && (buf[n - 1] & 0xC0) == 0x80) n--;  // a character cut off at the end
    if (n > 0 && (u8)buf[n - 1] >= 0xC0) n--;
    int m = 0;
    for (int i = 0; i < n; i++) {
        u8 c = (u8)buf[i];
        if (c == '\r') continue;
        buf[m++] = c == '\n' && k->kind == K_TEXTAREA ? '\n' : c < 32 ? ' ' : (char)c;
    }
    field_replace(k, buf, m);
}

// Tab: the next (or previous) text field, with its value selected, scrolled into view.
static void focus_next(int dir) {
    int i = focus;
    for (int step = 0; step < ncontrols; step++) {
        i = (i + dir + ncontrols) % ncontrols;
        Control *c = &controls[i];
        Item *it = control_item(i);
        if (!is_text_field(c) || c->disabled || !it) continue;
        activate_control(i);
        fanchor = 0;
        fcaret = c->vlen;
        if (it->y < scroll_y || it->y + it->h > scroll_y + H) scroll_to(it->y - H / 3);
        return;
    }
    focus = -1;
    dirty = 1;
}

LW_EXPORT(lw_char) void lw_char(int cp) {
    if (focus < 0) return;
    u8 buf[4];
    field_replace(&controls[focus], (const char *)buf, utf8_put(buf, (u32)cp));
}

LW_EXPORT(lw_key) int lw_key(int key, int mods, int down) {
    if (!down) return 0;
    restore_y = -1;  // the reader took over
    int ctrl = mods & LW_CTRL, shift = mods & LW_SHIFT;
    int copy = (ctrl && key == 'C') || (ctrl && key == LW_KEY_INSERT), paste = (ctrl && key == 'V') || (shift && key == LW_KEY_INSERT);
    int cut = ctrl && key == 'X';
    if (focus < 0 && !(mods & (LW_CTRL | LW_ALT)) && video_key(key)) return 1;
    if (focus >= 0) {
        Control *k = &controls[focus];
        int secret = k->kind == K_PASSWORD, area = k->kind == K_TEXTAREA, a, b;
        if (fcaret > k->vlen) fcaret = k->vlen;
        if (fanchor > k->vlen) fanchor = k->vlen;
        int has = field_sel(&a, &b);
        if (ctrl && key == 'A') { fanchor = 0; fcaret = k->vlen; dirty = 1; return 1; }
        if (paste) { paste_into_field(k); return 1; }
        if (copy || cut) {  // the selected part (never a password's)
            if (has && !secret) {
                lw_clipboard_set(k->value + a, b - a);
                if (cut) field_replace(k, "", 0);
            }
            return 1;
        }
        switch (key) {
        case LW_KEY_LEFT:
            field_set_caret(has && !shift ? a : ctrl ? prev_word(k->value, fcaret) : prev_char(k->value, fcaret), shift);
            fwant_x = -1;
            return 1;
        case LW_KEY_RIGHT:
            field_set_caret(has && !shift ? b : ctrl ? next_word(k->value, k->vlen, fcaret) : next_char(k->value, k->vlen, fcaret), shift);
            fwant_x = -1;
            return 1;
        case LW_KEY_HOME: case LW_KEY_END: {
            int to = key == LW_KEY_HOME ? 0 : k->vlen;
            if (area && !ctrl) {  // the start or end of the line
                field_lines(k, field_inner_w(control_item(focus)));
                int l = caret_line(fcaret);
                to = key == LW_KEY_HOME ? fl_start[l] : fl_start[l] + fl_len[l];
                if (key == LW_KEY_END && to > fl_start[l] && l + 1 < fl_n && k->value[to - 1] == ' ') to--;  // before a wrap
            }
            field_set_caret(to, shift);
            fwant_x = -1;
            return 1;
        }
        case LW_KEY_UP: case LW_KEY_DOWN: {
            if (!area) break;  // a one-line field: the page scrolls
            field_lines(k, field_inner_w(control_item(focus)));
            int l = caret_line(fcaret), nl = l + (key == LW_KEY_DOWN ? 1 : -1);
            if (fwant_x < 0) fwant_x = fw(k->value + fl_start[l], fcaret - fl_start[l]);
            int want = fwant_x;
            if (nl < 0) field_set_caret(0, shift);
            else if (nl >= fl_n) field_set_caret(k->vlen, shift);
            else field_set_caret(fl_start[nl] + nearest_boundary(k->value + fl_start[nl], fl_len[nl], want), shift);
            fwant_x = want;
            return 1;
        }
        case LW_KEY_BACKSPACE:
            if (!has) fanchor = ctrl ? prev_word(k->value, fcaret) : prev_cp(k->value, fcaret);
            field_replace(k, "", 0);
            return 1;
        case LW_KEY_DELETE:
            if (!has) fanchor = ctrl ? next_word(k->value, k->vlen, fcaret) : next_char(k->value, k->vlen, fcaret);
            field_replace(k, "", 0);
            return 1;
        case LW_KEY_ENTER:
            if (area) {
                field_replace(k, "\n", 1);
            } else {
                Control *by = 0;  // implicit submission uses the form's first submit button
                for (int i = 0; i < ncontrols; i++)
                    if (controls[i].form == k->form && (controls[i].kind == K_SUBMIT || controls[i].kind == K_IMAGE)) { by = &controls[i]; break; }
                submit(k->form, by);
            }
            return 1;
        case LW_KEY_TAB: focus_next(shift ? -1 : 1); return 1;
        case LW_KEY_ESCAPE: focus = -1; dirty = 1; return 1;
        }
        if (key != LW_KEY_UP && key != LW_KEY_DOWN && key != LW_KEY_PAGEUP && key != LW_KEY_PAGEDOWN) return 1;
    }
    if (ctrl && key == 'A') { select_all_text(); return 1; }
    if (copy) { copy_selection(); return 1; }
    if (key == LW_KEY_ESCAPE && sel_a) { sel_a = sel_b = 0; dirty = 1; return 1; }
    int line = (int)(40 * S), pageh = H - (int)(60 * S);
    switch (key) {
    case LW_KEY_DOWN: scroll_to(target_y + line); return 1;
    case LW_KEY_UP: scroll_to(target_y - line); return 1;
    case LW_KEY_PAGEDOWN: case LW_KEY_SPACE: scroll_to(target_y + pageh); return 1;
    case LW_KEY_PAGEUP: scroll_to(target_y - pageh); return 1;
    case LW_KEY_HOME: scroll_to(0); return 1;
    case LW_KEY_END: scroll_to(doc_h); return 1;
    }
    return 0;
}

// =====================================================================================
// entry points
// =====================================================================================

static int refresh_ms = -1;
static char refresh_url[1024];
static int refresh_len;

static void meta_refresh(Node *c) {
    const char *he = attr(c, "http-equiv"), *content = attr(c, "content");
    if (!he || !content || !ieq(he, "refresh")) return;
    int secs = parse_int(content, 0);
    const char *u = content;
    while (*u && !iprefix(u, lw_strlen(u), "url")) u++;
    if (!*u) return;
    u += 3;
    while (*u == ' ' || *u == '=' || *u == '\'' || *u == '"') u++;
    int len = lw_strlen(u);
    while (len && (u[len - 1] == '\'' || u[len - 1] == '"' || u[len - 1] == ' ')) len--;
    if (len && len < (int)sizeof refresh_url && secs >= 0 && secs <= 10) {
        __builtin_memcpy(refresh_url, u, (u32)len);
        refresh_len = len;
        refresh_ms = secs * 1000;
    }
}

static int count_text(Node *n) {
    int k = 0;
    for (Node *c = n->first; c; c = c->next) {
        if (c->type == N_TEXT) {
            for (u32 i = 0; i < c->len; i++) k += c->text[i] != ' ';
        } else if (c->type == N_ELEM && !(tag_flags[c->tag] & FS) && !is_hidden(c)) k += count_text(c);
        if (k > 200) return k;
    }
    return k;
}

static double doc_t0;
static double last_progress_draw;
#define SLICE_MS 12  // parse and layout time per frame; input is handled between frames

static void log_ms(const char *what, int ms) {
    char msg[160];
    int k = 0;
    while (*what && k < 100) msg[k++] = *what++;
    char tmp[12];
    int t = 0;
    do { tmp[t++] = (char)('0' + ms % 10); ms /= 10; } while (ms);
    while (t) msg[k++] = tmp[--t];
    msg[k++] = ' '; msg[k++] = 'm'; msg[k++] = 's';
    lw_log(msg, k);
}

// Starts a layout and runs it until the visible part is done, so a relayout (resize, late
// CSS, images) never shows a half-empty screen; the rest comes in later frames.
static void relayout(void) {
    if (css_width >= 0 && css_width != (int)(W / S) && nsheets) compute_css();  // @media may differ now
    need_layout = 0;
    images_changed = 0;
    layout_start();
    layout_step(lw_now() + SLICE_MS, scroll_y + H);
    if (!layout_running) last_layout = lw_now();
    dirty = 1;
}

// ---- the document, as it streams in ------------------------------------------------------

static int doc_started;
static double parse_cpu;
static int parse_stuck_at = -1;  // source length when the parser last ran out of whole tokens
static Node *first_main;         // the first <main> (as find_main would pick it), once parsed
static const char *frag;         // the URL's #fragment, to scroll to once it is laid out
static int frag_len, frag_found;

// Reader view needs to know the whole document: the <main>, or the only <article>.
static Node *pick_main(void) {
    int articles = 0;
    Node *article = 0;
    Node *m = find_main(root, &articles, &article);
    if (!m && articles == 1) m = article;
    if (m && count_text(m) < 40) m = 0;  // an empty <main> filled by scripts
    return m;
}

static void node_opened(Node *n) {
    if (!first_main) {
        const char *role = attr(n, "role");
        if (n->tag == T_MAIN || (role && ieq(role, "main"))) {
            int ok = 1;  // find_main doesn't look inside <article>s or skipped elements
            for (Node *p = n->parent; p && ok; p = p->parent)
                if (p->type == N_ELEM && (p->tag == T_ARTICLE || (tag_flags[p->tag] & FS))) ok = 0;
            if (ok) first_main = n;
        }
    }
    if (frag_len && !frag_found) {  // the #fragment's target (find_anchor's rule: id, else name)
        const char *a = attr(n, "id");
        if (!a) a = attr(n, "name");
        if (a && anchor_is(a, frag, frag_len)) {
            frag_found = 1;
            pending_anchor = n;
        }
    }
}

static void node_closed(Node *n) {
    if (n->type != N_ELEM) return;
    switch (n->tag) {
    case T_INPUT: case T_BUTTON: case T_SELECT: case T_TEXTAREA: make_control(n); break;
    case T_STYLE: case T_LINK: register_sheet(n); break;
    case T_META: meta_refresh(n); break;
    case T_TITLE:
        if (!title_node) {
            title_node = n;
            char buf[300];
            int tl = text_content(n, buf, sizeof buf, 0);
            int s = 0;
            while (s < tl && buf[s] == ' ') s++;
            while (tl > s && buf[tl - 1] == ' ') tl--;
            if (tl > s) lw_set_title(buf + s, tl - s);
        }
        break;
    }
}

// Shows the document: once its body has begun, the stylesheets found so far are in (or
// 2.5 s have passed), and it is clear whether reader view applies.
static void show_document(void) {
    has_doc = 1;
    if (parse_done) js_notice = script_count > 0 && count_text(root) < 40;
    double t0 = lw_now();
    relayout();
    log_ms("viewer: first screen laid out in ", (int)(lw_now() - t0));
    log_ms("viewer: first screen after ", (int)(lw_now() - doc_t0));
    char m[80];
    int k = 0, kb = (parse_dropped + parse_pos) >> 10;
    const char *w = "viewer: ... with this many KB parsed: ";
    while (*w) m[k++] = *w++;
    char tmp[12];
    int t = 0;
    do { tmp[t++] = (char)('0' + kb % 10); kb /= 10; } while (kb);
    while (t) m[k++] = tmp[--t];
    lw_log(m, k);
    last_progress_draw = lw_now();
}

static void maybe_show(void) {
    if (has_doc || !doc_started) return;
    if (!body_seen && !parse_done) return;
    if (sheets_pending && lw_now() < css_wait_until) return;
    if (!css_built) {
        double t0 = lw_now();
        compute_css();
        log_ms("viewer: css rules ", (int)(lw_now() - t0));
    }
    Node *m;
    if (parse_done) m = pick_main();
    else if (first_main) {
        if (count_text(first_main) >= 40) m = first_main;
        else if (!first_main->open) m = 0;
        else return;  // wait for more of it
    } else if (parse_dropped + parse_pos < (256 << 10) && lw_now() - doc_t0 < 1000) {
        return;  // no <main> yet; pages that have one mostly have it well before this much (or this long)
    } else {
        m = 0;  // show the full page for now (see maybe_late_main); settled when the parse is done
    }
    main_node = m;
    show_document();
}

// The full page is shown and a <main> turns up after all (a slow connection): switch to
// reader view now, if the reader hasn't scrolled away from the top yet.
static void maybe_late_main(void) {
    if (!has_doc || main_node || parse_done || !first_main || full_page || scroll_y || target_y) return;
    if (count_text(first_main) < 40) return;
    main_node = first_main;
    need_layout = 1;
}

static void finish_parse(void) {
    while (sp > 1) pop();
    root->open = 0;
    parse_done = 1;
    mem_free(raw_buf);
    mem_free(doc_buf);
    raw_buf = doc_buf = 0;
    raw_len = raw_cap = doc_len = doc_cap = 0;
    log_ms("viewer: parse ", (int)parse_cpu);
    if (has_doc) {  // what needed the whole document
        js_notice = script_count > 0 && count_text(root) < 40;
        Node *m = pick_main();
        if (m != main_node) {
            main_node = m;
            need_layout = 1;
        }
        dirty = 1;
    }
}

static void pump_parse(double deadline) {
    if (!doc_started || parse_done || !decide_charset()) return;
    convert_more();
    const u8 *d = doc_cs == CS_UTF8 ? raw_buf + raw_skip : doc_buf;
    int n = doc_cs == CS_UTF8 ? raw_len - raw_skip : doc_len;
    if (n < 0) n = 0;
    if (!raw_done && n == parse_stuck_at) return;  // nothing new since it needed more
    double t0 = lw_now();
    int finished_run = parse_some(d, n, raw_done, deadline);
    parse_cpu += lw_now() - t0;
    // What is parsed is not needed again (nodes keep their own copies): move the rest to the
    // front, so the buffer holds what came but isn't parsed yet, not the whole document.
    if (!raw_done && parse_pos >= (64 << 10) && parse_pos >= n - parse_pos) {
        int rest = n - parse_pos;
        if (doc_cs == CS_UTF8) {
            __builtin_memmove(raw_buf, raw_buf + raw_skip + parse_pos, (u32)rest);
            raw_len = rest;
            raw_skip = raw_used = 0;
        } else {  // all of raw_buf is converted by now
            __builtin_memmove(doc_buf, doc_buf + parse_pos, (u32)rest);
            doc_len = rest;
            raw_len = raw_used = 0;
        }
        parse_dropped += parse_pos;
        parse_pos = 0;
        n = rest;
    }
    parse_stuck_at = finished_run ? n : -1;
    if (raw_done && parse_pos >= n) finish_parse();
}

static void doc_begin(const char *type, int type_len, const char *url, int url_len) {
    if (doc_started) return;
    doc_started = 1;
    doc_t0 = lw_now();
    doc_url_len = MIN(url_len, (int)sizeof doc_url - 1);
    __builtin_memcpy(doc_url, url, (u32)doc_url_len);
    doc_ctype_len = MIN(type_len, (int)sizeof doc_ctype);
    __builtin_memcpy(doc_ctype, type, (u32)doc_ctype_len);
    lw_set_title(doc_url, doc_url_len);  // until the <title> is parsed
    for (int i = 0; i < doc_url_len && restore_y < 0; i++)
        if (doc_url[i] == '#') {
            frag = doc_url + i + 1;
            frag_len = doc_url_len - i - 1;
            break;
        }
    parse_begin();
}

static void doc_data(u8 *data, int len) {  // takes ownership of data (from lw_alloc)
    if (!doc_started || raw_done) { mem_free(data); return; }
    if (!raw_buf) {
        raw_buf = data;
        raw_len = raw_cap = len;
        return;
    }
    buf_append(&raw_buf, &raw_len, &raw_cap, data, len);
    mem_free(data);
}

static void doc_end(void) { raw_done = 1; }

LW_EXPORT(lw_start) void lw_start(void) {
    S = (float)lw_scale();
    if (S < 1) S = 1;
    lw_set_title(LW_STR("Loading…"));
}

LW_EXPORT(lw_resize) void lw_resize(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w > 8192) w = 8192;
    if (h > 8192) h = 8192;
    if (w * h > fb_cap) {
        mem_free(fb);
        fb = (u32 *)must_alloc((u32)w * (u32)h * 4);
        fb_cap = w * h;
    }
    int width_changed = w != W;
    W = w;
    H = h;
    if (has_doc && width_changed) need_layout = 1;
    scroll_y = target_y = clamp_scroll(target_y);
    dirty = 1;
}

// The document itself (fetch id 0) comes either streamed (begin, data..., end) or whole.
LW_EXPORT(lw_on_fetch_begin) void lw_on_fetch_begin(int id, int status, const char *type, int type_len, const char *url,
                                                    int url_len) {
    (void)status;
    if (id == 0) doc_begin(type, type_len, url, url_len);
    mem_free((void *)type);  // type and url share one block from lw_alloc
}

LW_EXPORT(lw_on_fetch_data) void lw_on_fetch_data(int id, u8 *data, int len) {
    if (id == 0) doc_data(data, len);
    else mem_free(data);
}

LW_EXPORT(lw_on_fetch_end) void lw_on_fetch_end(int id, int status) {
    if (id != 0) return;
    if (status == 0) show_toast("The page did not finish loading.");
    doc_end();
}

LW_EXPORT(lw_on_fetch_ex) void lw_on_fetch_ex(int id, int status, const u8 *data, int len, const char *type, int type_len,
                                              const char *url, int url_len) {
    if (id == 0) {
        doc_begin(type, type_len, url, url_len);
        doc_data((u8 *)data, len);
        doc_end();
        return;
    }
    if (id == save_fetch) {  // an image to save (the menu's Save image as...)
        save_fetch = -1;
        if (status == 200 && len > 0) lw_save_file(save_name, save_name_len, data, len);
        else show_toast("The image could not be downloaded.");
        mem_free((void *)data);
        return;
    }
    int slot = sheet_arrived(id, status, data, len);
    if (slot >= 0) {
        mem_free((void *)data);
        if (css_built) css_add_sheet(slot);  // else it is used when the page is first shown
        return;
    }
    for (int i = 0; i < nimgs; i++) {
        Img *im = &imgs[i];
        if (im->state != IMG_LOADING || im->fetch_id != id) continue;
        in_flight--;
        if (status == 200) image_from_bytes(im, data, len);
        else im->state = IMG_FAILED;
        images_changed = 1;
        break;
    }
    mem_free((void *)data);
    pump_images();
}

LW_EXPORT(lw_frame) void lw_frame(double now) {
    if (!fb) return;
    if (refresh_ms >= 0 && now >= refresh_ms) {
        refresh_ms = -1;
        lw_navigate(refresh_url, refresh_len);
    }
    if (toast_len && now > toast_until) { toast_len = 0; dirty = 1; }
    if (doc_started && !parse_done) pump_parse(lw_now() + (has_doc ? SLICE_MS : 2 * SLICE_MS));
    if (!has_doc) maybe_show();
    else maybe_late_main();
    if (css_new_rules) {  // a stylesheet came in after the page was shown
        css_new_rules = 0;
        if (layout_running) css_stale = 1;  // the part laid out so far used the old rules
        else if (has_doc) need_layout = 1;
    }
    // relayout when the width changed, or when images arrived (at most ~3 times a second,
    // and not while a layout is still going: it would start over and never get far)
    if (has_doc && (need_layout || (images_changed && !layout_running && now - last_layout > 300))) {
        relayout();
    } else if (has_doc && layout_running) {
        int was_y = laid_y;
        if (layout_step(lw_now() + SLICE_MS, 0)) {
            last_layout = now;
            char m[64];
            int k = 0;
            const char *w = "viewer: layout done, slices ";
            while (*w) m[k++] = *w++;
            int v = layout_slices, t = 0;
            char tmp[12];
            do { tmp[t++] = (char)('0' + v % 10); v /= 10; } while (v);
            while (t) m[k++] = tmp[--t];
            lw_log(m, k);
            log_ms("viewer: layout total ", (int)layout_cpu);
            dirty = 1;
            if (css_stale) need_layout = 1;
        }
        // redraw when new content reached the screen; otherwise now and then for the scrollbar
        if (was_y < scroll_y + H || now - last_progress_draw > 100) dirty = 1;
    }
    if (has_doc && !layout_running) {
        scroll_y = clamp_scroll(scroll_y);
        target_y = clamp_scroll(target_y);
    }
    check_anchor();
    check_restore();
    if (find_len && (find_pass != layout_pass || find_limit != draw_limit) && (!layout_running || now - find_t > 300)) {
        int keep = match_cur;  // the layout changed: look again (the browser asks for the count)
        find_run();
        match_cur = nmatches ? MIN(MAX(keep, 0), nmatches - 1) : -1;
        dirty = 1;
    }
    if (sel_drag && (drag_y < 0 || drag_y >= H)) {  // dragging a selection past the edge scrolls
        int step = (drag_y < 0 ? drag_y : drag_y - H + 1) / 2;
        step = step < 0 ? MIN(step, -(int)(4 * S)) : MAX(step, (int)(4 * S));
        scroll_y = target_y = clamp_scroll(scroll_y + step);
        const char *p = text_pos_at(drag_x, drag_y < 0 ? 0 : H - 1);
        if (p) sel_b = p;
        dirty = 1;
    }
    if (dirty) last_progress_draw = now;
    if (scroll_y != target_y) {
        int d = target_y - scroll_y;
        int step = d * 35 / 100;
        if (step == 0) step = d > 0 ? 1 : -1;
        scroll_y += step;
        dirty = 1;
    }
    if (mouse_in && scroll_y != hover_scroll && !press_down && !sel_drag) {  // the page moved under the mouse
        hover_scroll = scroll_y;
        int li = item_at(mouse_x, mouse_y, IT_TEXT), l = li >= 0 ? items[li].link : -1;
        if (l != hover_link) { hover_link = l; dirty = 1; }
    }
    if (focus >= 0 && (int)((now - caret_t) / 530) != (int)((now - 16 - caret_t) / 530)) dirty = 1;
    videos_tick();
    if (video_full >= 0 && !lw_fullscreen(-1)) { video_full = -1; dirty = 1; }
    if (!dirty) {
        if (bars_dirty && has_doc) redraw_bars();
        bars_dirty = 0;
        return;
    }
    dirty = 0;
    bars_dirty = 0;
    if (!has_doc) {
        u32 bg = RGB(255, 255, 255);
        fill_span(fb, W * H, bg);
        lw_present(fb, W, H);
        return;
    }
    redraw();
}
