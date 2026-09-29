// wasm.h — a small WebAssembly interpreter.
//
// Supports the MVP instruction set plus bulk memory, non-trapping float-to-int
// conversions, sign extension and multi-value blocks. Functions are translated
// once at load time into a flat instruction array with pre-resolved branch
// targets; common instruction sequences are then fused into single instructions
// ("superinstructions", about 2x fewer to run), and the most frequent ones dispatch
// the next instruction directly (threaded code). Its own call stack.
//
// Safety: every memory access, table access, call depth and stack height is
// checked, so a hostile module can trap but cannot touch host memory.
#pragma once
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace wasm {

struct Trap {
    std::string msg;
};

class Instance;
struct Reader;

// Memory reserved as address space and committed only as it is needed; untouched pages
// cost no RAM (the OS hands out zeroed pages on first use). Used for linear memory, which
// otherwise was a std::vector that doubled (and copied) itself as a page grew, and for the
// 8 MiB value stack, which was zero-filled up front.
void *vm_reserve(size_t bytes);
bool vm_commit(void *p, size_t bytes);
void vm_release(void *p);

template <class T> class VmArray {
public:
    VmArray() = default;
    VmArray(const VmArray &) = delete;
    VmArray &operator=(const VmArray &) = delete;
    ~VmArray() { if (p_) vm_release(p_); }
    T *data() const { return p_; }
    size_t size() const { return n_; }
    // Grows to n elements (new ones are zero). max: the most it may ever hold (first call).
    bool resize(size_t n, size_t max) {
        if (!p_) {
            p_ = (T *)vm_reserve(max * sizeof(T));
            if (!p_) return false;
            cap_ = max;
        }
        if (n > cap_) return false;
        size_t bytes = n * sizeof(T);
        if (bytes > committed_) {
            size_t want = std::min((bytes + 65535) & ~(size_t)65535, cap_ * sizeof(T));
            if (!vm_commit((char *)p_ + committed_, want - committed_)) return false;
            committed_ = want;
        }
        if (n > n_) n_ = n;
        return true;
    }

private:
    T *p_ = nullptr;
    size_t n_ = 0, cap_ = 0, committed_ = 0;
};

// A host function receives its arguments in args[0..nparams) and writes its
// results to args[0..nresults).
using HostFn = std::function<void(Instance &, uint64_t *args)>;

struct HostImport {
    std::string module, name;
    std::string sig;  // params ':' results, using i=i32 I=i64 f=f32 F=f64, e.g. "iii:i"
    HostFn fn;
};

struct FuncType {
    std::vector<uint8_t> params, results;
    bool operator==(const FuncType &o) const { return params == o.params && results == o.results; }
};

inline uint64_t from_f32(float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; }
inline uint64_t from_f64(double d) { uint64_t b; std::memcpy(&b, &d, 8); return b; }
inline float to_f32(uint64_t v) { uint32_t b = (uint32_t)v; float f; std::memcpy(&f, &b, 4); return f; }
inline double to_f64(uint64_t v) { double d; std::memcpy(&d, &v, 8); return d; }

class Instance {
public:
    Instance();

    // Parses, links and instantiates a module (running its start function).
    // Returns "" on success or an error message.
    std::string load(const uint8_t *data, size_t size, const std::vector<HostImport> &imports);

    // Index of an exported function, or -1.
    int export_func(const std::string &name) const;
    const FuncType &func_type(uint32_t func) const { return types_[funcs_[func].type]; }

    // Calls a function. args/results must hold as many values as its type says.
    // Throws Trap. Re-entrant: host functions may call back into the module.
    void call(uint32_t func, const uint64_t *args, uint64_t *results);

    uint8_t *memory() { return mem_.data(); }
    uint64_t memory_size() const { return mem_.size(); }
    // True if [addr, addr+len) lies inside linear memory.
    bool in_memory(uint64_t addr, uint64_t len) const { return addr + len <= mem_.size() && addr + len >= addr; }

    uint32_t time_limit_ms = 0;     // 0 = unlimited; otherwise a call that runs longer traps
    uint32_t max_memory_pages = 16384;  // 1 GiB
    void *user = nullptr;

    struct Ins {
        uint16_t op;
        uint32_t a;
        uint64_t b;
    };

private:
    struct Func {
        uint32_t type = 0;
        uint32_t nparams = 0, nresults = 0, nlocals = 0, max_stack = 0;
        bool imported = false;
        HostFn host;
        std::vector<Ins> code;
        std::vector<uint32_t> brtable;  // triples: target, drop, arity
    };
    struct Frame {
        const Func *f;
        const Ins *ip;
        uint64_t *fp;
    };

    void compile(Func &f, Reader &r);
    void execute(uint32_t func, uint64_t *fp);
    uint32_t grow(uint32_t pages);
    uint64_t const_expr(Reader &r);

    std::vector<FuncType> types_;
    std::vector<uint32_t> canon_;  // type index -> first structurally equal type index
    std::vector<Func> funcs_;
    std::vector<uint64_t> globals_;
    std::vector<uint32_t> table_;
    bool has_table_ = false, has_memory_ = false;
    uint32_t mem_max_pages_ = 65536;
    VmArray<uint8_t> mem_;
    std::vector<std::vector<uint8_t>> datas_;
    uint32_t data_count_ = 0;
    std::unordered_map<std::string, uint32_t> exports_;

    VmArray<uint64_t> stack_;
    uint64_t *stack_top_ = nullptr;
    std::vector<Frame> frames_;
    int depth_ = 0;
    uint64_t deadline_ = 0;
};

// Fused instructions on or off (tests compare the two); affects modules loaded afterwards.
void set_fusion(bool on);

#ifdef WASM_PROFILE
void profile_dump(FILE *f);
#endif

}  // namespace wasm
