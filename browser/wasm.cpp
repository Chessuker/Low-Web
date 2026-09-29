// wasm.cpp — WebAssembly decoder, translator and interpreter. See wasm.h.
#include "wasm.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <new>
#include <stdexcept>

namespace wasm {

namespace {

struct LoadError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

enum : uint8_t { T_I32 = 0x7F, T_I64 = 0x7E, T_F32 = 0x7D, T_F64 = 0x7C, T_FUNCREF = 0x70, T_EXTERNREF = 0x6F };

// Internal opcodes live above 0xFF; everything else keeps its wasm opcode.
enum : uint16_t {
    OP_JMP = 0x100,   // a = target
    OP_JMP_IF,        // pop cond; a = target
    OP_JMP_IFZ,       // pop cond; a = target
    OP_BR,            // a = target, b = drop | arity << 32
    OP_BR_IF,
    OP_BR_TABLE,      // a = offset into brtable, b = entry count (last is default)
    OP_FC = 0x110,    // 0xFC prefix: OP_FC + sub-opcode

    // Fused instructions ("superinstructions"): common sequences done in one step, chosen
    // from what the HTML viewer runs most (tests/viewerbench.cpp -DWASM_PROFILE).
    OPX_ADDI = 0x120,  // i32.const b; i32.add
    OPX_LGET_ADDI,     // local.get a; i32.const b; i32.add
    OPX_LGET_CONST,    // local.get a; i32.const b
    OPX_LGET2,         // local.get a; local.get b
    OPX_LSET_LGET,     // local.set a; local.get b
    OPX_ADD_LSET,      // i32.add; local.set a
    OPX_ADD_LTEE,      // i32.add; local.tee a
    OPX_LGET_LOAD,     // local.get a; i32.load offset=b
    OPX_LGET_LOAD8U,   // local.get a; i32.load8_u offset=b
    OPX_LGET_STORE,    // local.get a; i32.store offset=b (the local is the value)
    OPX_LGET_ADDI_LSET,  // local.get a; i32.const c; i32.add; local.set d   (b = c | d << 32)
    OPX_LGET_ADDI_LTEE,  // local.get a; i32.const c; i32.add; local.tee d   (b = c | d << 32)
    OPX_LTEE_LGET,       // local.tee a; local.get b
    OPX_LGET2_ADD,       // local.get a; local.get b; i32.add
    OPX_ADD_LOAD,        // i32.add; i32.load offset=b
    OPX_ADD_LOAD8U,      // i32.add; i32.load8_u offset=b
    OPX_CONST_BIN = 0x130,       // i32.const b; <kConstBin[k]>                     (+ k)
    OPX_CMP_JIF = 0x150,         // <kCmp[k]>; jump if true, a = target             (+ k)
    OPX_CMP_JIFZ = 0x160,        // <kCmp[k]>; jump if false                        (+ k)
    OPX_LGET_CONST_BIN = 0x170,  // local.get a; i32.const b; <kConstBin[k]>        (+ k)
    OPX_CONST_CMP_JIF = 0x190,   // i32.const b; <kCmp[k]>; jump if true, a = target (+ k, k >= 1)
    OPX_CONST_CMP_JIFZ = 0x1A0,  // ... jump if false
    OPX_LGET_CMP_JIF = 0x1B0,    // local.get b; <kCmp[k]>; jump if true, a = target (+ k)
    OPX_LGET_CMP_JIFZ = 0x1C0,   // ... jump if false
};

// the i32 operations that fuse with a constant operand, and the comparisons that fuse with a jump
const uint8_t kConstBin[] = {0x6B, 0x6C, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x46, 0x47,
                             0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F};
const uint8_t kCmp[] = {0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F};

constexpr uint32_t kNullFunc = 0xFFFFFFFFu;
constexpr size_t kStackSlots = 1 << 20;  // 8 MiB of values
constexpr size_t kMaxFrames = 20000;

uint64_t now_ms() {
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

[[noreturn]] void trap(const char *msg) { throw Trap{msg}; }

std::string hex(unsigned v) {
    char buf[16];
    snprintf(buf, sizeof buf, "0x%02X", v);
    return buf;
}

bool parse_sig(const std::string &sig, FuncType &out) {
    bool results = false;
    for (char c : sig) {
        uint8_t t;
        switch (c) {
        case ':': results = true; continue;
        case 'i': t = T_I32; break;
        case 'I': t = T_I64; break;
        case 'f': t = T_F32; break;
        case 'F': t = T_F64; break;
        default: return false;
        }
        (results ? out.results : out.params).push_back(t);
    }
    return true;
}

std::string type_str(const FuncType &t) {
    auto one = [](uint8_t v) { return v == T_I32 ? "i32" : v == T_I64 ? "i64" : v == T_F32 ? "f32" : v == T_F64 ? "f64" : "ref"; };
    std::string s = "(";
    for (size_t i = 0; i < t.params.size(); i++) s += (i ? ", " : "") + std::string(one(t.params[i]));
    s += ") -> (";
    for (size_t i = 0; i < t.results.size(); i++) s += (i ? ", " : "") + std::string(one(t.results[i]));
    return s + ")";
}

// ---- float helpers with wasm semantics --------------------------------------

template <class T> T fmin_w(T a, T b) {
    if (a != a || b != b) return std::numeric_limits<T>::quiet_NaN();
    if (a == 0 && b == 0) return std::signbit(a) ? a : b;
    return a < b ? a : b;
}
template <class T> T fmax_w(T a, T b) {
    if (a != a || b != b) return std::numeric_limits<T>::quiet_NaN();
    if (a == 0 && b == 0) return std::signbit(a) ? b : a;
    return a > b ? a : b;
}

// kind: 0 = i32 signed, 1 = i32 unsigned, 2 = i64 signed, 3 = i64 unsigned
uint64_t trunc_checked(double x, int kind) {
    if (x != x) trap("invalid conversion to integer");
    switch (kind) {
    case 0:
        if (!(x > -2147483649.0 && x < 2147483648.0)) break;
        return (uint32_t)(int32_t)x;
    case 1:
        if (!(x > -1.0 && x < 4294967296.0)) break;
        return (uint32_t)x;
    case 2:
        if (!(x >= -9223372036854775808.0 && x < 9223372036854775808.0)) break;
        return (uint64_t)(int64_t)x;
    default:
        if (!(x > -1.0 && x < 18446744073709551616.0)) break;
        return (uint64_t)x;
    }
    trap("integer overflow");
}

uint64_t trunc_sat(double x, int kind) {
    if (x != x) return 0;
    switch (kind) {
    case 0:
        if (x <= -2147483648.0) return (uint32_t)INT32_MIN;
        if (x >= 2147483648.0) return (uint32_t)INT32_MAX;
        return (uint32_t)(int32_t)x;
    case 1:
        if (x <= 0) return 0;
        if (x >= 4294967296.0) return UINT32_MAX;
        return (uint32_t)x;
    case 2:
        if (x <= -9223372036854775808.0) return (uint64_t)INT64_MIN;
        if (x >= 9223372036854775808.0) return (uint64_t)INT64_MAX;
        return (uint64_t)(int64_t)x;
    default:
        if (x <= 0) return 0;
        if (x >= 18446744073709551616.0) return UINT64_MAX;
        return (uint64_t)x;
    }
}

}  // namespace

// ---- byte reader --------------------------------------------------------------

struct Reader {
    const uint8_t *p, *end;

    bool eof() const { return p >= end; }
    size_t left() const { return (size_t)(end - p); }
    uint8_t byte() {
        if (p >= end) throw LoadError("unexpected end of module");
        return *p++;
    }
    const uint8_t *take(size_t n) {
        if (n > left()) throw LoadError("unexpected end of module");
        const uint8_t *r = p;
        p += n;
        return r;
    }
    uint64_t uleb(unsigned bits) {
        uint64_t r = 0;
        unsigned shift = 0;
        for (;;) {
            uint8_t b = byte();
            if (shift < 64) r |= (uint64_t)(b & 0x7F) << shift;
            shift += 7;
            if (!(b & 0x80)) return r;
            if (shift >= bits) throw LoadError("malformed LEB128 integer");
        }
    }
    int64_t sleb(unsigned bits) {
        int64_t r = 0;
        unsigned shift = 0;
        uint8_t b;
        do {
            b = byte();
            if (shift < 64) r |= (int64_t)((uint64_t)(b & 0x7F) << shift);
            shift += 7;
            if ((b & 0x80) && shift >= bits) throw LoadError("malformed LEB128 integer");
        } while (b & 0x80);
        if (shift < 64 && (b & 0x40)) r |= (int64_t)(~0ULL << shift);
        return r;
    }
    uint32_t u32() { return (uint32_t)uleb(32); }
    std::string name() {
        uint32_t n = u32();
        const uint8_t *s = take(n);
        return std::string((const char *)s, n);
    }
    uint32_t count(uint32_t limit, const char *what) {
        uint32_t n = u32();
        if (n > limit) throw LoadError(std::string("too many ") + what);
        return n;
    }
};

// ---- instance -----------------------------------------------------------------

void *vm_reserve(size_t bytes) { return VirtualAlloc(nullptr, bytes, MEM_RESERVE, PAGE_NOACCESS); }
bool vm_commit(void *p, size_t bytes) { return VirtualAlloc(p, bytes, MEM_COMMIT, PAGE_READWRITE) != nullptr; }
void vm_release(void *p) { VirtualFree(p, 0, MEM_RELEASE); }

Instance::Instance() {
    if (!stack_.resize(kStackSlots, kStackSlots)) throw std::bad_alloc();  // committed, but only used pages take RAM
    stack_top_ = stack_.data();
}

int Instance::export_func(const std::string &name) const {
    auto it = exports_.find(name);
    return it == exports_.end() ? -1 : (int)it->second;
}

uint32_t Instance::grow(uint32_t pages) {
    uint64_t old = mem_.size() / 65536;
    uint64_t limit = std::min<uint64_t>(mem_max_pages_, max_memory_pages);
    if (!has_memory_ || old + pages > limit) return 0xFFFFFFFFu;
    if (!mem_.resize((old + pages) * 65536, (size_t)limit * 65536)) return 0xFFFFFFFFu;
    return (uint32_t)old;
}

uint64_t Instance::const_expr(Reader &r) {
    uint64_t v = 0;
    uint8_t op = r.byte();
    switch (op) {
    case 0x41: v = (uint32_t)(int32_t)r.sleb(32); break;
    case 0x42: v = (uint64_t)r.sleb(64); break;
    case 0x43: { uint32_t b; std::memcpy(&b, r.take(4), 4); v = b; break; }
    case 0x44: std::memcpy(&v, r.take(8), 8); break;
    case 0x23: {
        uint32_t g = r.u32();
        if (g >= globals_.size()) throw LoadError("constant expression uses an unknown global");
        v = globals_[g];
        break;
    }
    case 0xD0: r.byte(); v = kNullFunc; break;          // ref.null
    case 0xD2: v = r.u32(); break;                      // ref.func
    default: throw LoadError("unsupported constant expression " + hex(op));
    }
    if (r.byte() != 0x0B) throw LoadError("constant expression too complex");
    return v;
}

std::string Instance::load(const uint8_t *data, size_t size, const std::vector<HostImport> &imports) {
    try {
        Reader r{data, data + size};
        static const uint8_t magic[8] = {0, 'a', 's', 'm', 1, 0, 0, 0};
        if (size < 8 || std::memcmp(data, magic, 8) != 0) throw LoadError("not a WebAssembly module (bad header)");
        r.p += 8;

        uint32_t num_imported = 0;
        std::vector<uint32_t> func_decls;
        struct Elem { uint32_t offset; std::vector<uint32_t> funcs; bool active; };
        std::vector<Elem> elems;
        struct Data { uint32_t offset; const uint8_t *bytes; uint32_t len; bool active; };
        std::vector<Data> datas;
        int start_func = -1;

        while (!r.eof()) {
            uint8_t id = r.byte();
            uint32_t len = r.u32();
            Reader s{r.take(len), r.p};
            switch (id) {
            case 0: break;  // custom section
            case 1: {       // types
                uint32_t n = s.count(100000, "types");
                for (uint32_t i = 0; i < n; i++) {
                    if (s.byte() != 0x60) throw LoadError("unsupported type form");
                    FuncType t;
                    uint32_t np = s.count(1000, "params");
                    for (uint32_t k = 0; k < np; k++) t.params.push_back(s.byte());
                    uint32_t nr = s.count(1000, "results");
                    for (uint32_t k = 0; k < nr; k++) t.results.push_back(s.byte());
                    types_.push_back(std::move(t));
                }
                break;
            }
            case 2: {  // imports
                uint32_t n = s.count(100000, "imports");
                for (uint32_t i = 0; i < n; i++) {
                    std::string mod = s.name(), nm = s.name();
                    uint8_t kind = s.byte();
                    if (kind != 0) throw LoadError("import " + mod + "." + nm + ": only function imports are supported");
                    uint32_t ti = s.u32();
                    if (ti >= types_.size()) throw LoadError("bad import type");
                    const HostImport *hi = nullptr;
                    for (const auto &h : imports)
                        if (h.module == mod && h.name == nm) hi = &h;
                    if (!hi) throw LoadError("the browser does not provide " + mod + "." + nm);
                    FuncType want;
                    if (!parse_sig(hi->sig, want)) throw LoadError("bad host signature for " + nm);
                    if (!(want == types_[ti]))
                        throw LoadError("import " + mod + "." + nm + " has type " + type_str(types_[ti]) +
                                        ", expected " + type_str(want));
                    Func f;
                    f.type = ti;
                    f.imported = true;
                    f.host = hi->fn;
                    f.nparams = (uint32_t)want.params.size();
                    f.nresults = (uint32_t)want.results.size();
                    funcs_.push_back(std::move(f));
                    num_imported++;
                }
                break;
            }
            case 3: {  // function declarations
                uint32_t n = s.count(1000000, "functions");
                for (uint32_t i = 0; i < n; i++) {
                    uint32_t ti = s.u32();
                    if (ti >= types_.size()) throw LoadError("bad function type");
                    Func f;
                    f.type = ti;
                    f.nparams = (uint32_t)types_[ti].params.size();
                    f.nresults = (uint32_t)types_[ti].results.size();
                    funcs_.push_back(std::move(f));
                    func_decls.push_back(ti);
                }
                break;
            }
            case 4: {  // table
                uint32_t n = s.u32();
                if (n > 1) throw LoadError("only one table is supported");
                if (n == 1) {
                    if (s.byte() != T_FUNCREF) throw LoadError("only funcref tables are supported");
                    uint8_t flags = s.byte();
                    uint32_t mn = s.u32();
                    if (flags & 1) s.u32();
                    if (mn > 10000000) throw LoadError("table too large");
                    table_.assign(mn, kNullFunc);
                    has_table_ = true;
                }
                break;
            }
            case 5: {  // memory
                uint32_t n = s.u32();
                if (n > 1) throw LoadError("only one memory is supported");
                if (n == 1) {
                    uint8_t flags = s.byte();
                    uint32_t mn = s.u32();
                    if (flags & 1) mem_max_pages_ = std::min<uint32_t>(s.u32(), 65536);
                    if (mn > max_memory_pages || mn > mem_max_pages_) throw LoadError("initial memory too large");
                    if (!mem_.resize((size_t)mn * 65536, (size_t)std::min<uint32_t>(mem_max_pages_, max_memory_pages) * 65536))
                        throw LoadError("not enough memory for the page");
                    has_memory_ = true;
                }
                break;
            }
            case 6: {  // globals
                uint32_t n = s.count(1000000, "globals");
                for (uint32_t i = 0; i < n; i++) {
                    s.byte();  // type
                    s.byte();  // mutability
                    globals_.push_back(const_expr(s));
                }
                break;
            }
            case 7: {  // exports
                uint32_t n = s.count(1000000, "exports");
                for (uint32_t i = 0; i < n; i++) {
                    std::string nm = s.name();
                    uint8_t kind = s.byte();
                    uint32_t idx = s.u32();
                    if (kind == 0) {
                        if (idx >= funcs_.size()) throw LoadError("bad export index");
                        exports_[nm] = idx;
                    }
                }
                break;
            }
            case 8: {
                start_func = (int)s.u32();
                if ((size_t)start_func >= funcs_.size()) throw LoadError("bad start function");
                break;
            }
            case 9: {  // elements
                uint32_t n = s.count(1000000, "element segments");
                for (uint32_t i = 0; i < n; i++) {
                    uint32_t flags = s.u32();
                    Elem e{0, {}, false};
                    if (flags > 3) throw LoadError("unsupported element segment kind");
                    if (flags == 0 || flags == 2) {
                        if (flags == 2 && s.u32() != 0) throw LoadError("bad table index");
                        e.offset = (uint32_t)const_expr(s);
                        e.active = true;
                    }
                    if (flags != 0 && s.byte() != 0x00) throw LoadError("unsupported element kind");
                    uint32_t cnt = s.count(10000000, "elements");
                    for (uint32_t k = 0; k < cnt; k++) {
                        uint32_t fi = s.u32();
                        if (fi >= funcs_.size()) throw LoadError("bad element function index");
                        e.funcs.push_back(fi);
                    }
                    elems.push_back(std::move(e));
                }
                break;
            }
            case 12: data_count_ = s.u32(); break;
            case 10: {  // code
                uint32_t n = s.u32();
                if (n != func_decls.size()) throw LoadError("function and code section sizes differ");
                for (uint32_t i = 0; i < n; i++) {
                    uint32_t body_len = s.u32();
                    Reader b{s.take(body_len), s.p};
                    Func &f = funcs_[num_imported + i];
                    uint64_t nlocals = f.nparams;
                    uint32_t groups = b.count(100000, "local groups");
                    for (uint32_t g = 0; g < groups; g++) {
                        nlocals += b.u32();
                        b.byte();
                        if (nlocals > 50000) throw LoadError("too many locals");
                    }
                    f.nlocals = (uint32_t)nlocals;
                    compile(f, b);
                }
                break;
            }
            case 11: {  // data
                uint32_t n = s.count(1000000, "data segments");
                for (uint32_t i = 0; i < n; i++) {
                    uint32_t flags = s.u32();
                    Data d{0, nullptr, 0, flags != 1};
                    if (flags == 2 && s.u32() != 0) throw LoadError("bad memory index");
                    if (flags > 2) throw LoadError("unsupported data segment kind");
                    if (d.active) d.offset = (uint32_t)const_expr(s);
                    d.len = s.u32();
                    d.bytes = s.take(d.len);
                    datas.push_back(d);
                }
                break;
            }
            default: throw LoadError("unknown section " + std::to_string(id));
            }
            if (id != 0 && !s.eof()) throw LoadError("section " + std::to_string(id) + " has trailing bytes");
        }
        for (size_t i = num_imported; i < funcs_.size(); i++)
            if (funcs_[i].code.empty()) throw LoadError("function without body");

        canon_.resize(types_.size());
        for (size_t i = 0; i < types_.size(); i++) {
            canon_[i] = (uint32_t)i;
            for (size_t k = 0; k < i; k++)
                if (types_[k] == types_[i]) { canon_[i] = (uint32_t)k; break; }
        }

        for (const Elem &e : elems) {
            if (!e.active) continue;
            if ((uint64_t)e.offset + e.funcs.size() > table_.size()) throw LoadError("element segment out of bounds");
            for (size_t k = 0; k < e.funcs.size(); k++) table_[e.offset + k] = e.funcs[k];
        }
        for (const Data &d : datas) {
            if (d.active) {
                if ((uint64_t)d.offset + d.len > mem_.size()) throw LoadError("data segment out of bounds");
                std::memcpy(mem_.data() + d.offset, d.bytes, d.len);
                datas_.emplace_back();  // active segments are dropped after instantiation
            } else {
                datas_.emplace_back(d.bytes, d.bytes + d.len);
            }
        }
        if (datas_.size() < data_count_) datas_.resize(data_count_);
    } catch (const LoadError &e) {
        return e.what();
    } catch (const std::bad_alloc &) {
        return "out of memory while loading module";
    }
    return "";
}

// ---- translation ----------------------------------------------------------------

namespace {

bool is_unary(uint8_t op) {
    return op == 0x45 || op == 0x50 || (op >= 0x67 && op <= 0x69) || (op >= 0x79 && op <= 0x7B) ||
           (op >= 0x8B && op <= 0x91) || (op >= 0x99 && op <= 0x9F) || (op >= 0xA7 && op <= 0xC4);
}
bool is_binary(uint8_t op) {
    return (op >= 0x46 && op <= 0x4F) || (op >= 0x51 && op <= 0x66) || (op >= 0x6A && op <= 0x78) ||
           (op >= 0x7C && op <= 0x8A) || (op >= 0x92 && op <= 0x98) || (op >= 0xA0 && op <= 0xA6);
}

// Skips the immediates of an instruction inside unreachable code.
void skip_immediates(uint8_t op, Reader &r) {
    switch (op) {
    case 0x0C: case 0x0D: case 0x10:
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0xD2:
        r.u32();
        return;
    case 0x0E: {
        uint32_t n = r.u32();
        for (uint64_t i = 0; i <= n; i++) r.u32();
        return;
    }
    case 0x11: r.u32(); r.u32(); return;
    case 0x1C: { uint32_t n = r.u32(); for (uint32_t i = 0; i < n; i++) r.byte(); return; }
    case 0x3F: case 0x40: case 0xD0: r.byte(); return;
    case 0x41: r.sleb(32); return;
    case 0x42: r.sleb(64); return;
    case 0x43: r.take(4); return;
    case 0x44: r.take(8); return;
    case 0xFC: {
        uint32_t sub = r.u32();
        if (sub <= 7) return;
        if (sub == 8) { r.u32(); r.byte(); return; }
        if (sub == 9) { r.u32(); return; }
        if (sub == 10) { r.byte(); r.byte(); return; }
        if (sub == 11) { r.byte(); return; }
        throw LoadError("unsupported instruction 0xFC " + std::to_string(sub));
    }
    default:
        if (op >= 0x28 && op <= 0x3E) { r.u32(); r.u32(); return; }
        if (op <= 0x01 || op == 0x0F || op == 0x1A || op == 0x1B || op == 0xD1 || is_unary(op) || is_binary(op)) return;
        throw LoadError("unsupported instruction " + hex(op));
    }
}

}  // namespace

// Replaces common instruction sequences with fused instructions. A sequence is fused only if
// no branch lands inside it; branch targets are renumbered afterwards.
static bool g_fuse = true;
void set_fusion(bool on) { g_fuse = on; }

static void fuse(std::vector<Instance::Ins> &code, std::vector<uint32_t> &brtable) {
    if (!g_fuse) return;
    using Ins = Instance::Ins;
    auto is_jump = [](uint16_t op) {
        auto in = [&](uint16_t base) { return op >= base && op < base + sizeof kCmp; };
        return (op >= OP_JMP && op <= OP_BR_IF) || in(OPX_CMP_JIF) || in(OPX_CMP_JIFZ) || in(OPX_CONST_CMP_JIF) ||
               in(OPX_CONST_CMP_JIFZ) || in(OPX_LGET_CMP_JIF) || in(OPX_LGET_CMP_JIFZ);
    };
    const size_t n = code.size();
    std::vector<uint8_t> target(n + 1);
    for (const Ins &I : code)
        if (is_jump(I.op) && I.a <= n) target[I.a] = 1;
    for (size_t i = 0; i < brtable.size(); i += 3)
        if (brtable[i] <= n) target[brtable[i]] = 1;
    auto find = [](const uint8_t *list, size_t len, uint16_t op) -> int {
        for (size_t k = 0; k < len; k++)
            if (list[k] == op) return (int)k;
        return -1;
    };
    std::vector<Ins> out;
    out.reserve(n);
    std::vector<uint32_t> at(n + 1);  // old index -> new index
    for (size_t i = 0; i < n;) {
        const Ins &A = code[i];
        auto free_after = [&](size_t len) {  // code[i+1 .. i+len-1] exist and are not branch targets
            if (i + len > n) return false;
            for (size_t k = 1; k < len; k++)
                if (target[i + k]) return false;
            return true;
        };
        uint16_t op1 = i + 1 < n ? code[i + 1].op : 0, op2 = i + 2 < n ? code[i + 2].op : 0,
                 op3 = i + 3 < n ? code[i + 3].op : 0;
        Ins fused{};
        size_t len = 1;
        int k;
        if (A.op == 0x20 && op1 == 0x41 && op2 == 0x6A && (op3 == 0x21 || op3 == 0x22) && free_after(4)) {
            fused = {(uint16_t)(op3 == 0x21 ? OPX_LGET_ADDI_LSET : OPX_LGET_ADDI_LTEE), A.a,
                     (uint32_t)code[i + 1].b | (uint64_t)code[i + 3].a << 32};
            len = 4;
        } else if (A.op == 0x20 && op1 == 0x41 && op2 == 0x6A && free_after(3)) {
            fused = {OPX_LGET_ADDI, A.a, code[i + 1].b}, len = 3;
        } else if (A.op == 0x20 && op1 == 0x41 && (k = find(kConstBin, sizeof kConstBin, op2)) >= 0 && free_after(3)) {
            fused = {(uint16_t)(OPX_LGET_CONST_BIN + k), A.a, code[i + 1].b}, len = 3;
        } else if (A.op == 0x41 && (k = find(kCmp, sizeof kCmp, op1)) >= 1 && (op2 == OP_JMP_IF || op2 == OP_JMP_IFZ) && free_after(3)) {
            fused = {(uint16_t)((op2 == OP_JMP_IF ? OPX_CONST_CMP_JIF : OPX_CONST_CMP_JIFZ) + k), code[i + 2].a, A.b}, len = 3;
        } else if (A.op == 0x20 && (k = find(kCmp, sizeof kCmp, op1)) >= 0 && (op2 == OP_JMP_IF || op2 == OP_JMP_IFZ) && free_after(3)) {
            fused = {(uint16_t)((op2 == OP_JMP_IF ? OPX_LGET_CMP_JIF : OPX_LGET_CMP_JIFZ) + k), code[i + 2].a, A.a}, len = 3;
        } else if (A.op == 0x20 && op1 == 0x20 && op2 == 0x6A && free_after(3)) {
            fused = {OPX_LGET2_ADD, A.a, code[i + 1].a}, len = 3;
        } else if (free_after(2)) {
            const Ins &B = code[i + 1];
            len = 2;
            if (A.op == 0x41 && B.op == 0x6A) fused = {OPX_ADDI, 0, A.b};
            else if (A.op == 0x41 && (k = find(kConstBin, sizeof kConstBin, B.op)) >= 0) fused = {(uint16_t)(OPX_CONST_BIN + k), 0, A.b};
            else if ((k = find(kCmp, sizeof kCmp, A.op)) >= 0 && B.op == OP_JMP_IF) fused = {(uint16_t)(OPX_CMP_JIF + k), B.a, 0};
            else if ((k = find(kCmp, sizeof kCmp, A.op)) >= 0 && B.op == OP_JMP_IFZ) fused = {(uint16_t)(OPX_CMP_JIFZ + k), B.a, 0};
            else if (A.op == 0x20 && B.op == 0x28) fused = {OPX_LGET_LOAD, A.a, B.a};
            else if (A.op == 0x20 && B.op == 0x2D) fused = {OPX_LGET_LOAD8U, A.a, B.a};
            else if (A.op == 0x20 && B.op == 0x36) fused = {OPX_LGET_STORE, A.a, B.a};
            else if (A.op == 0x20 && B.op == 0x41) fused = {OPX_LGET_CONST, A.a, B.b};
            else if (A.op == 0x20 && B.op == 0x20) fused = {OPX_LGET2, A.a, B.a};
            else if (A.op == 0x21 && B.op == 0x20) fused = {OPX_LSET_LGET, A.a, B.a};
            else if (A.op == 0x6A && B.op == 0x21) fused = {OPX_ADD_LSET, B.a, 0};
            else if (A.op == 0x6A && B.op == 0x22) fused = {OPX_ADD_LTEE, B.a, 0};
            else if (A.op == 0x6A && B.op == 0x28) fused = {OPX_ADD_LOAD, 0, B.a};
            else if (A.op == 0x6A && B.op == 0x2D) fused = {OPX_ADD_LOAD8U, 0, B.a};
            else if (A.op == 0x22 && B.op == 0x20) fused = {OPX_LTEE_LGET, A.a, B.a};
            else len = 1;
        }
        for (size_t k2 = 0; k2 < len; k2++) at[i + k2] = (uint32_t)out.size();
        out.push_back(len == 1 ? A : fused);
        i += len;
    }
    at[n] = (uint32_t)out.size();
    for (Ins &I : out)
        if (is_jump(I.op)) I.a = at[I.a];
    for (size_t i = 0; i < brtable.size(); i += 3) brtable[i] = at[brtable[i]];
    code.swap(out);
}

void Instance::compile(Func &f, Reader &r) {
    enum { K_FUNC, K_BLOCK, K_LOOP, K_IF };
    struct Ctl {
        int kind;
        uint32_t height, nparams, nresults, start;
        int64_t if_ins;  // index of the IF jump while its else-branch is still missing
        std::vector<uint32_t> patches, tpatches;
    };
    const FuncType &ft = types_[f.type];
    std::vector<Ctl> ctl;
    ctl.push_back({K_FUNC, 0, 0, (uint32_t)ft.results.size(), 0, -1, {}, {}});
    std::vector<Ins> &code = f.code;
    uint32_t h = 0, maxh = 0, dead_depth = 0;
    bool dead = false;

    auto emit = [&](uint16_t op, uint32_t a = 0, uint64_t b = 0) { code.push_back({op, a, b}); };
    auto need = [&](uint32_t n) {
        if (h < ctl.back().height + n) throw LoadError("operand stack underflow");
    };
    auto pop = [&](uint32_t n) { need(n); h -= n; };
    auto push = [&](uint32_t n) { h += n; if (h > maxh) maxh = h; };
    auto blocktype = [&](uint32_t &np, uint32_t &nr) {
        int64_t bt = r.sleb(33);
        if (bt == -64) { np = nr = 0; }
        else if (bt < 0) { np = 0; nr = 1; }
        else {
            if ((uint64_t)bt >= types_.size()) throw LoadError("bad block type");
            np = (uint32_t)types_[bt].params.size();
            nr = (uint32_t)types_[bt].results.size();
        }
    };
    // Branch to the label `depth` levels out; returns the target entry fields.
    auto label = [&](uint32_t depth, uint32_t &drop, uint32_t &arity) -> Ctl & {
        if (depth >= ctl.size()) throw LoadError("branch depth out of range");
        Ctl &t = ctl[ctl.size() - 1 - depth];
        arity = t.kind == K_LOOP ? t.nparams : t.nresults;
        need(arity);
        drop = h - arity - t.height;
        return t;
    };
    auto branch = [&](uint32_t depth, uint16_t op_plain, uint16_t op_drop) {
        uint32_t drop, arity;
        Ctl &t = label(depth, drop, arity);
        uint32_t at = (uint32_t)code.size();
        if (drop == 0) emit(op_plain);
        else emit(op_drop, 0, drop | (uint64_t)arity << 32);
        if (t.kind == K_LOOP) code[at].a = t.start;
        else t.patches.push_back(at);
    };
    auto local = [&](uint32_t i) { if (i >= f.nlocals) throw LoadError("bad local index"); return i; };
    auto global = [&](uint32_t i) { if (i >= globals_.size()) throw LoadError("bad global index"); return i; };
    auto need_memory = [&] { if (!has_memory_) throw LoadError("memory instruction without memory"); };

    for (;;) {
        uint8_t op = r.byte();
        if (dead) {
            if (op == 0x02 || op == 0x03 || op == 0x04) { uint32_t a, b; blocktype(a, b); dead_depth++; continue; }
            if (op == 0x0B && dead_depth) { dead_depth--; continue; }
            if (op == 0x05 && dead_depth) continue;
            if (op != 0x0B && op != 0x05) { skip_immediates(op, r); continue; }
        }
        switch (op) {
        case 0x00: emit(0x00); dead = true; break;
        case 0x01: break;
        case 0x02:
        case 0x03: {
            uint32_t np, nr;
            blocktype(np, nr);
            need(np);
            ctl.push_back({op == 0x02 ? K_BLOCK : K_LOOP, h - np, np, nr, (uint32_t)code.size(), -1, {}, {}});
            break;
        }
        case 0x04: {
            uint32_t np, nr;
            blocktype(np, nr);
            pop(1);
            need(np);
            ctl.push_back({K_IF, h - np, np, nr, 0, (int64_t)code.size(), {}, {}});
            emit(OP_JMP_IFZ);
            break;
        }
        case 0x05: {
            Ctl &c = ctl.back();
            if (c.kind != K_IF || c.if_ins < 0) throw LoadError("else without if");
            if (!dead) {
                if (h != c.height + c.nresults) throw LoadError("stack height mismatch at else");
                c.patches.push_back((uint32_t)code.size());
                emit(OP_JMP);
            }
            code[c.if_ins].a = (uint32_t)code.size();
            c.if_ins = -1;
            h = c.height + c.nparams;
            dead = false;
            break;
        }
        case 0x0B: {
            Ctl c = std::move(ctl.back());
            if (!dead && h != c.height + c.nresults) throw LoadError("stack height mismatch at end of block");
            if (c.kind == K_IF && c.if_ins >= 0) {
                if (c.nparams != c.nresults) throw LoadError("if without else must not change the stack");
                code[c.if_ins].a = (uint32_t)code.size();
            }
            for (uint32_t i : c.patches) code[i].a = (uint32_t)code.size();
            for (uint32_t i : c.tpatches) f.brtable[i] = (uint32_t)code.size();
            ctl.pop_back();
            h = c.height + c.nresults;
            if (h > maxh) maxh = h;
            dead = false;
            if (ctl.empty()) {
                emit(0x0F);
                if (!r.eof()) throw LoadError("code after end of function");
                f.max_stack = maxh;
                fuse(f.code, f.brtable);
                return;
            }
            break;
        }
        case 0x0C: branch(r.u32(), OP_JMP, OP_BR); dead = true; break;
        case 0x0D: { uint32_t d = r.u32(); pop(1); branch(d, OP_JMP_IF, OP_BR_IF); break; }
        case 0x0E: {
            uint32_t n = r.count(1 << 20, "br_table entries");
            std::vector<uint32_t> depths(n + 1);
            for (auto &d : depths) d = r.u32();
            pop(1);
            emit(OP_BR_TABLE, (uint32_t)f.brtable.size(), n + 1);
            for (uint32_t d : depths) {
                uint32_t drop, arity;
                Ctl &t = label(d, drop, arity);
                if (t.kind == K_LOOP) f.brtable.push_back(t.start);
                else { t.tpatches.push_back((uint32_t)f.brtable.size()); f.brtable.push_back(0); }
                f.brtable.push_back(drop);
                f.brtable.push_back(arity);
            }
            dead = true;
            break;
        }
        case 0x0F: need(f.nresults); emit(0x0F); dead = true; break;
        case 0x10: {
            uint32_t fi = r.u32();
            if (fi >= funcs_.size()) throw LoadError("call to unknown function");
            const FuncType &t = types_[funcs_[fi].type];
            pop((uint32_t)t.params.size());
            push((uint32_t)t.results.size());
            emit(0x10, fi);
            break;
        }
        case 0x11: {
            uint32_t ti = r.u32(), tab = r.u32();
            if (ti >= types_.size() || tab != 0 || !has_table_) throw LoadError("bad call_indirect");
            const FuncType &t = types_[ti];
            pop(1);
            pop((uint32_t)t.params.size());
            push((uint32_t)t.results.size());
            emit(0x11, ti);
            break;
        }
        case 0x1A: pop(1); emit(0x1A); break;
        case 0x1B: pop(3); push(1); emit(0x1B); break;
        case 0x1C: {
            uint32_t n = r.u32();
            for (uint32_t i = 0; i < n; i++) r.byte();
            pop(3); push(1); emit(0x1B);
            break;
        }
        case 0x20: emit(0x20, local(r.u32())); push(1); break;
        case 0x21: emit(0x21, local(r.u32())); pop(1); break;
        case 0x22: emit(0x22, local(r.u32())); need(1); break;
        case 0x23: emit(0x23, global(r.u32())); push(1); break;
        case 0x24: emit(0x24, global(r.u32())); pop(1); break;
        case 0x3F: r.byte(); need_memory(); push(1); emit(0x3F); break;
        case 0x40: r.byte(); need_memory(); pop(1); push(1); emit(0x40); break;
        case 0x41: push(1); emit(0x41, 0, (uint32_t)(int32_t)r.sleb(32)); break;
        case 0x42: push(1); emit(0x42, 0, (uint64_t)r.sleb(64)); break;
        case 0x43: { uint32_t v; std::memcpy(&v, r.take(4), 4); push(1); emit(0x43, 0, v); break; }
        case 0x44: { uint64_t v; std::memcpy(&v, r.take(8), 8); push(1); emit(0x44, 0, v); break; }
        case 0xFC: {
            uint32_t sub = r.u32();
            if (sub <= 7) { pop(1); push(1); emit(OP_FC + sub); }
            else if (sub == 8) {
                uint32_t seg = r.u32(); r.byte(); need_memory();
                if (seg >= data_count_) throw LoadError("memory.init: bad data segment");
                pop(3); emit(OP_FC + 8, seg);
            } else if (sub == 9) {
                uint32_t seg = r.u32();
                if (seg >= data_count_) throw LoadError("data.drop: bad data segment");
                emit(OP_FC + 9, seg);
            } else if (sub == 10) { r.byte(); r.byte(); need_memory(); pop(3); emit(OP_FC + 10); }
            else if (sub == 11) { r.byte(); need_memory(); pop(3); emit(OP_FC + 11); }
            else throw LoadError("unsupported instruction 0xFC " + std::to_string(sub));
            break;
        }
        default:
            if (op >= 0x28 && op <= 0x35) { r.u32(); uint32_t off = r.u32(); need_memory(); pop(1); push(1); emit(op, off); }
            else if (op >= 0x36 && op <= 0x3E) { r.u32(); uint32_t off = r.u32(); need_memory(); pop(2); emit(op, off); }
            else if (is_unary(op)) { pop(1); push(1); emit(op); }
            else if (is_binary(op)) { pop(2); push(1); emit(op); }
            else throw LoadError("unsupported instruction " + hex(op));
        }
    }
}

// ---- execution ------------------------------------------------------------------

void Instance::call(uint32_t func, const uint64_t *args, uint64_t *results) {
    if (func >= funcs_.size()) trap("call to unknown function");
    const Func &F = funcs_[func];
    uint64_t *base = stack_top_;
    if (base + F.nparams + F.nresults + 16 > stack_.data() + stack_.size()) trap("call stack exhausted");
    for (uint32_t i = 0; i < F.nparams; i++) base[i] = args[i];

    uint64_t *saved_top = stack_top_;
    size_t saved_frames = frames_.size();
    if (depth_ == 0) deadline_ = time_limit_ms ? now_ms() + time_limit_ms : 0;
    depth_++;
    try {
        if (F.imported) {
            stack_top_ = base + std::max(F.nparams, F.nresults);
            F.host(*this, base);
        } else {
            execute(func, base);
        }
    } catch (...) {
        depth_--;
        stack_top_ = saved_top;
        frames_.resize(saved_frames);
        throw;
    }
    depth_--;
    stack_top_ = saved_top;
    for (uint32_t i = 0; i < F.nresults; i++) results[i] = base[i];
}

#ifdef WASM_PROFILE
// Instruction statistics (tests/viewerbench.cpp built with -DWASM_PROFILE): which opcodes,
// and which pairs and triples of them, run most. Guides the choice of fused instructions.
static uint64_t g_ops[0x200], g_pairs[0x200][0x200];
static std::unordered_map<uint64_t, uint64_t> g_triples;
static uint32_t g_prev1, g_prev2;
static std::vector<uint64_t> g_funcs;  // instructions run in each function
static const void *g_funcs_base;

void profile_dump(FILE *f) {
    uint64_t total = 0;
    for (uint64_t n : g_ops) total += n;
    fprintf(f, "%llu instructions\n", (unsigned long long)total);
    std::vector<std::pair<uint64_t, uint32_t>> v;
    for (uint32_t i = 0; i < 0x200; i++)
        if (g_ops[i]) v.push_back({g_ops[i], i});
    std::sort(v.rbegin(), v.rend());
    std::vector<std::pair<uint64_t, size_t>> fs;
    for (size_t i = 0; i < g_funcs.size(); i++)
        if (g_funcs[i]) fs.push_back({g_funcs[i], i});
    std::sort(fs.rbegin(), fs.rend());
    fprintf(f, "top functions (index):\n");
    for (size_t i = 0; i < fs.size() && i < 25; i++) fprintf(f, "  %5.2f%%  func %zu\n", 100.0 * fs[i].first / total, fs[i].second);
    fprintf(f, "top instructions:\n");
    for (size_t i = 0; i < v.size() && i < 30; i++) fprintf(f, "  %5.2f%%  %03x\n", 100.0 * v[i].first / total, v[i].second);
    std::vector<std::pair<uint64_t, uint32_t>> p;
    for (uint32_t a = 0; a < 0x200; a++)
        for (uint32_t b = 0; b < 0x200; b++)
            if (g_pairs[a][b]) p.push_back({g_pairs[a][b], a << 10 | b});
    std::sort(p.rbegin(), p.rend());
    fprintf(f, "top pairs:\n");
    for (size_t i = 0; i < p.size() && i < 40; i++)
        fprintf(f, "  %5.2f%%  %03x %03x\n", 100.0 * p[i].first / total, p[i].second >> 10, p[i].second & 1023);
    std::vector<std::pair<uint64_t, uint64_t>> t(g_triples.begin(), g_triples.end());
    for (auto &x : t) std::swap(x.first, x.second);
    std::sort(t.rbegin(), t.rend());
    fprintf(f, "top triples:\n");
    for (size_t i = 0; i < t.size() && i < 40; i++)
        fprintf(f, "  %5.2f%%  %03x %03x %03x\n", 100.0 * t[i].first / total, (unsigned)(t[i].second >> 20),
                (unsigned)(t[i].second >> 10 & 1023), (unsigned)(t[i].second & 1023));
}
#define PROF(op)                                                                         do {                                                                                     uint32_t o_ = (op);                                                                  g_ops[o_]++;                                                                         g_pairs[g_prev1][o_]++;                                                              g_triples[(uint64_t)g_prev2 << 20 | (uint64_t)g_prev1 << 10 | o_]++;                 g_prev2 = g_prev1;                                                                   g_prev1 = o_;                                                                    } while (0)
#else
#define PROF(op)
#endif

void Instance::execute(uint32_t func, uint64_t *fp) {
    const size_t frame_base = frames_.size();
    uint64_t *const stack_end = stack_.data() + stack_.size();
    const Func *F = &funcs_[func];
    const Ins *code, *ip;
    uint64_t *sp;
    uint8_t *mem = mem_.data();
    uint64_t msz = mem_.size();
    uint32_t fuel = 1 << 20;

#define ENTER(G)                                                                          \
    do {                                                                                  \
        if (fp + (G)->nlocals + (G)->max_stack + 4 > stack_end) trap("call stack exhausted"); \
        for (uint32_t i_ = (G)->nparams; i_ < (G)->nlocals; i_++) fp[i_] = 0;             \
        sp = fp + (G)->nlocals;                                                           \
        code = ip = (G)->code.data();                                                     \
    } while (0)
#define FUEL                                                              \
    do {                                                                  \
        if (--fuel == 0) {                                                \
            fuel = 1 << 20;                                               \
            if (deadline_ && now_ms() > deadline_) trap("page is not responding (script ran too long)"); \
        }                                                                 \
    } while (0)
#define MOVE_DOWN(drop, n)                                             \
    do {                                                               \
        uint64_t *s_ = sp - (n);                                       \
        for (uint32_t k_ = 0; k_ < (n); k_++) s_[k_ - (drop)] = s_[k_]; \
        sp -= (drop);                                                  \
    } while (0)
#define LOAD(T, CONV)                                            \
    {                                                            \
        uint64_t ea = (uint64_t)(uint32_t)sp[-1] + I.a;          \
        if (ea + sizeof(T) > msz) trap("out of bounds memory access"); \
        T v;                                                     \
        std::memcpy(&v, mem + ea, sizeof(T));                    \
        sp[-1] = CONV;                                           \
        break;                                                   \
    }
#define STORE(T)                                                 \
    {                                                            \
        T v = (T)sp[-1];                                         \
        uint64_t ea = (uint64_t)(uint32_t)sp[-2] + I.a;          \
        sp -= 2;                                                 \
        if (ea + sizeof(T) > msz) trap("out of bounds memory access"); \
        std::memcpy(mem + ea, &v, sizeof(T));                    \
        break;                                                   \
    }
#define UN32(e) { uint32_t a = (uint32_t)sp[-1]; sp[-1] = (uint32_t)(e); break; }
#define UN64(e) { uint64_t a = sp[-1]; sp[-1] = (uint64_t)(e); break; }
#define BIN32(e) { uint32_t b = (uint32_t)sp[-1], a = (uint32_t)sp[-2]; --sp; sp[-1] = (uint32_t)(e); break; }
#define BIN64(e) { uint64_t b = sp[-1], a = sp[-2]; --sp; sp[-1] = (uint64_t)(e); break; }
#define CMP64(e) { uint64_t b = sp[-1], a = sp[-2]; --sp; sp[-1] = (e) ? 1u : 0u; break; }
#define UNF32(e) { float a = to_f32(sp[-1]); sp[-1] = from_f32(e); break; }
#define UNF64(e) { double a = to_f64(sp[-1]); sp[-1] = from_f64(e); break; }
#define BINF32(e) { float b = to_f32(sp[-1]), a = to_f32(sp[-2]); --sp; sp[-1] = from_f32(e); break; }
#define BINF64(e) { double b = to_f64(sp[-1]), a = to_f64(sp[-2]); --sp; sp[-1] = from_f64(e); break; }
#define CMPF32(e) { float b = to_f32(sp[-1]), a = to_f32(sp[-2]); --sp; sp[-1] = (e) ? 1u : 0u; break; }
#define CMPF64(e) { double b = to_f64(sp[-1]), a = to_f64(sp[-2]); --sp; sp[-1] = (e) ? 1u : 0u; break; }
#define S32(x) ((int32_t)(x))
#define S64(x) ((int64_t)(x))

    // Threaded dispatch (a GNU extension): the most frequent instructions end by jumping
    // straight to the next one's code through this table, rather than back through the
    // switch; there are then many jump sites, which the CPU predicts much better.
    static const void *dispatch[0x200];
    static bool dispatch_ready = false;
    if (!dispatch_ready) {
        for (auto &d : dispatch) d = &&L_switch;
        dispatch[OP_JMP] = &&L_JMP, dispatch[OP_JMP_IF] = &&L_JMP_IF, dispatch[OP_JMP_IFZ] = &&L_JMP_IFZ;
        dispatch[0x1A] = &&L_1A, dispatch[0x20] = &&L_20, dispatch[0x21] = &&L_21, dispatch[0x22] = &&L_22;
        dispatch[0x28] = &&L_28, dispatch[0x2D] = &&L_2D, dispatch[0x36] = &&L_36, dispatch[0x3A] = &&L_3A;
        dispatch[0x41] = dispatch[0x42] = dispatch[0x43] = dispatch[0x44] = &&L_41;
        dispatch[0x45] = &&L_45, dispatch[0x46] = &&L_46, dispatch[0x47] = &&L_47;
        dispatch[0x6A] = &&L_6A, dispatch[0x6B] = &&L_6B, dispatch[0x71] = &&L_71;
        dispatch[OPX_ADDI] = &&L_ADDI, dispatch[OPX_LGET_ADDI] = &&L_LGET_ADDI, dispatch[OPX_LGET_CONST] = &&L_LGET_CONST;
        dispatch[OPX_LGET2] = &&L_LGET2, dispatch[OPX_LSET_LGET] = &&L_LSET_LGET, dispatch[OPX_ADD_LSET] = &&L_ADD_LSET;
        dispatch[OPX_ADD_LTEE] = &&L_ADD_LTEE, dispatch[OPX_LGET_LOAD] = &&L_LGET_LOAD, dispatch[OPX_LGET_LOAD8U] = &&L_LGET_LOAD8U;
        dispatch[OPX_LGET_STORE] = &&L_LGET_STORE, dispatch[OPX_LGET_ADDI_LSET] = &&L_LGET_ADDI_LSET;
        dispatch[OPX_LGET_ADDI_LTEE] = &&L_LGET_ADDI_LTEE, dispatch[OPX_LTEE_LGET] = &&L_LTEE_LGET;
        dispatch[OPX_LGET2_ADD] = &&L_LGET2_ADD, dispatch[OPX_ADD_LOAD] = &&L_ADD_LOAD, dispatch[OPX_ADD_LOAD8U] = &&L_ADD_LOAD8U;
#define T_CB(k) dispatch[OPX_CONST_BIN + k] = &&L_CB##k, dispatch[OPX_LGET_CONST_BIN + k] = &&L_LCB##k;
        T_CB(0) T_CB(1) T_CB(2) T_CB(3) T_CB(4) T_CB(5) T_CB(6) T_CB(7) T_CB(8) T_CB(9) T_CB(10) T_CB(11) T_CB(12)
        T_CB(13) T_CB(14) T_CB(15) T_CB(16) T_CB(17)
#undef T_CB
#define T_CJ(k)                                                                                           \
        dispatch[OPX_CMP_JIF + k] = &&L_CJ##k, dispatch[OPX_CMP_JIFZ + k] = &&L_CJZ##k;                   \
        dispatch[OPX_CONST_CMP_JIF + k] = &&L_CCJ##k, dispatch[OPX_CONST_CMP_JIFZ + k] = &&L_CCJZ##k;     \
        dispatch[OPX_LGET_CMP_JIF + k] = &&L_LCJ##k, dispatch[OPX_LGET_CMP_JIFZ + k] = &&L_LCJZ##k;
        T_CJ(1) T_CJ(2) T_CJ(3) T_CJ(4) T_CJ(5) T_CJ(6) T_CJ(7) T_CJ(8) T_CJ(9) T_CJ(10)
#undef T_CJ
        dispatch[OPX_CMP_JIF] = &&L_CJ0, dispatch[OPX_CMP_JIFZ] = &&L_CJZ0;
        dispatch[OPX_LGET_CMP_JIF] = &&L_LCJ0, dispatch[OPX_LGET_CMP_JIFZ] = &&L_LCJZ0;
        dispatch_ready = true;
    }
    const Ins *cur;
#define I (*cur)
#define NEXT                          \
    do {                              \
        cur = ip++;                   \
        PROF(cur->op);                \
        goto *dispatch[cur->op];      \
    } while (0)
#define LOAD_NEXT(T, CONV)                                       \
    {                                                            \
        uint64_t ea = (uint64_t)(uint32_t)sp[-1] + I.a;          \
        if (ea + sizeof(T) > msz) trap("out of bounds memory access"); \
        T v;                                                     \
        std::memcpy(&v, mem + ea, sizeof(T));                    \
        sp[-1] = CONV;                                           \
        NEXT;                                                    \
    }
#define STORE_NEXT(T)                                            \
    {                                                            \
        T v = (T)sp[-1];                                         \
        uint64_t ea = (uint64_t)(uint32_t)sp[-2] + I.a;          \
        sp -= 2;                                                 \
        if (ea + sizeof(T) > msz) trap("out of bounds memory access"); \
        std::memcpy(mem + ea, &v, sizeof(T));                    \
        NEXT;                                                    \
    }
#define BIN32_NEXT(e) { uint32_t b = (uint32_t)sp[-1], a = (uint32_t)sp[-2]; --sp; sp[-1] = (uint32_t)(e); NEXT; }

    ENTER(F);
    for (;;) {
        cur = ip++;
        PROF(cur->op);
    L_switch:
#ifdef WASM_PROFILE
        {
            size_t fi = (size_t)(F - funcs_.data());
            if (g_funcs.size() <= fi) g_funcs.resize(funcs_.size());
            g_funcs[fi]++;
        }
#endif
        switch (I.op) {
        case 0x00: trap("unreachable instruction executed");

        // ---- control ----
        case OP_JMP: L_JMP: ip = code + I.a; FUEL; NEXT;
        case OP_JMP_IF: L_JMP_IF: if ((uint32_t)*--sp) { ip = code + I.a; FUEL; } NEXT;
        case OP_JMP_IFZ: L_JMP_IFZ: if (!(uint32_t)*--sp) ip = code + I.a; NEXT;
        case OP_BR: {
            uint32_t drop = (uint32_t)I.b, n = (uint32_t)(I.b >> 32);
            MOVE_DOWN(drop, n);
            ip = code + I.a;
            FUEL;
            break;
        }
        case OP_BR_IF:
            if ((uint32_t)*--sp) {
                uint32_t drop = (uint32_t)I.b, n = (uint32_t)(I.b >> 32);
                MOVE_DOWN(drop, n);
                ip = code + I.a;
                FUEL;
            }
            break;
        case OP_BR_TABLE: {
            uint32_t idx = (uint32_t)*--sp, cnt = (uint32_t)I.b;
            if (idx >= cnt - 1) idx = cnt - 1;
            const uint32_t *e = &F->brtable[I.a + idx * 3];
            uint32_t drop = e[1], n = e[2];
            MOVE_DOWN(drop, n);
            ip = code + e[0];
            FUEL;
            break;
        }
        case 0x0F: {
            uint32_t n = F->nresults;
            uint64_t *s = sp - n;
            for (uint32_t k = 0; k < n; k++) fp[k] = s[k];
            sp = fp + n;
            if (frames_.size() == frame_base) return;
            const Frame &fr = frames_.back();
            F = fr.f;
            ip = fr.ip;
            fp = fr.fp;
            code = F->code.data();
            frames_.pop_back();
            break;
        }
        case 0x10:
        case 0x11: {
            const Func *G;
            if (I.op == 0x10) {
                G = &funcs_[I.a];
            } else {
                uint32_t i = (uint32_t)*--sp;
                if (i >= table_.size()) trap("undefined table element");
                uint32_t fi = table_[i];
                if (fi == kNullFunc) trap("uninitialized table element");
                G = &funcs_[fi];
                if (canon_[G->type] != canon_[I.a]) trap("indirect call signature mismatch");
            }
            if (G->imported) {
                uint64_t *args = sp - G->nparams;
                stack_top_ = args + std::max(G->nparams, G->nresults);
                G->host(*this, args);
                sp = args + G->nresults;
                mem = mem_.data();
                msz = mem_.size();
                break;
            }
            if (frames_.size() >= kMaxFrames) trap("call stack exhausted");
            frames_.push_back({F, ip, fp});
            F = G;
            fp = sp - G->nparams;
            ENTER(G);
            FUEL;
            break;
        }

        // ---- parametric / variables ----
        case 0x1A: L_1A: --sp; NEXT;
        case 0x1B: {
            uint32_t c = (uint32_t)sp[-1];
            sp -= 2;
            if (!c) sp[-1] = sp[0];
            break;
        }
        case 0x20: L_20: *sp++ = fp[I.a]; NEXT;
        case 0x21: L_21: fp[I.a] = *--sp; NEXT;
        case 0x22: L_22: fp[I.a] = sp[-1]; NEXT;
        case 0x23: *sp++ = globals_[I.a]; break;
        case 0x24: globals_[I.a] = *--sp; break;

        // ---- memory ----
        case 0x28: L_28: LOAD_NEXT(uint32_t, (uint64_t)v)
        case 0x29: LOAD(uint64_t, v)
        case 0x2A: LOAD(uint32_t, (uint64_t)v)
        case 0x2B: LOAD(uint64_t, v)
        case 0x2C: LOAD(int8_t, (uint64_t)(uint32_t)(int32_t)v)
        case 0x2D: L_2D: LOAD_NEXT(uint8_t, (uint64_t)v)
        case 0x2E: LOAD(int16_t, (uint64_t)(uint32_t)(int32_t)v)
        case 0x2F: LOAD(uint16_t, (uint64_t)v)
        case 0x30: LOAD(int8_t, (uint64_t)(int64_t)v)
        case 0x31: LOAD(uint8_t, (uint64_t)v)
        case 0x32: LOAD(int16_t, (uint64_t)(int64_t)v)
        case 0x33: LOAD(uint16_t, (uint64_t)v)
        case 0x34: LOAD(int32_t, (uint64_t)(int64_t)v)
        case 0x35: LOAD(uint32_t, (uint64_t)v)
        case 0x36: L_36: STORE_NEXT(uint32_t)
        case 0x37: STORE(uint64_t)
        case 0x38: STORE(uint32_t)
        case 0x39: STORE(uint64_t)
        case 0x3A: L_3A: STORE_NEXT(uint8_t)
        case 0x3B: STORE(uint16_t)
        case 0x3C: STORE(uint8_t)
        case 0x3D: STORE(uint16_t)
        case 0x3E: STORE(uint32_t)
        case 0x3F: *sp++ = msz / 65536; break;
        case 0x40: {
            sp[-1] = grow((uint32_t)sp[-1]);
            mem = mem_.data();
            msz = mem_.size();
            break;
        }

        // ---- constants ----
        case 0x41: case 0x42: case 0x43: case 0x44: L_41: *sp++ = I.b; NEXT;

        // ---- i32 ----
        case 0x45: L_45: { uint32_t a = (uint32_t)sp[-1]; sp[-1] = (uint32_t)(a == 0); NEXT; }
        case 0x46: L_46: BIN32_NEXT(a == b)
        case 0x47: L_47: BIN32_NEXT(a != b)
        case 0x48: BIN32(S32(a) < S32(b))
        case 0x49: BIN32(a < b)
        case 0x4A: BIN32(S32(a) > S32(b))
        case 0x4B: BIN32(a > b)
        case 0x4C: BIN32(S32(a) <= S32(b))
        case 0x4D: BIN32(a <= b)
        case 0x4E: BIN32(S32(a) >= S32(b))
        case 0x4F: BIN32(a >= b)
        // ---- i64 comparisons ----
        case 0x50: { sp[-1] = sp[-1] == 0 ? 1u : 0u; break; }
        case 0x51: CMP64(a == b)
        case 0x52: CMP64(a != b)
        case 0x53: CMP64(S64(a) < S64(b))
        case 0x54: CMP64(a < b)
        case 0x55: CMP64(S64(a) > S64(b))
        case 0x56: CMP64(a > b)
        case 0x57: CMP64(S64(a) <= S64(b))
        case 0x58: CMP64(a <= b)
        case 0x59: CMP64(S64(a) >= S64(b))
        case 0x5A: CMP64(a >= b)
        // ---- float comparisons ----
        case 0x5B: CMPF32(a == b)
        case 0x5C: CMPF32(a != b)
        case 0x5D: CMPF32(a < b)
        case 0x5E: CMPF32(a > b)
        case 0x5F: CMPF32(a <= b)
        case 0x60: CMPF32(a >= b)
        case 0x61: CMPF64(a == b)
        case 0x62: CMPF64(a != b)
        case 0x63: CMPF64(a < b)
        case 0x64: CMPF64(a > b)
        case 0x65: CMPF64(a <= b)
        case 0x66: CMPF64(a >= b)
        // ---- i32 arithmetic ----
        case 0x67: UN32(std::countl_zero(a))
        case 0x68: UN32(std::countr_zero(a))
        case 0x69: UN32(std::popcount(a))
        case 0x6A: L_6A: BIN32_NEXT(a + b)
        case 0x6B: L_6B: BIN32_NEXT(a - b)
        case 0x6C: BIN32(a * b)
        case 0x6D: {
            int32_t b = S32(sp[-1]), a = S32(sp[-2]);
            if (b == 0) trap("integer divide by zero");
            if (a == INT32_MIN && b == -1) trap("integer overflow");
            --sp;
            sp[-1] = (uint32_t)(a / b);
            break;
        }
        case 0x6E: {
            uint32_t b = (uint32_t)sp[-1], a = (uint32_t)sp[-2];
            if (b == 0) trap("integer divide by zero");
            --sp;
            sp[-1] = a / b;
            break;
        }
        case 0x6F: {
            int32_t b = S32(sp[-1]), a = S32(sp[-2]);
            if (b == 0) trap("integer divide by zero");
            --sp;
            sp[-1] = (uint32_t)(b == -1 ? 0 : a % b);
            break;
        }
        case 0x70: {
            uint32_t b = (uint32_t)sp[-1], a = (uint32_t)sp[-2];
            if (b == 0) trap("integer divide by zero");
            --sp;
            sp[-1] = a % b;
            break;
        }
        case 0x71: L_71: BIN32_NEXT(a & b)
        case 0x72: BIN32(a | b)
        case 0x73: BIN32(a ^ b)
        case 0x74: BIN32(a << (b & 31))
        case 0x75: BIN32(S32(a) >> (b & 31))
        case 0x76: BIN32(a >> (b & 31))
        case 0x77: BIN32(std::rotl(a, (int)(b & 31)))
        case 0x78: BIN32(std::rotr(a, (int)(b & 31)))
        // ---- i64 arithmetic ----
        case 0x79: UN64(std::countl_zero(a))
        case 0x7A: UN64(std::countr_zero(a))
        case 0x7B: UN64(std::popcount(a))
        case 0x7C: BIN64(a + b)
        case 0x7D: BIN64(a - b)
        case 0x7E: BIN64(a * b)
        case 0x7F: {
            int64_t b = S64(sp[-1]), a = S64(sp[-2]);
            if (b == 0) trap("integer divide by zero");
            if (a == INT64_MIN && b == -1) trap("integer overflow");
            --sp;
            sp[-1] = (uint64_t)(a / b);
            break;
        }
        case 0x80: {
            uint64_t b = sp[-1], a = sp[-2];
            if (b == 0) trap("integer divide by zero");
            --sp;
            sp[-1] = a / b;
            break;
        }
        case 0x81: {
            int64_t b = S64(sp[-1]), a = S64(sp[-2]);
            if (b == 0) trap("integer divide by zero");
            --sp;
            sp[-1] = (uint64_t)(b == -1 ? 0 : a % b);
            break;
        }
        case 0x82: {
            uint64_t b = sp[-1], a = sp[-2];
            if (b == 0) trap("integer divide by zero");
            --sp;
            sp[-1] = a % b;
            break;
        }
        case 0x83: BIN64(a & b)
        case 0x84: BIN64(a | b)
        case 0x85: BIN64(a ^ b)
        case 0x86: BIN64(a << (b & 63))
        case 0x87: BIN64(S64(a) >> (b & 63))
        case 0x88: BIN64(a >> (b & 63))
        case 0x89: BIN64(std::rotl(a, (int)(b & 63)))
        case 0x8A: BIN64(std::rotr(a, (int)(b & 63)))
        // ---- f32 arithmetic ----
        case 0x8B: { sp[-1] &= 0x7FFFFFFFu; break; }
        case 0x8C: { sp[-1] ^= 0x80000000u; break; }
        case 0x8D: UNF32(std::ceil(a))
        case 0x8E: UNF32(std::floor(a))
        case 0x8F: UNF32(std::trunc(a))
        case 0x90: UNF32(std::nearbyint(a))
        case 0x91: UNF32(std::sqrt(a))
        case 0x92: BINF32(a + b)
        case 0x93: BINF32(a - b)
        case 0x94: BINF32(a * b)
        case 0x95: BINF32(a / b)
        case 0x96: BINF32(fmin_w(a, b))
        case 0x97: BINF32(fmax_w(a, b))
        case 0x98: { uint64_t b = sp[-1]; --sp; sp[-1] = (sp[-1] & 0x7FFFFFFFu) | (b & 0x80000000u); break; }
        // ---- f64 arithmetic ----
        case 0x99: { sp[-1] &= 0x7FFFFFFFFFFFFFFFull; break; }
        case 0x9A: { sp[-1] ^= 0x8000000000000000ull; break; }
        case 0x9B: UNF64(std::ceil(a))
        case 0x9C: UNF64(std::floor(a))
        case 0x9D: UNF64(std::trunc(a))
        case 0x9E: UNF64(std::nearbyint(a))
        case 0x9F: UNF64(std::sqrt(a))
        case 0xA0: BINF64(a + b)
        case 0xA1: BINF64(a - b)
        case 0xA2: BINF64(a * b)
        case 0xA3: BINF64(a / b)
        case 0xA4: BINF64(fmin_w(a, b))
        case 0xA5: BINF64(fmax_w(a, b))
        case 0xA6: { uint64_t b = sp[-1]; --sp; sp[-1] = (sp[-1] & 0x7FFFFFFFFFFFFFFFull) | (b & 0x8000000000000000ull); break; }
        // ---- conversions ----
        case 0xA7: sp[-1] = (uint32_t)sp[-1]; break;
        case 0xA8: sp[-1] = trunc_checked(to_f32(sp[-1]), 0); break;
        case 0xA9: sp[-1] = trunc_checked(to_f32(sp[-1]), 1); break;
        case 0xAA: sp[-1] = trunc_checked(to_f64(sp[-1]), 0); break;
        case 0xAB: sp[-1] = trunc_checked(to_f64(sp[-1]), 1); break;
        case 0xAC: sp[-1] = (uint64_t)(int64_t)S32(sp[-1]); break;
        case 0xAD: sp[-1] = (uint32_t)sp[-1]; break;
        case 0xAE: sp[-1] = trunc_checked(to_f32(sp[-1]), 2); break;
        case 0xAF: sp[-1] = trunc_checked(to_f32(sp[-1]), 3); break;
        case 0xB0: sp[-1] = trunc_checked(to_f64(sp[-1]), 2); break;
        case 0xB1: sp[-1] = trunc_checked(to_f64(sp[-1]), 3); break;
        case 0xB2: sp[-1] = from_f32((float)S32(sp[-1])); break;
        case 0xB3: sp[-1] = from_f32((float)(uint32_t)sp[-1]); break;
        case 0xB4: sp[-1] = from_f32((float)S64(sp[-1])); break;
        case 0xB5: sp[-1] = from_f32((float)sp[-1]); break;
        case 0xB6: sp[-1] = from_f32((float)to_f64(sp[-1])); break;
        case 0xB7: sp[-1] = from_f64((double)S32(sp[-1])); break;
        case 0xB8: sp[-1] = from_f64((double)(uint32_t)sp[-1]); break;
        case 0xB9: sp[-1] = from_f64((double)S64(sp[-1])); break;
        case 0xBA: sp[-1] = from_f64((double)sp[-1]); break;
        case 0xBB: sp[-1] = from_f64((double)to_f32(sp[-1])); break;
        case 0xBC: case 0xBD: case 0xBE: case 0xBF: break;  // reinterpret: same bits
        case 0xC0: sp[-1] = (uint32_t)(int32_t)(int8_t)sp[-1]; break;
        case 0xC1: sp[-1] = (uint32_t)(int32_t)(int16_t)sp[-1]; break;
        case 0xC2: sp[-1] = (uint64_t)(int64_t)(int8_t)sp[-1]; break;
        case 0xC3: sp[-1] = (uint64_t)(int64_t)(int16_t)sp[-1]; break;
        case 0xC4: sp[-1] = (uint64_t)(int64_t)(int32_t)sp[-1]; break;

        // ---- 0xFC prefix ----
        case OP_FC + 0: sp[-1] = trunc_sat(to_f32(sp[-1]), 0); break;
        case OP_FC + 1: sp[-1] = trunc_sat(to_f32(sp[-1]), 1); break;
        case OP_FC + 2: sp[-1] = trunc_sat(to_f64(sp[-1]), 0); break;
        case OP_FC + 3: sp[-1] = trunc_sat(to_f64(sp[-1]), 1); break;
        case OP_FC + 4: sp[-1] = trunc_sat(to_f32(sp[-1]), 2); break;
        case OP_FC + 5: sp[-1] = trunc_sat(to_f32(sp[-1]), 3); break;
        case OP_FC + 6: sp[-1] = trunc_sat(to_f64(sp[-1]), 2); break;
        case OP_FC + 7: sp[-1] = trunc_sat(to_f64(sp[-1]), 3); break;
        case OP_FC + 8: {  // memory.init
            sp -= 3;
            uint64_t d = (uint32_t)sp[0], s = (uint32_t)sp[1], n = (uint32_t)sp[2];
            const std::vector<uint8_t> &seg = datas_[I.a];
            if (s + n > seg.size() || d + n > msz) trap("out of bounds memory access");
            if (n) std::memcpy(mem + d, seg.data() + s, n);
            break;
        }
        case OP_FC + 9: datas_[I.a].clear(); datas_[I.a].shrink_to_fit(); break;
        case OP_FC + 10: {  // memory.copy
            sp -= 3;
            uint64_t d = (uint32_t)sp[0], s = (uint32_t)sp[1], n = (uint32_t)sp[2];
            if (s + n > msz || d + n > msz) trap("out of bounds memory access");
            std::memmove(mem + d, mem + s, n);
            break;
        }
        case OP_FC + 11: {  // memory.fill
            sp -= 3;
            uint64_t d = (uint32_t)sp[0], n = (uint32_t)sp[2];
            if (d + n > msz) trap("out of bounds memory access");
            std::memset(mem + d, (uint8_t)sp[1], n);
            break;
        }
        // ---- fused instructions (see fuse) ----
        case OPX_ADDI: L_ADDI: sp[-1] = (uint32_t)((uint32_t)sp[-1] + (uint32_t)I.b); NEXT;
        case OPX_LGET_ADDI: L_LGET_ADDI: *sp++ = (uint32_t)((uint32_t)fp[I.a] + (uint32_t)I.b); NEXT;
        case OPX_LGET_CONST: L_LGET_CONST: sp[0] = fp[I.a]; sp[1] = I.b; sp += 2; NEXT;
        case OPX_LGET2: L_LGET2: sp[0] = fp[I.a]; sp[1] = fp[(uint32_t)I.b]; sp += 2; NEXT;
        case OPX_LSET_LGET: L_LSET_LGET: fp[I.a] = sp[-1]; sp[-1] = fp[(uint32_t)I.b]; NEXT;
        case OPX_ADD_LSET: L_ADD_LSET: fp[I.a] = (uint32_t)((uint32_t)sp[-2] + (uint32_t)sp[-1]); sp -= 2; NEXT;
        case OPX_ADD_LTEE: L_ADD_LTEE: {
            uint64_t v = (uint32_t)((uint32_t)sp[-2] + (uint32_t)sp[-1]);
            --sp;
            sp[-1] = v;
            fp[I.a] = v;
            NEXT;
        }
        case OPX_LGET_LOAD: L_LGET_LOAD: {
            uint64_t ea = (uint64_t)(uint32_t)fp[I.a] + (uint32_t)I.b;
            if (ea + 4 > msz) trap("out of bounds memory access");
            uint32_t v;
            std::memcpy(&v, mem + ea, 4);
            *sp++ = v;
            NEXT;
        }
        case OPX_LGET_LOAD8U: L_LGET_LOAD8U: {
            uint64_t ea = (uint64_t)(uint32_t)fp[I.a] + (uint32_t)I.b;
            if (ea + 1 > msz) trap("out of bounds memory access");
            *sp++ = mem[ea];
            NEXT;
        }
        case OPX_LGET_STORE: L_LGET_STORE: {
            uint32_t v = (uint32_t)fp[I.a];
            uint64_t ea = (uint64_t)(uint32_t)sp[-1] + (uint32_t)I.b;
            --sp;
            if (ea + 4 > msz) trap("out of bounds memory access");
            std::memcpy(mem + ea, &v, 4);
            NEXT;
        }
        case OPX_LGET_ADDI_LSET: L_LGET_ADDI_LSET: fp[I.b >> 32] = (uint32_t)((uint32_t)fp[I.a] + (uint32_t)I.b); NEXT;
        case OPX_LGET_ADDI_LTEE: L_LGET_ADDI_LTEE: {
            uint64_t v = (uint32_t)((uint32_t)fp[I.a] + (uint32_t)I.b);
            fp[I.b >> 32] = v;
            *sp++ = v;
            NEXT;
        }
        case OPX_LTEE_LGET: L_LTEE_LGET: fp[I.a] = sp[-1]; *sp++ = fp[(uint32_t)I.b]; NEXT;
        case OPX_LGET2_ADD: L_LGET2_ADD: *sp++ = (uint32_t)((uint32_t)fp[I.a] + (uint32_t)fp[(uint32_t)I.b]); NEXT;
        case OPX_ADD_LOAD: L_ADD_LOAD: {
            uint64_t ea = (uint64_t)(uint32_t)((uint32_t)sp[-2] + (uint32_t)sp[-1]) + (uint32_t)I.b;
            --sp;
            if (ea + 4 > msz) trap("out of bounds memory access");
            uint32_t v;
            std::memcpy(&v, mem + ea, 4);
            sp[-1] = v;
            NEXT;
        }
        case OPX_ADD_LOAD8U: L_ADD_LOAD8U: {
            uint64_t ea = (uint64_t)(uint32_t)((uint32_t)sp[-2] + (uint32_t)sp[-1]) + (uint32_t)I.b;
            --sp;
            if (ea + 1 > msz) trap("out of bounds memory access");
            sp[-1] = mem[ea];
            NEXT;
        }
#define CBIN(k, e)                                                                                \
        case OPX_CONST_BIN + k: L_CB##k: { uint32_t a = (uint32_t)sp[-1], b = (uint32_t)I.b; sp[-1] = (uint32_t)(e); NEXT; } \
        case OPX_LGET_CONST_BIN + k: L_LCB##k: { uint32_t a = (uint32_t)fp[I.a], b = (uint32_t)I.b; *sp++ = (uint32_t)(e); NEXT; }
        CBIN(0, a - b) CBIN(1, a * b) CBIN(2, a & b) CBIN(3, a | b) CBIN(4, a ^ b) CBIN(5, a << (b & 31))
        CBIN(6, S32(a) >> (b & 31)) CBIN(7, a >> (b & 31)) CBIN(8, a == b) CBIN(9, a != b) CBIN(10, S32(a) < S32(b))
        CBIN(11, a < b) CBIN(12, S32(a) > S32(b)) CBIN(13, a > b) CBIN(14, S32(a) <= S32(b)) CBIN(15, a <= b)
        CBIN(16, S32(a) >= S32(b)) CBIN(17, a >= b)
#undef CBIN
        case OPX_CMP_JIF + 0: L_CJ0: { uint32_t a = (uint32_t)*--sp; if (a == 0) { ip = code + I.a; FUEL; } NEXT; }
        case OPX_CMP_JIFZ + 0: L_CJZ0: { uint32_t a = (uint32_t)*--sp; if (a != 0) ip = code + I.a; NEXT; }
        case OPX_LGET_CMP_JIF + 0: L_LCJ0: if ((uint32_t)fp[(uint32_t)I.b] == 0) { ip = code + I.a; FUEL; } NEXT;
        case OPX_LGET_CMP_JIFZ + 0: L_LCJZ0: if ((uint32_t)fp[(uint32_t)I.b] != 0) ip = code + I.a; NEXT;
#define CJ(k, e)                                                                    \
        case OPX_CMP_JIF + k: L_CJ##k: {                                                     \
            uint32_t b = (uint32_t)sp[-1], a = (uint32_t)sp[-2];                    \
            sp -= 2;                                                                \
            if (e) { ip = code + I.a; FUEL; }                                       \
            NEXT;                                                                  \
        }                                                                           \
        case OPX_CMP_JIFZ + k: L_CJZ##k: {                                                    \
            uint32_t b = (uint32_t)sp[-1], a = (uint32_t)sp[-2];                    \
            sp -= 2;                                                                \
            if (!(e)) ip = code + I.a;                                              \
            NEXT;                                                                  \
        }                                                                           \
        case OPX_CONST_CMP_JIF + k: L_CCJ##k: {                                               \
            uint32_t b = (uint32_t)I.b, a = (uint32_t)*--sp;                        \
            if (e) { ip = code + I.a; FUEL; }                                       \
            NEXT;                                                                  \
        }                                                                           \
        case OPX_CONST_CMP_JIFZ + k: L_CCJZ##k: {                                              \
            uint32_t b = (uint32_t)I.b, a = (uint32_t)*--sp;                        \
            if (!(e)) ip = code + I.a;                                              \
            NEXT;                                                                  \
        }                                                                           \
        case OPX_LGET_CMP_JIF + k: L_LCJ##k: {                                                \
            uint32_t b = (uint32_t)fp[(uint32_t)I.b], a = (uint32_t)*--sp;          \
            if (e) { ip = code + I.a; FUEL; }                                       \
            NEXT;                                                                  \
        }                                                                           \
        case OPX_LGET_CMP_JIFZ + k: L_LCJZ##k: {                                               \
            uint32_t b = (uint32_t)fp[(uint32_t)I.b], a = (uint32_t)*--sp;          \
            if (!(e)) ip = code + I.a;                                              \
            NEXT;                                                                  \
        }
        CJ(1, a == b) CJ(2, a != b) CJ(3, S32(a) < S32(b)) CJ(4, a < b) CJ(5, S32(a) > S32(b)) CJ(6, a > b)
        CJ(7, S32(a) <= S32(b)) CJ(8, a <= b) CJ(9, S32(a) >= S32(b)) CJ(10, a >= b)
#undef CJ
        default: trap("internal error: bad opcode");
        }
    }
#undef I
#undef NEXT
}

}  // namespace wasm
