#pragma once
#include "sys.h"
#include "nfkd_table.h"

typedef struct {
    const char *p;
    uint32_t n;
} Str;

// ---------------- Bump Arena ----------------
typedef struct {
    char *base;
    uint32_t pos;
    uint32_t cap;
} Arena;

static inline char *arena_alloc(Arena *a, uint32_t n) {
    uint32_t p = a->pos;
    a->pos = p + n;
    return a->base + p;
}

static inline char *arena_alloc_aligned(Arena *a, uint32_t n, uint32_t align) {
    uint32_t p = (a->pos + align - 1u) & ~(align - 1u);
    a->pos = p + n;
    return a->base + p;
}

typedef struct {
    char *p;
    uint32_t n;
    uint32_t cap;
} Buf;

static __attribute__((cold, noinline)) void buf_grow(Buf *b, uint32_t need) {
    uint32_t ncap = b->cap ? b->cap * 2u : 65536u;
    while (ncap < b->n + need) ncap *= 2u;
    char *np = (char *)sys_mmap(NULL, ncap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (b->n) fast_memcpy(np, b->p, b->n);
    if (b->cap >= 131072u && b->p) sys_munmap(b->p, b->cap);
    b->p = np;
    b->cap = ncap;
}

static inline void buf_ensure(Buf *b, uint32_t need) {
    if (__builtin_expect(b->n + need > b->cap, 0)) buf_grow(b, need);
}

static inline void buf_char(Buf *b, char c) {
    buf_ensure(b, 1);
    b->p[b->n++] = c;
}

static inline void buf_raw(Buf *b, const char *s, uint32_t n) {
    buf_ensure(b, n);
    fast_memcpy(b->p + b->n, s, n);
    b->n += n;
}

#define BUF_LIT(b, s) buf_raw((b), (s), (uint32_t)(sizeof(s) - 1))

// ---------------- 256-byte Fast Classification LUTs ----------------
static const uint8_t JSON_SPEC_LUT[256] = {
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    ['"'] = 1, ['\\'] = 1
};

static const uint8_t HTML_JSON_SPEC_LUT[256] = {
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    ['"'] = 1, ['&'] = 1, ['\''] = 1, ['<'] = 1, ['>'] = 1, ['\\'] = 1
};

// ---------------- UTF-8 Decoding & Exact UTF-16 Code-Unit Compare ----------------
static inline uint32_t utf8_decode(const uint8_t *p, const uint8_t *end, uint32_t *out_len) {
    uint8_t b0 = p[0];
    if (b0 < 0x80) { *out_len = 1; return b0; }
    if ((b0 & 0xE0) == 0xC0 && p + 1 < end) {
        *out_len = 2;
        return ((uint32_t)(b0 & 0x1F) << 6) | (uint32_t)(p[1] & 0x3F);
    }
    if ((b0 & 0xF0) == 0xE0 && p + 2 < end) {
        *out_len = 3;
        return ((uint32_t)(b0 & 0x0F) << 12) | ((uint32_t)(p[1] & 0x3F) << 6) | (uint32_t)(p[2] & 0x3F);
    }
    if ((b0 & 0xF8) == 0xF0 && p + 3 < end) {
        *out_len = 4;
        return ((uint32_t)(b0 & 0x07) << 18) | ((uint32_t)(p[1] & 0x3F) << 12) |
               ((uint32_t)(p[2] & 0x3F) << 6) | (uint32_t)(p[3] & 0x3F);
    }
    *out_len = 1;
    return b0;
}

static inline uint32_t utf8_encode(uint32_t cp, char *out) {
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static inline int cmp_code_units(Str a, Str b) {
    const uint8_t *pa = (const uint8_t *)a.p, *ea = pa + a.n;
    const uint8_t *pb = (const uint8_t *)b.p, *eb = pb + b.n;
    while (pa + 8 <= ea && pb + 8 <= eb) {
        uint64_t wa, wb;
        __builtin_memcpy(&wa, pa, 8);
        __builtin_memcpy(&wb, pb, 8);
        if (wa != wb) {
            uint32_t diff = (uint32_t)__builtin_ctzll(wa ^ wb) >> 3;
            pa += diff; pb += diff;
            goto mismatch;
        }
        pa += 8; pb += 8;
    }
    while (pa < ea && pb < eb && *pa == *pb) { pa++; pb++; }
    if (pa == ea || pb == eb) return (pa < ea) ? 1 : (pb < eb) ? -1 : 0;
mismatch:
    if (__builtin_expect(*pa < 0x80 && *pb < 0x80, 1)) return (int)*pa - (int)*pb;
    while (pa > (const uint8_t *)a.p && (*pa & 0xC0) == 0x80) pa--;
    while (pb > (const uint8_t *)b.p && (*pb & 0xC0) == 0x80) pb--;
    uint32_t la, lb;
    uint32_t ca = utf8_decode(pa, ea, &la);
    uint32_t cb = utf8_decode(pb, eb, &lb);
    uint32_t ua = ca >= 0x10000 ? (0xD800u + ((ca - 0x10000u) >> 10)) : ca;
    uint32_t ub = cb >= 0x10000 ? (0xD800u + ((cb - 0x10000u) >> 10)) : cb;
    if (ua != ub) return ua < ub ? -1 : 1;
    uint32_t ta = 0xDC00u + ((ca - 0x10000u) & 0x3FFu);
    uint32_t tb = 0xDC00u + ((cb - 0x10000u) & 0x3FFu);
    return ta < tb ? -1 : (ta > tb ? 1 : 0);
}

// ---------------- JS Whitespace & Trimming ----------------
static inline int is_js_ws_cp(uint32_t cp) {
    if (cp <= 0x20) return cp == 0x20 || (cp >= 0x09 && cp <= 0x0D);
    if (cp < 0xA0) return 0;
    if (cp == 0xA0 || cp == 0x1680 || cp == 0x2028 || cp == 0x2029 ||
        cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF) return 1;
    return cp >= 0x2000 && cp <= 0x200A;
}

static inline Str trim_js_ws(Str s) {
    const uint8_t *p = (const uint8_t *)s.p;
    const uint8_t *end = p + s.n;
    while (p < end) {
        if (*p < 0x80) {
            if (*p == ' ' || (*p >= 9 && *p <= 13)) { p++; continue; }
            break;
        }
        uint32_t cl;
        uint32_t cp = utf8_decode(p, end, &cl);
        if (is_js_ws_cp(cp)) p += cl; else break;
    }
    while (end > p) {
        if (end[-1] < 0x80) {
            uint8_t c = end[-1];
            if (c == ' ' || (c >= 9 && c <= 13)) { end--; continue; }
            break;
        }
        const uint8_t *q = end - 1;
        while (q > p && (*q & 0xC0) == 0x80) q--;
        uint32_t cl;
        uint32_t cp = utf8_decode(q, end, &cl);
        if (is_js_ws_cp(cp)) end = q; else break;
    }
    return (Str){ (const char *)p, (uint32_t)(end - p) };
}

static inline int has_js_ws(Str s) {
    const uint8_t *p = (const uint8_t *)s.p, *end = p + s.n;
    while (p < end) {
        if (__builtin_expect(*p < 0x80, 1)) {
            if (*p == ' ' || (*p >= 9 && *p <= 13)) return 1;
            p++;
        } else {
            uint32_t cl;
            uint32_t cp = utf8_decode(p, end, &cl);
            if (is_js_ws_cp(cp)) return 1;
            p += cl;
        }
    }
    return 0;
}

// ---------------- Exact Node 22 NFKD Slugify ----------------
static inline int nfkd_is_mark(uint32_t cp) {
    uint32_t lo = 0, hi = NFKD_M_COUNT;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        if (cp < NFKD_M_RANGES[mid].lo) hi = mid;
        else if (cp > NFKD_M_RANGES[mid].hi) lo = mid + 1;
        else return 1;
    }
    return 0;
}

static inline const NfkdAlnum *nfkd_find_alnum(uint32_t cp) {
    uint32_t lo = 0, hi = NFKD_ALNUM_COUNT;
    while (lo < hi) {
        uint32_t mid = (lo + hi) >> 1;
        uint32_t mc = NFKD_ALNUM[mid].cp;
        if (cp < mc) hi = mid;
        else if (cp > mc) lo = mid + 1;
        else return &NFKD_ALNUM[mid];
    }
    return NULL;
}

static inline uint32_t slugify_into(Str in, char *out) {
    const uint8_t *p = (const uint8_t *)in.p;
    const uint8_t *end = p + in.n;
    uint32_t len = 0;
    int gap = 0;
    while (p < end && len < 64) {
        uint8_t b = *p;
        if (__builtin_expect(b < 0x80, 1)) {
            p++;
            uint8_t c = (b >= 'A' && b <= 'Z') ? (uint8_t)(b + 32) : b;
            if ((uint8_t)(c - 'a') < 26u || (uint8_t)(c - '0') < 10u) {
                if (gap) {
                    if (len >= 63) break;
                    out[len++] = '-';
                    gap = 0;
                }
                out[len++] = (char)c;
            } else if (len > 0) {
                gap = 1;
            }
            continue;
        }
        uint32_t cl;
        uint32_t cp = utf8_decode(p, end, &cl);
        p += cl;
        if (nfkd_is_mark(cp)) continue;
        const NfkdAlnum *e = nfkd_find_alnum(cp);
        if (!e) { if (len > 0) gap = 1; continue; }
        const char *seq = NFKD_POOL + e->off;
        for (uint32_t k = 0; k < e->len && len < 64; k++) {
            char ch = seq[k];
            if (ch == '-') { if (len > 0) gap = 1; }
            else {
                if (gap) {
                    if (len >= 63) { len = 64; break; }
                    out[len++] = '-';
                    gap = 0;
                }
                out[len++] = ch;
            }
        }
    }
    while (len > 0 && out[len - 1] == '-') len--;
    if (len == 0) { fast_memcpy(out, "untitled", 8); return 8; }
    return len;
}

// ---------------- Ultra-Fast 64B Unrolled AVX2 / 16B SSE2 wordsOf ----------------
static uint32_t words_of_scalar_tail(const uint8_t *cur, const uint8_t *end, uint32_t prev_ws) {
    uint32_t count = 0;
    int in_word = !prev_ws;
    while (cur < end) {
        uint8_t b = *cur;
        if (__builtin_expect(b < 0x80, 1)) {
            int ws = (b == ' ' || (uint8_t)(b - 9) <= 4u);
            if (ws) in_word = 0;
            else if (!in_word) { in_word = 1; count++; }
            cur++;
        } else {
            uint32_t cl;
            uint32_t cp = utf8_decode(cur, end, &cl);
            if (is_js_ws_cp(cp)) in_word = 0;
            else if (!in_word) { in_word = 1; count++; }
            cur += cl;
        }
    }
    return count;
}

#if defined(__AVX2__)
static inline int is_utf8_ws_at(const uint8_t *p, const uint8_t *end) {
    uint8_t b0 = p[0];
    if (b0 == 0xC2) return (p + 1 < end && p[1] == 0xA0);
    if (p + 2 >= end) return 0;
    uint8_t b1 = p[1], b2 = p[2];
    if (b0 == 0xE2) {
        if (b1 == 0x80) return (b2 >= 0x80 && b2 <= 0x8A) || b2 == 0xA8 || b2 == 0xA9 || b2 == 0xAF;
        return (b1 == 0x81 && b2 == 0x9F);
    }
    if (b0 == 0xE1) return (b1 == 0x9A && b2 == 0x80);
    if (b0 == 0xE3) return (b1 == 0x80 && b2 == 0x80);
    if (b0 == 0xEF) return (b1 == 0xBB && b2 == 0xBF);
    return 0;
}

static uint32_t words_of_avx2(const char *p, uint32_t n) {
    const uint8_t *s = (const uint8_t *)p;
    const uint8_t *end = s + n;
    uint32_t i = 0, count = 0;
    uint64_t prev_ws = 1;
    const __m256i v_sp = _mm256_set1_epi8(' ');
    const __m256i v_9  = _mm256_set1_epi8(9);
    const __m256i v_4  = _mm256_set1_epi8(4);
    const __m256i v_c2 = _mm256_set1_epi8((char)0xC2);
    const __m256i v_f0 = _mm256_set1_epi8((char)0xF0);
    const __m256i v_e0 = _mm256_set1_epi8((char)0xE0);

    // 64B/iter unrolled AVX2! Non-whitespace UTF-8 (em-dash, curly quotes, middot) NEVER falls back!
    while (i + 64 <= n) {
        __m256i v0 = _mm256_loadu_si256((const __m256i *)(s + i));
        __m256i v1 = _mm256_loadu_si256((const __m256i *)(s + i + 32));
        __m256i u0 = _mm256_or_si256(_mm256_cmpeq_epi8(v0, v_c2), _mm256_cmpeq_epi8(_mm256_and_si256(v0, v_f0), v_e0));
        __m256i u1 = _mm256_or_si256(_mm256_cmpeq_epi8(v1, v_c2), _mm256_cmpeq_epi8(_mm256_and_si256(v1, v_f0), v_e0));
        uint64_t um0 = (uint32_t)_mm256_movemask_epi8(u0);
        uint64_t um1 = (uint32_t)_mm256_movemask_epi8(u1);
        uint64_t um = um0 | (um1 << 32);
        if (__builtin_expect(um != 0, 0)) {
            while (um) {
                uint32_t pos = (uint32_t)__builtin_ctzll(um);
                if (__builtin_expect(is_utf8_ws_at(s + i + pos, end), 0)) goto slow64;
                um &= um - 1ULL;
            }
        }
        __m256i d0 = _mm256_sub_epi8(v0, v_9);
        __m256i ws0 = _mm256_or_si256(_mm256_cmpeq_epi8(v0, v_sp), _mm256_cmpeq_epi8(_mm256_min_epu8(d0, v_4), d0));
        __m256i d1 = _mm256_sub_epi8(v1, v_9);
        __m256i ws1 = _mm256_or_si256(_mm256_cmpeq_epi8(v1, v_sp), _mm256_cmpeq_epi8(_mm256_min_epu8(d1, v_4), d1));
        uint64_t m0 = (uint32_t)_mm256_movemask_epi8(ws0);
        uint64_t m1 = (uint32_t)_mm256_movemask_epi8(ws1);
        uint64_t wm = m0 | (m1 << 32);
        uint64_t starts = (~wm) & ((wm << 1) | prev_ws);
        count += (uint32_t)__builtin_popcountll(starts);
        prev_ws = wm >> 63;
        i += 64;
        continue;
    slow64:
        {
            const uint8_t *cur = s + i;
            uint32_t lim = i + 64;
            int in_word = !prev_ws;
            while ((uint32_t)(cur - s) < lim && cur < end) {
                uint8_t b = *cur;
                if (b < 0x80) {
                    int ws = (b == ' ' || (uint8_t)(b - 9) <= 4u);
                    if (ws) in_word = 0;
                    else if (!in_word) { in_word = 1; count++; }
                    cur++;
                } else {
                    uint32_t cl;
                    uint32_t cp = utf8_decode(cur, end, &cl);
                    if (is_js_ws_cp(cp)) in_word = 0;
                    else if (!in_word) { in_word = 1; count++; }
                    cur += cl;
                }
            }
            i = (uint32_t)(cur - s);
            prev_ws = (uint64_t)(!in_word);
        }
    }
    while (i + 32 <= n) {
        __m256i v0 = _mm256_loadu_si256((const __m256i *)(s + i));
        __m256i u0 = _mm256_or_si256(_mm256_cmpeq_epi8(v0, v_c2), _mm256_cmpeq_epi8(_mm256_and_si256(v0, v_f0), v_e0));
        uint32_t um = (uint32_t)_mm256_movemask_epi8(u0);
        if (__builtin_expect(um != 0, 0)) {
            int real_ws = 0;
            while (um) {
                uint32_t pos = (uint32_t)__builtin_ctz(um);
                if (is_utf8_ws_at(s + i + pos, end)) { real_ws = 1; break; }
                um &= um - 1u;
            }
            if (real_ws) break;
        }
        __m256i d0 = _mm256_sub_epi8(v0, v_9);
        __m256i ws0 = _mm256_or_si256(_mm256_cmpeq_epi8(v0, v_sp), _mm256_cmpeq_epi8(_mm256_min_epu8(d0, v_4), d0));
        uint32_t wm = (uint32_t)_mm256_movemask_epi8(ws0);
        uint32_t starts = (~wm) & ((wm << 1) | (uint32_t)prev_ws);
        count += (uint32_t)__builtin_popcount(starts);
        prev_ws = wm >> 31;
        i += 32;
    }
    if (i < n) count += words_of_scalar_tail(s + i, end, (uint32_t)prev_ws);
    return count;
}
#endif

static inline uint32_t swar_popcnt16(uint32_t x) {
    x = x - ((x >> 1) & 0x5555u);
    x = (x & 0x3333u) + ((x >> 2) & 0x3333u);
    return (((x + (x >> 4)) & 0x0F0Fu) * 0x0101u) >> 8;
}

#if defined(__x86_64__) && !defined(__AVX2__)
static uint32_t words_of_sse2(const char *p, uint32_t n) {
    const uint8_t *s = (const uint8_t *)p;
    uint32_t i = 0, count = 0, prev_ws = 1;
    const __m128i v_sp = _mm_set1_epi8(' ');
    const __m128i v_9  = _mm_set1_epi8(9);
    const __m128i v_4  = _mm_set1_epi8(4);
    while (i + 16 <= n) {
        __m128i v = _mm_loadu_si128((const __m128i *)(s + i));
        if (__builtin_expect(_mm_movemask_epi8(v) != 0, 0)) break;
        __m128i d = _mm_sub_epi8(v, v_9);
        __m128i ws = _mm_or_si128(_mm_cmpeq_epi8(v, v_sp), _mm_cmpeq_epi8(_mm_min_epu8(d, v_4), d));
        uint32_t wm = (uint32_t)_mm_movemask_epi8(ws);
        uint32_t starts = (~wm & 0xFFFFu) & ((wm << 1) | prev_ws);
        count += swar_popcnt16(starts);
        prev_ws = (wm >> 15) & 1u;
        i += 16;
    }
    if (i < n) count += words_of_scalar_tail(s + i, s + n, prev_ws);
    return count;
}
#endif

static inline uint32_t words_of(const char *p, uint32_t n) {
#if defined(__AVX2__)
    return words_of_avx2(p, n);
#elif defined(__x86_64__)
    return words_of_sse2(p, n);
#else
    return words_of_scalar_tail((const uint8_t *)p, (const uint8_t *)(p + n), 1);
#endif
}

// ---------------- SIMD Find Newline ----------------
#if defined(__AVX2__)
static inline const char *find_nl(const char *p, const char *end) {
    const __m256i v_nl = _mm256_set1_epi8('\n');
    while (p + 32 <= end) {
        __m256i v = _mm256_loadu_si256((const __m256i *)p);
        uint32_t m = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, v_nl));
        if (m) return p + __builtin_ctz(m);
        p += 32;
    }
    while (p < end && *p != '\n') p++;
    return p;
}
#elif defined(__x86_64__)
static inline const char *find_nl(const char *p, const char *end) {
    const __m128i v_nl = _mm_set1_epi8('\n');
    while (p + 16 <= end) {
        __m128i v = _mm_loadu_si128((const __m128i *)p);
        uint32_t m = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(v, v_nl));
        if (m) return p + __builtin_ctz(m);
        p += 16;
    }
    while (p < end && *p != '\n') p++;
    return p;
}
#else
static inline const char *find_nl(const char *p, const char *end) {
    while (p < end && *p != '\n') p++;
    return p;
}
#endif

// ---------------- Branchless 2-Instruction Escaping Tables ----------------
typedef struct {
    char s[7];
    uint8_t len;
} EscEntry;

#ifdef DUAL_TU_WORKER
extern const EscEntry HJ_ESC_TBL[128];
extern const EscEntry JSON_ESC_TBL[128];
#else
#ifdef DUAL_TU_DISPATCH
__attribute__((visibility("hidden")))
#else
static
#endif
const EscEntry HJ_ESC_TBL[128] = {
    [0]  = {"\\u0000", 6}, [1]  = {"\\u0001", 6}, [2]  = {"\\u0002", 6}, [3]  = {"\\u0003", 6},
    [4]  = {"\\u0004", 6}, [5]  = {"\\u0005", 6}, [6]  = {"\\u0006", 6}, [7]  = {"\\u0007", 6},
    [8]  = {"\\b",     2}, [9]  = {"\\t",     2}, [10] = {"\\n",     2}, [11] = {"\\u000b", 6},
    [12] = {"\\f",     2}, [13] = {"\\r",     2}, [14] = {"\\u000e", 6}, [15] = {"\\u000f", 6},
    [16] = {"\\u0010", 6}, [17] = {"\\u0011", 6}, [18] = {"\\u0012", 6}, [19] = {"\\u0013", 6},
    [20] = {"\\u0014", 6}, [21] = {"\\u0015", 6}, [22] = {"\\u0016", 6}, [23] = {"\\u0017", 6},
    [24] = {"\\u0018", 6}, [25] = {"\\u0019", 6}, [26] = {"\\u001a", 6}, [27] = {"\\u001b", 6},
    [28] = {"\\u001c", 6}, [29] = {"\\u001d", 6}, [30] = {"\\u001e", 6}, [31] = {"\\u001f", 6},
    ['"']  = {"&quot;", 6},
    ['&']  = {"&amp;",  5},
    ['\''] = {"&#39;",  5},
    ['<']  = {"&lt;",   4},
    ['>']  = {"&gt;",   4},
    ['\\'] = {"\\\\",   2},
};

#ifdef DUAL_TU_DISPATCH
__attribute__((visibility("hidden")))
#else
static
#endif
const EscEntry JSON_ESC_TBL[128] = {
    [0]  = {"\\u0000", 6}, [1]  = {"\\u0001", 6}, [2]  = {"\\u0002", 6}, [3]  = {"\\u0003", 6},
    [4]  = {"\\u0004", 6}, [5]  = {"\\u0005", 6}, [6]  = {"\\u0006", 6}, [7]  = {"\\u0007", 6},
    [8]  = {"\\b",     2}, [9]  = {"\\t",     2}, [10] = {"\\n",     2}, [11] = {"\\u000b", 6},
    [12] = {"\\f",     2}, [13] = {"\\r",     2}, [14] = {"\\u000e", 6}, [15] = {"\\u000f", 6},
    [16] = {"\\u0010", 6}, [17] = {"\\u0011", 6}, [18] = {"\\u0012", 6}, [19] = {"\\u0013", 6},
    [20] = {"\\u0014", 6}, [21] = {"\\u0015", 6}, [22] = {"\\u0016", 6}, [23] = {"\\u0017", 6},
    [24] = {"\\u0018", 6}, [25] = {"\\u0019", 6}, [26] = {"\\u001a", 6}, [27] = {"\\u001b", 6},
    [28] = {"\\u001c", 6}, [29] = {"\\u001d", 6}, [30] = {"\\u001e", 6}, [31] = {"\\u001f", 6},
    ['"']  = {"\\\"",   2},
    ['\\'] = {"\\\\",   2},
};
#endif

// Branchless 2-instruction escape (dst MUST have >= 8 bytes slack!)
static inline uint32_t emit_json_ctrl(char *dst, uint8_t c) {
    uint64_t w;
    __builtin_memcpy(&w, &JSON_ESC_TBL[c & 127u], 8);
    __builtin_memcpy(dst, &w, 8);
    return (uint32_t)(w >> 56);
}

static inline uint32_t emit_html_json_char(char *dst, uint8_t c) {
    uint64_t w;
    __builtin_memcpy(&w, &HJ_ESC_TBL[c & 127u], 8);
    __builtin_memcpy(dst, &w, 8);
    return (uint32_t)(w >> 56);
}

#if defined(__AVX2__)
// Ultra-fast 1..31B copy for padded src (>=32B slack) & padded dst (>=32B slack): ZERO size-branch tree!
static inline void copy_short_padded(char *dst, const char *src, uint32_t n) {
    if (n <= 16u) {
        _mm_storeu_si128((__m128i *)dst, _mm_loadu_si128((const __m128i *)src));
    } else {
        _mm256_storeu_si256((__m256i *)dst, _mm256_loadu_si256((const __m256i *)src));
    }
}
#endif

#if defined(__AVX2__)
static __attribute__((noinline)) char *json_escape_avx2(char *dst, const char *src, uint32_t n) {
    const __m256i v_qt = _mm256_set1_epi8('"');
    const __m256i v_bs = _mm256_set1_epi8('\\');
    const __m256i v_hi = _mm256_set1_epi8((char)0xE0);
    const __m256i v_z  = _mm256_setzero_si256();
    uint32_t i = 0;
    while (i + 32 <= n) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(src + i));
        _mm256_storeu_si256((__m256i *)dst, v);
        __m256i m1 = _mm256_or_si256(_mm256_cmpeq_epi8(v, v_qt), _mm256_cmpeq_epi8(v, v_bs));
        __m256i m2 = _mm256_cmpeq_epi8(_mm256_and_si256(v, v_hi), v_z);
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(_mm256_or_si256(m1, m2));
        if (__builtin_expect(mask == 0, 1)) {
            dst += 32; i += 32;
            continue;
        }
        uint32_t tz = (uint32_t)__builtin_ctz(mask);
        dst += tz;
        dst += emit_json_ctrl(dst, (uint8_t)src[i + tz]);
        uint32_t cur = tz + 1;
        mask &= mask - 1u;
        while (mask) {
            uint32_t nz = (uint32_t)__builtin_ctz(mask);
            uint32_t run = nz - cur;
            if (run) { fast_memcpy(dst, src + i + cur, run); dst += run; }
            dst += emit_json_ctrl(dst, (uint8_t)src[i + nz]);
            cur = nz + 1;
            mask &= mask - 1u;
        }
        if (cur < 32u) { fast_memcpy(dst, src + i + cur, 32u - cur); dst += 32u - cur; }
        i += 32;
    }
    while (i < n) {
        uint8_t c = (uint8_t)src[i++];
        if (__builtin_expect(!JSON_SPEC_LUT[c], 1)) *dst++ = (char)c;
        else dst += emit_json_ctrl(dst, c);
    }
    return dst;
}

static __attribute__((noinline)) char *html_json_escape_avx2(char *dst, const char *src, uint32_t n) {
    const __m256i lo_lut = _mm256_setr_epi8(
        0, 0, 1, 0, 0, 0, 2, 4, 0, 0, 0, 0, 8 | 32, 0, 16, 0,
        0, 0, 1, 0, 0, 0, 2, 4, 0, 0, 0, 0, 8 | 32, 0, 16, 0
    );
    const __m256i hi_lut = _mm256_setr_epi8(
        0, 0, 1 | 2 | 4, 8 | 16, 0, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 1 | 2 | 4, 8 | 16, 0, 32, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    );
    const __m256i v_0f = _mm256_set1_epi8(0x0F);
    const __m256i v_e0 = _mm256_set1_epi8((char)0xE0);
    const __m256i v_z  = _mm256_setzero_si256();
    uint32_t i = 0;
    while (i + 32 <= n) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(src + i));
        _mm256_storeu_si256((__m256i *)dst, v);
        __m256i lo = _mm256_and_si256(v, v_0f);
        __m256i hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), v_0f);
        __m256i hit = _mm256_and_si256(_mm256_shuffle_epi8(lo_lut, lo), _mm256_shuffle_epi8(hi_lut, hi));
        __m256i ctrl = _mm256_cmpeq_epi8(_mm256_and_si256(v, v_e0), v_z);
        uint32_t hit_mask = ~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(hit, v_z));
        uint32_t ctrl_mask = (uint32_t)_mm256_movemask_epi8(ctrl);
        uint32_t mask = hit_mask | ctrl_mask;
        if (__builtin_expect(mask == 0, 1)) {
            dst += 32; i += 32;
            continue;
        }
        uint32_t tz = (uint32_t)__builtin_ctz(mask);
        dst += tz;
        dst += emit_html_json_char(dst, (uint8_t)src[i + tz]);
        uint32_t cur = tz + 1;
        mask &= mask - 1u;
        while (mask) {
            uint32_t nz = (uint32_t)__builtin_ctz(mask);
            uint32_t run = nz - cur;
            if (run) { fast_memcpy(dst, src + i + cur, run); dst += run; }
            dst += emit_html_json_char(dst, (uint8_t)src[i + nz]);
            cur = nz + 1;
            mask &= mask - 1u;
        }
        if (cur < 32u) { fast_memcpy(dst, src + i + cur, 32u - cur); dst += 32u - cur; }
        i += 32;
    }
    while (i < n) {
        uint8_t c = (uint8_t)src[i++];
        if (__builtin_expect(!HTML_JSON_SPEC_LUT[c], 1)) *dst++ = (char)c;
        else dst += emit_html_json_char(dst, c);
    }
    return dst;
}
#endif

static inline char *json_escape_scalar(char *dst, const char *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)src[i];
        if (__builtin_expect(!JSON_SPEC_LUT[c], 1)) *dst++ = (char)c;
        else dst += emit_json_ctrl(dst, c);
    }
    return dst;
}

static inline char *html_json_escape_scalar(char *dst, const char *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)src[i];
        if (__builtin_expect(!HTML_JSON_SPEC_LUT[c], 1)) *dst++ = (char)c;
        else dst += emit_html_json_char(dst, c);
    }
    return dst;
}

static inline char *json_escape_to(char *dst, const char *src, uint32_t n) {
#if defined(__AVX2__)
    if (n >= 32) return json_escape_avx2(dst, src, n);
#endif
    return json_escape_scalar(dst, src, n);
}

static inline char *html_json_escape_to(char *dst, const char *src, uint32_t n) {
#if defined(__AVX2__)
    if (n >= 32) return html_json_escape_avx2(dst, src, n);
#endif
    return html_json_escape_scalar(dst, src, n);
}

// Pre-escape into arena (returns original slice if ZERO special chars -> zero-copy!)
static inline Str html_json_escape_arena(Str s, Arena *a) {
    if (s.n == 0) return s;
    uint32_t save_pos = a->pos;
    char *dst = arena_alloc(a, s.n * 6u + 32u);
    char *end = html_json_escape_to(dst, s.p, s.n);
    uint32_t out_n = (uint32_t)(end - dst);
    if (out_n == s.n) {
        a->pos = save_pos; // zero special chars: reclaim arena and zero-copy!
        return s;
    }
    a->pos = save_pos + out_n;
    return (Str){ dst, out_n };
}

static inline void buf_json_escaped(Buf *b, const char *src, uint32_t n) {
    buf_ensure(b, n * 6u + 32u);
    char *end = json_escape_to(b->p + b->n, src, n);
    b->n = (uint32_t)(end - b->p);
}

static inline void buf_html_json_escaped(Buf *b, const char *src, uint32_t n) {
    buf_ensure(b, n * 6u + 32u);
    char *end = html_json_escape_to(b->p + b->n, src, n);
    b->n = (uint32_t)(end - b->p);
}

static inline void out_json_escaped(const char *src, uint32_t n) {
    uint32_t off = 0;
    while (off < n) {
        uint32_t chunk = n - off;
        if (chunk > 8192u) chunk = 8192u;
        if (__builtin_expect(g_out.pos + chunk * 6u + 64u >= OUT_BUF_CAP, 0)) out_flush();
        char *end = json_escape_to(g_out.buf + g_out.pos, src + off, chunk);
        g_out.pos = (uint32_t)(end - g_out.buf);
        off += chunk;
    }
    out_maybe_flush();
}

static inline void out_html_json_escaped(const char *src, uint32_t n) {
    uint32_t off = 0;
    while (off < n) {
        uint32_t chunk = n - off;
        if (chunk > 8192u) chunk = 8192u;
        if (__builtin_expect(g_out.pos + chunk * 6u + 64u >= OUT_BUF_CAP, 0)) out_flush();
        char *end = html_json_escape_to(g_out.buf + g_out.pos, src + off, chunk);
        g_out.pos = (uint32_t)(end - g_out.buf);
        off += chunk;
    }
    out_maybe_flush();
}

static inline uint32_t u32_to_str(uint32_t v, char *dst) {
    char tmp[12];
    uint32_t k = 0;
    do { tmp[k++] = (char)('0' + (v % 10u)); v /= 10u; } while (v);
    for (uint32_t i = 0; i < k; i++) dst[i] = tmp[k - 1 - i];
    return k;
}

static inline void out_u32(uint32_t v) {
    char tmp[12];
    uint32_t k = u32_to_str(v, tmp);
    out_raw(tmp, k);
}
