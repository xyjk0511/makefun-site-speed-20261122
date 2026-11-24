#pragma once
#include "markdown.h"

// Direct Raw-JSON Markdown Renderer (v5):
// Operates DIRECTLY on mmap'd raw JSON strings (when no \r, \u, or \/) — ZERO unescape copy!
// - Zero-copy words_of directly on raw JSON (AVX2 32B/iter)
// - Zero-cycle \n passthrough inside 32B AVX2 speculative store
// - No ctrl_mask (<0x20) needed (JSON strings contain no raw control bytes!)
// - Zero-copy single-pass AVX2 fenced code blocks directly from raw JSON

// 0 = plain byte, 1 = HTML escape (&, <, >, '), 2 = md/json construct (\, `, *, [)
static const uint8_t RAW_INLINE_KIND_LUT[256] = {
    ['&'] = 1, ['\''] = 1, ['<'] = 1, ['>'] = 1,
    ['\\'] = 2, ['`'] = 2, ['*'] = 2, ['['] = 2
};

// ---------------- Direct Raw-JSON AVX2 words_of ----------------
static uint32_t words_of_raw_scalar(const uint8_t *cur, const uint8_t *end, uint32_t prev_ws) {
    uint32_t count = 0;
    int in_word = !prev_ws;
    while (cur < end) {
        uint8_t b = *cur;
        if (__builtin_expect(b == '\\', 0)) {
            if (cur + 1 < end) {
                uint8_t e = cur[1];
                if (e == 'n' || e == 't' || e == 'f' || e == 'r') {
                    in_word = 0;
                } else if (!in_word) {
                    in_word = 1; count++;
                }
                cur += 2;
                continue;
            }
        }
        if (__builtin_expect(b < 0x80, 1)) {
            if (b == ' ') in_word = 0;
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
static __attribute__((noinline)) uint32_t words_of_raw_avx2(const char *p, uint32_t n) {
    const uint8_t *s = (const uint8_t *)p;
    const uint8_t *end = s + n;
    uint32_t i = 0, count = 0;
    uint64_t prev_ws = 1;
    const __m256i v_sp = _mm256_set1_epi8(' ');
    const __m256i v_bs = _mm256_set1_epi8('\\');
    const __m256i v_n  = _mm256_set1_epi8('n');
    const __m256i v_t  = _mm256_set1_epi8('t');
    const __m256i v_f  = _mm256_set1_epi8('f');
    const __m256i v_c2 = _mm256_set1_epi8((char)0xC2);
    const __m256i v_f0 = _mm256_set1_epi8((char)0xF0);
    const __m256i v_e0 = _mm256_set1_epi8((char)0xE0);

    // 64B/iter unrolled AVX2! Does NOT fall back on \\* or non-whitespace UTF-8!
    while (i + 64 <= n) {
        __m256i v0 = _mm256_loadu_si256((const __m256i *)(s + i));
        __m256i v1 = _mm256_loadu_si256((const __m256i *)(s + i + 32));
        __m256i u0 = _mm256_or_si256(_mm256_cmpeq_epi8(v0, v_c2), _mm256_cmpeq_epi8(_mm256_and_si256(v0, v_f0), v_e0));
        __m256i u1 = _mm256_or_si256(_mm256_cmpeq_epi8(v1, v_c2), _mm256_cmpeq_epi8(_mm256_and_si256(v1, v_f0), v_e0));
        uint64_t um = (uint32_t)_mm256_movemask_epi8(u0) | ((uint64_t)(uint32_t)_mm256_movemask_epi8(u1) << 32);
        if (__builtin_expect(um != 0, 0)) {
            while (um) {
                uint32_t pos = (uint32_t)__builtin_ctzll(um);
                if (__builtin_expect(is_utf8_ws_at(s + i + pos, end), 0)) goto slow_raw64;
                um &= um - 1ULL;
            }
        }
        uint64_t m_bs = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v0, v_bs)) |
                        ((uint64_t)(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v1, v_bs)) << 32);
        if (__builtin_expect((m_bs >> 63) != 0, 0)) goto slow_raw64;
        uint64_t wm = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v0, v_sp)) |
                      ((uint64_t)(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v1, v_sp)) << 32);
        if (m_bs) {
            __m256i e0 = _mm256_or_si256(_mm256_cmpeq_epi8(v0, v_n), _mm256_or_si256(_mm256_cmpeq_epi8(v0, v_t), _mm256_cmpeq_epi8(v0, v_f)));
            __m256i e1 = _mm256_or_si256(_mm256_cmpeq_epi8(v1, v_n), _mm256_or_si256(_mm256_cmpeq_epi8(v1, v_t), _mm256_cmpeq_epi8(v1, v_f)));
            uint64_t m_ntf = (uint32_t)_mm256_movemask_epi8(e0) | ((uint64_t)(uint32_t)_mm256_movemask_epi8(e1) << 32);
            uint64_t m_wse = m_bs & (m_ntf >> 1);
            // Only fall back if consecutive '\\' is IMMEDIATELY before n/t/f (literally \\n, \\t, \\f)!
            if (__builtin_expect(((m_bs & (m_bs << 1)) & m_wse) != 0, 0)) goto slow_raw64;
            wm |= m_wse | (m_wse << 1);
        }
        uint64_t starts = (~wm) & ((wm << 1) | prev_ws);
        count += (uint32_t)__builtin_popcountll(starts);
        prev_ws = wm >> 63;
        i += 64;
        continue;
    slow_raw64:
        {
            const uint8_t *cur = s + i;
            uint32_t lim = i + 64;
            int in_word = !prev_ws;
            while ((uint32_t)(cur - s) < lim && cur < end) {
                uint8_t b = *cur;
                if (b == '\\' && cur + 1 < end) {
                    uint8_t e = cur[1];
                    if (e == 'n' || e == 't' || e == 'f' || e == 'r') in_word = 0;
                    else if (!in_word) { in_word = 1; count++; }
                    cur += 2;
                } else if (b < 0x80) {
                    if (b == ' ') in_word = 0;
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
    if (i < n) count += words_of_raw_scalar(s + i, end, (uint32_t)prev_ws);
    return count;
}
#endif

static inline uint32_t words_of_raw(const char *p, uint32_t n) {
#if defined(__AVX2__)
    return words_of_raw_avx2(p, n);
#else
    return words_of_raw_scalar((const uint8_t *)p, (const uint8_t *)(p + n), 1);
#endif
}

// ---------------- Raw-JSON Line & Whitespace Helpers (Single-vpmovmskb!) ----------------
static inline int raw_is_escaped(const char *start, const char *pos) {
    uint32_t cnt = 0;
    while (pos > start && pos[-1] == '\\') { pos--; cnt++; }
    return (int)(cnt & 1u);
}

static inline const char *find_raw_nl(const char *p, const char *end) {
    const char *orig = p;
#if defined(__AVX2__)
    const __m256i v_bs = _mm256_set1_epi8('\\');
    const __m256i v_n  = _mm256_set1_epi8('n');
    while (p + 33 <= end) {
        __m256i v0 = _mm256_loadu_si256((const __m256i *)p);
        __m256i v1 = _mm256_loadu_si256((const __m256i *)(p + 1));
        uint32_t m = (uint32_t)_mm256_movemask_epi8(
            _mm256_and_si256(_mm256_cmpeq_epi8(v0, v_bs), _mm256_cmpeq_epi8(v1, v_n))
        );
        if (__builtin_expect(m == 0, 1)) { p += 32; continue; }
        while (m) {
            uint32_t tz = (uint32_t)__builtin_ctz(m);
            const char *pos = p + tz;
            if (__builtin_expect(pos == orig || pos[-1] != '\\' || !raw_is_escaped(orig, pos), 1)) {
                return pos;
            }
            m &= m - 1u;
        }
        p += 32;
    }
    if (__builtin_expect(p > orig && p[-1] == '\\' && raw_is_escaped(orig, p), 0)) p++;
#endif
    while (p < end) {
        if (*p == '\\') {
            if (p + 1 < end && p[1] == 'n') return p;
            p += 2;
        } else {
            p++;
        }
    }
    return end;
}

static inline int md_raw_is_blank(const char *p, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        if (p[i] == ' ') { i++; continue; }
        if (p[i] == '\\' && i + 1 < n && p[i + 1] == 't') { i += 2; continue; }
        return 0;
    }
    return 1;
}

static inline Str md_raw_trim_st(const char *p, uint32_t n) {
    uint32_t a = 0, b = n;
    while (a < b) {
        if (p[a] == ' ') a++;
        else if (p[a] == '\\' && a + 1 < b && p[a + 1] == 't') a += 2;
        else break;
    }
    while (b > a) {
        if (p[b - 1] == ' ') b--;
        else if (b >= a + 2 && p[b - 1] == 't' && p[b - 2] == '\\' && !raw_is_escaped(p + a, p + b - 2)) b -= 2;
        else break;
    }
    return (Str){ p + a, b - a };
}

static inline int md_raw_is_clean_ends(const char *p, uint32_t n) {
    if (n == 0) return 0;
    if (p[0] == ' ' || (p[0] == '\\' && n >= 2 && p[1] == 't')) return 0;
    if (p[n - 1] == ' ' || (p[n - 1] == 't' && n >= 2 && p[n - 2] == '\\')) return 0;
    return 1;
}

static inline int md_raw_parse_fence_open(const char *p, uint32_t n, Str *out_lang) {
    if (n < 3 || p[0] != '`' || p[1] != '`' || p[2] != '`') return 0;
    uint32_t i = 3;
    while (i < n) {
        uint8_t c = (uint8_t)p[i];
        if ((uint8_t)(c - 'a') < 26u || (uint8_t)(c - 'A') < 26u || (uint8_t)(c - '0') < 10u ||
            c == '_' || c == '+' || c == '-') i++;
        else break;
    }
    uint32_t lang_end = i;
    while (i < n) {
        if (p[i] == ' ') i++;
        else if (p[i] == '\\' && i + 1 < n && p[i + 1] == 't') i += 2;
        else return 0;
    }
    *out_lang = (Str){ p + 3, lang_end - 3 };
    return 1;
}

static inline int md_raw_is_fence_close(const char *p, uint32_t n) {
    if (n < 3 || p[0] != '`' || p[1] != '`' || p[2] != '`') return 0;
    uint32_t i = 3;
    while (i < n) {
        if (p[i] == ' ') i++;
        else if (p[i] == '\\' && i + 1 < n && p[i + 1] == 't') i += 2;
        else return 0;
    }
    return 1;
}

static inline int md_raw_parse_heading(const char *p, uint32_t n, uint8_t *out_depth, Str *out_text) {
    if (n < 3 || p[0] != '#') return 0;
    uint32_t d = 1;
    while (d < 6 && d < n && p[d] == '#') d++;
    if (d >= n) return 0;
    if (p[d] != ' ' && !(p[d] == '\\' && d + 1 < n && p[d + 1] == 't')) return 0;
    Str tr = md_raw_trim_st(p + d, n - d);
    if (tr.n == 0) return 0;
    *out_depth = (uint8_t)d;
    *out_text = tr;
    return 1;
}

static inline int md_raw_is_hr(const char *p, uint32_t n) {
    if (n < 3) return 0;
    char c = p[0];
    if ((c != '-' && c != '*') || p[1] != c || p[2] != c) return 0;
    uint32_t i = 3;
    while (i < n) {
        if (p[i] == ' ') i++;
        else if (p[i] == '\\' && i + 1 < n && p[i + 1] == 't') i += 2;
        else return 0;
    }
    return 1;
}

static inline int md_raw_ends_paragraph(const char *p, uint32_t n) {
    if (n == 0) return 1;
    char c0 = p[0];
    if (__builtin_expect((uint8_t)c0 > ' ' && c0 != '\\' && c0 != '`' && c0 != '#' && c0 != '-' && c0 != '*', 1)) return 0;
    if (c0 == ' ' || (c0 == '\\' && n >= 2 && p[1] == 't')) return md_raw_is_blank(p, n);
    if (c0 == '`') { Str tmp; return md_raw_parse_fence_open(p, n, &tmp); }
    if (c0 == '#') { uint8_t d; Str t; return md_raw_parse_heading(p, n, &d, &t); }
    if (c0 == '-' || c0 == '*') return md_raw_is_hr(p, n);
    return 0;
}

static inline int md_raw_is_continued(const char *p, uint32_t n) {
    if (n < 3 || p[0] != ' ' || p[1] != ' ') return 0;
    uint32_t i = 2;
    while (i < n && p[i] == ' ') i++;
    if (i >= n) return 0;
    uint8_t b = (uint8_t)p[i];
    if (b == '\\' && i + 1 < n) {
        char e = p[i + 1];
        return !(e == 't' || e == 'n' || e == 'f' || e == 'r');
    }
    if (__builtin_expect(b < 0x80, 1)) return b != ' ';
    uint32_t cl;
    uint32_t cp = utf8_decode((const uint8_t *)(p + i), (const uint8_t *)(p + n), &cl);
    return !is_js_ws_cp(cp);
}

// ---------------- Raw-JSON HTML Escaper (For Fenced Code & Inline Code!) ----------------
static inline char *html_from_raw_scalar(char *dst, const char *s, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        uint8_t c = (uint8_t)s[i];
        if (__builtin_expect(c != '\\' && !HTML_JSON_SPEC_LUT[c], 1)) {
            *dst++ = (char)c; i++;
            continue;
        }
        if (c == '\\' && i + 1 < n) {
            char e = s[i + 1];
            if (e == '"') { DST_LIT(dst, "&quot;"); }
            else { dst[0] = '\\'; dst[1] = e; dst += 2; }
            i += 2;
            continue;
        }
        dst += emit_html_json_char(dst, c);
        i++;
    }
    return dst;
}

#if defined(__AVX2__)
static __attribute__((noinline)) char *html_from_raw_avx2(char *dst, const char *s, uint32_t n) {
    // Only &, <, >, ', \ need attention (\n, \t, \b, \f, \\ are ALREADY perfect JSON!)
    const __m256i lo_lut = _mm256_setr_epi8(
        0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 2 | 4, 0, 2, 0,
        0, 0, 0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 2 | 4, 0, 2, 0
    );
    const __m256i hi_lut = _mm256_setr_epi8(
        0, 0, 1, 2, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 1, 2, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    );
    const __m256i v_0f = _mm256_set1_epi8(0x0F);
    const __m256i v_z  = _mm256_setzero_si256();
    const __m256i v_bs = _mm256_set1_epi8('\\');
    const __m256i v_n  = _mm256_set1_epi8('n');
    uint32_t i = 0;
    while (i + 32 <= n) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(s + i));
        _mm256_storeu_si256((__m256i *)dst, v);
        __m256i lo = _mm256_and_si256(v, v_0f);
        __m256i hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), v_0f);
        __m256i hit = _mm256_and_si256(_mm256_shuffle_epi8(lo_lut, lo), _mm256_shuffle_epi8(hi_lut, hi));
        uint32_t mask = ~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(hit, v_z));
        if (mask) {
            uint32_t m_bs = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, v_bs));
            if (m_bs && (m_bs & (m_bs << 1)) == 0) {
                uint32_t m_n = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, v_n));
                mask &= ~((m_bs & (m_n >> 1)) & 0x7FFFFFFFu);
            }
        }
        if (__builtin_expect(mask == 0, 1)) { dst += 32; i += 32; continue; }
        uint32_t cur = 0, base = i;
        while (mask) {
            uint32_t tz = (uint32_t)__builtin_ctz(mask);
            uint32_t run = tz - cur;
            if (cur == 0) dst += run;
            else if (run) { fast_memcpy(dst, s + base + cur, run); dst += run; }
            uint32_t pos = base + tz;
            uint8_t c = (uint8_t)s[pos];
            if (c == '\\') {
                if (pos + 1 < n) {
                    char e = s[pos + 1];
                    if (e == '"') { DST_LIT(dst, "&quot;"); }
                    else { dst[0] = '\\'; dst[1] = e; dst += 2; }
                    if (tz + 2 < 32u) { cur = tz + 2; mask &= (~0u << cur); continue; }
                    i = pos + 2; goto next_hr;
                }
            }
            dst += emit_html_json_char(dst, c);
            cur = tz + 1;
            mask &= mask - 1u;
        }
        if (cur < 32u) { fast_memcpy(dst, s + base + cur, 32u - cur); dst += 32u - cur; }
        i = base + 32;
    next_hr:;
    }
    if (i < n) dst = html_from_raw_scalar(dst, s + i, n - i);
    return dst;
}
#endif

static inline char *html_from_raw(char *dst, const char *s, uint32_t n) {
#if defined(__AVX2__)
    if (n >= 32) return html_from_raw_avx2(dst, s, n);
#endif
    return html_from_raw_scalar(dst, s, n);
}

// ---------------- Raw-JSON Dirty-Tracked Delimiter Scanners ----------------
static inline int32_t md_raw_scan_backtick(const char *s, uint32_t n, uint32_t from, uint8_t *out_dirty) {
    uint8_t dirty = 0;
    for (uint32_t j = from; j < n; j++) {
        uint8_t c = (uint8_t)s[j];
        if (c == '`') { *out_dirty = dirty; return (int32_t)j; }
        if (c == '\\' && j + 1 < n) {
            if (s[j + 1] == '"') dirty = 1;
            j++;
            continue;
        }
        dirty |= (RAW_INLINE_KIND_LUT[c] == 1);
    }
    return -1;
}

static inline int32_t md_raw_scan_rparen(const char *s, uint32_t n, uint32_t from, uint8_t *out_has_ws, uint8_t *out_dirty, uint8_t *out_has_bs) {
    uint8_t ws = 0, dirty = 0, bs = 0;
    for (uint32_t j = from; j < n; j++) {
        uint8_t c = (uint8_t)s[j];
        if (c == ')') { *out_has_ws = ws; *out_dirty = dirty; *out_has_bs = bs; return (int32_t)j; }
        if (c == '\\' && j + 1 < n) {
            bs = 1; dirty = 1;
            char e = s[j + 1];
            if (e == 'n' || e == 't' || e == 'f' || e == 'r') ws = 1;
            j++;
            continue;
        }
        if (__builtin_expect(c < 0x80, 1)) {
            if (c == ' ') ws = 1;
            dirty |= HTML_JSON_SPEC_LUT[c];
        } else {
            uint32_t cl;
            uint32_t cp = utf8_decode((const uint8_t *)(s + j), (const uint8_t *)(s + n), &cl);
            if (is_js_ws_cp(cp)) ws = 1;
            j += cl - 1;
        }
    }
    return -1;
}

// Helper: advance past a JSON escape in raw JSON during delimiter scanning (`]` / `**` / `*`),
// also skipping MD-escaped char if it is `\\\\` + escapable!
static inline uint32_t raw_skip_md_escape(const char *s, uint32_t n, uint32_t j, uint8_t *dirty) {
    // s[j] == '\\', j + 1 < n
    char e = s[j + 1];
    if (e == '\\') {
        *dirty = 1;
        if (j + 2 < n) {
            uint8_t nc = (uint8_t)s[j + 2];
            if (nc == '\\' && j + 3 < n && s[j + 3] == '\\') return j + 3; // \\\\ -> escaped backslash (for-loop does j++)
            if (nc != '\\' && nc < 128 && MD_ESCAPABLE[nc]) return j + 2;  // \\X -> escaped MD char
        }
        return j + 1;
    }
    if (e == '"') *dirty = 1;
    // \n, \t, \b, \f are already JSON-escaped clean output (dirty unchanged!)
    return j + 1;
}

static inline int32_t md_raw_scan_rbracket(const char *s, uint32_t n, uint32_t from, uint8_t *out_dirty) {
    uint8_t dirty = 0;
    for (uint32_t j = from; j < n; j++) {
        uint8_t c = (uint8_t)s[j];
        if (c == ']') { *out_dirty = dirty; return (int32_t)j; }
        if (c == '\\' && j + 1 < n) { j = raw_skip_md_escape(s, n, j, &dirty); continue; }
        dirty |= RAW_INLINE_KIND_LUT[c];
    }
    return -1;
}

static inline int32_t md_raw_scan_dstar(const char *s, uint32_t n, uint32_t from, uint8_t *out_dirty) {
    if (n < 2) return -1;
    uint32_t lim = n - 1;
    uint8_t dirty = 0;
    for (uint32_t j = from; j < lim; j++) {
        uint8_t c = (uint8_t)s[j];
        if (c == '*' && s[j + 1] == '*') { *out_dirty = dirty; return (int32_t)j; }
        if (c == '\\' && j + 1 < n) { j = raw_skip_md_escape(s, n, j, &dirty); continue; }
        dirty |= RAW_INLINE_KIND_LUT[c];
    }
    return -1;
}

static inline int32_t md_raw_scan_sstar(const char *s, uint32_t n, uint32_t from, uint8_t *out_dirty) {
    uint8_t dirty = 0;
    for (uint32_t j = from; j < n; j++) {
        uint8_t c = (uint8_t)s[j];
        if (c == '*') {
            if (j + 1 < n && s[j + 1] == '*') { dirty = 1; j++; continue; }
            *out_dirty = dirty;
            return (int32_t)j;
        }
        if (c == '\\' && j + 1 < n) { j = raw_skip_md_escape(s, n, j, &dirty); continue; }
        dirty |= RAW_INLINE_KIND_LUT[c];
    }
    return -1;
}

// Unescape a tiny raw-JSON slice (no \r/\u/\/) into a stack buffer (for rare links/headings with \" or \\)
static inline uint32_t unescape_tiny_raw(Str raw, char *dst, uint32_t cap) {
    uint32_t out_n = 0;
    for (uint32_t i = 0; i < raw.n && out_n + 2 < cap; i++) {
        char c = raw.p[i];
        if (c == '\\' && i + 1 < raw.n) {
            char e = raw.p[++i];
            if (e == 'n') dst[out_n++] = '\n';
            else if (e == 't') dst[out_n++] = '\t';
            else if (e == 'b') dst[out_n++] = '\b';
            else if (e == 'f') dst[out_n++] = '\f';
            else dst[out_n++] = e;
        } else {
            dst[out_n++] = c;
        }
    }
    return out_n;
}

static inline char *emit_raw_md_link_href(Buf *out, char *dst, Str target, uint8_t hdirty, uint8_t has_bs, uint32_t extra_need, LinkResolveFn resolve, void *rctx) {
    if (__builtin_expect(!has_bs, 1)) {
        return emit_md_link_href(out, dst, target, hdirty, extra_need, resolve, rctx);
    }
    char utmp[2048];
    uint32_t ulen = unescape_tiny_raw(target, utmp, sizeof(utmp));
    return emit_md_link_href(out, dst, (Str){ utmp, ulen }, 1, extra_need, resolve, rctx);
}

// ---------------- Raw-JSON Inline Renderer (Scalar + Pure-AVX2 + 16B VEX Tail) ----------------
static __attribute__((noinline)) char *md_inline_raw_scalar(Buf *out, char *dst, const char *s, uint32_t n, LinkResolveFn resolve, void *rctx) {
    dst = inline_ensure(out, dst, n * 6u + 64u);
    uint32_t i = 0;
    while (i < n) {
        uint8_t c = (uint8_t)s[i];
        uint8_t kind = RAW_INLINE_KIND_LUT[c];
        if (__builtin_expect(kind == 0, 1)) { *dst++ = (char)c; i++; continue; }
        if (kind == 1) { dst += emit_html_json_char(dst, c); i++; continue; }
        if (c == '\\') {
            if (i + 1 < n) {
                char e = s[i + 1];
                if (e == '"') { DST_LIT(dst, "&quot;"); i += 2; continue; }
                if (e != '\\') { dst[0] = '\\'; dst[1] = e; dst += 2; i += 2; continue; }
                // Real Markdown backslash (\\\\ in raw JSON)!
                if (i + 2 < n) {
                    uint8_t nc = (uint8_t)s[i + 2];
                    if (nc == '\\' && i + 3 < n && s[i + 3] == '\\') {
                        dst[0] = '\\'; dst[1] = '\\'; dst += 2; i += 4; continue;
                    }
                    if (nc != '\\' && nc < 128 && MD_ESCAPABLE[nc]) {
                        if (nc == '>') DST_LIT(dst, "&gt;");
                        else *dst++ = (char)nc;
                        i += 3; continue;
                    }
                }
                dst[0] = '\\'; dst[1] = '\\'; dst += 2; i += 2;
                continue;
            }
            dst[0] = '\\'; dst[1] = '\\'; dst += 2; i++;
            continue;
        }
        if (c == '`') {
            uint8_t dirty = 0;
            int32_t j = md_raw_scan_backtick(s, n, i + 1, &dirty);
            if (j > (int32_t)(i + 1)) {
                uint32_t clen = (uint32_t)j - (i + 1);
                DST_LIT(dst, "<code>");
                if (__builtin_expect(!dirty, 1)) { fast_memcpy(dst, s + i + 1, clen); dst += clen; }
                else dst = html_from_raw_scalar(dst, s + i + 1, clen);
                DST_LIT(dst, "</code>");
                i = (uint32_t)j + 1;
            } else { *dst++ = '`'; i++; }
            continue;
        }
        if (c == '*') {
            if (i + 1 < n && s[i + 1] == '*') {
                uint8_t dirty = 0;
                int32_t j = md_raw_scan_dstar(s, n, i + 2, &dirty);
                if (j > (int32_t)(i + 2)) {
                    uint32_t slen = (uint32_t)j - (i + 2);
                    DST_LIT(dst, "<strong>");
                    if (__builtin_expect(!dirty, 1)) { fast_memcpy(dst, s + i + 2, slen); dst += slen; }
                    else dst = md_inline_raw_scalar(out, dst, s + i + 2, slen, resolve, rctx);
                    dst = inline_ensure(out, dst, 32u);
                    DST_LIT(dst, "</strong>");
                    i = (uint32_t)j + 2;
                } else { dst[0] = '*'; dst[1] = '*'; dst += 2; i += 2; }
                continue;
            }
            uint8_t dirty = 0;
            int32_t j = md_raw_scan_sstar(s, n, i + 1, &dirty);
            if (j > (int32_t)(i + 1)) {
                uint32_t slen = (uint32_t)j - (i + 1);
                DST_LIT(dst, "<em>");
                if (__builtin_expect(!dirty, 1)) { fast_memcpy(dst, s + i + 1, slen); dst += slen; }
                else dst = md_inline_raw_scalar(out, dst, s + i + 1, slen, resolve, rctx);
                dst = inline_ensure(out, dst, 32u);
                DST_LIT(dst, "</em>");
                i = (uint32_t)j + 1;
            } else { *dst++ = '*'; i++; }
            continue;
        }
        // '[' link
        uint8_t dirty = 0;
        int32_t k = md_raw_scan_rbracket(s, n, i + 1, &dirty);
        if (k != -1 && (uint32_t)k + 1 < n && s[k + 1] == '(') {
            uint8_t ws = 0, hdirty = 0, has_bs = 0;
            int32_t m = md_raw_scan_rparen(s, n, (uint32_t)k + 2, &ws, &hdirty, &has_bs);
            if (m > k + 2 && !ws) {
                Str target = { s + k + 2, (uint32_t)(m - (k + 2)) };
                uint32_t ltxt = (uint32_t)k - (i + 1);
                dst = emit_raw_md_link_href(out, dst, target, hdirty, has_bs, (n - i) * 6u + 64u, resolve, rctx);
                if (__builtin_expect(!dirty, 1)) { fast_memcpy(dst, s + i + 1, ltxt); dst += ltxt; }
                else dst = md_inline_raw_scalar(out, dst, s + i + 1, ltxt, resolve, rctx);
                dst = inline_ensure(out, dst, 32u);
                DST_LIT(dst, "</a>");
                i = (uint32_t)m + 1;
                continue;
            }
        }
        *dst++ = '['; i++;
    }
    return dst;
}

#if defined(__AVX2__)
static __attribute__((noinline)) char *md_inline_raw_avx2(Buf *out, char *dst, const char *s, uint32_t n, LinkResolveFn resolve, void *rctx) {
    dst = inline_ensure(out, dst, n * 6u + 512u);

    // Exact PSHUFB for 8 chars ONLY: & (26), ' (27), * (2A), < (3C), > (3E), [ (5B), \ (5C), ` (60)
    // NO " (never unescaped in raw JSON) and NO ctrl_mask (<0x20 never in raw JSON)!
    const __m256i lo_lut = _mm256_setr_epi8(
        8, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 4, 2 | 4, 0, 2, 0,
        8, 0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 4, 2 | 4, 0, 2, 0
    );
    const __m256i hi_lut = _mm256_setr_epi8(
        0, 0, 1, 2, 0, 4, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 1, 2, 0, 4, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0
    );
    const __m256i v_0f = _mm256_set1_epi8(0x0F);
    const __m256i v_z  = _mm256_setzero_si256();
    const __m256i v_bs = _mm256_set1_epi8('\\');
    const __m256i v_n  = _mm256_set1_epi8('n');

    uint32_t i = 0;
    while (i + 32 <= n) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(s + i));
        _mm256_storeu_si256((__m256i *)dst, v);
        __m256i lo = _mm256_and_si256(v, v_0f);
        __m256i hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), v_0f);
        __m256i hit = _mm256_and_si256(_mm256_shuffle_epi8(lo_lut, lo), _mm256_shuffle_epi8(hi_lut, hi));
        uint32_t mask = ~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(hit, v_z));
        // Zero-cycle \n passthrough! When no consecutive \\ in this 32B chunk, \n is ALREADY perfect in dst!
        if (mask) {
            uint32_t m_bs = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, v_bs));
            if (m_bs && (m_bs & (m_bs << 1)) == 0) {
                uint32_t m_n = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(v, v_n));
                mask &= ~((m_bs & (m_n >> 1)) & 0x7FFFFFFFu);
            }
        }
        if (__builtin_expect(mask == 0, 1)) {
            dst += 32;
            i += 32;
            continue;
        }
        uint32_t cur = 0;
        uint32_t base = i;
        while (mask) {
            uint32_t tz = (uint32_t)__builtin_ctz(mask);
            uint32_t run = tz - cur;
            if (cur == 0) dst += run;
            else if (run) { fast_memcpy(dst, s + base + cur, run); dst += run; }
            uint32_t pos = base + tz;
            uint8_t c = (uint8_t)s[pos];
            if (RAW_INLINE_KIND_LUT[c] == 1) {
                dst += emit_html_json_char(dst, c);
                cur = tz + 1;
                mask &= mask - 1u;
                continue;
            }
            // Instant zero-abort JSON escape & Markdown backslash handler!
            if (c == '\\') {
                if (pos + 1 < n) {
                    char e = s[pos + 1];
                    if (e == '"') {
                        DST_LIT(dst, "&quot;");
                        if (tz + 2 < 32u) { cur = tz + 2; mask &= (~0u << cur); continue; }
                        i = pos + 2; goto next_rchunk;
                    }
                    if (e != '\\') {
                        dst[0] = '\\'; dst[1] = e; dst += 2;
                        if (tz + 2 < 32u) { cur = tz + 2; mask &= (~0u << cur); continue; }
                        i = pos + 2; goto next_rchunk;
                    }
                    // Real Markdown backslash (\\\\ in raw JSON)
                    uint32_t adv = 2;
                    if (pos + 2 < n) {
                        uint8_t nc = (uint8_t)s[pos + 2];
                        if (nc == '\\' && pos + 3 < n && s[pos + 3] == '\\') {
                            dst[0] = '\\'; dst[1] = '\\'; dst += 2; adv = 4;
                        } else if (nc != '\\' && nc < 128 && MD_ESCAPABLE[nc]) {
                            if (nc == '>') DST_LIT(dst, "&gt;");
                            else *dst++ = (char)nc;
                            adv = 3;
                        } else {
                            dst[0] = '\\'; dst[1] = '\\'; dst += 2;
                        }
                    } else {
                        dst[0] = '\\'; dst[1] = '\\'; dst += 2;
                    }
                    if (tz + adv < 32u) { cur = tz + adv; mask &= (~0u << cur); continue; }
                    i = pos + adv; goto next_rchunk;
                }
                dst[0] = '\\'; dst[1] = '\\'; dst += 2;
                i = pos + 1; goto next_rchunk;
            }
            // Instant zero-scan bitmask fast-path for short `code`, *em*, **strong** closing within 32B!
            uint32_t rem = mask & (mask - 1u);
            if (c == '`' && rem) {
                uint32_t tz2 = (uint32_t)__builtin_ctz(rem);
                if (s[base + tz2] == '`' && tz2 > tz + 1) {
                    uint32_t clen = tz2 - tz - 1;
                    DST_LIT(dst, "<code>");
                    fast_memcpy(dst, s + pos + 1, clen); dst += clen;
                    DST_LIT(dst, "</code>");
                    cur = tz2 + 1;
                    mask = rem & (rem - 1u);
                    continue;
                }
            }
            if (c == '*') {
                if (pos + 1 < n && s[pos + 1] == '*') {
                    if (tz + 2 < 32u) {
                        uint32_t rem2 = mask & (~0u << (tz + 2));
                        if (rem2) {
                            uint32_t tz2 = (uint32_t)__builtin_ctz(rem2);
                            if (tz2 > tz + 2 && base + tz2 + 1 < n && s[base + tz2] == '*' && s[base + tz2 + 1] == '*') {
                                uint32_t slen = tz2 - (tz + 2);
                                DST_LIT(dst, "<strong>");
                                fast_memcpy(dst, s + pos + 2, slen); dst += slen;
                                DST_LIT(dst, "</strong>");
                                if (tz2 + 2 < 32u) { cur = tz2 + 2; mask &= (~0u << cur); continue; }
                                i = base + tz2 + 2; goto next_rchunk;
                            }
                        }
                    }
                } else if (rem) {
                    uint32_t tz2 = (uint32_t)__builtin_ctz(rem);
                    if (tz2 > tz + 1 && s[base + tz2] == '*' && (base + tz2 + 1 >= n || s[base + tz2 + 1] != '*')) {
                        uint32_t slen = tz2 - tz - 1;
                        DST_LIT(dst, "<em>");
                        fast_memcpy(dst, s + pos + 1, slen); dst += slen;
                        DST_LIT(dst, "</em>");
                        cur = tz2 + 1;
                        mask = rem & (rem - 1u);
                        continue;
                    }
                }
            }

            // General dirty-tracked construct handler
            i = pos;
            if (c == '`') {
                uint8_t dirty = 0;
                int32_t j = md_raw_scan_backtick(s, n, i + 1, &dirty);
                if (j > (int32_t)(i + 1)) {
                    uint32_t clen = (uint32_t)j - (i + 1);
                    DST_LIT(dst, "<code>");
                    if (__builtin_expect(!dirty, 1)) { fast_memcpy(dst, s + i + 1, clen); dst += clen; }
                    else dst = html_from_raw_scalar(dst, s + i + 1, clen);
                    DST_LIT(dst, "</code>");
                    i = (uint32_t)j + 1;
                } else { *dst++ = '`'; i++; }
            } else if (c == '*') {
                if (i + 1 < n && s[i + 1] == '*') {
                    uint8_t dirty = 0;
                    int32_t j = md_raw_scan_dstar(s, n, i + 2, &dirty);
                    if (j > (int32_t)(i + 2)) {
                        uint32_t slen = (uint32_t)j - (i + 2);
                        DST_LIT(dst, "<strong>");
                        if (__builtin_expect(!dirty, 1)) { fast_memcpy(dst, s + i + 2, slen); dst += slen; }
                        else dst = md_inline_raw_avx2(out, dst, s + i + 2, slen, resolve, rctx);
                        DST_LIT(dst, "</strong>");
                        i = (uint32_t)j + 2;
                    } else { dst[0] = '*'; dst[1] = '*'; dst += 2; i += 2; }
                } else {
                    uint8_t dirty = 0;
                    int32_t j = md_raw_scan_sstar(s, n, i + 1, &dirty);
                    if (j > (int32_t)(i + 1)) {
                        uint32_t slen = (uint32_t)j - (i + 1);
                        DST_LIT(dst, "<em>");
                        if (__builtin_expect(!dirty, 1)) { fast_memcpy(dst, s + i + 1, slen); dst += slen; }
                        else dst = md_inline_raw_avx2(out, dst, s + i + 1, slen, resolve, rctx);
                        DST_LIT(dst, "</em>");
                        i = (uint32_t)j + 1;
                    } else { *dst++ = '*'; i++; }
                }
            } else { // '['
                uint8_t dirty = 0;
                int32_t k = md_raw_scan_rbracket(s, n, i + 1, &dirty);
                if (k != -1 && (uint32_t)k + 1 < n && s[k + 1] == '(') {
                    uint8_t ws = 0, hdirty = 0, has_bs = 0;
                    int32_t m = md_raw_scan_rparen(s, n, (uint32_t)k + 2, &ws, &hdirty, &has_bs);
                    if (m > k + 2 && !ws) {
                        Str target = { s + k + 2, (uint32_t)(m - (k + 2)) };
                        uint32_t ltxt = (uint32_t)k - (i + 1);
                        dst = emit_raw_md_link_href(out, dst, target, hdirty, has_bs, (n - i) * 6u + 512u, resolve, rctx);
                        if (__builtin_expect(!dirty, 1)) { fast_memcpy(dst, s + i + 1, ltxt); dst += ltxt; }
                        else dst = md_inline_raw_avx2(out, dst, s + i + 1, ltxt, resolve, rctx);
                        DST_LIT(dst, "</a>");
                        i = (uint32_t)m + 1;
                        goto next_rchunk;
                    }
                }
                *dst++ = '['; i++;
            }
            if (i < base + 32u) { cur = i - base; mask &= (~0u << cur); continue; }
            goto next_rchunk;
        }
        if (cur < 32u) { fast_memcpy(dst, s + base + cur, 32u - cur); dst += 32u - cur; }
        i = base + 32;
    next_rchunk:;
    }

    // 16-byte VEX tail for 16..31B lines/remainders!
    if (i + 16 <= n) {
        const __m128i lo16 = _mm256_castsi256_si128(lo_lut);
        const __m128i hi16 = _mm256_castsi256_si128(hi_lut);
        const __m128i v0f16 = _mm256_castsi256_si128(v_0f);
        const __m128i vz16  = _mm_setzero_si128();
        __m128i v = _mm_loadu_si128((const __m128i *)(s + i));
        _mm_storeu_si128((__m128i *)dst, v);
        __m128i lo = _mm_and_si128(v, v0f16);
        __m128i hi = _mm_and_si128(_mm_srli_epi16(v, 4), v0f16);
        __m128i hit = _mm_and_si128(_mm_shuffle_epi8(lo16, lo), _mm_shuffle_epi8(hi16, hi));
        uint32_t mask = (~(uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(hit, vz16))) & 0xFFFFu;
        if (mask == 0) { dst += 16; i += 16; }
        else { uint32_t tz = (uint32_t)__builtin_ctz(mask); dst += tz; i += tz; }
    }

    if (i < n) dst = md_inline_raw_scalar(out, dst, s + i, n - i, resolve, rctx);
    return dst;
}
#endif

static inline char *md_inline_raw_ptr(Buf *out, char *dst, const char *s, uint32_t n, LinkResolveFn resolve, void *rctx) {
#if defined(__AVX2__)
    if (n >= 16) return md_inline_raw_avx2(out, dst, s, n, resolve, rctx);
    return md_inline_raw_scalar(out, dst, s, n, resolve, rctx);
#else
    return md_inline_raw_scalar(out, dst, s, n, resolve, rctx);
#endif
}

// ---------------- Direct Raw-JSON Block Markdown Renderer ----------------
static __attribute__((noinline)) MdResult render_markdown_raw_json(
    Str src,
    LinkResolveFn resolve,
    void *rctx,
    Buf *html_buf,
    Buf *join_buf,
    HeadingIdSet *hset,
    Arena *id_arena,
    TocItem *toc_arr,
    uint32_t toc_cap
) {
    html_buf->n = 0;
    buf_ensure(html_buf, src.n * 8u + 4096u);
    char *dst = html_buf->p;

    hset->gen++;
    uint32_t toc_len = 0;
    int has_excerpt = 0;
    uint32_t ex_off = 0, ex_len = 0;

    const char *cur = src.p;
    const char *end = src.p + src.n;
    const char *lp;
    uint32_t llen;
    int has_line = 0;
    int eof = 0;

    #define NEXT_RLINE() do { \
        if (eof) { has_line = 0; } \
        else { \
            const char *nl = find_raw_nl(cur, end); \
            lp = cur; \
            llen = (uint32_t)(nl - cur); \
            if (nl < end) cur = nl + 2; \
            else { cur = end; eof = 1; } \
            has_line = 1; \
        } \
    } while (0)

    NEXT_RLINE();
    while (has_line) {
        if (llen == 0) { NEXT_RLINE(); continue; }
        uint8_t c0 = (uint8_t)lp[0];
        if (__builtin_expect(!BLOCK_LEAD_LUT[c0] && c0 != '\\', 1)) goto parse_rpara;

        if ((c0 == ' ' || (c0 == '\\' && llen >= 2 && lp[1] == 't')) && md_raw_is_blank(lp, llen)) {
            NEXT_RLINE();
            continue;
        }
        if (c0 == '`') {
            Str lang;
            if (md_raw_parse_fence_open(lp, llen, &lang)) {
                dst = inline_ensure(html_buf, dst, lang.n * 6u + 64u);
                if (lang.n == 0) {
                    DST_LIT(dst, "<pre><code>");
                } else {
                    DST_LIT(dst, "<pre><code class=\\\"language-");
                    dst = html_json_escape_to(dst, lang.p, lang.n);
                    DST_LIT(dst, "\\\">");
                }
                NEXT_RLINE();
                const char *fstart = lp;
                int had_fence_line = 0;
                while (has_line && !md_raw_is_fence_close(lp, llen)) {
                    had_fence_line = 1;
                    NEXT_RLINE();
                }
                if (has_line) {
                    uint32_t flen = (uint32_t)(lp - fstart);
                    if (flen > 0) {
                        dst = inline_ensure(html_buf, dst, flen * 6u + 64u);
                        dst = html_from_raw(dst, fstart, flen);
                    }
                    NEXT_RLINE();
                } else if (had_fence_line) {
                    uint32_t flen = (uint32_t)(end - fstart);
                    if (flen > 0) {
                        dst = inline_ensure(html_buf, dst, flen * 6u + 64u);
                        dst = html_from_raw(dst, fstart, flen);
                    }
                    dst = inline_ensure(html_buf, dst, 16u);
                    DST_LIT(dst, "\\n");
                }
                dst = inline_ensure(html_buf, dst, 32u);
                DST_LIT(dst, "</code></pre>\\n");
                continue;
            }
        }
        if (c0 == '#') {
            uint8_t hdepth; Str htext;
            if (md_raw_parse_heading(lp, llen, &hdepth, &htext)) {
                // Fast check if heading has any '\\' before slugify
                int hbs = 0;
                for (uint32_t hi = 0; hi < htext.n; hi++) if (htext.p[hi] == '\\') { hbs = 1; break; }
                char slug_tmp[64];
                uint32_t slen;
                if (__builtin_expect(!hbs, 1)) {
                    slen = slugify_into(htext, slug_tmp);
                } else {
                    char htmp[1024];
                    uint32_t hun = unescape_tiny_raw(htext, htmp, sizeof(htmp));
                    slen = slugify_into((Str){ htmp, hun }, slug_tmp);
                }
                Str id = md_unique_id(hset, id_arena, slug_tmp, slen);
                dst = inline_ensure(html_buf, dst, 64u + id.n + htext.n * 6u);
                dst[0] = '<'; dst[1] = 'h'; dst[2] = (char)('0' + hdepth); dst += 3;
                DST_LIT(dst, " id=\\\"");
                fast_memcpy(dst, id.p, id.n); dst += id.n;
                DST_LIT(dst, "\\\">");
                uint32_t h_off = (uint32_t)(dst - html_buf->p);
                dst = md_inline_raw_ptr(html_buf, dst, htext.p, htext.n, resolve, rctx);
                uint32_t h_len = (uint32_t)(dst - html_buf->p) - h_off;
                dst = inline_ensure(html_buf, dst, 16u);
                DST_LIT(dst, "</h"); *dst++ = (char)('0' + hdepth); DST_LIT(dst, ">\\n");
                if ((hdepth == 2 || hdepth == 3) && toc_len < toc_cap) {
                    toc_arr[toc_len].level = hdepth;
                    toc_arr[toc_len].id = id;
                    toc_arr[toc_len].html.p = (const char *)(uintptr_t)h_off;
                    toc_arr[toc_len].html.n = h_len;
                    toc_len++;
                }
                NEXT_RLINE();
                continue;
            }
        }
        if ((c0 == '-' || c0 == '*') && md_raw_is_hr(lp, llen)) {
            dst = inline_ensure(html_buf, dst, 16u);
            DST_LIT(dst, "<hr>\\n");
            NEXT_RLINE();
            continue;
        }
        if (c0 == '>') {
            join_buf->n = 0;
            while (has_line && llen > 0 && lp[0] == '>') {
                uint32_t skip = (llen > 1 && lp[1] == ' ') ? 2u : 1u;
                Str tr = md_raw_trim_st(lp + skip, llen - skip);
                if (tr.n > 0) {
                    if (join_buf->n > 0) BUF_LIT(join_buf, "\\n");
                    buf_raw(join_buf, tr.p, tr.n);
                }
                NEXT_RLINE();
            }
            dst = inline_ensure(html_buf, dst, 64u);
            if (join_buf->n == 0) {
                DST_LIT(dst, "<blockquote>\\n</blockquote>\\n");
            } else {
                DST_LIT(dst, "<blockquote>\\n<p>");
                dst = md_inline_raw_ptr(html_buf, dst, join_buf->p, join_buf->n, resolve, rctx);
                dst = inline_ensure(html_buf, dst, 32u);
                DST_LIT(dst, "</p>\\n</blockquote>\\n");
            }
            continue;
        }
        {
            int is_bullet = (llen >= 2 && (c0 == '-' || c0 == '*') && lp[1] == ' ');
            uint32_t ord_start = 1;
            Str ord_rest = {0, 0};
            int is_ordered = !is_bullet && (c0 >= '0' && c0 <= '9') && md_parse_ordered(lp, llen, &ord_start, &ord_rest);
            if (is_bullet || is_ordered) {
                dst = inline_ensure(html_buf, dst, 32u);
                if (is_ordered) {
                    if (ord_start != 1) {
                        DST_LIT(dst, "<ol start=\\\"");
                        dst += u32_to_str(ord_start, dst);
                        DST_LIT(dst, "\\\">\\n");
                    } else {
                        DST_LIT(dst, "<ol>\\n");
                    }
                } else {
                    DST_LIT(dst, "<ul>\\n");
                }
                join_buf->n = 0;
                int have_item = 0;
                Str single_item = {0, 0};
                int item_in_join = 0;
                while (has_line) {
                    int matched = 0;
                    Str item_first = {0, 0};
                    if (!is_ordered) {
                        if (llen >= 2 && (lp[0] == '-' || lp[0] == '*') && lp[1] == ' ') {
                            matched = 1;
                            item_first = md_raw_trim_st(lp + 2, llen - 2);
                        }
                    } else {
                        uint32_t dummy_s; Str r;
                        if (md_parse_ordered(lp, llen, &dummy_s, &r)) {
                            matched = 1;
                            item_first = md_raw_trim_st(r.p, r.n);
                        }
                    }
                    if (matched) {
                        if (have_item) {
                            dst = inline_ensure(html_buf, dst, 32u);
                            DST_LIT(dst, "<li>");
                            if (!item_in_join) dst = md_inline_raw_ptr(html_buf, dst, single_item.p, single_item.n, resolve, rctx);
                            else dst = md_inline_raw_ptr(html_buf, dst, join_buf->p, join_buf->n, resolve, rctx);
                            dst = inline_ensure(html_buf, dst, 16u);
                            DST_LIT(dst, "</li>\\n");
                        }
                        single_item = item_first;
                        item_in_join = 0;
                        have_item = 1;
                        NEXT_RLINE();
                    } else if (have_item && md_raw_is_continued(lp, llen)) {
                        if (!item_in_join) {
                            join_buf->n = 0;
                            buf_raw(join_buf, single_item.p, single_item.n);
                            item_in_join = 1;
                        }
                        Str tr = md_raw_trim_st(lp, llen);
                        BUF_LIT(join_buf, "\\n");
                        buf_raw(join_buf, tr.p, tr.n);
                        NEXT_RLINE();
                    } else break;
                }
                if (have_item) {
                    dst = inline_ensure(html_buf, dst, 32u);
                    DST_LIT(dst, "<li>");
                    if (!item_in_join) dst = md_inline_raw_ptr(html_buf, dst, single_item.p, single_item.n, resolve, rctx);
                    else dst = md_inline_raw_ptr(html_buf, dst, join_buf->p, join_buf->n, resolve, rctx);
                    dst = inline_ensure(html_buf, dst, 16u);
                    DST_LIT(dst, "</li>\\n");
                }
                dst = inline_ensure(html_buf, dst, 16u);
                if (is_ordered) DST_LIT(dst, "</ol>\\n");
                else DST_LIT(dst, "</ul>\\n");
                continue;
            }
        }

    parse_rpara:;
        const char *para_start = lp;
        const char *para_end = lp + llen;
        int contiguous = md_raw_is_clean_ends(lp, llen);
        Str first = contiguous ? (Str){lp, llen} : md_raw_trim_st(lp, llen);
        NEXT_RLINE();
        if (!has_line || md_raw_ends_paragraph(lp, llen)) {
            dst = inline_ensure(html_buf, dst, 32u);
            DST_LIT(dst, "<p>");
            uint32_t p_off = (uint32_t)(dst - html_buf->p);
            dst = md_inline_raw_ptr(html_buf, dst, first.p, first.n, resolve, rctx);
            uint32_t p_len = (uint32_t)(dst - html_buf->p) - p_off;
            dst = inline_ensure(html_buf, dst, 16u);
            DST_LIT(dst, "</p>\\n");
            if (!has_excerpt) { has_excerpt = 1; ex_off = p_off; ex_len = p_len; }
        } else {
            if (!contiguous) {
                join_buf->n = 0;
                buf_raw(join_buf, first.p, first.n);
            }
            while (has_line && !md_raw_ends_paragraph(lp, llen)) {
                int clean = md_raw_is_clean_ends(lp, llen);
                if (contiguous && clean) {
                    para_end = lp + llen;
                } else {
                    if (contiguous) {
                        join_buf->n = 0;
                        buf_raw(join_buf, para_start, (uint32_t)(para_end - para_start));
                        contiguous = 0;
                    }
                    Str tr = clean ? (Str){lp, llen} : md_raw_trim_st(lp, llen);
                    BUF_LIT(join_buf, "\\n");
                    buf_raw(join_buf, tr.p, tr.n);
                }
                NEXT_RLINE();
            }
            dst = inline_ensure(html_buf, dst, 32u);
            DST_LIT(dst, "<p>");
            uint32_t p_off = (uint32_t)(dst - html_buf->p);
            if (contiguous) dst = md_inline_raw_ptr(html_buf, dst, para_start, (uint32_t)(para_end - para_start), resolve, rctx);
            else dst = md_inline_raw_ptr(html_buf, dst, join_buf->p, join_buf->n, resolve, rctx);
            uint32_t p_len = (uint32_t)(dst - html_buf->p) - p_off;
            dst = inline_ensure(html_buf, dst, 16u);
            DST_LIT(dst, "</p>\\n");
            if (!has_excerpt) { has_excerpt = 1; ex_off = p_off; ex_len = p_len; }
        }
    }
    #undef NEXT_RLINE

    html_buf->n = (uint32_t)(dst - html_buf->p);
    for (uint32_t i = 0; i < toc_len; i++) {
        toc_arr[i].html.p = html_buf->p + (uintptr_t)toc_arr[i].html.p;
    }

    MdResult res;
    res.html = (Str){ html_buf->p, html_buf->n };
    res.excerpt = has_excerpt ? (Str){ html_buf->p + ex_off, ex_len } : (Str){ "", 0 };
    res.toc = toc_arr;
    res.toc_len = toc_len;
    return res;
}
