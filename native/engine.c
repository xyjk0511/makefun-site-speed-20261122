// Freestanding zero-libc SIMD static site generator engine (100% byte-exact to makefun-site 1.0.0).
#include "template.h"

// ---------------- Reusable .bss Arenas (zero-filled lazily by OS, only touched pages fault!) ----------------
#define ARENA_CAP (32u * 1024u * 1024u)
static char g_arena_mem[ARENA_CAP];
static char g_id_arena_mem[65536];
static char g_md_buf_init[131072];
static char g_join_buf_init[65536];
static HeadingIdSet g_hset;
static TocItem g_toc_scratch[1024];

// ---------------- Generic Open-Addressed String->u32 Hash Map ----------------
typedef struct {
    Str *keys;
    uint32_t *hashes;
    uint32_t *vals;
    uint8_t *used;
    uint32_t mask;
} StrMap;

static void sm_init(StrMap *m, Arena *a, uint32_t cap_pow2) {
    m->keys   = (Str *)arena_alloc_aligned(a, cap_pow2 * (uint32_t)sizeof(Str), 8);
    m->hashes = (uint32_t *)arena_alloc_aligned(a, cap_pow2 * 4u, 4);
    m->vals   = (uint32_t *)arena_alloc_aligned(a, cap_pow2 * 4u, 4);
    m->used   = (uint8_t *)arena_alloc_aligned(a, cap_pow2, 8);
    __builtin_memset(m->used, 0, cap_pow2);
    m->mask = cap_pow2 - 1u;
}

static inline int sm_get(const StrMap *m, Str k, uint32_t h, uint32_t *out_val) {
    uint32_t idx = h & m->mask;
    while (m->used[idx]) {
        if (m->hashes[idx] == h && m->keys[idx].n == k.n && fast_memeq(m->keys[idx].p, k.p, k.n)) {
            if (out_val) *out_val = m->vals[idx];
            return 1;
        }
        idx = (idx + 1u) & m->mask;
    }
    return 0;
}

static inline int sm_put_new(StrMap *m, Str k, uint32_t h, uint32_t val) {
    uint32_t idx = h & m->mask;
    while (m->used[idx]) {
        if (m->hashes[idx] == h && m->keys[idx].n == k.n && fast_memeq(m->keys[idx].p, k.p, k.n)) {
            return 0;
        }
        idx = (idx + 1u) & m->mask;
    }
    m->used[idx] = 1;
    m->hashes[idx] = h;
    m->keys[idx] = k;
    m->vals[idx] = val;
    return 1;
}

static inline uint32_t next_pow2(uint32_t v) {
    uint32_t p = 16;
    while (p < v * 2u) p <<= 1;
    return p;
}

// ---------------- Fast In-Place Sorting Helpers ----------------
typedef struct { Str path; Str content; int has_cr; } InputFile;
typedef struct { Str name; Str src; } InputTpl;

static void sort_tpls(InputTpl *arr, int lo, int hi) {
    while (lo < hi) {
        int i = lo, j = hi;
        Str piv = arr[(lo + hi) >> 1].name;
        while (i <= j) {
            while (cmp_code_units(arr[i].name, piv) < 0) i++;
            while (cmp_code_units(arr[j].name, piv) > 0) j--;
            if (i <= j) {
                InputTpl tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
                i++; j--;
            }
        }
        if (j - lo < hi - i) { if (lo < j) sort_tpls(arr, lo, j); lo = i; }
        else { if (i < hi) sort_tpls(arr, i, hi); hi = j; }
    }
}

static void sort_files(InputFile *arr, int lo, int hi) {
    while (lo < hi) {
        int i = lo, j = hi;
        Str piv = arr[(lo + hi) >> 1].path;
        while (i <= j) {
            while (cmp_code_units(arr[i].path, piv) < 0) i++;
            while (cmp_code_units(arr[j].path, piv) > 0) j--;
            if (i <= j) {
                InputFile tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
                i++; j--;
            }
        }
        if (j - lo < hi - i) { if (lo < j) sort_files(arr, lo, j); lo = i; }
        else { if (i < hi) sort_files(arr, i, hi); hi = j; }
    }
}

static inline int cmp_posts(const PostData *x, const PostData *y) {
    int d = cmp_code_units(y->date, x->date);
    if (d != 0) return d;
    return cmp_code_units(x->slug, y->slug);
}

static void sort_posts(PostData **arr, int lo, int hi) {
    while (lo < hi) {
        int i = lo, j = hi;
        const PostData *piv = arr[(lo + hi) >> 1];
        while (i <= j) {
            while (cmp_posts(arr[i], piv) < 0) i++;
            while (cmp_posts(arr[j], piv) > 0) j--;
            if (i <= j) {
                PostData *tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
                i++; j--;
            }
        }
        if (j - lo < hi - i) { if (lo < j) sort_posts(arr, lo, j); lo = i; }
        else { if (i < hi) sort_posts(arr, i, hi); hi = j; }
    }
}

static inline int cmp_tags(const TagData *x, const TagData *y) {
    if (x->post_count != y->post_count) return (y->post_count > x->post_count) ? 1 : -1;
    return cmp_code_units(x->slug, y->slug);
}

static void sort_tags(TagData *arr, int lo, int hi) {
    while (lo < hi) {
        int i = lo, j = hi;
        TagData piv = arr[(lo + hi) >> 1];
        while (i <= j) {
            while (cmp_tags(&arr[i], &piv) < 0) i++;
            while (cmp_tags(&arr[j], &piv) > 0) j--;
            if (i <= j) {
                TagData tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
                i++; j--;
            }
        }
        if (j - lo < hi - i) { if (lo < j) sort_tags(arr, lo, j); lo = i; }
        else { if (i < hi) sort_tags(arr, i, hi); hi = j; }
    }
}

static void sort_strs(Str *arr, int lo, int hi) {
    while (lo < hi) {
        int i = lo, j = hi;
        Str piv = arr[(lo + hi) >> 1];
        while (i <= j) {
            while (cmp_code_units(arr[i], piv) < 0) i++;
            while (cmp_code_units(arr[j], piv) > 0) j--;
            if (i <= j) {
                Str tmp = arr[i]; arr[i] = arr[j]; arr[j] = tmp;
                i++; j--;
            }
        }
        if (j - lo < hi - i) { if (lo < j) sort_strs(arr, lo, j); lo = i; }
        else { if (i < hi) sort_strs(arr, i, hi); hi = j; }
    }
}

// ---------------- SIMD JSON Parser ----------------
static inline void skip_ws(const char **pp, const char *end) {
    const char *p = *pp;
    while (p < end && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    *pp = p;
}

static inline uint32_t hex_val(char c) {
    if (c >= '0' && c <= '9') return (uint32_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint32_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (uint32_t)(c - 'A' + 10);
    return 0;
}

static inline uint32_t parse_u4(const char *p) {
    return (hex_val(p[0]) << 12) | (hex_val(p[1]) << 8) | (hex_val(p[2]) << 4) | hex_val(p[3]);
}

#if defined(__x86_64__)
__attribute__((target("avx2")))
static Str parse_json_str_avx2(const char **pp, const char *end, Arena *a, int norm_cr, int *out_has_cr) {
    const char *p = *pp + 1; // skip opening '"'
    const char *start = p;
    const __m256i v_qt = _mm256_set1_epi8('"');
    const __m256i v_bs = _mm256_set1_epi8('\\');
    // Fast scan for unescaped string (zero-copy!)
    while (p + 32 <= end) {
        __m256i v = _mm256_loadu_si256((const __m256i *)p);
        uint32_t m = (uint32_t)_mm256_movemask_epi8(_mm256_or_si256(_mm256_cmpeq_epi8(v, v_qt), _mm256_cmpeq_epi8(v, v_bs)));
        if (m) {
            p += __builtin_ctz(m);
            if (*p == '"') {
                *pp = p + 1;
                return (Str){ start, (uint32_t)(p - start) };
            }
            goto has_escape;
        }
        p += 32;
    }
    while (p < end && *p != '"' && *p != '\\') p++;
    if (p < end && *p == '"') {
        *pp = p + 1;
        return (Str){ start, (uint32_t)(p - start) };
    }

has_escape:;
    char *dst_start = a->base + a->pos;
    char *dst = dst_start;
    uint32_t prefix = (uint32_t)(p - start);
    if (prefix) { fast_memcpy(dst, start, prefix); dst += prefix; }
    int has_cr = 0;

    while (p < end) {
        if (p + 32 <= end) {
            __m256i v = _mm256_loadu_si256((const __m256i *)p);
            _mm256_storeu_si256((__m256i *)dst, v);
            uint32_t m = (uint32_t)_mm256_movemask_epi8(_mm256_or_si256(_mm256_cmpeq_epi8(v, v_qt), _mm256_cmpeq_epi8(v, v_bs)));
            if (m == 0) { dst += 32; p += 32; continue; }
            uint32_t tz = (uint32_t)__builtin_ctz(m);
            dst += tz;
            p += tz;
        }
        char c = *p++;
        if (c == '"') break;
        if (c != '\\') { *dst++ = c; continue; }
        if (p >= end) break;
        char e = *p++;
        switch (e) {
            case 'n':  *dst++ = '\n'; break;
            case '"':  *dst++ = '"';  break;
            case '\\': *dst++ = '\\'; break;
            case '/':  *dst++ = '/';  break;
            case 't':  *dst++ = '\t'; break;
            case 'r':
                if (norm_cr) {
                    *dst++ = '\n';
                    if (p + 1 < end && p[0] == '\\' && p[1] == 'n') p += 2;
                } else {
                    *dst++ = '\r';
                    has_cr = 1;
                }
                break;
            case 'b':  *dst++ = '\b'; break;
            case 'f':  *dst++ = '\f'; break;
            case 'u':
                if (p + 4 <= end) {
                    uint32_t cp = parse_u4(p);
                    p += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= end && p[0] == '\\' && p[1] == 'u') {
                        uint32_t lo = parse_u4(p + 2);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
                            p += 6;
                        }
                    }
                    if (cp == '\r') {
                        if (norm_cr) {
                            *dst++ = '\n';
                            if (p + 1 < end && p[0] == '\\' && p[1] == 'n') p += 2;
                        } else {
                            *dst++ = '\r';
                            has_cr = 1;
                        }
                    } else {
                        dst += utf8_encode(cp, dst);
                    }
                }
                break;
            default: *dst++ = e; break;
        }
    }
    *pp = p;
    uint32_t len = (uint32_t)(dst - dst_start);
    a->pos = (uint32_t)(dst - a->base);
    if (out_has_cr) *out_has_cr = has_cr;
    return (Str){ dst_start, len };
}
#endif

static Str parse_json_str_scalar(const char **pp, const char *end, Arena *a, int norm_cr, int *out_has_cr) {
    const char *p = *pp + 1;
    const char *start = p;
    while (p < end && *p != '"' && *p != '\\') p++;
    if (p < end && *p == '"') {
        *pp = p + 1;
        return (Str){ start, (uint32_t)(p - start) };
    }
    char *dst_start = a->base + a->pos;
    char *dst = dst_start;
    uint32_t prefix = (uint32_t)(p - start);
    if (prefix) { fast_memcpy(dst, start, prefix); dst += prefix; }
    int has_cr = 0;
    while (p < end) {
        char c = *p++;
        if (c == '"') break;
        if (c != '\\') { *dst++ = c; continue; }
        if (p >= end) break;
        char e = *p++;
        switch (e) {
            case 'n':  *dst++ = '\n'; break;
            case '"':  *dst++ = '"';  break;
            case '\\': *dst++ = '\\'; break;
            case '/':  *dst++ = '/';  break;
            case 't':  *dst++ = '\t'; break;
            case 'r':
                if (norm_cr) {
                    *dst++ = '\n';
                    if (p + 1 < end && p[0] == '\\' && p[1] == 'n') p += 2;
                } else {
                    *dst++ = '\r';
                    has_cr = 1;
                }
                break;
            case 'b':  *dst++ = '\b'; break;
            case 'f':  *dst++ = '\f'; break;
            case 'u':
                if (p + 4 <= end) {
                    uint32_t cp = parse_u4(p);
                    p += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= end && p[0] == '\\' && p[1] == 'u') {
                        uint32_t lo = parse_u4(p + 2);
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000u + (((cp - 0xD800u) << 10) | (lo - 0xDC00u));
                            p += 6;
                        }
                    }
                    if (cp == '\r') {
                        if (norm_cr) {
                            *dst++ = '\n';
                            if (p + 1 < end && p[0] == '\\' && p[1] == 'n') p += 2;
                        } else {
                            *dst++ = '\r';
                            has_cr = 1;
                        }
                    } else {
                        dst += utf8_encode(cp, dst);
                    }
                }
                break;
            default: *dst++ = e; break;
        }
    }
    *pp = p;
    uint32_t len = (uint32_t)(dst - dst_start);
    a->pos = (uint32_t)(dst - a->base);
    if (out_has_cr) *out_has_cr = has_cr;
    return (Str){ dst_start, len };
}

static inline Str parse_json_str(const char **pp, const char *end, Arena *a, int norm_cr, int *out_has_cr) {
#if defined(__x86_64__)
    if (g_has_avx2) return parse_json_str_avx2(pp, end, a, norm_cr, out_has_cr);
#endif
    return parse_json_str_scalar(pp, end, a, norm_cr, out_has_cr);
}

static uint32_t parse_json_uint(const char **pp, const char *end) {
    const char *p = *pp;
    uint32_t v = 0;
    while (p < end && *p >= '0' && *p <= '9') { v = v * 10u + (uint32_t)(*p - '0'); p++; }
    *pp = p;
    return v;
}

static void skip_json_val(const char **pp, const char *end) {
    skip_ws(pp, end);
    const char *p = *pp;
    if (p >= end) return;
    if (*p == '"') {
        p++;
        while (p < end) {
            if (*p == '\\') { p += 2; continue; }
            if (*p == '"') { p++; break; }
            p++;
        }
        *pp = p;
    } else if (*p == '{') {
        p++; *pp = p; skip_ws(pp, end);
        if (*pp < end && **pp == '}') { (*pp)++; return; }
        while (*pp < end) {
            skip_json_val(pp, end); skip_ws(pp, end);
            if (*pp < end && **pp == ':') (*pp)++;
            skip_json_val(pp, end); skip_ws(pp, end);
            if (*pp < end && **pp == ',') { (*pp)++; skip_ws(pp, end); continue; }
            if (*pp < end && **pp == '}') (*pp)++;
            break;
        }
    } else if (*p == '[') {
        p++; *pp = p; skip_ws(pp, end);
        if (*pp < end && **pp == ']') { (*pp)++; return; }
        while (*pp < end) {
            skip_json_val(pp, end); skip_ws(pp, end);
            if (*pp < end && **pp == ',') { (*pp)++; skip_ws(pp, end); continue; }
            if (*pp < end && **pp == ']') (*pp)++;
            break;
        }
    } else {
        while (p < end && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') p++;
        *pp = p;
    }
}

// ---------------- Front Matter & Date Helpers ----------------
static const char *MONTH_NAMES[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
};
static const uint8_t MONTH_LENS[12] = { 7, 8, 5, 5, 3, 4, 4, 6, 9, 7, 8, 8 };

static Str make_long_date(Str date, Arena *a) {
    uint32_t m = (uint32_t)(date.p[5] - '0') * 10u + (uint32_t)(date.p[6] - '0');
    uint32_t d = (uint32_t)(date.p[8] - '0') * 10u + (uint32_t)(date.p[9] - '0');
    char *dst = arena_alloc(a, 24);
    uint32_t pos = u32_to_str(d, dst);
    dst[pos++] = ' ';
    uint8_t ml = MONTH_LENS[m - 1];
    fast_memcpy(dst + pos, MONTH_NAMES[m - 1], ml);
    pos += ml;
    dst[pos++] = ' ';
    fast_memcpy(dst + pos, date.p, 4);
    pos += 4;
    a->pos = (uint32_t)(dst - a->base) + pos;
    return (Str){ dst, pos };
}

static inline uint32_t days_in_month(uint32_t y, uint32_t m) {
    if (m == 2) return ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0)) ? 29u : 28u;
    if (m == 4 || m == 6 || m == 9 || m == 11) return 30u;
    return 31u;
}

static int is_valid_fm_slug(Str s) {
    if (s.n == 0 || s.n > 64) return 0;
    int prev_dash = 1;
    for (uint32_t i = 0; i < s.n; i++) {
        uint8_t c = (uint8_t)s.p[i];
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            prev_dash = 0;
        } else if (c == '-') {
            if (prev_dash) return 0;
            prev_dash = 1;
        } else {
            return 0;
        }
    }
    return !prev_dash;
}

typedef struct {
    Str title, date, layout, slug, summary;
    Str *tags;
    uint32_t tags_len;
    int has_title, has_date, has_tags, has_draft, has_layout, has_slug, has_summary;
    int draft;
    Str body;
    Str err;
} FmResult;

static Str make_refused_str(Arena *a, Str path, const char *msg1, Str mid, const char *msg2) {
    char tmp[1024];
    Buf b = { tmp, 0, sizeof(tmp) };
    BUF_LIT(&b, "refused ");
    buf_raw(&b, path.p, path.n);
    BUF_LIT(&b, ": ");
    if (msg1) { uint32_t l = 0; while (msg1[l]) l++; buf_raw(&b, msg1, l); }
    if (mid.n) buf_raw(&b, mid.p, mid.n);
    if (msg2) { uint32_t l = 0; while (msg2[l]) l++; buf_raw(&b, msg2, l); }
    char *dst = arena_alloc(a, b.n);
    fast_memcpy(dst, b.p, b.n);
    return (Str){ dst, b.n };
}

static int fm_parse_string(Str raw, Arena *a, Str *out_val, const char **out_err) {
    if (raw.n > 0 && raw.p[0] == '"') {
        if (raw.n < 2 || raw.p[raw.n - 1] != '"') {
            *out_err = "a quoted string is not closed";
            return 0;
        }
        uint32_t end = raw.n - 1;
        int has_bs = 0;
        for (uint32_t i = 1; i < end; i++) {
            char c = raw.p[i];
            if (c == '\\') {
                if (i + 1 >= end || (raw.p[i + 1] != '"' && raw.p[i + 1] != '\\')) {
                    *out_err = "a quoted string holds an escape other than \\\" or \\\\";
                    return 0;
                }
                has_bs = 1;
                i++;
            } else if (c == '"') {
                *out_err = "a quoted string holds a stray quote";
                return 0;
            }
        }
        if (!has_bs) {
            *out_val = (Str){ raw.p + 1, end - 1 };
            return 1;
        }
        char *dst = arena_alloc(a, end);
        uint32_t out_n = 0;
        for (uint32_t i = 1; i < end; i++) {
            if (raw.p[i] == '\\') { dst[out_n++] = raw.p[++i]; }
            else dst[out_n++] = raw.p[i];
        }
        a->pos = (uint32_t)(dst - a->base) + out_n;
        *out_val = (Str){ dst, out_n };
        return 1;
    }
    if (raw.n > 0 && raw.p[0] == '[') {
        *out_err = "a bare string opens on [";
        return 0;
    }
    *out_val = raw;
    return 1;
}

static int parse_front_matter(Str path, Str text, Arena *a, FmResult *out) {
    __builtin_memset(out, 0, sizeof(*out));
    const char *p = text.p, *end = text.p + text.n;
    const char *nl0 = find_nl(p, end);
    if ((uint32_t)(nl0 - p) != 3 || !fast_memeq(p, "---", 3)) {
        out->err = make_refused_str(a, path, "no front matter", (Str){0, 0}, NULL);
        return 0;
    }
    if (nl0 >= end) {
        out->err = make_refused_str(a, path, "the front matter is not closed", (Str){0, 0}, NULL);
        return 0;
    }
    // Scan lines until "---"
    const char *cur = nl0 + 1;
    const char *fm_start = cur;
    const char *close_line = NULL;
    const char *body_start = end;
    while (cur <= end) {
        const char *nl = find_nl(cur, end);
        if ((uint32_t)(nl - cur) == 3 && fast_memeq(cur, "---", 3)) {
            close_line = cur;
            body_start = (nl < end) ? (nl + 1) : end;
            break;
        }
        if (nl >= end) break;
        cur = nl + 1;
    }
    if (!close_line) {
        out->err = make_refused_str(a, path, "the front matter is not closed", (Str){0, 0}, NULL);
        return 0;
    }

    // Parse each front matter line
    cur = fm_start;
    uint32_t line_no = 2;
    while (cur < close_line) {
        const char *nl = find_nl(cur, close_line);
        uint32_t llen = (uint32_t)(nl - cur);
        if (md_is_blank(cur, llen) || (llen > 0 && cur[0] == '#')) {
            cur = nl + 1; line_no++; continue;
        }
        // Match ^([a-z_]+):(?: (.*))?$
        uint32_t k = 0;
        while (k < llen && ((cur[k] >= 'a' && cur[k] <= 'z') || cur[k] == '_')) k++;
        if (k == 0 || k >= llen || cur[k] != ':' || (k + 1 < llen && cur[k + 1] != ' ')) {
            char lnum[12]; uint32_t ln = u32_to_str(line_no, lnum);
            out->err = make_refused_str(a, path, "line ", (Str){lnum, ln}, " is not \"key: value\"");
            return 0;
        }
        Str key = { cur, k };
        Str raw = (k + 2 <= llen) ? (Str){ cur + k + 2, llen - (k + 2) } : (Str){ "", 0 };
        while (raw.n > 0 && (raw.p[raw.n - 1] == ' ' || raw.p[raw.n - 1] == '\t')) raw.n--;

        // Classify key (7 known keys)
        int kid = -1;
        if (key.n == 5 && fast_memeq(key.p, "title", 5)) kid = 0;
        else if (key.n == 4 && fast_memeq(key.p, "date", 4)) kid = 1;
        else if (key.n == 4 && fast_memeq(key.p, "tags", 4)) kid = 2;
        else if (key.n == 5 && fast_memeq(key.p, "draft", 5)) kid = 3;
        else if (key.n == 6 && fast_memeq(key.p, "layout", 6)) kid = 4;
        else if (key.n == 4 && fast_memeq(key.p, "slug", 4)) kid = 5;
        else if (key.n == 7 && fast_memeq(key.p, "summary", 7)) kid = 6;
        else {
            out->err = make_refused_str(a, path, "unknown key \"", key, "\"");
            return 0;
        }
        int *has_ptr = (kid == 0) ? &out->has_title : (kid == 1) ? &out->has_date :
                       (kid == 2) ? &out->has_tags : (kid == 3) ? &out->has_draft :
                       (kid == 4) ? &out->has_layout : (kid == 5) ? &out->has_slug : &out->has_summary;
        if (*has_ptr) {
            out->err = make_refused_str(a, path, "key \"", key, "\" is given twice");
            return 0;
        }
        if (raw.n == 0) {
            out->err = make_refused_str(a, path, "key \"", key, "\" has no value");
            return 0;
        }
        *has_ptr = 1;
        switch (kid) {
            case 0:
            case 6: {
                const char *serr = NULL;
                Str val;
                if (!fm_parse_string(raw, a, &val, &serr)) {
                    out->err = make_refused_str(a, path, kid == 0 ? "key \"title\": " : "key \"summary\": ", (Str){0, 0}, serr);
                    return 0;
                }
                if (kid == 0) out->title = val; else out->summary = val;
                break;
            }
            case 1: {
                int ok = (raw.n == 10 && raw.p[4] == '-' && raw.p[7] == '-');
                for (uint32_t j = 0; ok && j < 10; j++) {
                    if (j != 4 && j != 7 && (raw.p[j] < '0' || raw.p[j] > '9')) ok = 0;
                }
                if (ok) {
                    uint32_t y = (uint32_t)(raw.p[0] - '0') * 1000u + (uint32_t)(raw.p[1] - '0') * 100u +
                                 (uint32_t)(raw.p[2] - '0') * 10u + (uint32_t)(raw.p[3] - '0');
                    uint32_t mo = (uint32_t)(raw.p[5] - '0') * 10u + (uint32_t)(raw.p[6] - '0');
                    uint32_t d = (uint32_t)(raw.p[8] - '0') * 10u + (uint32_t)(raw.p[9] - '0');
                    if (y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > days_in_month(y, mo)) ok = 0;
                }
                if (!ok) {
                    out->err = make_refused_str(a, path, "key \"date\": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)", (Str){0, 0}, NULL);
                    return 0;
                }
                out->date = raw;
                break;
            }
            case 2: {
                if (raw.n < 2 || raw.p[0] != '[' || raw.p[raw.n - 1] != ']') {
                    out->err = make_refused_str(a, path, "key \"tags\": not a list [a, b]", (Str){0, 0}, NULL);
                    return 0;
                }
                Str inner = { raw.p + 1, raw.n - 2 };
                if (trim_js_ws(inner).n == 0) {
                    out->tags = NULL; out->tags_len = 0;
                } else {
                    // Count commas
                    uint32_t cnt = 1;
                    for (uint32_t j = 0; j < inner.n; j++) if (inner.p[j] == ',') cnt++;
                    Str *tarr = (Str *)arena_alloc_aligned(a, cnt * (uint32_t)sizeof(Str), 8);
                    uint32_t s = 0, ti = 0;
                    for (uint32_t j = 0; j <= inner.n; j++) {
                        if (j == inner.n || inner.p[j] == ',') {
                            Str item = trim_js_ws((Str){ inner.p + s, j - s });
                            if (item.n == 0) {
                                out->err = make_refused_str(a, path, "key \"tags\": an empty tag, or one holding \" [ or ]", (Str){0, 0}, NULL);
                                return 0;
                            }
                            for (uint32_t q = 0; q < item.n; q++) {
                                char ch = item.p[q];
                                if (ch == '"' || ch == '[' || ch == ']') {
                                    out->err = make_refused_str(a, path, "key \"tags\": an empty tag, or one holding \" [ or ]", (Str){0, 0}, NULL);
                                    return 0;
                                }
                            }
                            tarr[ti++] = item;
                            s = j + 1;
                        }
                    }
                    out->tags = tarr;
                    out->tags_len = ti;
                }
                break;
            }
            case 3:
                if (raw.n == 4 && fast_memeq(raw.p, "true", 4)) out->draft = 1;
                else if (raw.n == 5 && fast_memeq(raw.p, "false", 5)) out->draft = 0;
                else {
                    out->err = make_refused_str(a, path, "key \"draft\": true or false", (Str){0, 0}, NULL);
                    return 0;
                }
                break;
            case 4:
                if (!is_valid_tpl_name(raw)) {
                    out->err = make_refused_str(a, path, "key \"layout\": not a template name", (Str){0, 0}, NULL);
                    return 0;
                }
                out->layout = raw;
                break;
            case 5:
                if (!is_valid_fm_slug(raw)) {
                    out->err = make_refused_str(a, path, "key \"slug\": not a slug", (Str){0, 0}, NULL);
                    return 0;
                }
                out->slug = raw;
                break;
        }
        cur = nl + 1;
        line_no++;
    }
    if (!out->has_title) {
        out->err = make_refused_str(a, path, "no title", (Str){0, 0}, NULL);
        return 0;
    }
    out->body = (Str){ body_start, (uint32_t)(end - body_start) };
    return 1;
}

// ---------------- Path Resolution & Link Resolver ----------------
static inline Str dir_of(Str path) {
    for (int i = (int)path.n - 1; i >= 0; i--) {
        if (path.p[i] == '/') return (Str){ path.p, (uint32_t)i };
    }
    return (Str){ "", 0 };
}

static inline int is_scheme(Str target) {
    if (target.n < 2) return 0;
    uint8_t c0 = (uint8_t)target.p[0];
    if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z'))) return 0;
    for (uint32_t i = 1; i < target.n; i++) {
        uint8_t c = (uint8_t)target.p[i];
        if (c == ':') return 1;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '.' || c == '-')) {
            return 0;
        }
    }
    return 0;
}

static int resolve_rel_path(Str dir, Str rel, char *out, uint32_t *out_len) {
    Str parts[64];
    uint32_t nparts = 0;
    if (dir.n > 0) {
        uint32_t s = 0;
        for (uint32_t i = 0; i <= dir.n; i++) {
            if (i == dir.n || dir.p[i] == '/') {
                if (nparts < 64) parts[nparts++] = (Str){ dir.p + s, i - s };
                s = i + 1;
            }
        }
    }
    uint32_t s = 0;
    for (uint32_t i = 0; i <= rel.n; i++) {
        if (i == rel.n || rel.p[i] == '/') {
            Str seg = { rel.p + s, i - s };
            s = i + 1;
            if (seg.n == 0 || (seg.n == 1 && seg.p[0] == '.')) continue;
            if (seg.n == 2 && seg.p[0] == '.' && seg.p[1] == '.') {
                if (nparts == 0) return 0;
                nparts--;
            } else {
                if (nparts < 64) parts[nparts++] = seg;
            }
        }
    }
    uint32_t pos = 0;
    for (uint32_t i = 0; i < nparts; i++) {
        if (i > 0) out[pos++] = '/';
        fast_memcpy(out + pos, parts[i].p, parts[i].n);
        pos += parts[i].n;
    }
    *out_len = pos;
    return 1;
}

typedef struct {
    Str base;
    Str cur_path;
    Str cur_dir;
    const PostData *sources;
    const StrMap *by_source;
    const StrMap *static_paths;
    StrMap *broken_set;
    Str *broken_list;
    uint32_t *broken_len;
    Arena *arena;
    char tmp_href[2048];
} LinkCtx;

static Str resolve_link_cb(void *vctx, Str target) {
    LinkCtx *ctx = (LinkCtx *)vctx;
    if (target.n > 0 && (target.p[0] == '/' || target.p[0] == '#' || is_scheme(target))) return target;
    uint32_t hash_pos = target.n;
    for (uint32_t i = 0; i < target.n; i++) {
        if (target.p[i] == '#') { hash_pos = i; break; }
    }
    Str rel = { target.p, hash_pos };
    Str frag = { target.p + hash_pos, target.n - hash_pos };
    if (rel.n > 0) {
        char rbuf[1024];
        uint32_t rlen = 0;
        if (resolve_rel_path(ctx->cur_dir, rel, rbuf, &rlen)) {
            Str rstr = { rbuf, rlen };
            uint32_t h = fnv1a_hash(rbuf, rlen);
            uint32_t sidx;
            if (sm_get(ctx->by_source, rstr, h, &sidx)) {
                Str url = ctx->sources[sidx].url;
                if (frag.n == 0) return url;
                fast_memcpy(ctx->tmp_href, url.p, url.n);
                fast_memcpy(ctx->tmp_href + url.n, frag.p, frag.n);
                return (Str){ ctx->tmp_href, url.n + frag.n };
            }
            if (sm_get(ctx->static_paths, rstr, h, NULL)) {
                uint32_t pos = 0;
                fast_memcpy(ctx->tmp_href, ctx->base.p, ctx->base.n); pos += ctx->base.n;
                ctx->tmp_href[pos++] = '/';
                fast_memcpy(ctx->tmp_href + pos, rbuf, rlen); pos += rlen;
                if (frag.n) { fast_memcpy(ctx->tmp_href + pos, frag.p, frag.n); pos += frag.n; }
                return (Str){ ctx->tmp_href, pos };
            }
        }
    }
    // Record broken link (deduplicated via broken_set)
    uint32_t blen = 12 + ctx->cur_path.n + 4 + target.n;
    char btmp[2048];
    fast_memcpy(btmp, "broken-link ", 12);
    fast_memcpy(btmp + 12, ctx->cur_path.p, ctx->cur_path.n);
    fast_memcpy(btmp + 12 + ctx->cur_path.n, " -> ", 4);
    fast_memcpy(btmp + 16 + ctx->cur_path.n, target.p, target.n);
    uint32_t bh = fnv1a_hash(btmp, blen);
    if (!sm_get(ctx->broken_set, (Str){btmp, blen}, bh, NULL)) {
        char *saved = arena_alloc(ctx->arena, blen);
        fast_memcpy(saved, btmp, blen);
        Str bs = { saved, blen };
        sm_put_new(ctx->broken_set, bs, bh, 1);
        ctx->broken_list[(*ctx->broken_len)++] = bs;
    }
    return target;
}

// ---------------- Process One Case & Stream Directly to g_out ----------------
static void process_case(const char **pp, const char *end) {
    Arena arena = { g_arena_mem, 0, ARENA_CAP };
    Arena id_arena = { g_id_arena_mem, 0, sizeof(g_id_arena_mem) };

    // Parse case object: { "id": "...", "input": { "site": ..., "templates": ..., "files": ... } }
    Str site_title = { "", 0 }, site_base_raw = { "", 0 };
    uint32_t site_per_page = 10, site_recent = 5;

    InputTpl *in_tpls = (InputTpl *)arena_alloc_aligned(&arena, 256u * (uint32_t)sizeof(InputTpl), 8);
    uint32_t in_tpl_count = 0;

    InputFile *in_files = (InputFile *)arena_alloc_aligned(&arena, 4096u * (uint32_t)sizeof(InputFile), 8);
    uint32_t in_file_count = 0;

    skip_ws(pp, end);
    if (*pp < end && **pp == '{') (*pp)++;
    while (*pp < end) {
        skip_ws(pp, end);
        if (**pp == '}') { (*pp)++; break; }
        Str k = parse_json_str(pp, end, &arena, 0, NULL);
        skip_ws(pp, end); if (**pp == ':') (*pp)++;
        skip_ws(pp, end);
        if (k.n == 5 && fast_memeq(k.p, "input", 5)) {
            if (**pp == '{') (*pp)++;
            while (*pp < end) {
                skip_ws(pp, end);
                if (**pp == '}') { (*pp)++; break; }
                Str ik = parse_json_str(pp, end, &arena, 0, NULL);
                skip_ws(pp, end); if (**pp == ':') (*pp)++;
                skip_ws(pp, end);
                if (ik.n == 4 && fast_memeq(ik.p, "site", 4)) {
                    if (**pp == '{') (*pp)++;
                    while (*pp < end) {
                        skip_ws(pp, end);
                        if (**pp == '}') { (*pp)++; break; }
                        Str sk = parse_json_str(pp, end, &arena, 0, NULL);
                        skip_ws(pp, end); if (**pp == ':') (*pp)++;
                        skip_ws(pp, end);
                        if (sk.n == 5 && fast_memeq(sk.p, "title", 5)) site_title = parse_json_str(pp, end, &arena, 0, NULL);
                        else if (sk.n == 8 && fast_memeq(sk.p, "base_url", 8)) site_base_raw = parse_json_str(pp, end, &arena, 0, NULL);
                        else if (sk.n == 8 && fast_memeq(sk.p, "per_page", 8)) site_per_page = parse_json_uint(pp, end);
                        else if (sk.n == 6 && fast_memeq(sk.p, "recent", 6)) site_recent = parse_json_uint(pp, end);
                        else skip_json_val(pp, end);
                        skip_ws(pp, end);
                        if (*pp < end && **pp == ',') (*pp)++;
                    }
                } else if (ik.n == 9 && fast_memeq(ik.p, "templates", 9)) {
                    if (**pp == '{') (*pp)++;
                    while (*pp < end) {
                        skip_ws(pp, end);
                        if (**pp == '}') { (*pp)++; break; }
                        Str tn = parse_json_str(pp, end, &arena, 0, NULL);
                        skip_ws(pp, end); if (**pp == ':') (*pp)++;
                        skip_ws(pp, end);
                        Str ts = parse_json_str(pp, end, &arena, 0, NULL);
                        if (in_tpl_count < 256u) {
                            in_tpls[in_tpl_count].name = tn;
                            in_tpls[in_tpl_count].src = ts;
                            in_tpl_count++;
                        }
                        skip_ws(pp, end);
                        if (*pp < end && **pp == ',') (*pp)++;
                    }
                } else if (ik.n == 5 && fast_memeq(ik.p, "files", 5)) {
                    if (**pp == '[') (*pp)++;
                    while (*pp < end) {
                        skip_ws(pp, end);
                        if (**pp == ']') { (*pp)++; break; }
                        if (**pp == '{') (*pp)++;
                        Str fp = { "", 0 }, fc = { "", 0 };
                        int has_path = 0, is_md = 0, has_cr = 0;
                        while (*pp < end) {
                            skip_ws(pp, end);
                            if (**pp == '}') { (*pp)++; break; }
                            Str fk = parse_json_str(pp, end, &arena, 0, NULL);
                            skip_ws(pp, end); if (**pp == ':') (*pp)++;
                            skip_ws(pp, end);
                            if (fk.n == 4 && fast_memeq(fk.p, "path", 4)) {
                                fp = parse_json_str(pp, end, &arena, 0, NULL);
                                has_path = 1;
                                is_md = (fp.n >= 3 && fp.p[fp.n - 3] == '.' && fp.p[fp.n - 2] == 'm' && fp.p[fp.n - 1] == 'd');
                            } else if (fk.n == 7 && fast_memeq(fk.p, "content", 7)) {
                                // Fused CR normalization when path already known to be .md!
                                fc = parse_json_str(pp, end, &arena, has_path && is_md, &has_cr);
                            } else {
                                skip_json_val(pp, end);
                            }
                            skip_ws(pp, end);
                            if (*pp < end && **pp == ',') (*pp)++;
                        }
                        if (in_file_count < 4096u) {
                            in_files[in_file_count].path = fp;
                            in_files[in_file_count].content = fc;
                            in_files[in_file_count].has_cr = has_cr;
                            in_file_count++;
                        }
                        skip_ws(pp, end);
                        if (*pp < end && **pp == ',') (*pp)++;
                    }
                } else {
                    skip_json_val(pp, end);
                }
                skip_ws(pp, end);
                if (*pp < end && **pp == ',') (*pp)++;
            }
        } else {
            skip_json_val(pp, end);
        }
        skip_ws(pp, end);
        if (*pp < end && **pp == ',') (*pp)++;
    }

    // Strip trailing '/' from base_url
    Str base = site_base_raw;
    while (base.n > 0 && base.p[base.n - 1] == '/') base.n--;

    // Report lines array
    Str *report = (Str *)arena_alloc_aligned(&arena, 8192u * (uint32_t)sizeof(Str), 8);
    uint32_t report_len = 0;

    // 1. Parse templates in code-unit order
    if (in_tpl_count > 1) sort_tpls(in_tpls, 0, (int)in_tpl_count - 1);
    Template *templates = (Template *)arena_alloc_aligned(&arena, (in_tpl_count + 1u) * (uint32_t)sizeof(Template), 8);
    uint32_t tpl_count = 0;
    for (uint32_t i = 0; i < in_tpl_count; i++) {
        TNode *nodes = NULL;
        uint32_t nlen = 0;
        Str err = { "", 0 };
        if (parse_template(in_tpls[i].name, in_tpls[i].src, &arena, &nodes, &nlen, &err)) {
            Template *t = &templates[tpl_count++];
            __builtin_memset(t, 0, sizeof(*t));
            t->name = in_tpls[i].name;
            t->hash = fnv1a_hash(t->name.p, t->name.n);
            t->nodes = nodes;
            t->len = nlen;
        } else {
            report[report_len++] = err;
        }
    }

    // 2. Sort files in code-unit order, split .md vs static, parse front matter
    if (in_file_count > 1) sort_files(in_files, 0, (int)in_file_count - 1);
    PostData *sources = (PostData *)arena_alloc_aligned(&arena, (in_file_count + 1u) * (uint32_t)sizeof(PostData), 8);
    uint32_t src_count = 0;
    InputFile *statics = (InputFile *)arena_alloc_aligned(&arena, (in_file_count + 1u) * (uint32_t)sizeof(InputFile), 8);
    uint32_t static_count = 0;

    StrMap static_paths;
    sm_init(&static_paths, &arena, next_pow2(in_file_count + 8u));

    StrMap tpl_map;
    sm_init(&tpl_map, &arena, next_pow2(tpl_count + 8u));
    for (uint32_t i = 0; i < tpl_count; i++) sm_put_new(&tpl_map, templates[i].name, templates[i].hash, i);

    // Temp tag string lists per source (before slugifying into TagData)
    Str **src_raw_tags = (Str **)arena_alloc_aligned(&arena, (in_file_count + 1u) * (uint32_t)sizeof(Str *), 8);
    uint32_t *src_raw_tag_cnt = (uint32_t *)arena_alloc_aligned(&arena, (in_file_count + 1u) * 4u, 4);

    for (uint32_t i = 0; i < in_file_count; i++) {
        InputFile *f = &in_files[i];
        int is_md = (f->path.n >= 3 && f->path.p[f->path.n - 3] == '.' && f->path.p[f->path.n - 2] == 'm' && f->path.p[f->path.n - 1] == 'd');
        if (!is_md) {
            statics[static_count++] = *f;
            sm_put_new(&static_paths, f->path, fnv1a_hash(f->path.p, f->path.n), static_count - 1u);
            continue;
        }
        // Normalize CR if content appeared before path in JSON and contained '\r'
        Str text = f->content;
        if (f->has_cr) {
            char *nb = arena_alloc(&arena, text.n);
            uint32_t out_n = 0;
            for (uint32_t j = 0; j < text.n; j++) {
                if (text.p[j] == '\r') {
                    nb[out_n++] = '\n';
                    if (j + 1 < text.n && text.p[j + 1] == '\n') j++;
                } else {
                    nb[out_n++] = text.p[j];
                }
            }
            arena.pos = (uint32_t)(nb - arena.base) + out_n;
            text = (Str){ nb, out_n };
        }
        FmResult fm;
        if (!parse_front_matter(f->path, text, &arena, &fm)) {
            report[report_len++] = fm.err;
            continue;
        }
        int is_post = (f->path.n >= 6 && fast_memeq(f->path.p, "posts/", 6));
        if (!is_post) {
            if (f->path.n >= 5 && fast_memeq(f->path.p, "tags/", 5)) {
                report[report_len++] = make_refused_str(&arena, f->path, "tags/ is the generator's own", (Str){0, 0}, NULL);
                continue;
            }
            if (f->path.n >= 5 && fast_memeq(f->path.p, "page/", 5)) {
                report[report_len++] = make_refused_str(&arena, f->path, "page/ is the generator's own", (Str){0, 0}, NULL);
                continue;
            }
        }
        if (is_post && !fm.has_date) {
            report[report_len++] = make_refused_str(&arena, f->path, "a post needs a date", (Str){0, 0}, NULL);
            continue;
        }
        Str layout = fm.has_layout ? fm.layout : (is_post ? (Str){"post", 4} : (Str){"page", 4});
        if (!sm_get(&tpl_map, layout, fnv1a_hash(layout.p, layout.n), NULL)) {
            report[report_len++] = make_refused_str(&arena, f->path, "layout \"", layout, "\" is no template that parses");
            continue;
        }
        if (fm.has_draft && fm.draft) {
            char *d = arena_alloc(&arena, 6 + f->path.n);
            fast_memcpy(d, "draft ", 6);
            fast_memcpy(d + 6, f->path.p, f->path.n);
            report[report_len++] = (Str){ d, 6 + f->path.n };
            continue;
        }
        PostData *s = &sources[src_count];
        __builtin_memset(s, 0, sizeof(*s));
        s->path = f->path;
        s->title = fm.title;
        s->date = fm.has_date ? fm.date : (Str){ "", 0 };
        s->date_long = fm.has_date ? make_long_date(fm.date, &arena) : (Str){ "", 0 };
        s->summary = fm.has_summary ? fm.summary : (Str){ "", 0 };
        s->slug = fm.has_slug ? fm.slug : (Str){ "", 0 }; // empty means compute via slugify(title)
        s->body = fm.body;
        s->layout = layout;
        s->is_post = is_post;
        s->newer_idx = -1;
        s->older_idx = -1;
        src_raw_tags[src_count] = fm.tags;
        src_raw_tag_cnt[src_count] = fm.tags_len;
        src_count++;
    }

    // 3. Permalinks (unique slug per dir; root dir reserves posts, tags, page)
    // Key in `taken_slugs` is "<dir>\0<slug>"! Single hash map for all directories!
    StrMap taken_slugs;
    sm_init(&taken_slugs, &arena, next_pow2(src_count + 32u));
    // Pre-insert reserved root slugs ("posts", "tags", "page" with empty dir -> "\0posts", etc.)
    const char *res_keys[3] = { "\0posts", "\0tags", "\0page" };
    const uint32_t res_lens[3] = { 6, 5, 5 };
    for (int r = 0; r < 3; r++) {
        sm_put_new(&taken_slugs, (Str){res_keys[r], res_lens[r]}, fnv1a_hash(res_keys[r], res_lens[r]), 1);
    }

    StrMap by_source;
    sm_init(&by_source, &arena, next_pow2(src_count + 8u));

    for (uint32_t i = 0; i < src_count; i++) {
        PostData *s = &sources[i];
        Str dir = s->is_post ? (Str){"posts", 5} : dir_of(s->path);
        char base_slug_buf[64];
        Str base_slug;
        if (s->slug.n > 0) {
            base_slug = s->slug;
        } else {
            uint32_t slen = slugify_into(s->title, base_slug_buf);
            base_slug = (Str){ base_slug_buf, slen };
        }
        // Build "<dir>\0<slug>" candidate
        char kbuf[512];
        fast_memcpy(kbuf, dir.p, dir.n);
        kbuf[dir.n] = '\0';
        fast_memcpy(kbuf + dir.n + 1, base_slug.p, base_slug.n);
        uint32_t klen = dir.n + 1 + base_slug.n;
        uint32_t kh = fnv1a_hash(kbuf, klen);
        uint32_t final_slen = base_slug.n;
        if (sm_get(&taken_slugs, (Str){kbuf, klen}, kh, NULL)) {
            kbuf[dir.n + 1 + base_slug.n] = '-';
            for (uint32_t n = 2; ; n++) {
                uint32_t dn = u32_to_str(n, kbuf + dir.n + 1 + base_slug.n + 1);
                final_slen = base_slug.n + 1 + dn;
                klen = dir.n + 1 + final_slen;
                kh = fnv1a_hash(kbuf, klen);
                if (!sm_get(&taken_slugs, (Str){kbuf, klen}, kh, NULL)) break;
            }
        }
        char *saved_k = arena_alloc(&arena, klen);
        fast_memcpy(saved_k, kbuf, klen);
        sm_put_new(&taken_slugs, (Str){saved_k, klen}, kh, 1);

        s->slug = (Str){ saved_k + dir.n + 1, final_slen };
        // Build url = "<base>/<outDir>/" and out_dir as a slice inside url!
        uint32_t out_dir_len = (dir.n == 0) ? final_slen : (dir.n + 1 + final_slen);
        uint32_t url_len = base.n + 1 + out_dir_len + 1;
        char *ub = arena_alloc(&arena, url_len);
        uint32_t up = 0;
        fast_memcpy(ub, base.p, base.n); up += base.n;
        ub[up++] = '/';
        uint32_t od_off = up;
        if (dir.n > 0) {
            fast_memcpy(ub + up, dir.p, dir.n); up += dir.n;
            ub[up++] = '/';
        }
        fast_memcpy(ub + up, s->slug.p, final_slen); up += final_slen;
        ub[up++] = '/';
        s->out_dir = (Str){ ub + od_off, out_dir_len };
        s->url = (Str){ ub, url_len };
        sm_put_new(&by_source, s->path, fnv1a_hash(s->path.p, s->path.n), i);
    }

    // 4. Collections (sort posts BEFORE Markdown!)
    PostData **posts = (PostData **)arena_alloc_aligned(&arena, (src_count + 1u) * (uint32_t)sizeof(PostData *), 8);
    uint32_t posts_len = 0;
    for (uint32_t i = 0; i < src_count; i++) {
        if (sources[i].is_post) posts[posts_len++] = &sources[i];
    }
    if (posts_len > 1) sort_posts(posts, 0, (int)posts_len - 1);
    for (uint32_t k = 0; k < posts_len; k++) {
        posts[k]->newer_idx = (k == 0) ? -1 : (int32_t)(k - 1);
        posts[k]->older_idx = (k + 1 == posts_len) ? -1 : (int32_t)(k + 1);
    }

    // 5. Tags (grouped by slug, named as first met in posts order!)
    TagData *tags_arr = (TagData *)arena_alloc_aligned(&arena, 2048u * (uint32_t)sizeof(TagData), 8);
    uint32_t tags_len = 0;
    StrMap tag_by_slug;
    sm_init(&tag_by_slug, &arena, 2048);
    // Cache tag name -> slug string!
    StrMap tag_slug_cache;
    sm_init(&tag_slug_cache, &arena, 2048);
    Str *cached_slugs = (Str *)arena_alloc_aligned(&arena, 2048u * (uint32_t)sizeof(Str), 8);
    uint32_t cached_slug_cnt = 0;

    // Helper: slugify with cache
    #define GET_TAG_SLUG(tname, out_slug) do { \
        uint32_t _th = fnv1a_hash((tname).p, (tname).n); \
        uint32_t _ci; \
        if (sm_get(&tag_slug_cache, (tname), _th, &_ci)) { \
            (out_slug) = cached_slugs[_ci]; \
        } else { \
            char _sb[64]; \
            uint32_t _sl = slugify_into((tname), _sb); \
            char *_sp = arena_alloc(&arena, _sl); \
            fast_memcpy(_sp, _sb, _sl); \
            (out_slug) = (Str){ _sp, _sl }; \
            if (cached_slug_cnt < 2048u) { \
                cached_slugs[cached_slug_cnt] = (out_slug); \
                sm_put_new(&tag_slug_cache, (tname), _th, cached_slug_cnt); \
                cached_slug_cnt++; \
            } \
        } \
    } while (0)

    for (uint32_t k = 0; k < posts_len; k++) {
        PostData *p = posts[k];
        uint32_t sidx = (uint32_t)(p - sources);
        uint32_t rcnt = src_raw_tag_cnt[sidx];
        if (rcnt == 0) continue;
        Str *rtags = src_raw_tags[sidx];
        TagRef *refs = (TagRef *)arena_alloc_aligned(&arena, rcnt * (uint32_t)sizeof(TagRef), 8);
        uint32_t ref_n = 0;
        for (uint32_t j = 0; j < rcnt; j++) {
            Str tslug;
            GET_TAG_SLUG(rtags[j], tslug);
            // Dedup within this post (rcnt is tiny, typically <= 8!)
            int dup = 0;
            for (uint32_t d = 0; d < ref_n; d++) {
                if (refs[d].slug.n == tslug.n && fast_memeq(refs[d].slug.p, tslug.p, tslug.n)) { dup = 1; break; }
            }
            if (dup) continue;
            uint32_t sh = fnv1a_hash(tslug.p, tslug.n);
            uint32_t tidx;
            if (!sm_get(&tag_by_slug, tslug, sh, &tidx)) {
                tidx = tags_len++;
                TagData *td = &tags_arr[tidx];
                td->name = rtags[j];
                td->slug = tslug;
                uint32_t ulen = base.n + 6 + tslug.n + 1;
                char *up = arena_alloc(&arena, ulen);
                fast_memcpy(up, base.p, base.n);
                fast_memcpy(up + base.n, "/tags/", 6);
                fast_memcpy(up + base.n + 6, tslug.p, tslug.n);
                up[ulen - 1] = '/';
                td->url = (Str){ up, ulen };
                td->post_cap = 16;
                td->post_idxs = (uint32_t *)arena_alloc_aligned(&arena, 16u * 4u, 4);
                td->post_count = 0;
                sm_put_new(&tag_by_slug, tslug, sh, tidx);
            }
            TagData *td = &tags_arr[tidx];
            if (td->post_count == td->post_cap) {
                uint32_t ncap = td->post_cap * 2u;
                uint32_t *na = (uint32_t *)arena_alloc_aligned(&arena, ncap * 4u, 4);
                fast_memcpy(na, td->post_idxs, td->post_count * 4u);
                td->post_idxs = na;
                td->post_cap = ncap;
            }
            td->post_idxs[td->post_count++] = k;
            refs[ref_n++] = (TagRef){ td->name, td->slug, td->url };
        }
        p->tag_refs = refs;
        p->tag_count = ref_n;
    }

    // Non-post page tags
    for (uint32_t i = 0; i < src_count; i++) {
        PostData *s = &sources[i];
        if (s->is_post) continue;
        uint32_t rcnt = src_raw_tag_cnt[i];
        if (rcnt == 0) continue;
        Str *rtags = src_raw_tags[i];
        TagRef *refs = (TagRef *)arena_alloc_aligned(&arena, rcnt * (uint32_t)sizeof(TagRef), 8);
        uint32_t ref_n = 0;
        for (uint32_t j = 0; j < rcnt; j++) {
            Str tslug;
            GET_TAG_SLUG(rtags[j], tslug);
            int dup = 0;
            for (uint32_t d = 0; d < ref_n; d++) {
                if (refs[d].slug.n == tslug.n && fast_memeq(refs[d].slug.p, tslug.p, tslug.n)) { dup = 1; break; }
            }
            if (dup) continue;
            uint32_t sh = fnv1a_hash(tslug.p, tslug.n);
            uint32_t tidx;
            if (sm_get(&tag_by_slug, tslug, sh, &tidx)) {
                refs[ref_n++] = (TagRef){ tags_arr[tidx].name, tags_arr[tidx].slug, tags_arr[tidx].url };
            } else {
                refs[ref_n++] = (TagRef){ rtags[j], tslug, (Str){"", 0} };
            }
        }
        s->tag_refs = refs;
        s->tag_count = ref_n;
    }
    #undef GET_TAG_SLUG

    if (tags_len > 1) sort_tags(tags_arr, 0, (int)tags_len - 1);

    // Prepare broken-link set & scratch buffers for Markdown
    StrMap broken_set;
    sm_init(&broken_set, &arena, 4096);
    Str *broken_list = (Str *)arena_alloc_aligned(&arena, 4096u * (uint32_t)sizeof(Str), 8);
    uint32_t broken_len = 0;

    Buf md_buf = { g_md_buf_init, 0, sizeof(g_md_buf_init) };
    Buf join_buf = { g_join_buf_init, 0, sizeof(g_join_buf_init) };

    LinkCtx lctx;
    lctx.base = base;
    lctx.sources = sources;
    lctx.by_source = &by_source;
    lctx.static_paths = &static_paths;
    lctx.broken_set = &broken_set;
    lctx.broken_list = broken_list;
    lctx.broken_len = &broken_len;
    lctx.arena = &arena;

    // Pre-render ONLY the top `recent_len` posts (typically 3-6 posts) so `recent` cards are 100% ready before site-baking!
    uint32_t recent_len = (posts_len < site_recent) ? posts_len : site_recent;
    for (uint32_t k = 0; k < recent_len; k++) {
        PostData *p = posts[k];
        uint32_t w = words_of(p->body.p, p->body.n);
        p->reading_minutes = (w == 0) ? 1u : (w + 199u) / 200u;
        p->words_computed = 1;
        lctx.cur_path = p->path;
        lctx.cur_dir = dir_of(p->path);
        id_arena.pos = 0;
        MdResult md = render_markdown_json(p->body, resolve_link_cb, &lctx, &md_buf, &join_buf, &g_hset, &id_arena, g_toc_scratch, 1024);
        // Save html + toc + excerpt into arena so we don't re-render these 3-6 posts later!
        char *hcopy = arena_alloc(&arena, md.html.n);
        fast_memcpy(hcopy, md.html.p, md.html.n);
        p->pre_html_json = (Str){ hcopy, md.html.n };
        if (md.excerpt.n > 0) {
            uint32_t off = (uint32_t)(md.excerpt.p - md.html.p);
            p->excerpt_json = (Str){ hcopy + off, md.excerpt.n };
        }
        p->has_excerpt_cached = 1;
        if (md.toc_len > 0) {
            TocItem *tcopy = (TocItem *)arena_alloc_aligned(&arena, md.toc_len * (uint32_t)sizeof(TocItem), 8);
            for (uint32_t ti = 0; ti < md.toc_len; ti++) {
                tcopy[ti].level = md.toc[ti].level;
                char *idp = arena_alloc(&arena, md.toc[ti].id.n);
                fast_memcpy(idp, md.toc[ti].id.p, md.toc[ti].id.n);
                tcopy[ti].id = (Str){ idp, md.toc[ti].id.n };
                uint32_t hoff = (uint32_t)(md.toc[ti].html.p - md.html.p);
                tcopy[ti].html = (Str){ hcopy + hoff, md.toc[ti].html.n };
            }
            p->pre_toc = tcopy;
            p->pre_toc_len = md.toc_len;
        }
        p->md_prerendered = 1;
    }

    // Build RenderCtx & compile site-baked layout ASTs!
    RenderCtx rc;
    __builtin_memset(&rc, 0, sizeof(rc));
    rc.site_title = site_title;
    rc.site_base_url = base;
    rc.tags = tags_arr;
    rc.tags_len = tags_len;
    rc.recent = posts;
    rc.recent_len = recent_len;
    rc.all_sources = sources;
    rc.sorted_posts = posts;
    rc.sorted_posts_len = posts_len;
    rc.templates = templates;
    rc.tpl_count = tpl_count;
    rc.card_arena = &arena;

    Template *t_index = find_template(&rc, (Str){"index", 5});
    Template *t_tag   = find_template(&rc, (Str){"tag", 3});
    Template *t_tags  = find_template(&rc, (Str){"tags", 4});
    if (t_index) compile_layout(&rc, t_index, 2, &arena);
    if (t_tag)   compile_layout(&rc, t_tag, 3, &arena);
    if (t_tags)  compile_layout(&rc, t_tags, 4, &arena);
    unify_card_cache_slots(t_index, t_tag);

    // Pre-register all generated output paths so we can detect shadowed static files while streaming!
    StrMap out_paths;
    uint32_t per = site_per_page ? site_per_page : 1u;
    uint32_t num_index_pages = (posts_len == 0) ? 1u : (posts_len + per - 1u) / per;
    sm_init(&out_paths, &arena, next_pow2(src_count + tags_len + num_index_pages + 16u));

    // Stream case JSON object directly to 256 KB L2 g_out!
    out_char('{');
    int first_kv = 1;

    // A. Stream all pages/posts with LAZY per-page Markdown rendering!
    for (uint32_t i = 0; i < src_count; i++) {
        PostData *s = &sources[i];
        if (!s->words_computed) {
            uint32_t w = words_of(s->body.p, s->body.n);
            s->reading_minutes = (w == 0) ? 1u : (w + 199u) / 200u;
            s->words_computed = 1;
        }
        MdResult md;
        const MdResult *md_ptr = NULL;
        if (!s->md_prerendered) {
            lctx.cur_path = s->path;
            lctx.cur_dir = dir_of(s->path);
            id_arena.pos = 0;
            md = render_markdown_json(s->body, resolve_link_cb, &lctx, &md_buf, &join_buf, &g_hset, &id_arena, g_toc_scratch, 1024);
            md_ptr = &md;
            // Save tiny excerpt for posts (needed for index/tag cards!)
            if (s->is_post && !s->has_excerpt_cached) {
                if (md.excerpt.n > 0) {
                    char *ec = arena_alloc(&arena, md.excerpt.n);
                    fast_memcpy(ec, md.excerpt.p, md.excerpt.n);
                    s->excerpt_json = (Str){ ec, md.excerpt.n };
                }
                s->has_excerpt_cached = 1;
            }
        }
        // Lookup and lazily compile layout for kind=1 (page/post)
        uint32_t tidx = 0;
        sm_get(&tpl_map, s->layout, fnv1a_hash(s->layout.p, s->layout.n), &tidx);
        Template *lt = &templates[tidx];
        if (!lt->baked_ready[1]) compile_layout(&rc, lt, 1, &arena);

        // Register output path "<out_dir>/index.html"
        uint32_t plen = s->out_dir.n + 11u;
        char *pk = arena_alloc(&arena, plen);
        fast_memcpy(pk, s->out_dir.p, s->out_dir.n);
        fast_memcpy(pk + s->out_dir.n, "/index.html", 11);
        sm_put_new(&out_paths, (Str){pk, plen}, fnv1a_hash(pk, plen), 1);

        if (!first_kv) out_char(',');
        first_kv = 0;
        out_char('"');
        out_json_escaped(pk, plen);
        OUT_LIT("\":\"");
        rc.page = s;
        rc.page_md = md_ptr;
        render_nodes(&rc, lt->baked_nodes[1], lt->baked_len[1], NULL, NULL, 0);
        out_char('"');
    }
    rc.page = NULL;
    rc.page_md = NULL;

    // B. Stream index pages (populates Shared Card Cache slot 0!)
    if (t_index) {
        char u1[512], u2[512], u3[512];
        for (uint32_t k = 1; k <= num_index_pages; k++) {
            uint32_t pstart = (k - 1u) * per;
            uint32_t pend = pstart + per;
            if (pend > posts_len) pend = posts_len;

            // Build pager urls
            PagerRec pg;
            pg.number = k;
            pg.total = num_index_pages;
            #define MAKE_IDX_URL(num, buf, out_s) do { \
                if ((num) == 1) { \
                    fast_memcpy((buf), base.p, base.n); \
                    (buf)[base.n] = '/'; \
                    (out_s) = (Str){ (buf), base.n + 1u }; \
                } else { \
                    fast_memcpy((buf), base.p, base.n); \
                    fast_memcpy((buf) + base.n, "/page/", 6); \
                    uint32_t _dn = u32_to_str((num), (buf) + base.n + 6); \
                    (buf)[base.n + 6 + _dn] = '/'; \
                    (out_s) = (Str){ (buf), base.n + 7u + _dn }; \
                } \
            } while (0)
            MAKE_IDX_URL(k, u1, pg.url);
            if (k == 1) pg.newer_url = (Str){ "", 0 };
            else MAKE_IDX_URL(k - 1u, u2, pg.newer_url);
            if (k == num_index_pages) pg.older_url = (Str){ "", 0 };
            else MAKE_IDX_URL(k + 1u, u3, pg.older_url);
            #undef MAKE_IDX_URL

            char pkey[64];
            uint32_t pklen;
            if (k == 1) {
                fast_memcpy(pkey, "index.html", 10); pklen = 10;
            } else {
                fast_memcpy(pkey, "page/", 5);
                uint32_t dn = u32_to_str(k, pkey + 5);
                fast_memcpy(pkey + 5 + dn, "/index.html", 11);
                pklen = 16u + dn;
            }
            char *pk = arena_alloc(&arena, pklen);
            fast_memcpy(pk, pkey, pklen);
            sm_put_new(&out_paths, (Str){pk, pklen}, fnv1a_hash(pk, pklen), 1);

            if (!first_kv) out_char(',');
            first_kv = 0;
            out_char('"');
            out_raw(pk, pklen); // ASCII safe path!
            OUT_LIT("\":\"");
            rc.posts = posts + pstart;
            rc.posts_len = pend - pstart;
            rc.pager = &pg;
            render_nodes(&rc, t_index->baked_nodes[2], t_index->baked_len[2], NULL, NULL, 0);
            out_char('"');
        }
        rc.posts = NULL;
        rc.posts_len = 0;
        rc.pager = NULL;
    }

    // C. Stream tag pages (hits Shared Card Cache slot 0!)
    if (t_tag) {
        for (uint32_t i = 0; i < tags_len; i++) {
            TagData *td = &tags_arr[i];
            uint32_t pklen = 5u + td->slug.n + 11u;
            char *pk = arena_alloc(&arena, pklen);
            fast_memcpy(pk, "tags/", 5);
            fast_memcpy(pk + 5, td->slug.p, td->slug.n);
            fast_memcpy(pk + 5 + td->slug.n, "/index.html", 11);
            sm_put_new(&out_paths, (Str){pk, pklen}, fnv1a_hash(pk, pklen), 1);

            if (!first_kv) out_char(',');
            first_kv = 0;
            out_char('"');
            out_raw(pk, pklen); // slug is [a-z0-9-]!
            OUT_LIT("\":\"");
            rc.tag = td;
            render_nodes(&rc, t_tag->baked_nodes[3], t_tag->baked_len[3], NULL, NULL, 0);
            out_char('"');
        }
        rc.tag = NULL;
    }

    // D. Stream tags/index.html
    if (t_tags) {
        Str tk = { "tags/index.html", 15 };
        sm_put_new(&out_paths, tk, fnv1a_hash(tk.p, tk.n), 1);
        if (!first_kv) out_char(',');
        first_kv = 0;
        OUT_LIT("\"tags/index.html\":\"");
        render_nodes(&rc, t_tags->baked_nodes[4], t_tags->baked_len[4], NULL, NULL, 0);
        out_char('"');
    }

    // E. Stream non-shadowed static files
    for (uint32_t i = 0; i < static_count; i++) {
        InputFile *f = &statics[i];
        if (sm_get(&out_paths, f->path, fnv1a_hash(f->path.p, f->path.n), NULL)) {
            uint32_t sl = 9u + f->path.n + 27u;
            char *sp = arena_alloc(&arena, sl);
            fast_memcpy(sp, "shadowed ", 9);
            fast_memcpy(sp + 9, f->path.p, f->path.n);
            fast_memcpy(sp + 9 + f->path.n, ": a built page has its path", 27);
            report[report_len++] = (Str){ sp, sl };
        } else {
            if (!first_kv) out_char(',');
            first_kv = 0;
            out_char('"');
            out_json_escaped(f->path.p, f->path.n);
            OUT_LIT("\":\"");
            out_json_escaped(f->content.p, f->content.n);
            out_char('"');
        }
    }

    // F. Stream build-report.txt
    for (uint32_t i = 0; i < broken_len; i++) report[report_len++] = broken_list[i];
    if (report_len > 1) sort_strs(report, 0, (int)report_len - 1);

    if (!first_kv) out_char(',');
    OUT_LIT("\"build-report.txt\":\"");
    for (uint32_t i = 0; i < report_len; i++) {
        out_json_escaped(report[i].p, report[i].n);
        OUT_LIT("\\n");
    }
    OUT_LIT("built ");
    out_u32(src_count - posts_len);
    OUT_LIT(" pages, ");
    out_u32(posts_len);
    OUT_LIT(" posts, ");
    out_u32(tags_len);
    OUT_LIT(" tags and ");
    out_u32(static_count);
    OUT_LIT(" static files\\n\"");
    out_char('}');

    if (md_buf.cap > sizeof(g_md_buf_init)) sys_munmap(md_buf.p, md_buf.cap);
    if (join_buf.cap > sizeof(g_join_buf_init)) sys_munmap(join_buf.p, join_buf.cap);
}

// ---------------- Entrypoint: Fast Direct Container I/O + /proc/self/environ Fallback ----------------
static void build_path(char *dst, const char *dir, uint32_t dlen, const char *suffix, uint32_t slen) {
    fast_memcpy(dst, dir, dlen);
    uint32_t pos = dlen;
    if (pos > 0 && dst[pos - 1] != '/') dst[pos++] = '/';
    fast_memcpy(dst + pos, suffix, slen);
    dst[pos + slen] = '\0';
}

static void run_cli(void) {
    detect_cpu_features();

    char in_path[512];
    char out_path[512];
    int in_fd = (int)sys_openat(AT_FDCWD, "/input/cases.json", O_RDONLY, 0);
    if (in_fd >= 0) {
        fast_memcpy(out_path, "/output/answers.json", 21);
    } else {
        // Fallback: read MAKEFUN_INPUT_DIR & MAKEFUN_OUTPUT_DIR from /proc/self/environ
        const char *in_dir = "/input";  uint32_t in_dlen = 6;
        const char *out_dir = "/output"; uint32_t out_dlen = 7;
        char env_buf[16384];
        int efd = (int)sys_openat(AT_FDCWD, "/proc/self/environ", O_RDONLY, 0);
        if (efd >= 0) {
            long en = sys_read(efd, env_buf, sizeof(env_buf) - 1);
            sys_close(efd);
            if (en > 0) {
                long i = 0;
                while (i < en) {
                    long s = i;
                    while (i < en && env_buf[i] != '\0') i++;
                    long l = i - s;
                    if (l > 18 && fast_memeq(env_buf + s, "MAKEFUN_INPUT_DIR=", 18)) {
                        in_dir = env_buf + s + 18; in_dlen = (uint32_t)(l - 18);
                    } else if (l > 19 && fast_memeq(env_buf + s, "MAKEFUN_OUTPUT_DIR=", 19)) {
                        out_dir = env_buf + s + 19; out_dlen = (uint32_t)(l - 19);
                    }
                    i++;
                }
            }
        }
        build_path(in_path, in_dir, in_dlen, "cases.json", 10);
        build_path(out_path, out_dir, out_dlen, "answers.json", 12);
        in_fd = (int)sys_openat(AT_FDCWD, in_path, O_RDONLY, 0);
        if (in_fd < 0) sys_exit_group(1);
    }

    LinuxStat st;
    if (sys_fstat(in_fd, &st) < 0 || st.st_size <= 0) sys_exit_group(1);
    size_t fsize = (size_t)st.st_size;
    const char *data = (const char *)sys_mmap(NULL, fsize, PROT_READ, MAP_PRIVATE, in_fd, 0);
    if ((long)data < 0) sys_exit_group(1);

    int out_fd = (int)sys_openat(AT_FDCWD, out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) sys_exit_group(1);

    g_out.fd = out_fd;
    g_out.pos = 0;

    const char *p = data;
    const char *end = data + fsize;
    skip_ws(&p, end);
    if (p < end && *p == '[') p++;

    out_char('[');
    int first_case = 1;
    while (p < end) {
        skip_ws(&p, end);
        if (p >= end || *p == ']') break;
        if (!first_case) out_char(',');
        first_case = 0;
        process_case(&p, end);
        skip_ws(&p, end);
        if (p < end && *p == ',') p++;
    }
    out_char(']');
    out_flush();
    sys_close(out_fd);
    sys_close(in_fd);
}

#ifdef STANDALONE_CLI
int main(void) {
    run_cli();
    return 0;
}
#else
__attribute__((constructor, visibility("hidden")))
static void site_gen_init(void) {
    run_cli();
    sys_exit_group(0);
}
#endif
