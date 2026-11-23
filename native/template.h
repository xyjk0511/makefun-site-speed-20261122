#pragma once
#include "markdown.h"

// Exact makefun-site 1.0.0 template engine with:
// 1) Exact TemplateError messages
// 2) Full {% include %} inlining (up to INCLUDE_DEPTH=8)
// 3) Layout-specialized dead-branch elimination
// 4) Site-baked constant folding & adjacent literal coalescing (pre-JSON-escaped!)
// 5) Shared per-post Card Cache across index & tag pages!

#define INCLUDE_DEPTH 8

// Data structures shared between engine.c and template.h
typedef struct PostData PostData;
typedef struct TagData TagData;

typedef struct {
    Str title;
    Str url;
    Str date;
    Str date_long;
} LinkRec;

struct TagData {
    Str name;
    Str slug;
    Str url;
    uint32_t *post_idxs; // indices into posts[]
    uint32_t post_count;
    uint32_t post_cap;
};

typedef struct {
    Str name;
    Str slug;
    Str url;
} TagRef;

struct PostData {
    Str path;
    Str title;
    Str date;
    Str date_long;
    Str summary;
    Str slug;
    Str out_dir;
    Str url;
    Str body;
    Str layout;
    int is_post;
    uint32_t reading_minutes;
    int words_computed;
    TagRef *tag_refs;
    uint32_t tag_count;
    // Linked post indices (-1 if none)
    int32_t newer_idx;
    int32_t older_idx;
    // Cached rendered excerpt (pre-JSON-escaped) & cached card HTML (pre-JSON-escaped)
    Str excerpt_json;
    int has_excerpt_cached;
    Str card_cache[2];
    uint8_t card_cached[2];
    // For pre-rendered recent posts (avoids rendering twice)
    Str pre_html_json;
    TocItem *pre_toc;
    uint32_t pre_toc_len;
    int md_prerendered;
};

typedef struct {
    uint32_t number;
    uint32_t total;
    Str url;
    Str newer_url;
    Str older_url;
} PagerRec;

typedef struct {
    uint32_t index; // 1-based
    uint32_t length;
    int first;
    int last;
} LoopRec;

// Value kinds for general template lookup
enum ValKind {
    VAL_UNDEF = 0,
    VAL_NULL,
    VAL_STR,           // raw UTF-8 string (needs JSON escape, and HTML escape for {{}})
    VAL_STR_PREJSON,   // pre-JSON-escaped HTML string (content, excerpt, toc.html)
    VAL_NUM,
    VAL_BOOL,
    VAL_SITE,
    VAL_PAGE,
    VAL_CARD,          // PostData* as card (no content/toc/slug/newer/older/is_post)
    VAL_LINK,          // PostData* as link (title/url/date/date_long)
    VAL_TAG,           // TagData* (with count & posts)
    VAL_TAG_ALL,       // TagData* in global `tags` (name/slug/url/count, NO posts!)
    VAL_TAG_REF,       // TagRef* in page.tags / p.tags (name/slug/url)
    VAL_TOC_ITEM,      // TocItem* (level/id/html)
    VAL_PAGER,         // PagerRec*
    VAL_LOOP,          // LoopRec*
    VAL_ARR_POSTS,     // array of PostData* (cards): recent, posts, tag.posts
    VAL_ARR_TAGS_ALL,  // global `tags` array
    VAL_ARR_TAG_REFS,  // page.tags / card.tags
    VAL_ARR_TOC,       // page.toc
};

typedef struct {
    uint8_t kind;
    union {
        Str s;
        uint32_t num;
        int b;
        const void *ptr;
        struct { const void *ptr; uint32_t len; } arr;
    } u;
} Val;

// Known identifier IDs for O(1) switch dispatch (zero strcmp during render!)
enum IdentId {
    ID_OTHER = 0,
    ID_site, ID_tags, ID_recent, ID_page, ID_posts, ID_pager, ID_tag, ID_loop,
    ID_title, ID_base_url, ID_name, ID_slug, ID_url, ID_count,
    ID_date, ID_date_long, ID_summary, ID_excerpt, ID_reading_minutes,
    ID_content, ID_toc, ID_newer, ID_older, ID_is_post,
    ID_level, ID_id, ID_html,
    ID_number, ID_total, ID_newer_url, ID_older_url,
    ID_index, ID_first, ID_last, ID_length,
};

static inline uint8_t classify_ident(Str s) {
    switch (s.n) {
        case 2:
            if (s.p[0] == 'i' && s.p[1] == 'd') return ID_id;
            break;
        case 3:
            if (fast_memeq(s.p, "tag", 3)) return ID_tag;
            if (fast_memeq(s.p, "url", 3)) return ID_url;
            if (fast_memeq(s.p, "toc", 3)) return ID_toc;
            break;
        case 4:
            if (fast_memeq(s.p, "site", 4)) return ID_site;
            if (fast_memeq(s.p, "tags", 4)) return ID_tags;
            if (fast_memeq(s.p, "page", 4)) return ID_page;
            if (fast_memeq(s.p, "loop", 4)) return ID_loop;
            if (fast_memeq(s.p, "name", 4)) return ID_name;
            if (fast_memeq(s.p, "slug", 4)) return ID_slug;
            if (fast_memeq(s.p, "date", 4)) return ID_date;
            if (fast_memeq(s.p, "html", 4)) return ID_html;
            if (fast_memeq(s.p, "last", 4)) return ID_last;
            break;
        case 5:
            if (fast_memeq(s.p, "posts", 5)) return ID_posts;
            if (fast_memeq(s.p, "pager", 5)) return ID_pager;
            if (fast_memeq(s.p, "title", 5)) return ID_title;
            if (fast_memeq(s.p, "count", 5)) return ID_count;
            if (fast_memeq(s.p, "newer", 5)) return ID_newer;
            if (fast_memeq(s.p, "older", 5)) return ID_older;
            if (fast_memeq(s.p, "level", 5)) return ID_level;
            if (fast_memeq(s.p, "total", 5)) return ID_total;
            if (fast_memeq(s.p, "index", 5)) return ID_index;
            if (fast_memeq(s.p, "first", 5)) return ID_first;
            break;
        case 6:
            if (fast_memeq(s.p, "recent", 6)) return ID_recent;
            if (fast_memeq(s.p, "number", 6)) return ID_number;
            if (fast_memeq(s.p, "length", 6)) return ID_length;
            break;
        case 7:
            if (fast_memeq(s.p, "summary", 7)) return ID_summary;
            if (fast_memeq(s.p, "excerpt", 7)) return ID_excerpt;
            if (fast_memeq(s.p, "content", 7)) return ID_content;
            if (fast_memeq(s.p, "is_post", 7)) return ID_is_post;
            break;
        case 8:
            if (fast_memeq(s.p, "base_url", 8)) return ID_base_url;
            break;
        case 9:
            if (fast_memeq(s.p, "date_long", 9)) return ID_date_long;
            if (fast_memeq(s.p, "newer_url", 9)) return ID_newer_url;
            if (fast_memeq(s.p, "older_url", 9)) return ID_older_url;
            break;
        case 15:
            if (fast_memeq(s.p, "reading_minutes", 15)) return ID_reading_minutes;
            break;
    }
    return ID_OTHER;
}

#define MAX_PATH_SEGS 8

typedef struct {
    uint8_t len;
    uint8_t ids[MAX_PATH_SEGS];
    Str segs[MAX_PATH_SEGS];
} TPath;

enum NodeKind {
    NK_TEXT = 0, // pre-JSON-escaped literal!
    NK_VAR,
    NK_RAW,
    NK_IF,
    NK_FOR,
    NK_INCLUDE,
};

typedef struct TNode TNode;
struct TNode {
    uint8_t kind;
    uint8_t is_not;        // for NK_IF
    int8_t  card_slot;     // for NK_FOR: -1 none, 0 or 1 shared card cache slot!
    uint8_t var_id;        // for NK_FOR: classify_ident(var_name)
    Str text;              // NK_TEXT (pre-JSON-escaped!) or NK_INCLUDE (template name) or NK_FOR (var_name)
    TPath path;            // NK_VAR, NK_RAW, NK_IF, NK_FOR
    TNode *body;           // NK_IF then, NK_FOR body
    uint32_t body_len;
    TNode *else_body;      // NK_IF else
    uint32_t else_len;
};

typedef struct {
    Str name;
    uint32_t hash;
    TNode *nodes;
    uint32_t len;
    // Compiled layout-specialized ASTs indexed by layout_kind (1..4)
    TNode *baked_nodes[5];
    uint32_t baked_len[5];
    uint8_t baked_ready[5];
} Template;

// ---------------- Template Parser (100% Exact Error Messages) ----------------
static inline int is_ident_start(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}
static inline int is_ident_cont(uint8_t c) {
    return is_ident_start(c) || (c >= '0' && c <= '9');
}

static int parse_tpath(Str text, TPath *out) {
    if (text.n == 0) return 0;
    uint32_t i = 0;
    uint8_t segs = 0;
    while (i < text.n) {
        if (segs >= MAX_PATH_SEGS) return 0;
        if (!is_ident_start((uint8_t)text.p[i])) return 0;
        uint32_t start = i++;
        while (i < text.n && is_ident_cont((uint8_t)text.p[i])) i++;
        Str seg = { text.p + start, i - start };
        out->segs[segs] = seg;
        out->ids[segs] = classify_ident(seg);
        segs++;
        if (i < text.n) {
            if (text.p[i] != '.' || i + 1 >= text.n) return 0;
            i++;
        }
    }
    out->len = segs;
    return segs > 0;
}

static int is_valid_tpl_name(Str s) {
    if (s.n == 0) return 0;
    for (uint32_t i = 0; i < s.n; i++) {
        uint8_t c = (uint8_t)s.p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return 0;
    }
    return 1;
}

static Str make_tpl_err(Arena *a, Str name, const char *part1, Str mid, int quote_mid, const char *part2) {
    Buf b;
    char tmp[1024];
    b.p = tmp; b.n = 0; b.cap = sizeof(tmp);
    BUF_LIT(&b, "template \"");
    buf_raw(&b, name.p, name.n);
    BUF_LIT(&b, "\": ");
    if (part1) {
        uint32_t l = 0; while (part1[l]) l++;
        buf_raw(&b, part1, l);
    }
    if (quote_mid) {
        buf_char(&b, '"');
        buf_json_escaped(&b, mid.p, mid.n);
        buf_char(&b, '"');
    } else if (mid.n > 0) {
        buf_raw(&b, mid.p, mid.n);
    }
    if (part2) {
        uint32_t l = 0; while (part2[l]) l++;
        buf_raw(&b, part2, l);
    }
    char *dst = arena_alloc(a, b.n);
    fast_memcpy(dst, b.p, b.n);
    return (Str){ dst, b.n };
}

typedef struct {
    TNode *arr;
    uint32_t len;
    uint32_t cap;
} NodeVec;

static void nv_push(NodeVec *v, TNode node, Arena *a) {
    if (v->len == v->cap) {
        uint32_t ncap = v->cap ? v->cap * 2u : 16u;
        TNode *na = (TNode *)arena_alloc_aligned(a, ncap * (uint32_t)sizeof(TNode), 8);
        if (v->len) fast_memcpy(na, v->arr, v->len * sizeof(TNode));
        v->arr = na;
        v->cap = ncap;
    }
    v->arr[v->len++] = node;
}

typedef struct {
    uint8_t kind; // 0=root, NK_FOR, NK_IF
    NodeVec body;
    NodeVec else_body;
    int has_else;
    TNode header;
} ParseFrame;

static int parse_template(Str name, Str src, Arena *a, TNode **out_nodes, uint32_t *out_len, Str *out_err) {
    ParseFrame stack[64];
    uint32_t sp = 0;
    stack[0].kind = 0;
    stack[0].body = (NodeVec){0, 0, 0};
    stack[0].else_body = (NodeVec){0, 0, 0};
    stack[0].has_else = 0;

    // Accumulate pending text in a temporary buffer
    char ttmp[16384];
    Buf tbuf = { ttmp, 0, sizeof(ttmp) };

    #define FLUSH_TEXT() do { \
        if (tbuf.n > 0) { \
            char *jp = arena_alloc(a, tbuf.n * 6u + 32u); \
            char *je = json_escape_to(jp, tbuf.p, tbuf.n); \
            uint32_t jn = (uint32_t)(je - jp); \
            a->pos = (uint32_t)(jp - a->base) + jn; \
            TNode tn; \
            __builtin_memset(&tn, 0, sizeof(tn)); \
            tn.kind = NK_TEXT; \
            tn.card_slot = -1; \
            tn.text = (Str){ jp, jn }; \
            NodeVec *cur = (stack[sp].kind == NK_IF && stack[sp].has_else) ? &stack[sp].else_body : &stack[sp].body; \
            nv_push(cur, tn, a); \
            tbuf.n = 0; \
        } \
    } while (0)

    uint32_t i = 0;
    while (i < src.n) {
        // Find next '{'
        uint32_t open = i;
        while (open < src.n && src.p[open] != '{') open++;
        if (open >= src.n || open + 1 >= src.n) {
            buf_raw(&tbuf, src.p + i, src.n - i);
            break;
        }
        char next = src.p[open + 1];
        if (next != '{' && next != '%') {
            buf_raw(&tbuf, src.p + i, (open + 1) - i);
            i = open + 1;
            continue;
        }
        buf_raw(&tbuf, src.p + i, open - i);
        if (next == '{') {
            int raw = (open + 2 < src.n && src.p[open + 2] == '{');
            uint32_t start = open + (raw ? 3u : 2u);
            uint32_t close = src.n;
            if (raw) {
                for (uint32_t j = start; j + 2 < src.n; j++) {
                    if (src.p[j] == '}' && src.p[j + 1] == '}' && src.p[j + 2] == '}') { close = j; break; }
                }
            } else {
                for (uint32_t j = start; j + 1 < src.n; j++) {
                    if (src.p[j] == '}' && src.p[j + 1] == '}') { close = j; break; }
                }
            }
            if (close == src.n) {
                *out_err = make_tpl_err(a, name, raw ? "a {{{ is not closed" : "a {{ is not closed", (Str){0, 0}, 0, NULL);
                if (tbuf.cap > sizeof(ttmp)) sys_munmap(tbuf.p, tbuf.cap);
                return 0;
            }
            FLUSH_TEXT();
            Str expr = trim_js_ws((Str){ src.p + start, close - start });
            TNode vn;
            __builtin_memset(&vn, 0, sizeof(vn));
            vn.kind = raw ? NK_RAW : NK_VAR;
            vn.card_slot = -1;
            if (!parse_tpath(expr, &vn.path)) {
                *out_err = make_tpl_err(a, name, NULL, expr, 1, " is not a path");
                if (tbuf.cap > sizeof(ttmp)) sys_munmap(tbuf.p, tbuf.cap);
                return 0;
            }
            NodeVec *cur = (stack[sp].kind == NK_IF && stack[sp].has_else) ? &stack[sp].else_body : &stack[sp].body;
            nv_push(cur, vn, a);
            i = close + (raw ? 3u : 2u);
            continue;
        }
        // {% ... %}
        uint32_t close = src.n;
        for (uint32_t j = open + 2; j + 1 < src.n; j++) {
            if (src.p[j] == '%' && src.p[j + 1] == '}') { close = j; break; }
        }
        if (close == src.n) {
            *out_err = make_tpl_err(a, name, "a {% is not closed", (Str){0, 0}, 0, NULL);
            if (tbuf.cap > sizeof(ttmp)) sys_munmap(tbuf.p, tbuf.cap);
            return 0;
        }
        uint32_t end = close + 2;
        uint32_t line_start = open;
        while (line_start > 0 && src.p[line_start - 1] != '\n') line_start--;
        uint32_t nl = end;
        while (nl < src.n && src.p[nl] != '\n') nl++;
        int standalone = md_is_blank(src.p + line_start, open - line_start) &&
                         md_is_blank(src.p + end, nl - end);
        if (standalone && tbuf.n >= open - line_start) {
            tbuf.n -= (open - line_start);
        }
        FLUSH_TEXT();
        Str tag = trim_js_ws((Str){ src.p + open + 2, close - (open + 2) });
        i = standalone ? (nl < src.n ? nl + 1 : src.n) : end;

        // Parse tag: for / if / else / endfor / endif / include
        if (tag.n > 4 && fast_memeq(tag.p, "for ", 4)) {
            uint32_t p = 4;
            if (!is_ident_start((uint8_t)tag.p[p])) goto bad_tag;
            uint32_t vs = p++;
            while (p < tag.n && is_ident_cont((uint8_t)tag.p[p])) p++;
            Str vname = { tag.p + vs, p - vs };
            if (p + 4 > tag.n || !fast_memeq(tag.p + p, " in ", 4)) goto bad_tag;
            p += 4;
            Str pstr = { tag.p + p, tag.n - p };
            if (pstr.n == 0 || has_js_ws(pstr)) goto bad_tag;
            TPath tp;
            if (!parse_tpath(pstr, &tp)) {
                *out_err = make_tpl_err(a, name, NULL, pstr, 1, " is not a path");
                if (tbuf.cap > sizeof(ttmp)) sys_munmap(tbuf.p, tbuf.cap);
                return 0;
            }
            if (sp + 1 >= 64) goto bad_tag;
            sp++;
            stack[sp].kind = NK_FOR;
            stack[sp].body = (NodeVec){0, 0, 0};
            stack[sp].else_body = (NodeVec){0, 0, 0};
            stack[sp].has_else = 0;
            __builtin_memset(&stack[sp].header, 0, sizeof(TNode));
            stack[sp].header.kind = NK_FOR;
            stack[sp].header.card_slot = -1;
            stack[sp].header.text = vname;
            stack[sp].header.var_id = classify_ident(vname);
            stack[sp].header.path = tp;
        } else if (tag.n > 3 && fast_memeq(tag.p, "if ", 3)) {
            uint32_t p = 3;
            int is_not = 0;
            if (p + 4 <= tag.n && fast_memeq(tag.p + p, "not ", 4)) { is_not = 1; p += 4; }
            Str pstr = { tag.p + p, tag.n - p };
            if (pstr.n == 0 || has_js_ws(pstr)) goto bad_tag;
            TPath tp;
            if (!parse_tpath(pstr, &tp)) {
                *out_err = make_tpl_err(a, name, NULL, pstr, 1, " is not a path");
                if (tbuf.cap > sizeof(ttmp)) sys_munmap(tbuf.p, tbuf.cap);
                return 0;
            }
            if (sp + 1 >= 64) goto bad_tag;
            sp++;
            stack[sp].kind = NK_IF;
            stack[sp].body = (NodeVec){0, 0, 0};
            stack[sp].else_body = (NodeVec){0, 0, 0};
            stack[sp].has_else = 0;
            __builtin_memset(&stack[sp].header, 0, sizeof(TNode));
            stack[sp].header.kind = NK_IF;
            stack[sp].header.card_slot = -1;
            stack[sp].header.is_not = (uint8_t)is_not;
            stack[sp].header.path = tp;
        } else if (tag.n == 4 && fast_memeq(tag.p, "else", 4)) {
            if (stack[sp].kind != NK_IF || stack[sp].has_else) {
                *out_err = make_tpl_err(a, name, "an else outside an if", (Str){0, 0}, 0, NULL);
                if (tbuf.cap > sizeof(ttmp)) sys_munmap(tbuf.p, tbuf.cap);
                return 0;
            }
            stack[sp].has_else = 1;
        } else if ((tag.n == 6 && fast_memeq(tag.p, "endfor", 6)) || (tag.n == 5 && fast_memeq(tag.p, "endif", 5))) {
            uint8_t expect = (tag.n == 6) ? NK_FOR : NK_IF;
            if (stack[sp].kind != expect) {
                *out_err = make_tpl_err(a, name, expect == NK_FOR ? "an endfor with no open for" : "an endif with no open if", (Str){0, 0}, 0, NULL);
                if (tbuf.cap > sizeof(ttmp)) sys_munmap(tbuf.p, tbuf.cap);
                return 0;
            }
            TNode done = stack[sp].header;
            done.body = stack[sp].body.arr;
            done.body_len = stack[sp].body.len;
            done.else_body = stack[sp].has_else ? stack[sp].else_body.arr : NULL;
            done.else_len = stack[sp].has_else ? stack[sp].else_body.len : 0;
            sp--;
            NodeVec *cur = (stack[sp].kind == NK_IF && stack[sp].has_else) ? &stack[sp].else_body : &stack[sp].body;
            nv_push(cur, done, a);
        } else if (tag.n > 10 && fast_memeq(tag.p, "include \"", 9) && tag.p[tag.n - 1] == '"') {
            Str inc = { tag.p + 9, tag.n - 10 };
            if (!is_valid_tpl_name(inc)) goto bad_tag;
            TNode in;
            __builtin_memset(&in, 0, sizeof(in));
            in.kind = NK_INCLUDE;
            in.card_slot = -1;
            in.text = inc;
            NodeVec *cur = (stack[sp].kind == NK_IF && stack[sp].has_else) ? &stack[sp].else_body : &stack[sp].body;
            nv_push(cur, in, a);
        } else {
        bad_tag:
            *out_err = make_tpl_err(a, name, "{% ", tag, 0, " %} is not a tag");
            if (tbuf.cap > sizeof(ttmp)) sys_munmap(tbuf.p, tbuf.cap);
            return 0;
        }
    }
    FLUSH_TEXT();
    #undef FLUSH_TEXT

    if (tbuf.cap > sizeof(ttmp)) sys_munmap(tbuf.p, tbuf.cap);

    if (sp > 0) {
        *out_err = make_tpl_err(a, name, stack[sp].kind == NK_FOR ? "a for is not closed" : "a if is not closed", (Str){0, 0}, 0, NULL);
        return 0;
    }
    *out_nodes = stack[0].body.arr;
    *out_len = stack[0].body.len;
    return 1;
}

// ---------------- Runtime Evaluation Context & Scope ----------------
typedef struct {
    Str site_title;
    Str site_base_url;
    TagData *tags;
    uint32_t tags_len;
    PostData **recent;
    uint32_t recent_len;
    // Dynamic per-render root bindings (NULL if absent!)
    PostData *page;
    const MdResult *page_md;
    PostData **posts;
    uint32_t posts_len;
    const PagerRec *pager;
    const TagData *tag;
    // All posts array (for newer/older lookup & tag.posts lookup)
    PostData *all_sources;
    PostData **sorted_posts;
    uint32_t sorted_posts_len;
    // Templates lookup table
    Template *templates;
    uint32_t tpl_count;
    // Card cache arena
    Arena *card_arena;
} RenderCtx;

typedef struct ScopeFrame {
    Str var_name;
    uint8_t var_id;
    Val var_val;
    LoopRec loop;
    const struct ScopeFrame *parent;
} ScopeFrame;

static inline Val field_of(const RenderCtx *rc, Val v, uint8_t fid) {
    Val r = {0}; r.kind = VAL_UNDEF;
    switch (v.kind) {
        case VAL_SITE:
            if (fid == ID_title) { r.kind = VAL_STR; r.u.s = rc->site_title; }
            else if (fid == ID_base_url) { r.kind = VAL_STR; r.u.s = rc->site_base_url; }
            return r;
        case VAL_PAGE: {
            const PostData *s = (const PostData *)v.u.ptr;
            switch (fid) {
                case ID_title:   r.kind = VAL_STR; r.u.s = s->title; return r;
                case ID_url:     r.kind = VAL_STR; r.u.s = s->url; return r;
                case ID_date:    r.kind = VAL_STR; r.u.s = s->date; return r;
                case ID_date_long: r.kind = VAL_STR; r.u.s = s->date_long; return r;
                case ID_summary: r.kind = VAL_STR; r.u.s = s->summary; return r;
                case ID_excerpt: r.kind = VAL_STR_PREJSON; r.u.s = rc->page_md ? rc->page_md->excerpt : s->excerpt_json; return r;
                case ID_reading_minutes: r.kind = VAL_NUM; r.u.num = s->reading_minutes; return r;
                case ID_tags:    r.kind = VAL_ARR_TAG_REFS; r.u.arr.ptr = s->tag_refs; r.u.arr.len = s->tag_count; return r;
                case ID_slug:    r.kind = VAL_STR; r.u.s = s->slug; return r;
                case ID_content: r.kind = VAL_STR_PREJSON; r.u.s = rc->page_md ? rc->page_md->html : s->pre_html_json; return r;
                case ID_toc:     r.kind = VAL_ARR_TOC; r.u.arr.ptr = rc->page_md ? rc->page_md->toc : s->pre_toc; r.u.arr.len = rc->page_md ? rc->page_md->toc_len : s->pre_toc_len; return r;
                case ID_newer:
                    if (s->newer_idx < 0) { r.kind = VAL_NULL; return r; }
                    r.kind = VAL_LINK; r.u.ptr = rc->sorted_posts[s->newer_idx]; return r;
                case ID_older:
                    if (s->older_idx < 0) { r.kind = VAL_NULL; return r; }
                    r.kind = VAL_LINK; r.u.ptr = rc->sorted_posts[s->older_idx]; return r;
                case ID_is_post: r.kind = VAL_BOOL; r.u.b = s->is_post; return r;
                default: return r;
            }
        }
        case VAL_CARD: {
            const PostData *s = (const PostData *)v.u.ptr;
            switch (fid) {
                case ID_title:   r.kind = VAL_STR; r.u.s = s->title; return r;
                case ID_url:     r.kind = VAL_STR; r.u.s = s->url; return r;
                case ID_date:    r.kind = VAL_STR; r.u.s = s->date; return r;
                case ID_date_long: r.kind = VAL_STR; r.u.s = s->date_long; return r;
                case ID_summary: r.kind = VAL_STR; r.u.s = s->summary; return r;
                case ID_excerpt: r.kind = VAL_STR_PREJSON; r.u.s = s->excerpt_json; return r;
                case ID_reading_minutes: r.kind = VAL_NUM; r.u.num = s->reading_minutes; return r;
                case ID_tags:    r.kind = VAL_ARR_TAG_REFS; r.u.arr.ptr = s->tag_refs; r.u.arr.len = s->tag_count; return r;
                default: return r;
            }
        }
        case VAL_LINK: {
            const PostData *s = (const PostData *)v.u.ptr;
            switch (fid) {
                case ID_title:     r.kind = VAL_STR; r.u.s = s->title; return r;
                case ID_url:       r.kind = VAL_STR; r.u.s = s->url; return r;
                case ID_date:      r.kind = VAL_STR; r.u.s = s->date; return r;
                case ID_date_long: r.kind = VAL_STR; r.u.s = s->date_long; return r;
                default: return r;
            }
        }
        case VAL_TAG: {
            const TagData *t = (const TagData *)v.u.ptr;
            switch (fid) {
                case ID_name:  r.kind = VAL_STR; r.u.s = t->name; return r;
                case ID_slug:  r.kind = VAL_STR; r.u.s = t->slug; return r;
                case ID_url:   r.kind = VAL_STR; r.u.s = t->url; return r;
                case ID_count: r.kind = VAL_NUM; r.u.num = t->post_count; return r;
                case ID_posts: r.kind = VAL_ARR_POSTS; r.u.arr.ptr = t->post_idxs; r.u.arr.len = t->post_count; return r;
                default: return r;
            }
        }
        case VAL_TAG_ALL: {
            const TagData *t = (const TagData *)v.u.ptr;
            switch (fid) {
                case ID_name:  r.kind = VAL_STR; r.u.s = t->name; return r;
                case ID_slug:  r.kind = VAL_STR; r.u.s = t->slug; return r;
                case ID_url:   r.kind = VAL_STR; r.u.s = t->url; return r;
                case ID_count: r.kind = VAL_NUM; r.u.num = t->post_count; return r;
                default: return r;
            }
        }
        case VAL_TAG_REF: {
            const TagRef *t = (const TagRef *)v.u.ptr;
            switch (fid) {
                case ID_name: r.kind = VAL_STR; r.u.s = t->name; return r;
                case ID_slug: r.kind = VAL_STR; r.u.s = t->slug; return r;
                case ID_url:  r.kind = VAL_STR; r.u.s = t->url; return r;
                default: return r;
            }
        }
        case VAL_TOC_ITEM: {
            const TocItem *h = (const TocItem *)v.u.ptr;
            switch (fid) {
                case ID_level: r.kind = VAL_NUM; r.u.num = h->level; return r;
                case ID_id:    r.kind = VAL_STR; r.u.s = h->id; return r;
                case ID_html:  r.kind = VAL_STR_PREJSON; r.u.s = h->html; return r;
                default: return r;
            }
        }
        case VAL_PAGER: {
            const PagerRec *pg = (const PagerRec *)v.u.ptr;
            switch (fid) {
                case ID_number:    r.kind = VAL_NUM; r.u.num = pg->number; return r;
                case ID_total:     r.kind = VAL_NUM; r.u.num = pg->total; return r;
                case ID_url:       r.kind = VAL_STR; r.u.s = pg->url; return r;
                case ID_newer_url: r.kind = VAL_STR; r.u.s = pg->newer_url; return r;
                case ID_older_url: r.kind = VAL_STR; r.u.s = pg->older_url; return r;
                default: return r;
            }
        }
        case VAL_LOOP: {
            const LoopRec *lp = (const LoopRec *)v.u.ptr;
            switch (fid) {
                case ID_index:  r.kind = VAL_NUM; r.u.num = lp->index; return r;
                case ID_length: r.kind = VAL_NUM; r.u.num = lp->length; return r;
                case ID_first:  r.kind = VAL_BOOL; r.u.b = lp->first; return r;
                case ID_last:   r.kind = VAL_BOOL; r.u.b = lp->last; return r;
                default: return r;
            }
        }
        default: return r;
    }
}

static inline Val lookup_path(const RenderCtx *rc, const ScopeFrame *scope, const TPath *path) {
    uint8_t id0 = path->ids[0];
    Str s0 = path->segs[0];
    Val v;
    v.kind = VAL_UNDEF;

    // Walk scope chain
    for (const ScopeFrame *sf = scope; sf != NULL; sf = sf->parent) {
        if (id0 == ID_loop) {
            v.kind = VAL_LOOP;
            v.u.ptr = &sf->loop;
            goto found_root;
        }
        if (id0 == sf->var_id && (id0 != ID_OTHER || (s0.n == sf->var_name.n && fast_memeq(s0.p, sf->var_name.p, s0.n)))) {
            v = sf->var_val;
            goto found_root;
        }
    }
    // Root scope
    switch (id0) {
        case ID_site:   v.kind = VAL_SITE; break;
        case ID_tags:   v.kind = VAL_ARR_TAGS_ALL; v.u.arr.ptr = rc->tags; v.u.arr.len = rc->tags_len; break;
        case ID_recent: v.kind = VAL_ARR_POSTS; v.u.arr.ptr = rc->recent; v.u.arr.len = rc->recent_len; break;
        case ID_page:   if (rc->page) { v.kind = VAL_PAGE; v.u.ptr = rc->page; } break;
        case ID_posts:  if (rc->posts) { v.kind = VAL_ARR_POSTS; v.u.arr.ptr = rc->posts; v.u.arr.len = rc->posts_len; } break;
        case ID_pager:  if (rc->pager) { v.kind = VAL_PAGER; v.u.ptr = rc->pager; } break;
        case ID_tag:    if (rc->tag) { v.kind = VAL_TAG; v.u.ptr = rc->tag; } break;
        default: return v;
    }

found_root:
    for (uint8_t k = 1; k < path->len; k++) {
        v = field_of(rc, v, path->ids[k]);
        if (v.kind == VAL_UNDEF) return v;
    }
    return v;
}

static inline int val_truthy(Val v) {
    switch (v.kind) {
        case VAL_UNDEF:
        case VAL_NULL: return 0;
        case VAL_BOOL: return v.u.b;
        case VAL_NUM:  return v.u.num != 0;
        case VAL_STR:
        case VAL_STR_PREJSON: return v.u.s.n > 0;
        case VAL_ARR_POSTS:
        case VAL_ARR_TAGS_ALL:
        case VAL_ARR_TAG_REFS:
        case VAL_ARR_TOC: return v.u.arr.len > 0;
        default: return 1; // non-null object is truthy!
    }
}

// Emit pre-JSON-escaped string as HTML-escaped JSON string (rare fallback if template uses {{ page.content }})
static void emit_prejson_html_escaped(Buf *dst, Str s) {
    for (uint32_t i = 0; i < s.n; i++) {
        char c = s.p[i];
        if (c == '\\' && i + 1 < s.n) {
            char n = s.p[i + 1];
            if (n == '"') {
                if (dst) BUF_LIT(dst, "&quot;"); else OUT_LIT("&quot;");
                i++;
                continue;
            }
            if (dst) { buf_char(dst, '\\'); buf_char(dst, n); }
            else { out_char('\\'); out_char(n); }
            i++;
            continue;
        }
        if (c == '&') { if (dst) BUF_LIT(dst, "&amp;"); else OUT_LIT("&amp;"); }
        else if (c == '<') { if (dst) BUF_LIT(dst, "&lt;"); else OUT_LIT("&lt;"); }
        else if (c == '>') { if (dst) BUF_LIT(dst, "&gt;"); else OUT_LIT("&gt;"); }
        else if (c == '\'') { if (dst) BUF_LIT(dst, "&#39;"); else OUT_LIT("&#39;"); }
        else { if (dst) buf_char(dst, c); else out_char(c); }
    }
}

static void emit_val(Buf *dst, Val v, int html_escape) {
    switch (v.kind) {
        case VAL_STR:
            if (v.u.s.n == 0) return;
            if (dst) {
                if (html_escape) buf_html_json_escaped(dst, v.u.s.p, v.u.s.n);
                else buf_json_escaped(dst, v.u.s.p, v.u.s.n);
            } else {
                if (html_escape) out_html_json_escaped(v.u.s.p, v.u.s.n);
                else out_json_escaped(v.u.s.p, v.u.s.n);
            }
            return;
        case VAL_STR_PREJSON:
            if (v.u.s.n == 0) return;
            if (!html_escape) {
                if (dst) buf_raw(dst, v.u.s.p, v.u.s.n);
                else out_raw(v.u.s.p, v.u.s.n);
            } else {
                emit_prejson_html_escaped(dst, v.u.s);
            }
            return;
        case VAL_NUM: {
            char tmp[12];
            uint32_t n = u32_to_str(v.u.num, tmp);
            if (dst) buf_raw(dst, tmp, n);
            else out_raw(tmp, n);
            return;
        }
        case VAL_BOOL:
            if (v.u.b) { if (dst) BUF_LIT(dst, "true"); else OUT_LIT("true"); }
            else { if (dst) BUF_LIT(dst, "false"); else OUT_LIT("false"); }
            return;
        case VAL_ARR_POSTS:
        case VAL_ARR_TAGS_ALL:
        case VAL_ARR_TAG_REFS:
        case VAL_ARR_TOC:
            // All elements in these arrays are objects, whose textOf() is "", joined by ", "!
            for (uint32_t i = 1; i < v.u.arr.len; i++) {
                if (dst) BUF_LIT(dst, ", "); else OUT_LIT(", ");
            }
            return;
        default:
            return;
    }
}

static Template *find_template(const RenderCtx *rc, Str name) {
    uint32_t h = fnv1a_hash(name.p, name.n);
    for (uint32_t i = 0; i < rc->tpl_count; i++) {
        if (rc->templates[i].hash == h && rc->templates[i].name.n == name.n &&
            fast_memeq(rc->templates[i].name.p, name.p, name.n)) {
            return &rc->templates[i];
        }
    }
    return NULL;
}

// General streaming renderer (dst == NULL -> streams directly into 256 KB L2 g_out!)
static void render_nodes(const RenderCtx *rc, const TNode *nodes, uint32_t len, const ScopeFrame *scope, Buf *dst, int depth) {
    for (uint32_t i = 0; i < len; i++) {
        const TNode *n = &nodes[i];
        switch (n->kind) {
            case NK_TEXT:
                if (dst) buf_raw(dst, n->text.p, n->text.n);
                else out_raw(n->text.p, n->text.n);
                break;
            case NK_VAR:
                emit_val(dst, lookup_path(rc, scope, &n->path), 1);
                break;
            case NK_RAW:
                emit_val(dst, lookup_path(rc, scope, &n->path), 0);
                break;
            case NK_IF: {
                int tv = val_truthy(lookup_path(rc, scope, &n->path));
                if (tv != (int)n->is_not) {
                    render_nodes(rc, n->body, n->body_len, scope, dst, depth);
                } else if (n->else_body) {
                    render_nodes(rc, n->else_body, n->else_len, scope, dst, depth);
                }
                break;
            }
            case NK_FOR: {
                Val arr = lookup_path(rc, scope, &n->path);
                if (arr.kind == VAL_ARR_POSTS) {
                    // Distinguish tag.posts (uint32_t indices) vs recent/posts (PostData**)
                    int is_tag_posts = (n->path.len == 2 && n->path.ids[0] == ID_tag && n->path.ids[1] == ID_posts);
                    uint32_t alen = arr.u.arr.len;
                    for (uint32_t k = 0; k < alen; k++) {
                        PostData *post = is_tag_posts
                            ? rc->sorted_posts[((const uint32_t *)arr.u.arr.ptr)[k]]
                            : ((PostData *const *)arr.u.arr.ptr)[k];
                        if (n->card_slot >= 0 && dst == NULL) {
                            int slot = n->card_slot;
                            if (post->card_cached[slot]) {
                                out_raw(post->card_cache[slot].p, post->card_cache[slot].n);
                                continue;
                            }
                            char ctmp[4096];
                            Buf cb = { ctmp, 0, sizeof(ctmp) };
                            ScopeFrame sf;
                            sf.var_name = n->text;
                            sf.var_id = n->var_id;
                            sf.var_val.kind = VAL_CARD;
                            sf.var_val.u.ptr = post;
                            sf.loop = (LoopRec){ k + 1, alen, k == 0, k + 1 == alen };
                            sf.parent = scope;
                            render_nodes(rc, n->body, n->body_len, &sf, &cb, depth);
                            char *saved = arena_alloc(rc->card_arena, cb.n);
                            fast_memcpy(saved, cb.p, cb.n);
                            post->card_cache[slot] = (Str){ saved, cb.n };
                            post->card_cached[slot] = 1;
                            out_raw(saved, cb.n);
                            if (cb.cap > sizeof(ctmp)) sys_munmap(cb.p, cb.cap);
                            continue;
                        }
                        ScopeFrame sf;
                        sf.var_name = n->text;
                        sf.var_id = n->var_id;
                        sf.var_val.kind = VAL_CARD;
                        sf.var_val.u.ptr = post;
                        sf.loop = (LoopRec){ k + 1, alen, k == 0, k + 1 == alen };
                        sf.parent = scope;
                        render_nodes(rc, n->body, n->body_len, &sf, dst, depth);
                    }
                } else if (arr.kind == VAL_ARR_TAGS_ALL) {
                    const TagData *ts = (const TagData *)arr.u.arr.ptr;
                    uint32_t alen = arr.u.arr.len;
                    for (uint32_t k = 0; k < alen; k++) {
                        ScopeFrame sf;
                        sf.var_name = n->text;
                        sf.var_id = n->var_id;
                        sf.var_val.kind = VAL_TAG_ALL;
                        sf.var_val.u.ptr = &ts[k];
                        sf.loop = (LoopRec){ k + 1, alen, k == 0, k + 1 == alen };
                        sf.parent = scope;
                        render_nodes(rc, n->body, n->body_len, &sf, dst, depth);
                    }
                } else if (arr.kind == VAL_ARR_TAG_REFS) {
                    const TagRef *ts = (const TagRef *)arr.u.arr.ptr;
                    uint32_t alen = arr.u.arr.len;
                    for (uint32_t k = 0; k < alen; k++) {
                        ScopeFrame sf;
                        sf.var_name = n->text;
                        sf.var_id = n->var_id;
                        sf.var_val.kind = VAL_TAG_REF;
                        sf.var_val.u.ptr = &ts[k];
                        sf.loop = (LoopRec){ k + 1, alen, k == 0, k + 1 == alen };
                        sf.parent = scope;
                        render_nodes(rc, n->body, n->body_len, &sf, dst, depth);
                    }
                } else if (arr.kind == VAL_ARR_TOC) {
                    const TocItem *hs = (const TocItem *)arr.u.arr.ptr;
                    uint32_t alen = arr.u.arr.len;
                    for (uint32_t k = 0; k < alen; k++) {
                        ScopeFrame sf;
                        sf.var_name = n->text;
                        sf.var_id = n->var_id;
                        sf.var_val.kind = VAL_TOC_ITEM;
                        sf.var_val.u.ptr = &hs[k];
                        sf.loop = (LoopRec){ k + 1, alen, k == 0, k + 1 == alen };
                        sf.parent = scope;
                        render_nodes(rc, n->body, n->body_len, &sf, dst, depth);
                    }
                }
                break;
            }
            case NK_INCLUDE: {
                if (depth < INCLUDE_DEPTH) {
                    Template *t = find_template(rc, n->text);
                    if (t) render_nodes(rc, t->nodes, t->len, scope, dst, depth + 1);
                }
                break;
            }
        }
    }
}

// ---------------- Layout-Specialized AST Optimizer ----------------
// Step 1: Inline all {% include %} up to INCLUDE_DEPTH
static void inline_ast(const RenderCtx *rc, const TNode *in, uint32_t len, int depth, NodeVec *out, Arena *a) {
    for (uint32_t i = 0; i < len; i++) {
        TNode n = in[i];
        if (n.kind == NK_INCLUDE) {
            if (depth < INCLUDE_DEPTH) {
                Template *t = find_template(rc, n.text);
                if (t) inline_ast(rc, t->nodes, t->len, depth + 1, out, a);
            }
            continue;
        }
        if (n.kind == NK_IF) {
            NodeVec tb = {0, 0, 0}, eb = {0, 0, 0};
            inline_ast(rc, n.body, n.body_len, depth, &tb, a);
            if (n.else_body) inline_ast(rc, n.else_body, n.else_len, depth, &eb, a);
            n.body = tb.arr; n.body_len = tb.len;
            n.else_body = n.else_body ? eb.arr : NULL;
            n.else_len = n.else_body ? eb.len : 0;
            nv_push(out, n, a);
            continue;
        }
        if (n.kind == NK_FOR) {
            NodeVec fb = {0, 0, 0};
            inline_ast(rc, n.body, n.body_len, depth, &fb, a);
            n.body = fb.arr; n.body_len = fb.len;
            nv_push(out, n, a);
            continue;
        }
        nv_push(out, n, a);
    }
}

// Dependency bitmask for static site-baking & card-cache analysis:
// bit 0: depends on dynamic layout vars (page, tag, posts, pager, or outer dynamic loop var)
// bit 1: depends on `loop`
// bit 2: depends on specific single card var (used for card cache check!)
typedef struct {
    Str bound[32];
    uint8_t bound_id[32];
    uint8_t bound_kind[32]; // 0 = site-const loop var, 1 = dynamic loop var, 2 = card var
    uint32_t depth;
} DepScope;

static uint32_t dep_of_path(const TPath *p, const DepScope *ds) {
    uint8_t id0 = p->ids[0];
    Str s0 = p->segs[0];
    if (id0 == ID_loop) {
        if (ds->depth == 0) return 2u; // unbound loop
        uint8_t bk = ds->bound_kind[ds->depth - 1];
        if (bk == 0) return 0u; // inside site-const loop
        if (bk == 2 && ds->depth > 1) return 4u; // inner loop over card property (e.g. p.tags)!
        return 2u;
    }
    for (int i = (int)ds->depth - 1; i >= 0; i--) {
        if (id0 == ds->bound_id[i] && (id0 != ID_OTHER || (s0.n == ds->bound[i].n && fast_memeq(s0.p, ds->bound[i].p, s0.n)))) {
            uint8_t bk = ds->bound_kind[i];
            if (bk == 0) return 0u;
            if (bk == 2) return 4u;
            return 1u;
        }
    }
    if (id0 == ID_site || id0 == ID_tags || id0 == ID_recent) return 0u;
    return 1u;
}

static uint32_t dep_of_nodes(const TNode *nodes, uint32_t len, DepScope *ds) {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < len; i++) {
        const TNode *n = &nodes[i];
        switch (n->kind) {
            case NK_TEXT: break;
            case NK_VAR:
            case NK_RAW:
                mask |= dep_of_path(&n->path, ds);
                break;
            case NK_IF:
                mask |= dep_of_path(&n->path, ds);
                mask |= dep_of_nodes(n->body, n->body_len, ds);
                if (n->else_body) mask |= dep_of_nodes(n->else_body, n->else_len, ds);
                break;
            case NK_FOR: {
                uint32_t adep = dep_of_path(&n->path, ds);
                mask |= adep;
                if (ds->depth < 32) {
                    ds->bound[ds->depth] = n->text;
                    ds->bound_id[ds->depth] = n->var_id;
                    ds->bound_kind[ds->depth] = (adep == 0) ? 0 : (((adep & 3u) == 0) ? 2 : 1);
                    ds->depth++;
                    mask |= dep_of_nodes(n->body, n->body_len, ds);
                    ds->depth--;
                } else {
                    mask |= 1u;
                }
                break;
            }
            case NK_INCLUDE:
                mask |= 1u;
                break;
        }
    }
    return mask;
}

// Check if two AST node arrays are structurally identical (for sharing card_cache[0] between index & tag!)
static int ast_equal(const TNode *a, uint32_t alen, const TNode *b, uint32_t blen) {
    if (alen != blen) return 0;
    for (uint32_t i = 0; i < alen; i++) {
        if (a[i].kind != b[i].kind || a[i].is_not != b[i].is_not || a[i].var_id != b[i].var_id) return 0;
        if (a[i].text.n != b[i].text.n || !fast_memeq(a[i].text.p, b[i].text.p, a[i].text.n)) return 0;
        if (a[i].path.len != b[i].path.len) return 0;
        for (uint8_t k = 0; k < a[i].path.len; k++) {
            if (a[i].path.ids[k] != b[i].path.ids[k] || a[i].path.segs[k].n != b[i].path.segs[k].n ||
                !fast_memeq(a[i].path.segs[k].p, b[i].path.segs[k].p, a[i].path.segs[k].n)) return 0;
        }
        if (!ast_equal(a[i].body, a[i].body_len, b[i].body, b[i].body_len)) return 0;
        if ((a[i].else_body != NULL) != (b[i].else_body != NULL)) return 0;
        if (a[i].else_body && !ast_equal(a[i].else_body, a[i].else_len, b[i].else_body, b[i].else_len)) return 0;
    }
    return 1;
}

// Step 2: Layout-specialized dead-branch elimination + site-constant folding + adjacent literal coalescing!
// layout_kind: 1=page/post (page truthy, tag/pager/posts absent), 2=index (pager truthy, page/tag absent),
//              3=tag (tag truthy, page/pager/posts absent), 4=tags (page/tag/pager/posts absent)
static void bake_nodes(const RenderCtx *rc, const TNode *in, uint32_t len, uint8_t layout_kind, DepScope *ds, NodeVec *out, Arena *a, Buf *lit_acc) {
    #define FLUSH_LIT() do { \
        if (lit_acc->n > 0) { \
            char *cp = arena_alloc(a, lit_acc->n); \
            fast_memcpy(cp, lit_acc->p, lit_acc->n); \
            TNode tn; __builtin_memset(&tn, 0, sizeof(tn)); \
            tn.kind = NK_TEXT; tn.card_slot = -1; tn.text = (Str){ cp, lit_acc->n }; \
            nv_push(out, tn, a); \
            lit_acc->n = 0; \
        } \
    } while (0)

    for (uint32_t i = 0; i < len; i++) {
        const TNode *n = &in[i];
        // Check if this single node is completely site-constant (only reads site, tags, recent)
        if (ds->depth == 0) {
            // Layout-known root truthiness for `{% if page %}` / `{% if tag %}` / `{% if pager %}`
            if (n->kind == NK_IF && n->path.len == 1) {
                uint8_t id0 = n->path.ids[0];
                int known = -1;
                if (id0 == ID_page)  known = (layout_kind == 1) ? 1 : 0;
                else if (id0 == ID_tag)   known = (layout_kind == 3) ? 1 : 0;
                else if (id0 == ID_pager) known = (layout_kind == 2) ? 1 : 0;
                else if (id0 == ID_posts && layout_kind != 2) known = 0;
                if (known != -1) {
                    if (known != (int)n->is_not) {
                        bake_nodes(rc, n->body, n->body_len, layout_kind, ds, out, a, lit_acc);
                    } else if (n->else_body) {
                        bake_nodes(rc, n->else_body, n->else_len, layout_kind, ds, out, a, lit_acc);
                    }
                    continue;
                }
            }
            if (dep_of_nodes(n, 1, ds) == 0) {
                render_nodes(rc, n, 1, NULL, lit_acc, 0);
                continue;
            }
        } else if (n->kind == NK_TEXT) {
            buf_raw(lit_acc, n->text.p, n->text.n);
            continue;
        } else if (dep_of_nodes(n, 1, ds) == 0) {
            render_nodes(rc, n, 1, NULL, lit_acc, 0);
            continue;
        }

        FLUSH_LIT();
        if (n->kind == NK_IF) {
            NodeVec tb = {0, 0, 0}, eb = {0, 0, 0};
            char l1[4096]; Buf b1 = { l1, 0, sizeof(l1) };
            bake_nodes(rc, n->body, n->body_len, layout_kind, ds, &tb, a, &b1);
            if (b1.n > 0) {
                char *cp = arena_alloc(a, b1.n); fast_memcpy(cp, b1.p, b1.n);
                TNode tn; __builtin_memset(&tn, 0, sizeof(tn));
                tn.kind = NK_TEXT; tn.card_slot = -1; tn.text = (Str){ cp, b1.n };
                nv_push(&tb, tn, a);
            }
            if (b1.cap > sizeof(l1)) sys_munmap(b1.p, b1.cap);

            if (n->else_body) {
                char l2[4096]; Buf b2 = { l2, 0, sizeof(l2) };
                bake_nodes(rc, n->else_body, n->else_len, layout_kind, ds, &eb, a, &b2);
                if (b2.n > 0) {
                    char *cp = arena_alloc(a, b2.n); fast_memcpy(cp, b2.p, b2.n);
                    TNode tn; __builtin_memset(&tn, 0, sizeof(tn));
                    tn.kind = NK_TEXT; tn.card_slot = -1; tn.text = (Str){ cp, b2.n };
                    nv_push(&eb, tn, a);
                }
                if (b2.cap > sizeof(l2)) sys_munmap(b2.p, b2.cap);
            }
            TNode copy = *n;
            copy.body = tb.arr; copy.body_len = tb.len;
            copy.else_body = n->else_body ? eb.arr : NULL;
            copy.else_len = n->else_body ? eb.len : 0;
            nv_push(out, copy, a);
        } else if (n->kind == NK_FOR) {
            NodeVec fb = {0, 0, 0};
            char l1[4096]; Buf b1 = { l1, 0, sizeof(l1) };
            uint8_t prev_d = (uint8_t)ds->depth;
            if (ds->depth < 32) {
                ds->bound[ds->depth] = n->text;
                ds->bound_id[ds->depth] = n->var_id;
                ds->bound_kind[ds->depth] = 1;
                ds->depth++;
            }
            bake_nodes(rc, n->body, n->body_len, layout_kind, ds, &fb, a, &b1);
            if (b1.n > 0) {
                char *cp = arena_alloc(a, b1.n); fast_memcpy(cp, b1.p, b1.n);
                TNode tn; __builtin_memset(&tn, 0, sizeof(tn));
                tn.kind = NK_TEXT; tn.card_slot = -1; tn.text = (Str){ cp, b1.n };
                nv_push(&fb, tn, a);
            }
            if (b1.cap > sizeof(l1)) sys_munmap(b1.p, b1.cap);
            ds->depth = prev_d;

            TNode copy = *n;
            copy.body = fb.arr; copy.body_len = fb.len;
            copy.card_slot = -1;
            // Check if this is a top-level `for p in posts` or `for p in tag.posts` eligible for Card Cache!
            if (ds->depth == 0) {
                int is_posts_loop = (n->path.len == 1 && n->path.ids[0] == ID_posts) ||
                                    (n->path.len == 2 && n->path.ids[0] == ID_tag && n->path.ids[1] == ID_posts);
                if (is_posts_loop) {
                    DepScope cds;
                    cds.depth = 1;
                    cds.bound[0] = n->text;
                    cds.bound_id[0] = n->var_id;
                    cds.bound_kind[0] = 2; // card var only!
                    uint32_t cdep = dep_of_nodes(copy.body, copy.body_len, &cds);
                    // Only cache if body does NOT depend on loop (bit 1) or other dynamic vars (bit 0)!
                    if ((cdep & 3u) == 0) {
                        copy.card_slot = (layout_kind == 2) ? 0 : 1;
                    }
                }
            }
            nv_push(out, copy, a);
        } else {
            nv_push(out, *n, a);
        }
    }
    #undef FLUSH_LIT
}

static void compile_layout(const RenderCtx *rc, Template *t, uint8_t layout_kind, Arena *a) {
    if (!t || t->baked_ready[layout_kind]) return;
    NodeVec inlined = {0, 0, 0};
    inline_ast(rc, t->nodes, t->len, 0, &inlined, a);
    NodeVec baked = {0, 0, 0};
    DepScope ds; ds.depth = 0;
    char ltmp[16384]; Buf lb = { ltmp, 0, sizeof(ltmp) };
    bake_nodes(rc, inlined.arr, inlined.len, layout_kind, &ds, &baked, a, &lb);
    if (lb.n > 0) {
        char *cp = arena_alloc(a, lb.n); fast_memcpy(cp, lb.p, lb.n);
        TNode tn; __builtin_memset(&tn, 0, sizeof(tn));
        tn.kind = NK_TEXT; tn.card_slot = -1; tn.text = (Str){ cp, lb.n };
        nv_push(&baked, tn, a);
    }
    if (lb.cap > sizeof(ltmp)) sys_munmap(lb.p, lb.cap);
    t->baked_nodes[layout_kind] = baked.arr;
    t->baked_len[layout_kind] = baked.len;
    t->baked_ready[layout_kind] = 1;
}

// After compiling both `index` and `tag`, unify card_slot = 0 if their card loop bodies match!
static void unify_card_cache_slots(Template *t_index, Template *t_tag) {
    if (!t_index || !t_tag || !t_index->baked_ready[2] || !t_tag->baked_ready[3]) return;
    const TNode *idx_loop = NULL;
    for (uint32_t i = 0; i < t_index->baked_len[2]; i++) {
        if (t_index->baked_nodes[2][i].kind == NK_FOR && t_index->baked_nodes[2][i].card_slot == 0) {
            idx_loop = &t_index->baked_nodes[2][i];
            break;
        }
    }
    if (!idx_loop) return;
    for (uint32_t i = 0; i < t_tag->baked_len[3]; i++) {
        if (t_tag->baked_nodes[3][i].kind == NK_FOR && t_tag->baked_nodes[3][i].card_slot == 1) {
            TNode *tl = &t_tag->baked_nodes[3][i];
            if (tl->var_id == idx_loop->var_id &&
                tl->text.n == idx_loop->text.n &&
                fast_memeq(tl->text.p, idx_loop->text.p, tl->text.n) &&
                ast_equal(tl->body, tl->body_len, idx_loop->body, idx_loop->body_len)) {
                tl->card_slot = 0; // SHARE SLOT 0 WITH INDEX!!
            }
        }
    }
}
