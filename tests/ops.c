// ops.c — exercises many WebAssembly instructions. Each export returns a checksum;
// tests/check_ops.mjs compares our interpreter against V8 (Node).
typedef unsigned long long u64;
typedef long long i64;
typedef unsigned u32;
#define EXPORT(n) __attribute__((export_name(#n)))

static u64 mix(u64 h, u64 v) { h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2); return h; }

static volatile u32 vals32[] = {0, 1, 2, 3, 7, 31, 32, 33, 100, 255, 256, 65535, 0x7FFFFFFF, 0x80000000u, 0x80000001u, 0xFFFFFFFFu, 0xFFFFFFFEu, 12345678, 0xDEADBEEF};
static volatile u64 vals64[] = {0, 1, 2, 63, 64, 65, 0x7FFFFFFFFFFFFFFFull, 0x8000000000000000ull, 0xFFFFFFFFFFFFFFFFull, 0x123456789ABCDEFull, 1000000007ull, 0xFFFFFFFF00000000ull};
static volatile double valsf[] = {0.0, -0.0, 0.5, -0.5, 1.5, -1.5, 2.5, 3.7, -3.7, 1e10, -1e10, 1e30, 4294967295.5, 2147483647.9, -2147483648.9, 1e-30, 123456.789, 1.0/0.0, -1.0/0.0};

EXPORT(int32) u64 int32(void) {
    u64 h = 1;
    int n = sizeof vals32 / sizeof vals32[0];
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            u32 a = vals32[i], b = vals32[j];
            h = mix(h, a + b); h = mix(h, a - b); h = mix(h, a * b);
            h = mix(h, a & b); h = mix(h, a | b); h = mix(h, a ^ b);
            h = mix(h, a << (b & 31)); h = mix(h, a >> (b & 31)); h = mix(h, (u32)((int)a >> (b & 31)));
            h = mix(h, (a << (b & 31)) | (a >> ((32 - b) & 31)));
            if (b) { h = mix(h, a / b); h = mix(h, a % b); }
            if (b && !(a == 0x80000000u && b == 0xFFFFFFFFu)) { h = mix(h, (u32)((int)a / (int)b)); h = mix(h, (u32)((int)a % (int)b)); }
            h = mix(h, (int)a < (int)b); h = mix(h, a < b); h = mix(h, (int)a >= (int)b); h = mix(h, a == b);
            h = mix(h, __builtin_clz(a | 1)); h = mix(h, __builtin_ctz(b | 0x80000000u)); h = mix(h, __builtin_popcount(a));
            h = mix(h, (u32)(signed char)a); h = mix(h, (u32)(short)b);
        }
    return h;
}

EXPORT(int64) u64 int64(void) {
    u64 h = 2;
    int n = sizeof vals64 / sizeof vals64[0];
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            u64 a = vals64[i], b = vals64[j];
            h = mix(h, a + b); h = mix(h, a - b); h = mix(h, a * b);
            h = mix(h, a << (b & 63)); h = mix(h, a >> (b & 63)); h = mix(h, (u64)((i64)a >> (b & 63)));
            if (b) { h = mix(h, a / b); h = mix(h, a % b); }
            if (b && !(a == 0x8000000000000000ull && b == ~0ull)) { h = mix(h, (u64)((i64)a / (i64)b)); h = mix(h, (u64)((i64)a % (i64)b)); }
            h = mix(h, (i64)a < (i64)b); h = mix(h, a > b);
            h = mix(h, __builtin_clzll(a | 1)); h = mix(h, __builtin_ctzll(b | (1ull << 63))); h = mix(h, __builtin_popcountll(a));
            h = mix(h, (u64)(i64)(int)a); h = mix(h, (u64)(u32)a);
        }
    return h;
}

static u64 fbits(double d) { u64 b; __builtin_memcpy(&b, &d, 8); return b; }
static u64 f32bits(float f) { u32 b; __builtin_memcpy(&b, &f, 4); return b; }

EXPORT(floats) u64 floats(void) {
    u64 h = 3;
    int n = sizeof valsf / sizeof valsf[0];
    for (int i = 0; i < n; i++) {
        double a = valsf[i];
        float fa = (float)a;
        h = mix(h, fbits(__builtin_floor(a))); h = mix(h, fbits(__builtin_ceil(a)));
        h = mix(h, fbits(__builtin_trunc(a))); h = mix(h, fbits(__builtin_nearbyint(a)));
        h = mix(h, fbits(__builtin_sqrt(a < 0 ? -a : a)));
        h = mix(h, f32bits(__builtin_floorf(fa))); h = mix(h, f32bits(__builtin_nearbyintf(fa)));
        h = mix(h, f32bits(__builtin_sqrtf(fa < 0 ? -fa : fa)));
        h = mix(h, f32bits(fa)); h = mix(h, fbits((double)fa));
        // saturating conversions (clang emits trunc_sat with -mnontrapping-fptoint)
        h = mix(h, (u32)(int)a); h = mix(h, (u32)a); h = mix(h, (u64)(i64)a); h = mix(h, (u64)a);
        h = mix(h, (u32)(int)fa); h = mix(h, (u64)fa);
        for (int j = 0; j < n; j++) {
            double b = valsf[j];
            float fb = (float)b;
            h = mix(h, fbits(a + b)); h = mix(h, fbits(a * b)); h = mix(h, fbits(a / b));
            h = mix(h, fbits(__builtin_elementwise_minimum(a, b))); h = mix(h, fbits(__builtin_elementwise_maximum(a, b)));
            h = mix(h, fbits(__builtin_copysign(a, b)));
            h = mix(h, f32bits(fa - fb)); h = mix(h, f32bits(fa / fb)); h = mix(h, f32bits(__builtin_elementwise_minimum(fa, fb)));
            h = mix(h, a < b); h = mix(h, a == b); h = mix(h, fa >= fb); h = mix(h, a != b);
        }
    }
    for (int i = 0; i < 12; i++) {
        i64 v = (i64)vals64[i];
        h = mix(h, fbits((double)v)); h = mix(h, fbits((double)(u64)v));
        h = mix(h, f32bits((float)v)); h = mix(h, f32bits((float)(u64)v));
        h = mix(h, f32bits((float)(int)v)); h = mix(h, f32bits((float)(u32)v));
    }
    return h;
}

// Control flow: switch (br_table), recursion, function pointers (call_indirect), nested loops.
static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static u64 op_add(u64 a, u64 b) { return a + b; }
static u64 op_xor(u64 a, u64 b) { return a ^ b; }
static u64 op_mul(u64 a, u64 b) { return a * b | 1; }
static u64 (*volatile ops[3])(u64, u64) = {op_add, op_xor, op_mul};

EXPORT(control) u64 control(void) {
    u64 h = 4;
    for (int i = 0; i < 200; i++) {
        switch (i % 9) {
        case 0: h = mix(h, 11); break;
        case 1: h = mix(h, 12); /* fallthrough */
        case 2: h = mix(h, 13); break;
        case 3: h = mix(h, 14); break;
        case 5: h = mix(h, 15); break;
        case 6: continue;
        case 7: h = mix(h, i * 3); break;
        default: h = mix(h, 99);
        }
        h = ops[i % 3](h, (u64)i);
        for (int j = 0; j < i % 7; j++)
            for (int k = 0; k < 3; k++) {
                if (k == j) break;
                h = mix(h, j * 10 + k);
            }
    }
    h = mix(h, (u64)fib(24));
    return h;
}

// Memory: loads/stores of every width, memcpy/memset (bulk memory), memory.grow.
static unsigned char buf[4096];
extern unsigned char __heap_base;

EXPORT(memory_ops) u64 memory_ops(void) {
    u64 h = 5;
    for (int i = 0; i < 4096; i++) buf[i] = (unsigned char)(i * 37 + 11);
    __builtin_memcpy(buf + 100, buf + 7, 1000);
    __builtin_memmove(buf + 50, buf + 60, 500);
    __builtin_memset(buf + 3000, 0xA5, 777);
    for (int i = 0; i < 4000; i += 3) {
        signed char s8; short s16; int s32; long long s64;
        __builtin_memcpy(&s8, buf + i, 1); __builtin_memcpy(&s16, buf + i, 2);
        __builtin_memcpy(&s32, buf + i, 4); __builtin_memcpy(&s64, buf + i, 8);
        h = mix(h, (u64)(i64)s8); h = mix(h, (u64)(i64)s16); h = mix(h, (u64)(i64)s32); h = mix(h, (u64)s64);
        h = mix(h, (unsigned short)s16); h = mix(h, (u32)s32);
    }
    unsigned long before = __builtin_wasm_memory_size(0);
    unsigned long r = __builtin_wasm_memory_grow(0, 3);
    h = mix(h, r == before); h = mix(h, __builtin_wasm_memory_size(0) - before);
    unsigned char *top = (unsigned char *)(__builtin_wasm_memory_size(0) * 65536 - 16);
    for (int i = 0; i < 16; i++) top[i] = (unsigned char)i;
    long long v; __builtin_memcpy(&v, top + 8, 8);
    h = mix(h, (u64)v);
    return h;
}

EXPORT(trap_div) int trap_div(int a, int b) { return a / b; }
EXPORT(trap_oob) int trap_oob(void) { return *(volatile int *)0xFFFFFFF0; }
EXPORT(spin) int spin(void) { volatile int x = 0; for (;;) x++; }
EXPORT(add3) double add3(int a, long long b, double c) { return a + b + c; }
