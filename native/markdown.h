#pragma once
#include "simd_text.h"

// Exact makefun-site 1.0.0 Markdown renderer emitting directly into pre-JSON-escaped HTML!
// Uses runtime AVX2 PSHUFB speculative-store inline scanner + single reusable scratch buffer.

typedef struct {
    uint8_t level;
    Str id;
    Str html; // pre-JSON-escaped inline HTML (or raw if raw_mode)
} TocItem;

// Callback to resolve a Markdown link target -> href Str (raw, unescaped).
typedef Str (*LinkResolveFn)(void *ctx, Str target);

// 128-byte LUT for Markdown backslash-escapable characters: \ ` * _ [ ] ( ) # + - . ! >
static const uint8_t MD_ESCAPABLE[128] = {
    ['\\'] = 1, ['`'] = 1, ['*'] = 1, ['_'] = 1, ['['] = 1, [']'] = 1,
    ['('] = 1, [')'] = 1, ['#'] = 1, ['+'] = 1, ['-'] = 1, ['.'] = 1,
    ['!'] = 1, ['>'] = 1,
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
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
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
    if (md_is_blank(p, n)) return 1;
    char c0 = p[0];
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
    // Check \S (not JS whitespace)
    uint8_t b = (uint8_t)p[i];
    if (b < 0x80) return !(b == ' ' || (b >= 9 && b <= 13));
    uint32_t cl;
    uint32_t cp = utf8_decode((const uint8_t *)(p + i), (const uint8_t *)(p + n), &cl);
    return !is_js_ws_cp(cp);
}

// Inline search helpers matching lib/markdown.js byte-for-byte
static inline int32_t md_find_unescaped2(const char *s, uint32_t n, char c1, char c2, uint32_t from) {
    if (n < 2) return -1;
    uint32_t lim = n - 1;
    for (uint32_t j = from; j < lim; j++) {
        uint8_t c = (uint8_t)s[j];
        if (c == '\\' && j + 1 < n) {
            uint8_t nc = (uint8_t)s[j + 1];
            if (nc < 128 && MD_ESCAPABLE[nc]) { j++; continue; }
        }
        if (c == (uint8_t)c1 && s[j + 1] == c2) return (int32_t)j;
    }
    return -1;
}

static inline int32_t md_find_unescaped1(const char *s, uint32_t n, char target, uint32_t from) {
    for (uint32_t j = from; j < n; j++) {
        uint8_t c = (uint8_t)s[j];
        if (c == '\\' && j + 1 < n) {
            uint8_t nc = (uint8_t)s[j + 1];
            if (nc < 128 && MD_ESCAPABLE[nc]) { j++; continue; }
        }
        if (c == (uint8_t)target) return (int32_t)j;
    }
    return -1;
}

static inline int32_t md_find_single_star(const char *s, uint32_t n, uint32_t from) {
    for (uint32_t j = from; j < n; j++) {
        uint8_t c = (uint8_t)s[j];
        if (c == '\\' && j + 1 < n) {
            uint8_t nc = (uint8_t)s[j + 1];
            if (nc < 128 && MD_ESCAPABLE[nc]) { j++; continue; }
        }
        if (c != '*') continue;
        if (j + 1 < n && s[j + 1] == '*') { j++; continue; }
        return (int32_t)j;
    }
    return -1;
}

static inline int32_t md_find_char(const char *s, uint32_t n, char target, uint32_t from) {
    for (uint32_t j = from; j < n; j++) {
        if (s[j] == target) return (int32_t)j;
    }
    return -1;
}

// Forward declaration for recursive inline rendering (pre-JSON-escaped mode!)
static void md_inline_json(Buf *out, const char *s, uint32_t n, LinkResolveFn resolve, void *rctx);

#if defined(__x86_64__)
__attribute__((target("avx2")))
static void md_inline_json_avx2(Buf *out, const char *s, uint32_t n, LinkResolveFn resolve, void *rctx) {
    // Special bytes >= 0x20: " (22), & (26), ' (27), * (2A), < (3C), > (3E), [ (5B), \ (5C), ` (60)
    // Exact zero-false-positive PSHUFB nibble lookup!
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
    while (i < n) {
        if (i + 32 <= n) {
            buf_ensure(out, 64);
            __m256i v = _mm256_loadu_si256((const __m256i *)(s + i));
            _mm256_storeu_si256((__m256i *)(out->p + out->n), v);
            __m256i lo = _mm256_and_si256(v, v_0f);
            __m256i hi = _mm256_and_si256(_mm256_srli_epi16(v, 4), v_0f);
            __m256i hit = _mm256_and_si256(_mm256_shuffle_epi8(lo_lut, lo), _mm256_shuffle_epi8(hi_lut, hi));
            uint32_t hit_mask = ~(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(hit, v_z));
            uint32_t ctrl_mask = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_and_si256(v, v_e0), v_z));
            uint32_t mask = hit_mask | ctrl_mask;
            if (__builtin_expect(mask == 0, 1)) {
                out->n += 32;
                i += 32;
                continue;
            }
            uint32_t tz = (uint32_t)__builtin_ctz(mask);
            out->n += tz;
            i += tz;
        }
        uint8_t c = (uint8_t)s[i];
        if (c == '\\') {
            if (i + 1 < n) {
                uint8_t nc = (uint8_t)s[i + 1];
                if (nc < 128 && MD_ESCAPABLE[nc]) {
                    if (nc == '>') BUF_LIT(out, "&gt;");
                    else if (nc == '\\') BUF_LIT(out, "\\\\");
                    else buf_char(out, (char)nc);
                    i += 2;
                    continue;
                }
            }
            BUF_LIT(out, "\\\\");
            i++;
            continue;
        }
        if (c == '`') {
            int32_t j = md_find_char(s, n, '`', i + 1);
            if (j > (int32_t)(i + 1)) {
                BUF_LIT(out, "<code>");
                buf_html_json_escaped(out, s + i + 1, (uint32_t)j - (i + 1));
                BUF_LIT(out, "</code>");
                i = (uint32_t)j + 1;
            } else {
                buf_char(out, '`');
                i++;
            }
            continue;
        }
        if (c == '*') {
            if (i + 1 < n && s[i + 1] == '*') {
                int32_t j = md_find_unescaped2(s, n, '*', '*', i + 2);
                if (j > (int32_t)(i + 2)) {
                    BUF_LIT(out, "<strong>");
                    md_inline_json_avx2(out, s + i + 2, (uint32_t)j - (i + 2), resolve, rctx);
                    BUF_LIT(out, "</strong>");
                    i = (uint32_t)j + 2;
                } else {
                    BUF_LIT(out, "**");
                    i += 2;
                }
                continue;
            }
            int32_t j = md_find_single_star(s, n, i + 1);
            if (j > (int32_t)(i + 1)) {
                BUF_LIT(out, "<em>");
                md_inline_json_avx2(out, s + i + 1, (uint32_t)j - (i + 1), resolve, rctx);
                BUF_LIT(out, "</em>");
                i = (uint32_t)j + 1;
            } else {
                buf_char(out, '*');
                i++;
            }
            continue;
        }
        if (c == '[') {
            int32_t k = md_find_unescaped1(s, n, ']', i + 1);
            if (k != -1 && (uint32_t)k + 1 < n && s[k + 1] == '(') {
                int32_t m = md_find_char(s, n, ')', (uint32_t)k + 2);
                if (m > k + 2) {
                    Str target = { s + k + 2, (uint32_t)(m - (k + 2)) };
                    if (!has_js_ws(target)) {
                        Str href = resolve(rctx, target);
                        BUF_LIT(out, "<a href=\\\"");
                        buf_html_json_escaped(out, href.p, href.n);
                        BUF_LIT(out, "\\\">");
                        md_inline_json_avx2(out, s + i + 1, (uint32_t)k - (i + 1), resolve, rctx);
                        BUF_LIT(out, "</a>");
                        i = (uint32_t)m + 1;
                        continue;
                    }
                }
            }
            buf_char(out, '[');
            i++;
            continue;
        }
        if (c < 0x20 || c == '&' || c == '<' || c == '>' || c == '"' || c == '\'') {
            buf_ensure(out, 8);
            out->n += emit_html_json_char(out->p + out->n, c);
            i++;
            continue;
        }
        buf_char(out, (char)c);
        i++;
    }
}
#endif

static void md_inline_json_scalar(Buf *out, const char *s, uint32_t n, LinkResolveFn resolve, void *rctx) {
    uint32_t i = 0;
    while (i < n) {
        uint8_t c = (uint8_t)s[i];
        if (__builtin_expect(c >= 0x20 && c != '\\' && c != '`' && c != '*' && c != '[' &&
                             c != '&' && c != '<' && c != '>' && c != '"' && c != '\'', 1)) {
            buf_char(out, (char)c);
            i++;
            continue;
        }
        if (c == '\\') {
            if (i + 1 < n) {
                uint8_t nc = (uint8_t)s[i + 1];
                if (nc < 128 && MD_ESCAPABLE[nc]) {
                    if (nc == '>') BUF_LIT(out, "&gt;");
                    else if (nc == '\\') BUF_LIT(out, "\\\\");
                    else buf_char(out, (char)nc);
                    i += 2;
                    continue;
                }
            }
            BUF_LIT(out, "\\\\");
            i++;
            continue;
        }
        if (c == '`') {
            int32_t j = md_find_char(s, n, '`', i + 1);
            if (j > (int32_t)(i + 1)) {
                BUF_LIT(out, "<code>");
                buf_html_json_escaped(out, s + i + 1, (uint32_t)j - (i + 1));
                BUF_LIT(out, "</code>");
                i = (uint32_t)j + 1;
            } else {
                buf_char(out, '`');
                i++;
            }
            continue;
        }
        if (c == '*') {
            if (i + 1 < n && s[i + 1] == '*') {
                int32_t j = md_find_unescaped2(s, n, '*', '*', i + 2);
                if (j > (int32_t)(i + 2)) {
                    BUF_LIT(out, "<strong>");
                    md_inline_json_scalar(out, s + i + 2, (uint32_t)j - (i + 2), resolve, rctx);
                    BUF_LIT(out, "</strong>");
                    i = (uint32_t)j + 2;
                } else {
                    BUF_LIT(out, "**");
                    i += 2;
                }
                continue;
            }
            int32_t j = md_find_single_star(s, n, i + 1);
            if (j > (int32_t)(i + 1)) {
                BUF_LIT(out, "<em>");
                md_inline_json_scalar(out, s + i + 1, (uint32_t)j - (i + 1), resolve, rctx);
                BUF_LIT(out, "</em>");
                i = (uint32_t)j + 1;
            } else {
                buf_char(out, '*');
                i++;
            }
            continue;
        }
        if (c == '[') {
            int32_t k = md_find_unescaped1(s, n, ']', i + 1);
            if (k != -1 && (uint32_t)k + 1 < n && s[k + 1] == '(') {
                int32_t m = md_find_char(s, n, ')', (uint32_t)k + 2);
                if (m > k + 2) {
                    Str target = { s + k + 2, (uint32_t)(m - (k + 2)) };
                    if (!has_js_ws(target)) {
                        Str href = resolve(rctx, target);
                        BUF_LIT(out, "<a href=\\\"");
                        buf_html_json_escaped(out, href.p, href.n);
                        BUF_LIT(out, "\\\">");
                        md_inline_json_scalar(out, s + i + 1, (uint32_t)k - (i + 1), resolve, rctx);
                        BUF_LIT(out, "</a>");
                        i = (uint32_t)m + 1;
                        continue;
                    }
                }
            }
            buf_char(out, '[');
            i++;
            continue;
        }
        buf_ensure(out, 8);
        out->n += emit_html_json_char(out->p + out->n, c);
        i++;
    }
}

static void md_inline_json(Buf *out, const char *s, uint32_t n, LinkResolveFn resolve, void *rctx) {
#if defined(__x86_64__)
    if (g_has_avx2) { md_inline_json_avx2(out, s, n, resolve, rctx); return; }
#endif
    md_inline_json_scalar(out, s, n, resolve, rctx);
}

// Heading ID hash set (per document, small open-addressed table)
#define HID_CAP 512
typedef struct {
    Str keys[HID_CAP];
    uint32_t hashes[HID_CAP];
    uint32_t used[HID_CAP]; // generation stamp! Zero memset needed between pages!
    uint32_t gen;
} HeadingIdSet;

static inline int hid_has(HeadingIdSet *set, Str k, uint32_t h) {
    uint32_t mask = HID_CAP - 1;
    uint32_t idx = h & mask;
    while (set->used[idx] == set->gen) {
        if (set->hashes[idx] == h && set->keys[idx].n == k.n && fast_memeq(set->keys[idx].p, k.p, k.n)) {
            return 1;
        }
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
    if (!hid_has(set, (Str){base, blen}, h)) {
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
    Str html;          // pre-JSON-escaped HTML
    Str excerpt;       // pre-JSON-escaped excerpt (slice of html!)
    TocItem *toc;
    uint32_t toc_len;
} MdResult;

// Renders Markdown `src` into reusable `html_buf`, `join_buf`, `toc_arr`, `id_arena`.
// Returns slices into `html_buf` (valid until next page!).
static MdResult render_markdown_json(
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
    hset->gen++;
    uint32_t toc_len = 0;
    int has_excerpt = 0;
    uint32_t ex_off = 0, ex_len = 0;

    const char *p = src.p;
    const char *end = src.p + src.n;

    // Split lines on the fly without allocating a lines array!
    // Helper macro to read next line: (lp, llen), advancing `p`.
    const char *lp;
    uint32_t llen;
    int has_line = 0;

    // Notice JS `"".split('\n')` yields `[""]`, and `"a\n".split('\n')` yields `["a", ""]`
    // All trailing empty lines are blank and skipped by `md_is_blank`, except inside unclosed code fence!
    // So we track exact line boundaries including trailing empty line after final '\n' (or empty string).
    const char *cur = p;
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
        if (md_is_blank(lp, llen)) {
            NEXT_LINE();
            continue;
        }
        // Fenced code
        Str lang;
        if (lp[0] == '`' && md_parse_fence_open(lp, llen, &lang)) {
            if (lang.n == 0) {
                BUF_LIT(html_buf, "<pre><code>");
            } else {
                BUF_LIT(html_buf, "<pre><code class=\\\"language-");
                buf_html_json_escaped(html_buf, lang.p, lang.n);
                BUF_LIT(html_buf, "\\\">");
            }
            NEXT_LINE();
            while (has_line && !md_is_fence_close(lp, llen)) {
                buf_html_json_escaped(html_buf, lp, llen);
                BUF_LIT(html_buf, "\\n");
                NEXT_LINE();
            }
            if (has_line) NEXT_LINE(); // skip closing fence
            BUF_LIT(html_buf, "</code></pre>\\n");
            continue;
        }
        // ATX Heading
        uint8_t hdepth;
        Str htext;
        if (lp[0] == '#' && md_parse_heading(lp, llen, &hdepth, &htext)) {
            char slug_tmp[64];
            uint32_t slen = slugify_into(htext, slug_tmp);
            Str id = md_unique_id(hset, id_arena, slug_tmp, slen);
            buf_ensure(html_buf, 32 + id.n);
            buf_char(html_buf, '<');
            buf_char(html_buf, 'h');
            buf_char(html_buf, (char)('0' + hdepth));
            BUF_LIT(html_buf, " id=\\\"");
            buf_raw(html_buf, id.p, id.n);
            BUF_LIT(html_buf, "\\\">");
            uint32_t h_off = html_buf->n;
            md_inline_json(html_buf, htext.p, htext.n, resolve, rctx);
            uint32_t h_len = html_buf->n - h_off;
            BUF_LIT(html_buf, "</h");
            buf_char(html_buf, (char)('0' + hdepth));
            BUF_LIT(html_buf, ">\\n");
            if ((hdepth == 2 || hdepth == 3) && toc_len < toc_cap) {
                // Store offset temporarily in .p via base-relative fixup after loop (in case html_buf grew!)
                toc_arr[toc_len].level = hdepth;
                toc_arr[toc_len].id = id;
                toc_arr[toc_len].html.p = (const char *)(uintptr_t)h_off;
                toc_arr[toc_len].html.n = h_len;
                toc_len++;
            }
            NEXT_LINE();
            continue;
        }
        // HR
        if ((lp[0] == '-' || lp[0] == '*') && md_is_hr(lp, llen)) {
            BUF_LIT(html_buf, "<hr>\\n");
            NEXT_LINE();
            continue;
        }
        // Blockquote
        if (lp[0] == '>') {
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
            if (join_buf->n == 0) {
                BUF_LIT(html_buf, "<blockquote>\\n</blockquote>\\n");
            } else {
                BUF_LIT(html_buf, "<blockquote>\\n<p>");
                md_inline_json(html_buf, join_buf->p, join_buf->n, resolve, rctx);
                BUF_LIT(html_buf, "</p>\\n</blockquote>\\n");
            }
            continue;
        }
        // Lists (bullet or ordered)
        int is_bullet = (llen >= 2 && (lp[0] == '-' || lp[0] == '*') && lp[1] == ' ');
        uint32_t ord_start = 1;
        Str ord_rest = {0, 0};
        int is_ordered = !is_bullet && md_parse_ordered(lp, llen, &ord_start, &ord_rest);
        if (is_bullet || is_ordered) {
            if (is_ordered) {
                if (ord_start != 1) {
                    BUF_LIT(html_buf, "<ol start=\\\"");
                    char num[12];
                    uint32_t nl = u32_to_str(ord_start, num);
                    buf_raw(html_buf, num, nl);
                    BUF_LIT(html_buf, "\\\">\\n");
                } else {
                    BUF_LIT(html_buf, "<ol>\\n");
                }
            } else {
                BUF_LIT(html_buf, "<ul>\\n");
            }
            join_buf->n = 0;
            int have_item = 0;
            while (has_line) {
                int matched = 0;
                Str item_first = {0, 0};
                if (!is_ordered) {
                    if (llen >= 2 && (lp[0] == '-' || lp[0] == '*') && lp[1] == ' ') {
                        matched = 1;
                        item_first = md_trim_st(lp + 2, llen - 2);
                    }
                } else {
                    uint32_t dummy_s;
                    Str r;
                    if (md_parse_ordered(lp, llen, &dummy_s, &r)) {
                        matched = 1;
                        item_first = md_trim_st(r.p, r.n);
                    }
                }
                if (matched) {
                    if (have_item) {
                        BUF_LIT(html_buf, "<li>");
                        md_inline_json(html_buf, join_buf->p, join_buf->n, resolve, rctx);
                        BUF_LIT(html_buf, "</li>\\n");
                    }
                    join_buf->n = 0;
                    buf_raw(join_buf, item_first.p, item_first.n);
                    have_item = 1;
                    NEXT_LINE();
                } else if (have_item && md_is_continued(lp, llen)) {
                    Str tr = md_trim_st(lp, llen);
                    buf_char(join_buf, '\n');
                    buf_raw(join_buf, tr.p, tr.n);
                    NEXT_LINE();
                } else {
                    break;
                }
            }
            if (have_item) {
                BUF_LIT(html_buf, "<li>");
                md_inline_json(html_buf, join_buf->p, join_buf->n, resolve, rctx);
                BUF_LIT(html_buf, "</li>\\n");
            }
            if (is_ordered) BUF_LIT(html_buf, "</ol>\\n");
            else BUF_LIT(html_buf, "</ul>\\n");
            continue;
        }
        // Paragraph (zero-copy single-line fast path!)
        Str first = md_trim_st(lp, llen);
        NEXT_LINE();
        if (!has_line || md_ends_paragraph(lp, llen)) {
            BUF_LIT(html_buf, "<p>");
            uint32_t p_off = html_buf->n;
            md_inline_json(html_buf, first.p, first.n, resolve, rctx);
            uint32_t p_len = html_buf->n - p_off;
            BUF_LIT(html_buf, "</p>\\n");
            if (!has_excerpt) { has_excerpt = 1; ex_off = p_off; ex_len = p_len; }
        } else {
            join_buf->n = 0;
            buf_raw(join_buf, first.p, first.n);
            while (has_line && !md_ends_paragraph(lp, llen)) {
                Str tr = md_trim_st(lp, llen);
                buf_char(join_buf, '\n');
                buf_raw(join_buf, tr.p, tr.n);
                NEXT_LINE();
            }
            BUF_LIT(html_buf, "<p>");
            uint32_t p_off = html_buf->n;
            md_inline_json(html_buf, join_buf->p, join_buf->n, resolve, rctx);
            uint32_t p_len = html_buf->n - p_off;
            BUF_LIT(html_buf, "</p>\\n");
            if (!has_excerpt) { has_excerpt = 1; ex_off = p_off; ex_len = p_len; }
        }
    }
    #undef NEXT_LINE

    // Fix up TOC pointers relative to final html_buf->p!
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
