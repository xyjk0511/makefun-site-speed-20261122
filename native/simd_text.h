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

// Growable scratch buffer (backed by fixed/arena buffer)
typedef struct {
    char *p;
    uint32_t n;
    uint32_t cap;
} Buf;

static inline void buf_grow(Buf *b, uint32_t need) {
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
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
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
    // Fast path: advance while bytes match
    while (pa < ea && pb < eb && *pa == *pb) { pa++; pb++; }
    if (pa == ea || pb == eb) return (pa < ea) ? 1 : (pb < eb) ? -1 : 0;
    if (*pa < 0x80 && *pb < 0x80) return (int)*pa - (int)*pb;
    // Back up to UTF-8 codepoint start if inside a multibyte char
    while (pa > (const uint8_t *)a.p && (*pa & 0xC0) == 0x80) pa--;
    while (pb > (const uint8_t *)b.p && (*pb & 0xC0) == 0x80) pb--;
    uint32_t la, lb;
    uint32_t ca = utf8_decode(pa, ea, &la);
    uint32_t cb = utf8_decode(pb, eb, &lb);
    // Convert first differing codepoint to first UTF-16 code unit
    uint32_t ua = ca >= 0x10000 ? (0xD800u + ((ca - 0x10000u) >> 10)) : ca;
    uint32_t ub = cb >= 0x10000 ? (0xD800u + ((cb - 0x10000u) >> 10)) : cb;
    if (ua != ub) return ua < ub ? -1 : 1;
    // Same lead surrogate: compare trail surrogates
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
        if (is_js_ws_cp(cp)) p += cl;
        else break;
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
        if (is_js_ws_cp(cp)) end = q;
        else break;
    }
    return (Str){ (const char *)p, (uint32_t)(end - p) };
}

static inline int has_js_ws(Str s) {
    const uint8_t *p = (const uint8_t *)s.p, *end = p + s.n;
    while (p < end) {
        if (*p < 0x80) {
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
        if (b < 0x80) {
            p++;
            uint8_t c = b;
            if (c >= 'A' && c <= 'Z') c += 32;
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
                if (gap) {
                    if (len >= 63) break; // '-' at pos 63 would be stripped anyway
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
        if (nfkd_is_mark(cp)) continue; // combining mark: completely ignored, no gap!
        const NfkdAlnum *e = nfkd_find_alnum(cp);
        if (!e) {
            if (len > 0) gap = 1;
            continue;
        }
        const char *seq = NFKD_POOL + e->off;
        for (uint32_t k = 0; k < e->len && len < 64; k++) {
            char ch = seq[k];
            if (ch == '-') {
                if (len > 0) gap = 1;
            } else {
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
    if (len == 0) {
        fast_memcpy(out, "untitled", 8);
        return 8;
    }
    return len;
}

// ---------------- SIMD wordsOf (Exact JS /\S+/g) ----------------
static uint32_t words_of_scalar(const char *p, uint32_t n) {
    const uint8_t *s = (const uint8_t *)p, *end = s + n;
    uint32_t count = 0;
    int in_word = 0;
    while (s < end) {
        uint8_t b = *s;
        if (b < 0x80) {
            int ws = (b == ' ' || (b >= 9 && b <= 13));
            if (ws) in_word = 0;
            else if (!in_word) { in_word = 1; count++; }
            s++;
        } else {
            uint32_t cl;
            uint32_t cp = utf8_decode(s, end, &cl);
            if (is_js_ws_cp(cp)) in_word = 0;
            else if (!in_word) { in_word = 1; count++; }
            s += cl;
        }
    }
    return count;
}

#if defined(__x86_64__)
__attribute__((target("avx2,popcnt")))
static uint32_t words_of_avx2(const char *p, uint32_t n) {
    const uint8_t *s = (const uint8_t *)p;
    uint32_t i = 0, count = 0;
    uint32_t prev_ws = 1; // virtual whitespace before byte 0
    const __m256i v_sp  = _mm256_set1_epi8(' ');
    const __m256i v_nl  = _mm256_set1_epi8('\n');
    const __m256i v_tab = _mm256_set1_epi8('\t');
    const __m256i v_cr  = _mm256_set1_epi8('\r');
    const __m256i v_vt  = _mm256_set1_epi8(0x0B);
    const __m256i v_ff  = _mm256_set1_epi8(0x0C);
    while (i + 32 <= n) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(s + i));
        if (__builtin_expect(_mm256_movemask_epi8(v) != 0, 0)) {
            // Non-ASCII byte in this chunk: handle chunk scalar
            const uint8_t *cur = s + i, *end = s + n;
            uint32_t chunk_end = i + 32;
            int in_word = !prev_ws;
            while ((uint32_t)(cur - s) < chunk_end && cur < end) {
                if (*cur < 0x80) {
                    int ws = (*cur == ' ' || (*cur >= 9 && *cur <= 13));
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
            prev_ws = !in_word;
            continue;
        }
        __m256i ws = _mm256_or_si256(
            _mm256_or_si256(_mm256_cmpeq_epi8(v, v_sp), _mm256_cmpeq_epi8(v, v_nl)),
            _mm256_or_si256(
                _mm256_or_si256(_mm256_cmpeq_epi8(v, v_tab), _mm256_cmpeq_epi8(v, v_cr)),
                _mm256_or_si256(_mm256_cmpeq_epi8(v, v_vt), _mm256_cmpeq_epi8(v, v_ff))
            )
        );
        uint32_t ws_mask = (uint32_t)_mm256_movemask_epi8(ws);
        uint32_t non_ws = ~ws_mask;
        uint32_t shifted_ws = (ws_mask << 1) | prev_ws;
        uint32_t starts = non_ws & shifted_ws;
        count += (uint32_t)__builtin_popcount(starts);
        prev_ws = ws_mask >> 31;
        i += 32;
    }
    if (i < n) {
        int in_word = !prev_ws;
        const uint8_t *cur = s + i, *end = s + n;
        while (cur < end) {
            if (*cur < 0x80) {
                int ws = (*cur == ' ' || (*cur >= 9 && *cur <= 13));
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
    }
    return count;
}
#endif

static inline uint32_t words_of(const char *p, uint32_t n) {
#if defined(__x86_64__)
    if (g_has_avx2) return words_of_avx2(p, n);
#endif
    return words_of_scalar(p, n);
}

// ---------------- SIMD Find Newline ----------------
#if defined(__x86_64__)
__attribute__((target("avx2")))
static inline const char *find_nl_avx2(const char *p, const char *end) {
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

static inline const char *find_nl_sse2(const char *p, const char *end) {
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
#endif

static inline const char *find_nl(const char *p, const char *end) {
#if defined(__x86_64__)
    if (g_has_avx2) return find_nl_avx2(p, end);
    return find_nl_sse2(p, end);
#else
    while (p < end && *p != '\n') p++;
    return p;
#endif
}

// ---------------- Escaping Helpers (JSON & Fused HTML+JSON) ----------------
static const char HEX_LC[16] = "0123456789abcdef";

static inline uint32_t emit_json_ctrl(char *dst, uint8_t c) {
    if (c == '"')  { dst[0] = '\\'; dst[1] = '"';  return 2; }
    if (c == '\\') { dst[0] = '\\'; dst[1] = '\\'; return 2; }
    if (c == '\n') { dst[0] = '\\'; dst[1] = 'n';  return 2; }
    if (c == '\r') { dst[0] = '\\'; dst[1] = 'r';  return 2; }
    if (c == '\t') { dst[0] = '\\'; dst[1] = 't';  return 2; }
    if (c == '\b') { dst[0] = '\\'; dst[1] = 'b';  return 2; }
    if (c == '\f') { dst[0] = '\\'; dst[1] = 'f';  return 2; }
    dst[0] = '\\'; dst[1] = 'u'; dst[2] = '0'; dst[3] = '0';
    dst[4] = HEX_LC[c >> 4]; dst[5] = HEX_LC[c & 15];
    return 6;
}

static inline uint32_t emit_html_json_char(char *dst, uint8_t c) {
    if (c == '&')  { fast_memcpy(dst, "&amp;", 5);  return 5; }
    if (c == '<')  { fast_memcpy(dst, "&lt;", 4);   return 4; }
    if (c == '>')  { fast_memcpy(dst, "&gt;", 4);   return 4; }
    if (c == '"')  { fast_memcpy(dst, "&quot;", 6); return 6; }
    if (c == '\'') { fast_memcpy(dst, "&#39;", 5);  return 5; }
    return emit_json_ctrl(dst, c);
}

// Fast JSON-escape into buffer dst (caller guarantees dst has >= n*6 + 32 bytes!)
#if defined(__x86_64__)
__attribute__((target("avx2")))
static char *json_escape_avx2(char *dst, const char *src, uint32_t n) {
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
            dst += 32;
            i += 32;
        } else {
            uint32_t tz = (uint32_t)__builtin_ctz(mask);
            dst += tz;
            i += tz;
            dst += emit_json_ctrl(dst, (uint8_t)src[i++]);
        }
    }
    while (i < n) {
        uint8_t c = (uint8_t)src[i++];
        if (__builtin_expect(c >= 0x20 && c != '"' && c != '\\', 1)) *dst++ = (char)c;
        else dst += emit_json_ctrl(dst, c);
    }
    return dst;
}

__attribute__((target("avx2")))
static char *html_json_escape_avx2(char *dst, const char *src, uint32_t n) {
    // Special >= 0x20: " (0x22), & (0x26), ' (0x27), < (0x3C), > (0x3E), \ (0x5C)
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
        __m256i any = _mm256_or_si256(_mm256_cmpeq_epi8(hit, v_z), v_z);
        // any is 0xFF where hit == 0; invert with cmp against ctrl
        uint32_t hit_mask = ~(uint32_t)_mm256_movemask_epi8(any);
        uint32_t ctrl_mask = (uint32_t)_mm256_movemask_epi8(ctrl);
        uint32_t mask = hit_mask | ctrl_mask;
        if (__builtin_expect(mask == 0, 1)) {
            dst += 32;
            i += 32;
        } else {
            uint32_t tz = (uint32_t)__builtin_ctz(mask);
            dst += tz;
            i += tz;
            dst += emit_html_json_char(dst, (uint8_t)src[i++]);
        }
    }
    while (i < n) {
        uint8_t c = (uint8_t)src[i++];
        if (__builtin_expect(c >= 0x20 && c != '&' && c != '<' && c != '>' && c != '"' && c != '\'' && c != '\\', 1)) {
            *dst++ = (char)c;
        } else {
            dst += emit_html_json_char(dst, c);
        }
    }
    return dst;
}
#endif

static char *json_escape_scalar(char *dst, const char *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)src[i];
        if (__builtin_expect(c >= 0x20 && c != '"' && c != '\\', 1)) *dst++ = (char)c;
        else dst += emit_json_ctrl(dst, c);
    }
    return dst;
}

static char *html_json_escape_scalar(char *dst, const char *src, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint8_t c = (uint8_t)src[i];
        if (__builtin_expect(c >= 0x20 && c != '&' && c != '<' && c != '>' && c != '"' && c != '\'' && c != '\\', 1)) {
            *dst++ = (char)c;
        } else {
            dst += emit_html_json_char(dst, c);
        }
    }
    return dst;
}

static inline char *json_escape_to(char *dst, const char *src, uint32_t n) {
#if defined(__x86_64__)
    if (g_has_avx2 && n >= 32) return json_escape_avx2(dst, src, n);
#endif
    return json_escape_scalar(dst, src, n);
}

static inline char *html_json_escape_to(char *dst, const char *src, uint32_t n) {
#if defined(__x86_64__)
    if (g_has_avx2 && n >= 32) return html_json_escape_avx2(dst, src, n);
#endif
    return html_json_escape_scalar(dst, src, n);
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
