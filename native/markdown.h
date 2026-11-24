#pragma once
#include "simd_text.h"

// Ultra-fast makefun-site 1.0.0 Markdown renderer (v5):
// - Pure AVX2 + 16B VEX tail (ZERO vzeroupper, ZERO scalar fallback for 16..31B lines!)
// - Branchless 2-instruction HJ_ESC_TBL + padded SIMD short-run copy
// - Instant bitmask fast-paths for \, `code`, *em*, **strong** inside 32B/16B vectors
// - Inline external/root/anchor link fast-path + pre-escaped url_hj for internal links
// - Register-resident `char *dst` + single-pass AVX2 fenced code blocks

typedef struct {
    uint8_t level;
    Str id;
    Str html;
} TocItem;

typedef Str (*LinkResolveFn)(void *ctx, Str target, int *out_preescaped);

static const uint8_t MD_ESCAPABLE[128] = {
    ['\\'] = 1, ['`'] = 1, ['*'] = 1, ['_'] = 1, ['['] = 1, [']'] = 1,
    ['('] = 1, [')'] = 1, ['#'] = 1, ['+'] = 1, ['-'] = 1, ['.'] = 1,
    ['!'] = 1, ['>'] = 1,
};

// 0 = plain byte, 1 = simple HTML/JSON escape (<0x20, ", &, ', <, >), 2 = markdown construct (\, `, *, [)
static const uint8_t INLINE_KIND_LUT[256] = {
    1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
    ['"'] = 1, ['&'] = 1, ['\''] = 1, ['<'] = 1, ['>'] = 1,
    ['\\'] = 2, ['`'] = 2, ['*'] = 2, ['['] = 2
};

static const uint8_t BLOCK_LEAD_LUT[256] = {
    [' '] = 1, ['\t'] = 1, ['`'] = 1, ['#'] = 1, ['-'] = 1, ['*'] = 1, ['>'] = 1,
    ['0'] = 1, ['1'] = 1, ['2'] = 1, ['3'] = 1, ['4'] = 1,
    ['5'] = 1, ['6'] = 1, ['7'] = 1, ['8'] = 1, ['9'] = 1,
};

static inline int md_is_blank(const char *p, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (p[i] != ' ' && p[i] != '\t') return 0;
    }
    return 1;
}

static inline Str md_trim_st(const char *p, uint32_t n) {
    uint32_t a = 0, b = n;
    while (a < b && (p[a] == ' ' || p[a] == '\t')) a++;
    while (b > a && (p[b - 1] == ' ' || p[b - 1] == '\t')) b--;
    return (Str){ p + a, b - a };
}

static inline int md_parse_fence_open(const char *p, uint32_t n, Str *out_lang) {
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
        if (p[i] != ' ' && p[i] != '\t') return 0;
        i++;
    }
    *out_lang = (Str){ p + 3, lang_end - 3 };
    return 1;
}

static inline int md_is_fence_close(const char *p, uint32_t n) {
    if (n < 3 || p[0] != '`' || p[1] != '`' || p[2] != '`') return 0;
    for (uint32_t i = 3; i < n; i++) {
        if (p[i] != ' ' && p[i] != '\t') return 0;
    }
    return 1;
}

static inline int md_parse_heading(const char *p, uint32_t n, uint8_t *out_depth, Str *out_text) {
    if (n < 3 || p[0] != '#') return 0;
    uint32_t d = 1;
    while (d < 6 && d < n && p[d] == '#') d++;
    if (d >= n || (p[d] != ' ' && p[d] != '\t')) return 0;
    uint32_t i = d;
    while (i < n && (p[i] == ' ' || p[i] == '\t')) i++;
    uint32_t end = n;
    while (end > i && (p[end - 1] == ' ' || p[end - 1] == '\t')) end--;
    if (end <= i) return 0;
    *out_depth = (uint8_t)d;
    *out_text = (Str){ p + i, end - i };
    return 1;
}

static inline int md_is_hr(const char *p, uint32_t n) {
    if (n < 3) return 0;
    char c = p[0];
    if ((c != '-' && c != '*') || p[1] != c || p[2] != c) return 0;
    for (uint32_t i = 3; i < n; i++) {
        if (p[i] != ' ' && p[i] != '\t') return 0;
    }
    return 1;
}

static inline int md_ends_paragraph(const char *p, uint32_t n) {
    if (n == 0) return 1;
    char c0 = p[0];
    if (__builtin_expect((uint8_t)c0 > ' ' && c0 != '`' && c0 != '#' && c0 != '-' && c0 != '*', 1)) return 0;
    if (c0 == ' ' || c0 == '\t') return md_is_blank(p, n);
    if (c0 == '`') { Str tmp; return md_parse_fence_open(p, n, &tmp); }
    if (c0 == '#') { uint8_t d; Str t; return md_parse_heading(p, n, &d, &t); }
    if (c0 == '-' || c0 == '*') return md_is_hr(p, n);
    return 0;
}

static inline int md_parse_ordered(const char *p, uint32_t n, uint32_t *out_start, Str *out_rest) {
    if (n < 3 || p[0] < '0' || p[0] > '9') return 0;
    uint32_t i = 0, val = 0;
    while (i < n && i < 9 && p[i] >= '0' && p[i] <= '9') {
        val = val * 10u + (uint32_t)(p[i] - '0');
        i++;
    }
    if (i == 0 || i + 1 >= n || p[i] != '.' || p[i + 1] != ' ') return 0;
    *out_start = val;
    *out_rest = (Str){ p + i + 2, n - (i + 2) };
    return 1;
}

static inline int md_is_continued(const char *p, uint32_t n) {
    if (n < 3 || p[0] != ' ' || p[1] != ' ') return 0;
    uint32_t i = 2;
    while (i < n && p[i] == ' ') i++;
    if (i >= n) return 0;
    uint8_t b = (uint8_t)p[i];
    if (__builtin_expect(b < 0x80, 1)) return !(b == ' ' || (uint8_t)(b - 9) <= 4u);
    uint32_t cl;
    uint32_t cp = utf8_decode((const uint8_t *)(p + i), (const uint8_t *)(p + n), &cl);
    return !is_js_ws_cp(cp);
}

// ---------------- Dirty-Tracked Delimiter Scanners ----------------
static inline int32_t md_scan_backtick(const char *s, uint32_t n, uint32_t from, uint8_t *out_dirty) {
    uint8_t dirty = 0;
    for (uint32_t j = from; j < n; j++) {
        uint8_t c = (uint8_t)s[j];
        if (c == '`') { *out_dirty = dirty; return (int32_t)j; }
        dirty |= HTML_JSON_SPEC_LUT[c];
    }
    return -1;
}

// Track BOTH JS whitespace AND HTML/JSON dirty bit during link target scan!
static inline int32_t md_scan_rparen(const char *s, uint32_t n, uint32_t from, uint8_t *out_has_ws, uint8_t *out_dirty) {
    uint8_t ws = 0, dirty = 0;
    for (uint32_t j = from; j < n; j++) {
        uint8_t c = (uint8_t)s[j];
        if (c == ')') { *out_has_ws = ws; *out_dirty = dirty; return (int32_t)j; }
        if (__builtin_expect(c < 0x80, 1)) {
            if (c == ' ' || (uint8_t)(c - 9) <= 4u) ws = 1;
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

static inline int32_t md_scan_rbracket(const char *s, uint32_t n, uint32_t from, uint8_t *out_dirty) {
    uint8_t dirty = 0;
    for (uint32_t j = from; j < n; j++) {
        uint8_t c = (uint8_t)s[j];
        if (c == ']') { *out_dirty = dirty; return (int32_t)j; }
        dirty |= INLINE_KIND_LUT[c];
        if (c == '\\' && j + 1 < n && (uint8_t)s[j + 1] < 128 && MD_ESCAPABLE[(uint8_t)s[j + 1]]) j++;
    }
    return -1;
}

static inline int32_t md_scan_dstar(const char *s, uint32_t n, uint32_t from, uint8_t *out_dirty) {
    if (n < 2) return -1;
    uint32_t lim = n - 1;
    uint8_t dirty = 0;
    for (uint32_t j = from; j < lim; j++) {
        uint8_t c = (uint8_t)s[j];
        if (c == '*' && s[j + 1] == '*') { *out_dirty = dirty; return (int32_t)j; }
        dirty |= INLINE_KIND_LUT[c];
        if (c == '\\' && j + 1 < n && (uint8_t)s[j + 1] < 128 && MD_ESCAPABLE[(uint8_t)s[j + 1]]) j++;
    }
    return -1;
}

static inline int32_t md_scan_sstar(const char *s, uint32_t n, uint32_t from, uint8_t *out_dirty) {
    uint8_t dirty = 0;
    for (uint32_t j = from; j < n; j++) {
        uint8_t c = (uint8_t)s[j];
        if (c == '*') {
            if (j + 1 < n && s[j + 1] == '*') { dirty = 1; j++; continue; }
            *out_dirty = dirty;
            return (int32_t)j;
        }
        dirty |= INLINE_KIND_LUT[c];
        if (c == '\\' && j + 1 < n && (uint8_t)s[j + 1] < 128 && MD_ESCAPABLE[(uint8_t)s[j + 1]]) j++;
    }
    return -1;
}

static inline char *inline_ensure(Buf *out, char *dst, uint32_t need) {
    uint32_t used = (uint32_t)(dst - out->p);
    if (__builtin_expect(used + need > out->cap, 0)) {
        out->n = used;
        buf_grow(out, need);
        return out->p + used;
    }
    return dst;
}

#define DST_LIT(dst, s) do { __builtin_memcpy((dst), (s), sizeof(s) - 1); (dst) += sizeof(s) - 1; } while (0)

// Inline fast-path for external/root/anchor links + pre-escaped url_hj for internal links!
static inline char *emit_md_link_href(Buf *out, char *dst, Str target, uint8_t hdirty, uint32_t extra_need, LinkResolveFn resolve, void *rctx) {
    if (target.p[0] == '/' || target.p[0] == '#' ||
        (target.n >= 8 && (fast_memeq(target.p, "https://", 8) || fast_memeq(target.p, "http://", 7)))) {
        dst = inline_ensure(out, dst, target.n * 6u + extra_need);
        DST_LIT(dst, "<a href=\\\"");
        if (__builtin_expect(!hdirty, 1)) { fast_memcpy(dst, target.p, target.n); dst += target.n; }
        else dst = html_json_escape_scalar(dst, target.p, target.n);
        DST_LIT(dst, "\\\">");
        return dst;
    }
    int preesc = 0;
    Str href = resolve(rctx, target, &preesc);
    dst = inline_ensure(out, dst, href.n * 6u + extra_need);
    DST_LIT(dst, "<a href=\\\"");
    if (__builtin_expect(preesc, 1)) { fast_memcpy(dst, href.p, href.n); dst += href.n; }
    else dst = html_json_escape_scalar(dst, href.p, href.n);
    DST_LIT(dst, "\\\">");
    return dst;
}

// ---------------- Pure Scalar Dirty-Tracked Inline Renderer ----------------
static __attribute__((cold, noinline)) char *md_inline_ptr_scalar(Buf *out, char *dst, const char *s, uint32_t n, LinkResolveFn resolve, void *rctx) {
    dst = inline_ensure(out, dst, n * 6u + 64u);
    uint32_t i = 0;
    while (i < n) {
        uint8_t c = (uint8_t)s[i];
        uint8_t kind = INLINE_KIND_LUT[c];
        if (__builtin_expect(kind == 0, 1)) { *dst++ = (char)c; i++; continue; }
        if (kind == 1) { dst += emit_html_json_char(dst, c); i++; continue; }
        if (c == '\\') {
            if (i + 1 < n && (uint8_t)s[i + 1] < 128 && MD_ESCAPABLE[(uint8_t)s[i + 1]]) {
                uint8_t nc = (uint8_t)s[i + 1];
                if (nc == '>') DST_LIT(dst, "&gt;");
                else if (nc == '\\') { dst[0] = '\\'; dst[1] = '\\'; dst += 2; }
                else *dst++ = (char)nc;
                i += 2;
            } else { dst[0] = '\\'; dst[1] = '\\'; dst += 2; i++; }
            continue;
        }
        if (c == '`') {
            uint8_t dirty = 0;
            int32_t j = md_scan_backtick(s, n, i + 1, &dirty);
            if (j > (int32_t)(i + 1)) {
                uint32_t clen = (uint32_t)j - (i + 1);
                DST_LIT(dst, "<code>");
                if (__builtin_expect(!dirty, 1)) { fast_memcpy(dst, s + i + 1, clen); dst += clen; }
                else dst = html_json_escape_scalar(dst, s + i + 1, clen);
                DST_LIT(dst, "</code>");
                i = (uint32_t)j + 1;
            } else { *dst++ = '`'; i++; }
            continue;
        }
        if (c == '*') {
            if (i + 1 < n && s[i + 1] == '*') {
                uint8_t dirty = 0;
                int32_t j = md_scan_dstar(s, n, i + 2, &dirty);
                if (j > (int32_t)(i + 2)) {
                    uint32_t slen = (uint32_t)j - (i + 2);
                    DST_LIT(dst, "<strong>");
                    if (__builtin_expect(!dirty, 1)) { fast_memcpy(dst, s + i + 2, slen); dst += slen; }
                    else dst = md_inline_ptr_scalar(out, dst, s + i + 2, slen, resolve, rctx);
                    dst = inline_ensure(out, dst, 32u);
                    DST_LIT(dst, "</strong>");
                    i = (uint32_t)j + 2;
                } else { dst[0] = '*'; dst[1] = '*'; dst += 2; i += 2; }
                continue;
            }
            uint8_t dirty = 0;
            int32_t j = md_scan_sstar(s, n, i + 1, &dirty);
            if (j > (int32_t)(i + 1)) {
                uint32_t slen = (uint32_t)j - (i + 1);
                DST_LIT(dst, "<em>");
                if (__builtin_expect(!dirty, 1)) { fast_memcpy(dst, s + i + 1, slen); dst += slen; }
                else dst = md_inline_ptr_scalar(out, dst, s + i + 1, slen, resolve, rctx);
                dst = inline_ensure(out, dst, 32u);
                DST_LIT(dst, "</em>");
                i = (uint32_t)j + 1;
            } else { *dst++ = '*'; i++; }
            continue;
        }
        // '[' link
        uint8_t dirty = 0;
        int32_t k = md_scan_rbracket(s, n, i + 1, &dirty);
        if (k != -1 && (uint32_t)k + 1 < n && s[k + 1] == '(') {
            uint8_t ws = 0, hdirty = 0;
            int32_t m = md_scan_rparen(s, n, (uint32_t)k + 2, &ws, &hdirty);
            if (m > k + 2 && !ws) {
                Str target = { s + k + 2, (uint32_t)(m - (k + 2)) };
                uint32_t ltxt = (uint32_t)k - (i + 1);
                dst = emit_md_link_href(out, dst, target, hdirty, (n - i) * 6u + 64u, resolve, rctx);
                if (__builtin_expect(!dirty, 1)) { fast_memcpy(dst, s + i + 1, ltxt); dst += ltxt; }
                else dst = md_inline_ptr_scalar(out, dst, s + i + 1, ltxt, resolve, rctx);
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

// ---------------- Pure-AVX2 + 16B VEX Dirty-Tracked Mask-Draining Inline Renderer ----------------
#if defined(__AVX2__)
static __attribute__((cold, noinline)) char *md_inline_ptr_avx2(Buf *out, char *dst, const char *s, uint32_t n, LinkResolveFn resolve, void *rctx) {
    dst = inline_ensure(out, dst, n * 6u + 512u);

    const __m256i lo_lut = _mm256_setr_epi8(
        8, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 4, 2 | 4, 0, 2, 0,
        8, 0, 1, 0, 0, 0, 1, 1, 0, 0, 1, 4, 2 | 4, 0, 2, 0
    );
    const __m256i hi_lut = _mm256_setr_epi8(
        0, 0, 1, 2, 0, 4, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 1, 2, 0, 4, 8, 0, 0, 0, 0, 0, 0, 0, 0, 0
    );
    const __m256i v_0f = _mm256_set1_epi8(0x0F);
    const __m256i v_e0 = _mm256_set1_epi8((char)0xE0);
    const __m256i v_z  = _mm256_setzero_si256();

    uint32_t i = 0;
    while (i + 32 <= n) {
        __m256i v = _mm256_loadu_si256((const __m256i *)(s + i));
        _mm256_storeu_si256((__m256i *)dst, v);
        __m256i lo = _mm256_and_si256(v, v_0f);
        __m256i hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), v_0f);
        __m256i hit = _mm256_and_si256(_mm256_shuffle_epi8(lo_lut, lo), _mm256_shuffle_epi8(hi_lut, hi));
        uint32_t hit_mask = ~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(hit, v_z));
        uint32_t ctrl_mask = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_and_si256(v, v_e0), v_z));
        uint32_t mask = hit_mask | ctrl_mask;
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
            else if (run) { copy_short_padded(dst, s + base + cur, run); dst += run; }
            uint32_t pos = base + tz;
            uint8_t c = (uint8_t)s[pos];
            if (__builtin_expect(INLINE_KIND_LUT[c] == 1, 1)) {
                dst += emit_html_json_char(dst, c);
                cur = tz + 1;
                mask &= mask - 1u;
                continue;
            }
            // Instant zero-abort fast-path for backslash escape inside 32B vector!
            if (c == '\\') {
                if (pos + 1 < n && (uint8_t)s[pos + 1] < 128 && MD_ESCAPABLE[(uint8_t)s[pos + 1]]) {
                    uint8_t nc = (uint8_t)s[pos + 1];
                    if (nc == '>') DST_LIT(dst, "&gt;");
                    else if (nc == '\\') { dst[0] = '\\'; dst[1] = '\\'; dst += 2; }
                    else *dst++ = (char)nc;
                    if (tz + 2 < 32u) {
                        cur = tz + 2;
                        mask &= (~0u << cur);
                        continue;
                    }
                    i = pos + 2;
                    goto next_chunk;
                }
                dst[0] = '\\'; dst[1] = '\\'; dst += 2;
                cur = tz + 1;
                mask &= mask - 1u;
                continue;
            }
            // Instant zero-scan bitmask fast path for short `code`, *em*, **strong** closing within same 32B vector!
            uint32_t rem = mask & (mask - 1u);
            if (c == '`' && rem) {
                uint32_t tz2 = (uint32_t)__builtin_ctz(rem);
                if (s[base + tz2] == '`' && tz2 > tz + 1) {
                    uint32_t clen = tz2 - tz - 1;
                    DST_LIT(dst, "<code>");
                    copy_short_padded(dst, s + pos + 1, clen); dst += clen;
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
                                copy_short_padded(dst, s + pos + 2, slen); dst += slen;
                                DST_LIT(dst, "</strong>");
                                if (tz2 + 2 < 32u) {
                                    cur = tz2 + 2;
                                    mask &= (~0u << cur);
                                    continue;
                                }
                                i = base + tz2 + 2;
                                goto next_chunk;
                            }
                        }
                    }
                } else if (rem) {
                    uint32_t tz2 = (uint32_t)__builtin_ctz(rem);
                    if (tz2 > tz + 1 && s[base + tz2] == '*' && (base + tz2 + 1 >= n || s[base + tz2 + 1] != '*')) {
                        uint32_t slen = tz2 - tz - 1;
                        DST_LIT(dst, "<em>");
                        copy_short_padded(dst, s + pos + 1, slen); dst += slen;
                        DST_LIT(dst, "</em>");
                        cur = tz2 + 1;
                        mask = rem & (rem - 1u);
                        continue;
                    }
                }
            }

            // General dirty-tracked construct handler (stays inside AVX2, ZERO vzeroupper!)
            i = pos;
            if (c == '`') {
                uint8_t dirty = 0;
                int32_t j = md_scan_backtick(s, n, i + 1, &dirty);
                if (j > (int32_t)(i + 1)) {
                    uint32_t clen = (uint32_t)j - (i + 1);
                    DST_LIT(dst, "<code>");
                    if (__builtin_expect(!dirty, 1)) { fast_memcpy(dst, s + i + 1, clen); dst += clen; }
                    else dst = html_json_escape_scalar(dst, s + i + 1, clen);
                    DST_LIT(dst, "</code>");
                    i = (uint32_t)j + 1;
                } else { *dst++ = '`'; i++; }
            } else if (c == '*') {
                if (i + 1 < n && s[i + 1] == '*') {
                    uint8_t dirty = 0;
                    int32_t j = md_scan_dstar(s, n, i + 2, &dirty);
                    if (j > (int32_t)(i + 2)) {
                        uint32_t slen = (uint32_t)j - (i + 2);
                        DST_LIT(dst, "<strong>");
                        if (__builtin_expect(!dirty, 1)) { fast_memcpy(dst, s + i + 2, slen); dst += slen; }
                        else dst = md_inline_ptr_avx2(out, dst, s + i + 2, slen, resolve, rctx);
                        DST_LIT(dst, "</strong>");
                        i = (uint32_t)j + 2;
                    } else { dst[0] = '*'; dst[1] = '*'; dst += 2; i += 2; }
                } else {
                    uint8_t dirty = 0;
                    int32_t j = md_scan_sstar(s, n, i + 1, &dirty);
                    if (j > (int32_t)(i + 1)) {
                        uint32_t slen = (uint32_t)j - (i + 1);
                        DST_LIT(dst, "<em>");
                        if (__builtin_expect(!dirty, 1)) { fast_memcpy(dst, s + i + 1, slen); dst += slen; }
                        else dst = md_inline_ptr_avx2(out, dst, s + i + 1, slen, resolve, rctx);
                        DST_LIT(dst, "</em>");
                        i = (uint32_t)j + 1;
                    } else { *dst++ = '*'; i++; }
                }
            } else { // '['
                uint8_t dirty = 0;
                int32_t k = md_scan_rbracket(s, n, i + 1, &dirty);
                if (k != -1 && (uint32_t)k + 1 < n && s[k + 1] == '(') {
                    uint8_t ws = 0, hdirty = 0;
                    int32_t m = md_scan_rparen(s, n, (uint32_t)k + 2, &ws, &hdirty);
                    if (m > k + 2 && !ws) {
                        Str target = { s + k + 2, (uint32_t)(m - (k + 2)) };
                        uint32_t ltxt = (uint32_t)k - (i + 1);
                        dst = emit_md_link_href(out, dst, target, hdirty, (n - i) * 6u + 512u, resolve, rctx);
                        if (__builtin_expect(!dirty, 1)) { fast_memcpy(dst, s + i + 1, ltxt); dst += ltxt; }
                        else dst = md_inline_ptr_avx2(out, dst, s + i + 1, ltxt, resolve, rctx);
                        DST_LIT(dst, "</a>");
                        i = (uint32_t)m + 1;
                        goto next_chunk;
                    }
                }
                *dst++ = '['; i++;
            }
            if (i < base + 32u) {
                cur = i - base;
                mask &= (~0u << cur);
                continue;
            }
            goto next_chunk;
        }
        if (cur < 32u) { copy_short_padded(dst, s + base + cur, 32u - cur); dst += 32u - cur; }
        i = base + 32;
    next_chunk:;
    }

    // 16-byte VEX-encoded SIMD fast-path for 16..31B lines & tails! (ZERO vzeroupper!)
    if (i + 16 <= n) {
        const __m128i lo16 = _mm256_castsi256_si128(lo_lut);
        const __m128i hi16 = _mm256_castsi256_si128(hi_lut);
        const __m128i v0f16 = _mm256_castsi256_si128(v_0f);
        const __m128i ve016 = _mm256_castsi256_si128(v_e0);
        const __m128i vz16  = _mm_setzero_si128();
        __m128i v = _mm_loadu_si128((const __m128i *)(s + i));
        _mm_storeu_si128((__m128i *)dst, v);
        __m128i lo = _mm_and_si128(v, v0f16);
        __m128i hi = _mm_and_si128(_mm_srli_epi16(v, 4), v0f16);
        __m128i hit = _mm_and_si128(_mm_shuffle_epi8(lo16, lo), _mm_shuffle_epi8(hi16, hi));
        uint32_t hm = (~(uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(hit, vz16))) & 0xFFFFu;
        uint32_t cm = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_and_si128(v, ve016), vz16));
        uint32_t mask = hm | cm;
        if (__builtin_expect(mask == 0, 1)) {
            dst += 16;
            i += 16;
        } else {
            uint32_t tz = (uint32_t)__builtin_ctz(mask);
            dst += tz;
            i += tz;
        }
    }

    if (i < n) dst = md_inline_ptr_scalar(out, dst, s + i, n - i, resolve, rctx);
    return dst;
}
#endif

static inline char *md_inline_ptr(Buf *out, char *dst, const char *s, uint32_t n, LinkResolveFn resolve, void *rctx) {
#if defined(__AVX2__)
    if (n >= 16) return md_inline_ptr_avx2(out, dst, s, n, resolve, rctx);
    return md_inline_ptr_scalar(out, dst, s, n, resolve, rctx);
#else
    return md_inline_ptr_scalar(out, dst, s, n, resolve, rctx);
#endif
}

// ---------------- Heading ID Hash Set ----------------
#define HID_CAP 512
typedef struct {
    Str keys[HID_CAP];
    uint32_t hashes[HID_CAP];
    uint32_t used[HID_CAP];
    uint32_t gen;
} HeadingIdSet;

static inline int hid_has(HeadingIdSet *set, Str k, uint32_t h) {
    uint32_t mask = HID_CAP - 1;
    uint32_t idx = h & mask;
    while (set->used[idx] == set->gen) {
        if (set->hashes[idx] == h && set->keys[idx].n == k.n && fast_memeq(set->keys[idx].p, k.p, k.n)) return 1;
        idx = (idx + 1) & mask;
    }
    return 0;
}

static inline void hid_add(HeadingIdSet *set, Str k, uint32_t h) {
    uint32_t mask = HID_CAP - 1;
    uint32_t idx = h & mask;
    while (set->used[idx] == set->gen) idx = (idx + 1) & mask;
    set->used[idx] = set->gen;
    set->hashes[idx] = h;
    set->keys[idx] = k;
}

static inline Str md_unique_id(HeadingIdSet *set, Arena *scratch, const char *base, uint32_t blen) {
    uint32_t h = fnv1a_hash(base, blen);
    if (__builtin_expect(!hid_has(set, (Str){base, blen}, h), 1)) {
        char *cp = arena_alloc(scratch, blen);
        fast_memcpy(cp, base, blen);
        Str res = { cp, blen };
        hid_add(set, res, h);
        return res;
    }
    char tmp[80];
    fast_memcpy(tmp, base, blen);
    tmp[blen] = '-';
    for (uint32_t n = 2; ; n++) {
        uint32_t dlen = u32_to_str(n, tmp + blen + 1);
        uint32_t tlen = blen + 1 + dlen;
        uint32_t th = fnv1a_hash(tmp, tlen);
        if (!hid_has(set, (Str){tmp, tlen}, th)) {
            char *cp = arena_alloc(scratch, tlen);
            fast_memcpy(cp, tmp, tlen);
            Str res = { cp, tlen };
            hid_add(set, res, th);
            return res;
        }
    }
}

typedef struct {
    Str html;
    Str excerpt;
    TocItem *toc;
    uint32_t toc_len;
} MdResult;

static __attribute__((cold, noinline)) MdResult render_markdown_json(
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

    #define NEXT_LINE() do { \
        if (eof) { has_line = 0; } \
        else { \
            const char *nl = find_nl(cur, end); \
            lp = cur; \
            llen = (uint32_t)(nl - cur); \
            if (nl < end) cur = nl + 1; \
            else { cur = end; eof = 1; } \
            has_line = 1; \
        } \
    } while (0)

    NEXT_LINE();
    while (has_line) {
        if (llen == 0) { NEXT_LINE(); continue; }
        uint8_t c0 = (uint8_t)lp[0];
        if (__builtin_expect(!BLOCK_LEAD_LUT[c0], 1)) goto parse_para;

        if ((c0 == ' ' || c0 == '\t') && md_is_blank(lp, llen)) {
            NEXT_LINE();
            continue;
        }
        if (c0 == '`') {
            Str lang;
            if (md_parse_fence_open(lp, llen, &lang)) {
                dst = inline_ensure(html_buf, dst, lang.n * 6u + 64u);
                if (lang.n == 0) {
                    DST_LIT(dst, "<pre><code>");
                } else {
                    DST_LIT(dst, "<pre><code class=\\\"language-");
                    dst = html_json_escape_to(dst, lang.p, lang.n);
                    DST_LIT(dst, "\\\">");
                }
                NEXT_LINE();
                const char *fstart = lp;
                int had_fence_line = 0;
                while (has_line && !md_is_fence_close(lp, llen)) {
                    had_fence_line = 1;
                    NEXT_LINE();
                }
                if (has_line) {
                    // Closed fence: [fstart, lp) is contiguous and already ends with '\n'! Single AVX2 pass!
                    uint32_t flen = (uint32_t)(lp - fstart);
                    if (flen > 0) {
                        dst = inline_ensure(html_buf, dst, flen * 6u + 64u);
                        dst = html_json_escape_to(dst, fstart, flen);
                    }
                    NEXT_LINE();
                } else if (had_fence_line) {
                    // Unclosed fence at EOF: escape [fstart, end) + trailing \n
                    uint32_t flen = (uint32_t)(end - fstart);
                    if (flen > 0) {
                        dst = inline_ensure(html_buf, dst, flen * 6u + 64u);
                        dst = html_json_escape_to(dst, fstart, flen);
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
            if (md_parse_heading(lp, llen, &hdepth, &htext)) {
                char slug_tmp[64];
                uint32_t slen = slugify_into(htext, slug_tmp);
                Str id = md_unique_id(hset, id_arena, slug_tmp, slen);
                dst = inline_ensure(html_buf, dst, 64u + id.n + htext.n * 6u);
                dst[0] = '<'; dst[1] = 'h'; dst[2] = (char)('0' + hdepth); dst += 3;
                DST_LIT(dst, " id=\\\"");
                fast_memcpy(dst, id.p, id.n); dst += id.n;
                DST_LIT(dst, "\\\">");
                uint32_t h_off = (uint32_t)(dst - html_buf->p);
                dst = md_inline_ptr(html_buf, dst, htext.p, htext.n, resolve, rctx);
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
                NEXT_LINE();
                continue;
            }
        }
        if ((c0 == '-' || c0 == '*') && md_is_hr(lp, llen)) {
            dst = inline_ensure(html_buf, dst, 16u);
            DST_LIT(dst, "<hr>\\n");
            NEXT_LINE();
            continue;
        }
        if (c0 == '>') {
            join_buf->n = 0;
            while (has_line && llen > 0 && lp[0] == '>') {
                uint32_t skip = (llen > 1 && lp[1] == ' ') ? 2u : 1u;
                Str tr = md_trim_st(lp + skip, llen - skip);
                if (tr.n > 0) {
                    if (join_buf->n > 0) buf_char(join_buf, '\n');
                    buf_raw(join_buf, tr.p, tr.n);
                }
                NEXT_LINE();
            }
            dst = inline_ensure(html_buf, dst, 64u);
            if (join_buf->n == 0) {
                DST_LIT(dst, "<blockquote>\\n</blockquote>\\n");
            } else {
                buf_ensure(join_buf, 64u); // pad for safe SIMD over-reads
                DST_LIT(dst, "<blockquote>\\n<p>");
                dst = md_inline_ptr(html_buf, dst, join_buf->p, join_buf->n, resolve, rctx);
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
                            item_first = md_trim_st(lp + 2, llen - 2);
                        }
                    } else {
                        uint32_t dummy_s; Str r;
                        if (md_parse_ordered(lp, llen, &dummy_s, &r)) {
                            matched = 1;
                            item_first = md_trim_st(r.p, r.n);
                        }
                    }
                    if (matched) {
                        if (have_item) {
                            dst = inline_ensure(html_buf, dst, 32u);
                            DST_LIT(dst, "<li>");
                            if (!item_in_join) {
                                dst = md_inline_ptr(html_buf, dst, single_item.p, single_item.n, resolve, rctx);
                            } else {
                                buf_ensure(join_buf, 64u);
                                dst = md_inline_ptr(html_buf, dst, join_buf->p, join_buf->n, resolve, rctx);
                            }
                            dst = inline_ensure(html_buf, dst, 16u);
                            DST_LIT(dst, "</li>\\n");
                        }
                        single_item = item_first;
                        item_in_join = 0;
                        have_item = 1;
                        NEXT_LINE();
                    } else if (have_item && md_is_continued(lp, llen)) {
                        if (!item_in_join) {
                            join_buf->n = 0;
                            buf_raw(join_buf, single_item.p, single_item.n);
                            item_in_join = 1;
                        }
                        Str tr = md_trim_st(lp, llen);
                        buf_char(join_buf, '\n');
                        buf_raw(join_buf, tr.p, tr.n);
                        NEXT_LINE();
                    } else break;
                }
                if (have_item) {
                    dst = inline_ensure(html_buf, dst, 32u);
                    DST_LIT(dst, "<li>");
                    if (!item_in_join) {
                        dst = md_inline_ptr(html_buf, dst, single_item.p, single_item.n, resolve, rctx);
                    } else {
                        buf_ensure(join_buf, 64u);
                        dst = md_inline_ptr(html_buf, dst, join_buf->p, join_buf->n, resolve, rctx);
                    }
                    dst = inline_ensure(html_buf, dst, 16u);
                    DST_LIT(dst, "</li>\\n");
                }
                dst = inline_ensure(html_buf, dst, 16u);
                if (is_ordered) DST_LIT(dst, "</ol>\\n");
                else DST_LIT(dst, "</ul>\\n");
                continue;
            }
        }

    parse_para:;
        const char *para_start = lp;
        const char *para_end = lp + llen;
        int contiguous = (lp[0] != ' ' && lp[0] != '\t' && lp[llen - 1] != ' ' && lp[llen - 1] != '\t');
        Str first = contiguous ? (Str){lp, llen} : md_trim_st(lp, llen);
        NEXT_LINE();
        if (!has_line || md_ends_paragraph(lp, llen)) {
            dst = inline_ensure(html_buf, dst, 32u);
            DST_LIT(dst, "<p>");
            uint32_t p_off = (uint32_t)(dst - html_buf->p);
            dst = md_inline_ptr(html_buf, dst, first.p, first.n, resolve, rctx);
            uint32_t p_len = (uint32_t)(dst - html_buf->p) - p_off;
            dst = inline_ensure(html_buf, dst, 16u);
            DST_LIT(dst, "</p>\\n");
            if (!has_excerpt) { has_excerpt = 1; ex_off = p_off; ex_len = p_len; }
        } else {
            if (!contiguous) {
                join_buf->n = 0;
                buf_raw(join_buf, first.p, first.n);
            }
            while (has_line && !md_ends_paragraph(lp, llen)) {
                int clean = (llen > 0 && lp[0] != ' ' && lp[0] != '\t' && lp[llen - 1] != ' ' && lp[llen - 1] != '\t');
                if (contiguous && clean) {
                    para_end = lp + llen;
                } else {
                    if (contiguous) {
                        join_buf->n = 0;
                        buf_raw(join_buf, para_start, (uint32_t)(para_end - para_start));
                        contiguous = 0;
                    }
                    Str tr = clean ? (Str){lp, llen} : md_trim_st(lp, llen);
                    buf_char(join_buf, '\n');
                    buf_raw(join_buf, tr.p, tr.n);
                }
                NEXT_LINE();
            }
            dst = inline_ensure(html_buf, dst, 32u);
            DST_LIT(dst, "<p>");
            uint32_t p_off = (uint32_t)(dst - html_buf->p);
            if (contiguous) {
                dst = md_inline_ptr(html_buf, dst, para_start, (uint32_t)(para_end - para_start), resolve, rctx);
            } else {
                buf_ensure(join_buf, 64u);
                dst = md_inline_ptr(html_buf, dst, join_buf->p, join_buf->n, resolve, rctx);
            }
            uint32_t p_len = (uint32_t)(dst - html_buf->p) - p_off;
            dst = inline_ensure(html_buf, dst, 16u);
            DST_LIT(dst, "</p>\\n");
            if (!has_excerpt) { has_excerpt = 1; ex_off = p_off; ex_len = p_len; }
        }
    }
    #undef NEXT_LINE

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
