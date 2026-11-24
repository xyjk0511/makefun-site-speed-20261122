#pragma once
#include <stdint.h>
#include <stddef.h>

#if defined(__x86_64__)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#endif

// ---------------- Freestanding Linux Syscalls ----------------
#define AT_FDCWD (-100)
#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0100
#define O_TRUNC  01000
#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20

#if defined(__x86_64__)
#define SYS_read       0
#define SYS_write      1
#define SYS_close      3
#define SYS_fstat      5
#define SYS_mmap       9
#define SYS_munmap     11
#define SYS_exit_group 231
#define SYS_openat     257

typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    uint64_t st_atime;
    uint64_t st_atime_nsec;
    uint64_t st_mtime;
    uint64_t st_mtime_nsec;
    uint64_t st_ctime;
    uint64_t st_ctime_nsec;
    int64_t  __unused[3];
} LinuxStat;

static inline long sys_call1(long n, long a1) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1) : "rcx", "r11", "memory");
    return ret;
}
static inline long sys_call2(long n, long a1, long a2) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2) : "rcx", "r11", "memory");
    return ret;
}
static inline long sys_call3(long n, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3) : "rcx", "r11", "memory");
    return ret;
}
static inline long sys_call4(long n, long a1, long a2, long a3, long a4) {
    long ret;
    register long r10 __asm__("r10") = a4;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10) : "rcx", "r11", "memory");
    return ret;
}
static inline long sys_call6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return ret;
}

#elif defined(__aarch64__)
#define SYS_openat     56
#define SYS_close      57
#define SYS_read       63
#define SYS_write      64
#define SYS_fstat      80
#define SYS_exit_group 94
#define SYS_munmap     215
#define SYS_mmap       222

typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t __pad1;
    int64_t  st_size;
    int32_t  st_blksize;
    int32_t  __pad2;
    int64_t  st_blocks;
    uint64_t st_atime;
    uint64_t st_atime_nsec;
    uint64_t st_mtime;
    uint64_t st_mtime_nsec;
    uint64_t st_ctime;
    uint64_t st_ctime_nsec;
    uint32_t __unused[2];
} LinuxStat;

static inline long sys_call1(long n, long a1) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a1;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8) : "memory");
    return x0;
}
static inline long sys_call2(long n, long a1, long a2) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a1;
    register long x1 __asm__("x1") = a2;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory");
    return x0;
}
static inline long sys_call3(long n, long a1, long a2, long a3) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a1;
    register long x1 __asm__("x1") = a2;
    register long x2 __asm__("x2") = a3;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}
static inline long sys_call4(long n, long a1, long a2, long a3, long a4) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a1;
    register long x1 __asm__("x1") = a2;
    register long x2 __asm__("x2") = a3;
    register long x3 __asm__("x3") = a4;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
    return x0;
}
static inline long sys_call6(long n, long a1, long a2, long a3, long a4, long a5, long a6) {
    register long x8 __asm__("x8") = n;
    register long x0 __asm__("x0") = a1;
    register long x1 __asm__("x1") = a2;
    register long x2 __asm__("x2") = a3;
    register long x3 __asm__("x3") = a4;
    register long x4 __asm__("x4") = a5;
    register long x5 __asm__("x5") = a6;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    return x0;
}
#endif

static inline long sys_openat(int dfd, const char *path, int flags, int mode) {
    return sys_call4(SYS_openat, dfd, (long)path, flags, mode);
}
static inline long sys_read(int fd, void *buf, size_t len) {
    return sys_call3(SYS_read, fd, (long)buf, (long)len);
}
static inline long sys_write(int fd, const void *buf, size_t len) {
    return sys_call3(SYS_write, fd, (long)buf, (long)len);
}
static inline long sys_close(int fd) {
    return sys_call1(SYS_close, fd);
}
static inline long sys_fstat(int fd, LinuxStat *st) {
    return sys_call2(SYS_fstat, fd, (long)st);
}
static inline void *sys_mmap(void *addr, size_t len, int prot, int flags, int fd, long off) {
    return (void *)sys_call6(SYS_mmap, (long)addr, (long)len, prot, flags, fd, off);
}
static inline long sys_munmap(void *addr, size_t len) {
    return sys_call2(SYS_munmap, (long)addr, (long)len);
}
static inline __attribute__((noreturn)) void sys_exit_group(int code) {
    sys_call1(SYS_exit_group, code);
    __builtin_unreachable();
}

// ---------------- Runtime AVX2 Detection (Zero-Libgcc) ----------------
static int g_has_avx2 = 0;

static inline void detect_cpu_features(void) {
#if defined(__x86_64__)
    uint32_t eax, ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0), "c"(0));
    if (eax >= 7) {
        uint32_t eax1, ebx1, ecx1, edx1;
        __asm__ volatile("cpuid" : "=a"(eax1), "=b"(ebx1), "=c"(ecx1), "=d"(edx1) : "a"(1), "c"(0));
        // Check OSXSAVE (bit 27) and AVX (bit 28)
        if ((ecx1 & ((1u << 27) | (1u << 28))) == ((1u << 27) | (1u << 28))) {
            uint32_t xcr0_lo, xcr0_hi;
            __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
            if ((xcr0_lo & 6u) == 6u) {
                uint32_t eax7, ebx7, ecx7, edx7;
                __asm__ volatile("cpuid" : "=a"(eax7), "=b"(ebx7), "=c"(ecx7), "=d"(edx7) : "a"(7), "c"(0));
                if (ebx7 & (1u << 5)) g_has_avx2 = 1;
            }
        }
    }
#endif
}

// ---------------- Freestanding Fast Memory Ops ----------------
static inline void fast_memcpy(void *restrict dst, const void *restrict src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (n <= 32) {
        if (n >= 16) {
            uint64_t a, b;
            __builtin_memcpy(&a, s, 8);
            __builtin_memcpy(&b, s + 8, 8);
            __builtin_memcpy(d, &a, 8);
            __builtin_memcpy(d + 8, &b, 8);
            __builtin_memcpy(&a, s + n - 16, 8);
            __builtin_memcpy(&b, s + n - 8, 8);
            __builtin_memcpy(d + n - 16, &a, 8);
            __builtin_memcpy(d + n - 8, &b, 8);
            return;
        }
        if (n >= 8) {
            uint64_t a, b;
            __builtin_memcpy(&a, s, 8);
            __builtin_memcpy(&b, s + n - 8, 8);
            __builtin_memcpy(d, &a, 8);
            __builtin_memcpy(d + n - 8, &b, 8);
            return;
        }
        if (n >= 4) {
            uint32_t a, b;
            __builtin_memcpy(&a, s, 4);
            __builtin_memcpy(&b, s + n - 4, 4);
            __builtin_memcpy(d, &a, 4);
            __builtin_memcpy(d + n - 4, &b, 4);
            return;
        }
        if (n >= 2) {
            uint16_t a;
            __builtin_memcpy(&a, s, 2);
            __builtin_memcpy(d, &a, 2);
            d[n - 1] = s[n - 1];
            return;
        }
        if (n == 1) d[0] = s[0];
        return;
    }
#if defined(__x86_64__)
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
#else
    while (n >= 16) {
        vst1q_u8(d, vld1q_u8(s));
        d += 16; s += 16; n -= 16;
    }
    while (n--) *d++ = *s++;
#endif
}

#if !defined(STANDALONE_CLI) && !defined(DUAL_TU_WORKER)
__attribute__((visibility("hidden")))
void *memcpy(void *dst, const void *src, size_t n) {
    fast_memcpy(dst, src, n);
    return dst;
}
__attribute__((visibility("hidden")))
void *memmove(void *dst, const void *src, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0) return dst;
    if (d < s || d >= s + n) {
        fast_memcpy(d, s, n);
    } else {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dst;
}
__attribute__((visibility("hidden")))
void *memset(void *dst, int v, size_t n) {
    uint8_t *d = (uint8_t *)dst;
    uint8_t b = (uint8_t)v;
#if defined(__x86_64__)
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(b) : "memory");
#else
    for (size_t i = 0; i < n; i++) d[i] = b;
#endif
    return dst;
}
__attribute__((visibility("hidden")))
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *p = (const uint8_t *)a;
    const uint8_t *q = (const uint8_t *)b;
    for (size_t i = 0; i < n; i++) {
        if (p[i] != q[i]) return (int)p[i] - (int)q[i];
    }
    return 0;
}
#endif

static inline int fast_memeq(const char *a, const char *b, uint32_t n) {
    uint32_t i = 0;
    while (i + 8 <= n) {
        uint64_t x, y;
        __builtin_memcpy(&x, a + i, 8);
        __builtin_memcpy(&y, b + i, 8);
        if (x != y) return 0;
        i += 8;
    }
    while (i < n) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return 1;
}

static inline uint32_t fnv1a_hash(const char *p, uint32_t n) {
    uint64_t h = 14695981039346656037ULL;
    uint32_t i = 0;
    while (i + 8 <= n) {
        uint64_t w;
        __builtin_memcpy(&w, p + i, 8);
        h ^= w;
        h *= 1099511628211ULL;
        i += 8;
    }
    while (i < n) {
        h ^= (uint8_t)p[i++];
        h *= 1099511628211ULL;
    }
    h ^= h >> 33;
    return (uint32_t)h;
}

// ---------------- 256 KB L2-Hot Output Streamer ----------------
#define OUT_FLUSH_AT 262144
#define OUT_BUF_CAP  (OUT_FLUSH_AT + 65536)

typedef struct {
    int fd;
    uint32_t pos;
    char buf[OUT_BUF_CAP];
} OutStream;

#ifdef DUAL_TU_WORKER
extern OutStream g_out;
#elif defined(DUAL_TU_DISPATCH)
__attribute__((visibility("hidden"))) OutStream g_out;
#else
static OutStream g_out;
#endif

static inline void out_flush(void) {
    uint32_t off = 0;
    while (off < g_out.pos) {
        long w = sys_write(g_out.fd, g_out.buf + off, g_out.pos - off);
        if (w == -4) continue; // EINTR
        if (w <= 0) break;
        off += (uint32_t)w;
    }
    g_out.pos = 0;
}

static inline void out_maybe_flush(void) {
    if (__builtin_expect(g_out.pos >= OUT_FLUSH_AT, 0)) out_flush();
}

static inline void out_char(char c) {
    g_out.buf[g_out.pos++] = c;
    if (__builtin_expect(g_out.pos >= OUT_FLUSH_AT, 0)) out_flush();
}

static __attribute__((cold, noinline)) void out_raw_slow(const char *p, uint32_t n) {
    if (n > 65536u) {
        out_flush();
        uint32_t off = 0;
        while (off < n) {
            long w = sys_write(g_out.fd, p + off, n - off);
            if (w == -4) continue; // EINTR
            if (w <= 0) break;
            off += (uint32_t)w;
        }
        return;
    }
    if (g_out.pos + n >= OUT_BUF_CAP) out_flush();
    fast_memcpy(g_out.buf + g_out.pos, p, n);
    g_out.pos += n;
    if (g_out.pos >= OUT_FLUSH_AT) out_flush();
}

static inline void out_raw(const char *p, uint32_t n) {
    uint32_t pos = g_out.pos;
    if (__builtin_expect(n < OUT_FLUSH_AT - pos, 1)) {
        fast_memcpy(g_out.buf + pos, p, n);
        g_out.pos = pos + n;
        return;
    }
    out_raw_slow(p, n);
}

static inline void out_lit(const char *s, uint32_t n) {
    out_raw(s, n);
}

#define OUT_LIT(s) out_raw((s), (uint32_t)(sizeof(s) - 1))
