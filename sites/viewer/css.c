// css.c — the part of CSS the viewer uses: which elements are hidden, and the properties
// that place boxes (display: flex/grid/block/inline, float, clear, widths, flex and grid
// settings, gaps), and the box's own spacing and colour (margin, padding, background).
// Included by viewer.c (not compiled on its own).
//
// Modern pages hide menus, dialogs and dropdowns with stylesheets; without this the reader
// view would show all of them. We parse <style> and <link rel=stylesheet>, evaluate @media
// against the view width, match selectors, and run the cascade (!important, specificity,
// order) for `display` and `visibility`, plus the usual "visually hidden" tricks (clip to
// nothing, pushed far off-screen). Selectors that depend on state we don't have
// (:hover, :focus, :target, ::before...) never match, which keeps collapsed things
// collapsed.

typedef struct Compound Compound;
struct Compound {
    int tag;                 // -1 = any
    const char *tag_name;    // for unknown tags
    const char *id;
    const char **classes;
    int nclasses;
    const char **attr_names, **attr_values;  // value NULL = [name]; ops in attr_ops
    u8 *attr_ops;            // '=', '~', '^', '$', '*', '|', 0 = exists
    int nattrs;
    Compound *nots;          // :not(a, b): the element must match none of these
    int nnots;
    Compound *ises;          // :is(a, b) / :where(a, b): must match one of these
    int nises;
    u8 first_child, last_child, checked, link, impossible;
    u8 comb;                 // combinator to the next compound on the LEFT: ' ', '>', '+', '~', 0 = none
};

// Layout properties. Each has a value (lval: x100 for lengths and numbers) and a unit.
enum { LP_FLOAT, LP_CLEAR, LP_WIDTH, LP_MAXW, LP_DIR, LP_WRAP, LP_JUSTIFY, LP_ALIGN, LP_RGAP, LP_CGAP,
       LP_GROW, LP_SHRINK, LP_BASIS, LP_COLS, LP_SPAN, LP_MINW, LP_OVER,  // LP_OVER: overflow-x, 1 visible, 2 clipped
       LP_ORDER, LP_ASELF,                                                 // flex items: order, align-self (as LP_ALIGN)
       LP_MT, LP_MR, LP_MB, LP_ML, LP_PT, LP_PR, LP_PB, LP_PL,             // margin, padding (U_AUTO: margin auto)
       LP_BG,                                                              // background colour (0xAARRGGBB; 0 = none)
       LP_COUNT };
enum { U_PX = 1, U_EM, U_REM, U_PCT, U_AUTO, U_NUM, U_STR, U_VW };  // U_STR: lval is a string (grid-template-columns)
enum { LD_BLOCK = 1, LD_INLINE, LD_INLINE_BLOCK, LD_FLEX, LD_GRID, LD_OTHER };  // display, besides none
// Properties that matter only for a float, a flex or grid container, or an item in one: looked
// up when one asks (lay_items), not for every element (matching all the rules that set
// widths, gaps and the like for every element costs ~15% of the layout of a big page).
// Every element needs only display, float and clear. The box's own spacing and colour
// (LP_BOX_MASK) are a third set, looked up for the blocks layout reaches (box_css).
#define LP_BOX_MASK ((((1u << (LP_BG + 1)) - 1) & ~((1u << LP_MT) - 1)))
#define LP_ITEM_MASK (((1u << LP_COUNT) - 1) & ~((1u << LP_FLOAT) | (1u << LP_CLEAR)) & ~LP_BOX_MASK)
#define LP_BOXPASS_MASK (LP_BOX_MASK | (1u << LP_WIDTH) | (1u << LP_MAXW))  // (a block's width too)

// What a rule's declarations say. 0 = not set.
typedef struct {
    u8 disp;       // 1 none, 2 anything else
    u8 vis;        // 1 hidden/collapse, 2 visible
    u8 sr;         // 1 = visually hidden (clip, off-screen)
    u8 imp_disp, imp_vis;
    u8 ldisp;      // with disp (the same property): LD_*
    u8 lhas[LP_COUNT];  // 1 set, 2 set !important
    u8 lunit[LP_COUNT];
    int lval[LP_COUNT];
} Decl;

// An element's computed layout properties (only made for elements that have some).
typedef struct Lay {
    u8 disp;            // LD_*, 0 = what its tag says
    u8 items_done;      // the LP_ITEM_MASK properties are in (lay_items)
    u8 has[LP_COUNT];
    u8 unit[LP_COUNT];
    int val[LP_COUNT];
} Lay;

typedef struct {
    Compound *parts;  // right to left
    int nparts;
    int spec;         // specificity a*10000 + b*100 + c
    int order;
    u32 lmask;        // the layout properties it sets (1 << LP_*)
    Decl d;
    u8 has_need;
    u64 need[4];      // ancestor Bloom bits this rule's ancestor compounds require (see anc_bloom)
} Rule;

static Rule *rules;
static int nrules, rules_cap;

// ---- tiny parsing helpers -----------------------------------------------------------------

typedef struct { const char *p, *end; } Cur;

static void skip_ws(Cur *c) { while (c->p < c->end && is_space((u8)*c->p)) c->p++; }

static int ident_char(u8 ch) { return is_alnum(ch) || ch == '-' || ch == '_' || ch >= 0x80 || ch == '\\'; }

static const char *read_ident(Cur *c, int lower_case) {
    const char *s = c->p;
    char buf[128];
    int n = 0;
    while (c->p < c->end && ident_char((u8)*c->p)) {
        char ch = *c->p++;
        if (ch == '\\' && c->p < c->end) ch = *c->p++;  // escaped character: taken literally
        if (n < 127) buf[n++] = lower_case ? lower(ch) : ch;
    }
    if (c->p == s) return 0;
    return arena_str(buf, n);
}

// Skips a balanced (...) or [...] or string starting at *c->p.
static void skip_balanced(Cur *c) {
    char open = *c->p, close = open == '(' ? ')' : open == '[' ? ']' : open;
    int depth = 0;
    while (c->p < c->end) {
        char ch = *c->p++;
        if (ch == '"' || ch == '\'') {
            while (c->p < c->end && *c->p != ch) { if (*c->p == '\\') c->p++; c->p++; }
            if (c->p < c->end) c->p++;
            continue;
        }
        if (ch == open) depth++;
        else if (ch == close && --depth == 0) return;
    }
}

static int parse_compound(Cur *c, Compound *out);

// Parses "a, b, c" (compounds only) inside :not()/:is(); returns the count.
static int parse_compound_list(const char *s, const char *e, Compound **out) {
    Compound tmp[16];
    int n = 0;
    Cur c = {s, e};
    while (c.p < c.end && n < 16) {
        skip_ws(&c);
        __builtin_memset(&tmp[n], 0, sizeof(Compound));
        if (!parse_compound(&c, &tmp[n])) tmp[n].impossible = 1;
        skip_ws(&c);
        if (c.p < c.end && *c.p != ',') tmp[n].impossible = 1;  // combinators inside :not() etc.: unsupported
        while (c.p < c.end && *c.p != ',') {
            if (*c.p == '(' || *c.p == '[') skip_balanced(&c);
            else c.p++;
        }
        if (c.p < c.end) c.p++;
        n++;
    }
    *out = (Compound *)arena((u32)n * sizeof(Compound));
    __builtin_memcpy(*out, tmp, (u32)n * sizeof(Compound));
    return n;
}

static int spec_of(const Compound *k) {
    int s = 0;
    if (k->id) s += 10000;
    s += 100 * (k->nclasses + k->nattrs + k->first_child + k->last_child + k->checked + k->link);
    if (k->tag >= 0) s += 1;
    for (int i = 0; i < k->nnots; i++) s += spec_of(&k->nots[i]);
    int best = 0;
    for (int i = 0; i < k->nises; i++) best = MAX(best, spec_of(&k->ises[i]));
    return s + best;
}

// One compound selector ("div.a#b[x]:not(.c)"). Returns 0 if nothing was read.
static int parse_compound(Cur *c, Compound *k) {
    k->tag = -1;
    const char *start = c->p;
    const char *cls[16], *an[8], *av[8];
    u8 ao[8];
    int ncls = 0, na = 0;
    if (c->p < c->end && *c->p == '*') c->p++;
    else if (c->p < c->end && ident_char((u8)*c->p)) {
        const char *name = read_ident(c, 1);
        k->tag = tag_lookup(name, lw_strlen(name));
        if (k->tag == T_UNKNOWN) k->tag_name = name;
        if (k->tag == T_IMAGE) k->tag = T_IMG;
    }
    while (c->p < c->end) {
        char ch = *c->p;
        if (ch == '#') {
            c->p++;
            k->id = read_ident(c, 0);
            if (!k->id) k->impossible = 1;
        } else if (ch == '.') {
            c->p++;
            const char *cn = read_ident(c, 0);
            if (!cn) k->impossible = 1;
            else if (ncls < 16) cls[ncls++] = cn;
        } else if (ch == '[') {
            const char *s = c->p + 1;
            skip_balanced(c);
            const char *e = c->p - 1;  // at ']'
            Cur a = {s, e};
            skip_ws(&a);
            const char *name = read_ident(&a, 1);
            skip_ws(&a);
            u8 op = 0;
            const char *val = 0;
            if (a.p < a.end && *a.p != '=' && a.p + 1 < a.end && a.p[1] == '=') op = (u8)*a.p++;
            if (a.p < a.end && *a.p == '=') {
                if (!op) op = '=';
                a.p++;
                skip_ws(&a);
                if (a.p < a.end && (*a.p == '"' || *a.p == '\'')) {
                    char q = *a.p++;
                    const char *vs = a.p;
                    while (a.p < a.end && *a.p != q) a.p++;
                    val = arena_str(vs, (int)(a.p - vs));
                } else {
                    val = read_ident(&a, 0);
                }
            }
            if (!name) k->impossible = 1;
            else if (na < 8) { an[na] = name; av[na] = val; ao[na] = val ? op : 0; na++; }
        } else if (ch == ':') {
            c->p++;
            int element = c->p < c->end && *c->p == ':';
            if (element) c->p++;
            const char *pc = read_ident(c, 1);
            const char *args = 0, *args_end = 0;
            if (c->p < c->end && *c->p == '(') {
                args = c->p + 1;
                skip_balanced(c);
                args_end = c->p - 1;
            }
            if (element || !pc) { k->impossible = 1; continue; }
            if (ieq(pc, "not") && args) k->nnots = parse_compound_list(args, args_end, &k->nots);
            else if ((ieq(pc, "is") || ieq(pc, "where") || ieq(pc, "matches")) && args) {
                k->nises = parse_compound_list(args, args_end, &k->ises);
                if (!k->nises) k->impossible = 1;  // :is() with nothing in it matches nothing (not everything)
            }
            else if (ieq(pc, "first-child")) k->first_child = 1;
            else if (ieq(pc, "last-child")) k->last_child = 1;
            else if (ieq(pc, "only-child")) k->first_child = k->last_child = 1;
            else if (ieq(pc, "checked")) k->checked = 1;
            else if (ieq(pc, "link") || ieq(pc, "any-link")) k->link = 1;
            else if (ieq(pc, "before") || ieq(pc, "after") || ieq(pc, "first-letter") || ieq(pc, "first-line")) k->impossible = 1;
            else k->impossible = 1;  // :hover, :focus, :target, :nth-child()...
        } else {
            break;
        }
    }
    if (ncls) {
        k->classes = (const char **)arena((u32)ncls * sizeof(char *));
        __builtin_memcpy(k->classes, cls, (u32)ncls * sizeof(char *));
        k->nclasses = ncls;
    }
    if (na) {
        k->attr_names = (const char **)arena((u32)na * sizeof(char *));
        k->attr_values = (const char **)arena((u32)na * sizeof(char *));
        k->attr_ops = (u8 *)arena((u32)na);
        __builtin_memcpy(k->attr_names, an, (u32)na * sizeof(char *));
        __builtin_memcpy(k->attr_values, av, (u32)na * sizeof(char *));
        __builtin_memcpy(k->attr_ops, ao, (u32)na);
        k->nattrs = na;
    }
    return c->p > start;
}

// ---- ancestor Bloom filter ----------------------------------------------------------------
// Each element gets a 256-bit set of hashes of its ancestors' tags, ids and classes. A rule
// like ".navbox .hlist a" can only match inside a .navbox and a .hlist, which the filter
// rules out for most elements without walking up the tree. (Browsers do the same.)

static u32 bloom_bit(const char *s, int kind, int tag) {  // kind 0 tag, 1 id (any case), 2 class
    u32 h = 2166136261u ^ (u32)(kind * 131 + tag);
    if (s) while (*s) h = (h ^ (u8)(kind == 1 ? lower(*s) : *s)) * 16777619u, s++;
    h ^= h >> 13;
    return (h * 2654435761u) >> 24;
}

static void bloom_set(u64 *f, u32 bit) { f[bit >> 6] |= (u64)1 << (bit & 63); }

static void bloom_add_element(u64 *f, Node *n) {
    bloom_set(f, bloom_bit(0, 0, n->tag));
    const char *id = attr(n, "id");
    if (id && *id) bloom_set(f, bloom_bit(id, 1, 0));
    const char *c = attr(n, "class");
    if (!c) return;
    char one[128];
    while (*c) {
        while (*c && is_space((u8)*c)) c++;
        int k = 0;
        while (*c && !is_space((u8)*c)) { if (k < 127) one[k++] = *c; c++; }
        one[k] = 0;
        if (k) bloom_set(f, bloom_bit(one, 2, 0));
    }
}

static const u64 bloom_none[4];
static int any_need;  // some rule has ancestor compounds (else the filters aren't worth making)

static const u64 *anc_bloom(Node *n) {
    if (n->anc) return n->anc;
    Node *p = n->parent;
    if (!p || p->type != N_ELEM) return bloom_none;
    u64 *f = (u64 *)arena(4 * sizeof(u64));
    const u64 *pf = anc_bloom(p);
    for (int i = 0; i < 4; i++) f[i] = pf[i];
    bloom_add_element(f, p);
    n->anc = f;
    return f;
}

// A full selector ("nav > ul li.x"); adds a Rule. Returns 0 if unparseable.
static void add_selector(const char *s, const char *e, const Decl *d, int order) {
    Compound parts[16];
    u8 combs[16];
    int n = 0;
    Cur c = {s, e};
    skip_ws(&c);
    u8 comb = 0;
    while (c.p < c.end) {
        if (n == 16) return;
        __builtin_memset(&parts[n], 0, sizeof(Compound));
        if (!parse_compound(&c, &parts[n])) return;
        combs[n] = comb;
        n++;
        int ws = 0;
        while (c.p < c.end && is_space((u8)*c.p)) { c.p++; ws = 1; }
        if (c.p >= c.end) break;
        if (*c.p == '>' || *c.p == '+' || *c.p == '~') {
            comb = (u8)*c.p++;
            skip_ws(&c);
        } else if (ws) {
            comb = ' ';
        } else {
            return;  // something we don't understand
        }
    }
    if (!n) return;
    rules = (Rule *)grow_array(rules, nrules, &rules_cap, sizeof(Rule));
    Rule *r = &rules[nrules++];
    r->nparts = n;
    r->parts = (Compound *)arena((u32)n * sizeof(Compound));
    int spec = 0;
    for (int i = 0; i < n; i++) {  // store right to left; comb = relation to the part on the left
        r->parts[i] = parts[n - 1 - i];
        r->parts[i].comb = i + 1 < n ? combs[n - 1 - i] : 0;
        spec += spec_of(&parts[i]);
    }
    r->spec = spec;
    r->order = order;
    r->d = *d;
    r->lmask = 0;
    for (int i = 0; i < LP_COUNT; i++)
        if (d->lhas[i]) r->lmask |= 1u << i;
    __builtin_memset(r->need, 0, sizeof r->need);
    r->has_need = 0;
    int ancestor = 0;
    for (int i = 1; i < n; i++) {  // parts past a ' ' or '>' are ancestors of the subject
        u8 cb = r->parts[i - 1].comb;
        if (cb == ' ' || cb == '>') ancestor = 1;
        if (!ancestor) continue;
        const Compound *k = &r->parts[i];
        if (k->tag >= 0 && k->tag != T_UNKNOWN) bloom_set(r->need, bloom_bit(0, 0, k->tag));
        if (k->id) bloom_set(r->need, bloom_bit(k->id, 1, 0));
        for (int j = 0; j < k->nclasses; j++) bloom_set(r->need, bloom_bit(k->classes[j], 2, 0));
        r->has_need = 1;
        any_need = 1;
    }
}

// ---- @media ------------------------------------------------------------------------------

// Media queries in the range syntax: (width >= 600px), (width<=calc(48rem - .02px)),
// (400px <= width < 900px).

// A length in CSS px: 600px, 37.5em, 48rem (16 px), a plain number, calc(a - b); -1 if none.
static float media_len(Cur *c) {
    skip_ws(c);
    if (c->end - c->p >= 5 && iprefix(c->p, 5, "calc(")) {
        c->p += 5;
        float a = media_len(c);
        skip_ws(c);
        char op = c->p < c->end ? *c->p : 0;
        if (op == '-' || op == '+') {
            c->p++;
            float b = media_len(c);
            if (a >= 0 && b >= 0) a = op == '-' ? a - b : a + b;
        }
        while (c->p < c->end && *c->p != ')') c->p++;
        if (c->p < c->end) c->p++;
        return a;
    }
    float v = 0, scale = 0;
    int any = 0;
    while (c->p < c->end && (is_digit((u8)*c->p) || *c->p == '.')) {
        if (*c->p == '.') scale = 1;
        else {
            any = 1;
            if (scale) { scale /= 10; v += (float)(*c->p - '0') * scale; }
            else v = v * 10 + (float)(*c->p - '0');
        }
        c->p++;
    }
    if (!any) return -1;
    if (c->end - c->p >= 2 && iprefix(c->p, 2, "px")) c->p += 2;
    else if (c->end - c->p >= 3 && iprefix(c->p, 3, "rem")) { c->p += 3; v *= 16; }
    else if (c->end - c->p >= 2 && iprefix(c->p, 2, "em")) { c->p += 2; v *= 16; }
    return v;
}

static int media_op(Cur *c) {  // 1 <, 2 <=, 3 >, 4 >=, 5 =; 0 none
    skip_ws(c);
    if (c->p >= c->end) return 0;
    char ch = *c->p;
    if (ch == '=') { c->p++; return 5; }
    if (ch != '<' && ch != '>') return 0;
    c->p++;
    int eq = c->p < c->end && *c->p == '=';
    if (eq) c->p++;
    return (ch == '<' ? 1 : 3) + eq;
}

static int media_cmp(float a, int op, float b) {
    return op == 1 ? a < b : op == 2 ? a <= b : op == 3 ? a > b : op == 4 ? a >= b : a == b;
}

// 1 / 0 for a range feature (inside its parentheses), -1 if it is not one.
static int media_range(Cur f, float vw, float vh) {
    skip_ws(&f);
    if (f.p >= f.end) return -1;
    if (is_digit((u8)*f.p) || *f.p == '.' || iprefix(f.p, (int)(f.end - f.p), "calc(")) {  // 400px <= width [< 900px]
        float a = media_len(&f);
        int op = media_op(&f);
        skip_ws(&f);
        const char *name = read_ident(&f, 1);
        if (a < 0 || !op || !name) return 0;
        float v = ieq(name, "width") ? vw : ieq(name, "height") ? vh : -1;
        if (v < 0 || !media_cmp(a, op, v)) return 0;
        int op2 = media_op(&f);
        if (!op2) return 1;
        float b = media_len(&f);
        return b >= 0 && media_cmp(v, op2, b);
    }
    const char *name = read_ident(&f, 1);
    int op = media_op(&f);
    if (!name || !op) return -1;
    float v = ieq(name, "width") ? vw : ieq(name, "height") ? vh : -1, b = media_len(&f);
    return v >= 0 && b >= 0 && media_cmp(v, op, b);
}

static int media_matches(const char *s, const char *e) {
    // comma = OR; each part: [not|only] [type] [and (feature)]*
    Cur c = {s, e};
    if (c.p >= c.end) return 1;
    while (c.p < c.end) {
        const char *ps = c.p;
        int depth = 0;
        while (c.p < c.end && !(*c.p == ',' && depth == 0)) {
            if (*c.p == '(') depth++;
            if (*c.p == ')') depth--;
            c.p++;
        }
        Cur q = {ps, c.p};
        if (c.p < c.end) c.p++;
        int ok = 1, negate = 0;
        skip_ws(&q);
        while (q.p < q.end) {
            skip_ws(&q);
            if (q.p >= q.end) break;
            if (*q.p == '(') {
                const char *fs = q.p + 1;
                skip_balanced(&q);
                Cur f = {fs, q.p - 1};
                int range = media_range(f, W / S, H / S);
                if (range >= 0) {
                    ok &= range;
                    continue;
                }
                skip_ws(&f);
                const char *name = read_ident(&f, 1);
                skip_ws(&f);
                const char *val = 0;
                int num = -1;
                if (f.p < f.end && *f.p == ':') {
                    f.p++;
                    skip_ws(&f);
                    val = f.p;
                    char nb[16];
                    int k = 0;
                    for (const char *q = f.p; q < f.end && k < 15 && (is_digit((u8)*q) || *q == '.'); q++) nb[k++] = *q;
                    nb[k] = 0;
                    num = parse_int(nb, 0);
                }
                int vw = (int)(W / S), vh = (int)(H / S);
                int em = 0;
                if (val) {
                    const char *u = val;
                    while (u < f.end && (is_digit((u8)*u) || *u == '.')) u++;
                    em = u + 1 < f.end && lower(u[0]) == 'e' && lower(u[1]) == 'm';
                }
                if (em && num >= 0) num *= 16;
                int fok = 0;
                if (!name) fok = 0;
                else if (ieq(name, "min-width")) fok = num >= 0 && vw >= num;
                else if (ieq(name, "max-width")) fok = num >= 0 && vw <= num;
                else if (ieq(name, "min-height")) fok = num >= 0 && vh >= num;
                else if (ieq(name, "max-height")) fok = num >= 0 && vh <= num;
                else if (ieq(name, "orientation")) fok = val && (iprefix(val, (int)(f.end - val), "landscape") ? vw >= vh : vw < vh);
                else if (ieq(name, "prefers-color-scheme")) fok = val && iprefix(val, (int)(f.end - val), "light");
                else if (ieq(name, "hover") || ieq(name, "any-hover")) fok = !val || iprefix(val, (int)(f.end - val), "hover");
                else if (ieq(name, "pointer") || ieq(name, "any-pointer")) fok = !val || iprefix(val, (int)(f.end - val), "fine");
                else if (ieq(name, "prefers-reduced-motion")) fok = val && iprefix(val, (int)(f.end - val), "no-preference");
                else if (ieq(name, "min-resolution") || ieq(name, "-webkit-min-device-pixel-ratio")) fok = S >= 1.5f;
                else fok = 0;
                ok &= fok;
            } else {
                const char *w = read_ident(&q, 1);
                if (!w) { q.p++; continue; }
                if (ieq(w, "not")) negate = 1;
                else if (ieq(w, "only") || ieq(w, "and")) {}
                else if (ieq(w, "screen") || ieq(w, "all")) {}
                else ok = 0;  // print, speech, unknown types
            }
        }
        if (ok != negate) return 1;
    }
    return 0;
}

// ---- stylesheets -------------------------------------------------------------------------

// Finds the matching '}' for the '{' at s (strings and nested braces skipped).
static const char *block_end(const char *s, const char *e) {
    int depth = 0;
    for (const char *p = s; p < e; p++) {
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (p < e && *p != q) { if (*p == '\\') p++; p++; }
            continue;
        }
        if (*p == '{') depth++;
        else if (*p == '}' && --depth == 0) return p;
    }
    return e;
}

static int value_is(const char *v, const char *e, const char *word) { return iprefix(v, (int)(e - v), word); }

// A number with its unit ("12px", "1.5em", "50%", "2fr", "0", "auto") at *p; 0 if none.
static int read_len(const char **p, const char *e, int *val, u8 *unit) {
    const char *q = *p;
    while (q < e && is_space((u8)*q)) q++;
    if (q < e && value_is(q, e, "auto")) { *val = 0; *unit = U_AUTO; *p = q + 4; return 1; }
    int neg = 0;
    if (q < e && (*q == '-' || *q == '+')) neg = *q++ == '-';
    long long v = 0;
    int digits = 0, fd = 0;  // the value x100: two digits after the point
    while (q < e && is_digit((u8)*q)) { if (v < 100000000) v = v * 10 + (*q - '0'); digits++; q++; }
    if (q < e && *q == '.') {
        q++;
        while (q < e && is_digit((u8)*q)) { if (fd < 2) { v = v * 10 + (*q - '0'); fd++; } digits++; q++; }
    }
    if (!digits) return 0;
    while (fd < 2) { v *= 10; fd++; }
    if (v > 100000000) v = 100000000;
    *val = (int)(neg ? -v : v);
    u8 u = U_NUM;
    if (value_is(q, e, "px")) { u = U_PX; q += 2; }
    else if (value_is(q, e, "rem")) { u = U_REM; q += 3; }
    else if (value_is(q, e, "em")) { u = U_EM; q += 2; }
    else if (value_is(q, e, "fr")) { u = U_NUM; q += 2; }
    else if (q < e && *q == '%') { u = U_PCT; q++; }
    else if (value_is(q, e, "pt")) { u = U_PX; *val = *val * 4 / 3; q += 2; }
    else if (value_is(q, e, "vw")) { u = U_VW; q += 2; }
    else if (value_is(q, e, "vh") || value_is(q, e, "ch") || value_is(q, e, "ex")) { u = U_AUTO; q += 2; }
    while (q < e && is_alnum((u8)*q)) q++;  // a unit we don't know
    *unit = u;
    *p = q;
    return 1;
}

static void set_lp(Decl *d, int prop, int val, u8 unit, int imp) {
    d->lval[prop] = val;
    d->lunit[prop] = unit;
    d->lhas[prop] = (u8)(imp ? 2 : 1);
}

// margin, padding and their -top/-right/-bottom/-left, -inline, -block, -inline-start/end
// and -block-start/end forms (left to right, top to bottom). Values we can't read (var(),
// calc()) leave the whole declaration out.
static void box_sides(Decl *d, const char *ns, int nl, const char *vs, const char *ve, int imp) {
    int pad = lower(ns[0]) == 'p', base = pad ? LP_PT : LP_MT, k = pad ? 7 : 6;  // after "padding" / "margin"
    const char *sub = ns + k;
    int sl = nl - k;
    int v[4], n = 0, val;
    u8 u[4], unit;
    const char *q = vs;
    while (n < 4 && read_len(&q, ve, &val, &unit)) {
        if (pad && unit == U_AUTO) return;
        v[n] = val; u[n] = unit; n++;
        while (q < ve && is_space((u8)*q)) q++;
    }
    if (!n || q < ve) return;
    // which sides (top, right, bottom, left) get which value
    int side[4] = {-1, -1, -1, -1};
    if (sl == 0) {
        static const u8 map[4][4] = {{0, 0, 0, 0}, {0, 1, 0, 1}, {0, 1, 2, 1}, {0, 1, 2, 3}};
        for (int i = 0; i < 4; i++) side[i] = map[n - 1][i];
    } else if (n == 1 && (value_is(sub, ns + nl, "-top") || value_is(sub, ns + nl, "-block-start"))) side[0] = 0;
    else if (n == 1 && (value_is(sub, ns + nl, "-right") || value_is(sub, ns + nl, "-inline-end"))) side[1] = 0;
    else if (n == 1 && (value_is(sub, ns + nl, "-bottom") || value_is(sub, ns + nl, "-block-end"))) side[2] = 0;
    else if (n == 1 && (value_is(sub, ns + nl, "-left") || value_is(sub, ns + nl, "-inline-start"))) side[3] = 0;
    else if (n <= 2 && sl == 7 && value_is(sub, ns + nl, "-inline")) { side[3] = 0; side[1] = n - 1; }
    else if (n <= 2 && sl == 6 && value_is(sub, ns + nl, "-block")) { side[0] = 0; side[2] = n - 1; }
    else return;
    for (int i = 0; i < 4; i++)
        if (side[i] >= 0) set_lp(d, base + i, v[side[i]], u[side[i]], imp);
}

// The colour in a background (shorthand) value: #hex, rgb(), a name; 0 if it has none.
static int parse_color(const char *s, int len, u32 *out);
static int background_colour(const char *vs, const char *ve, u32 *col) {
    const char *p = vs;
    while (p < ve) {
        while (p < ve && is_space((u8)*p)) p++;
        const char *t = p;
        int depth = 0;
        while (p < ve && !(depth == 0 && is_space((u8)*p))) {
            if (*p == '(') depth++;
            if (*p == ')') depth--;
            p++;
        }
        if (p > t && !value_is(t, p, "url(") && !value_is(t, p, "var(") && !value_is(t, p, "linear-gradient") &&
            parse_color(t, (int)(p - t), col))
            return 1;
    }
    return 0;
}

static int keyword(const char *v, const char *e, const char *const *words, int n) {  // 1 + index, 0 = none
    for (int i = 0; i < n; i++)
        if (value_is(v, e, words[i])) return i + 1;
    return 0;
}

// Reads a declaration block (or a style="" attribute) for the properties we care about.
static void scan_decls(const char *s, const char *e, Decl *d) {
    __builtin_memset(d, 0, sizeof *d);
    int abs_pos = 0, far_left = 0, clipped = 0;
    const char *p = s;
    while (p < e) {
        while (p < e && (is_space((u8)*p) || *p == ';')) p++;
        const char *ns = p;
        while (p < e && *p != ':' && *p != ';') p++;
        const char *ne = p;
        while (ne > ns && is_space((u8)ne[-1])) ne--;
        if (p >= e || *p != ':') continue;
        p++;
        const char *vs = p;
        int depth = 0;
        while (p < e && !(*p == ';' && depth == 0)) {
            if (*p == '(') depth++;
            if (*p == ')') depth--;
            p++;
        }
        const char *ve = p;
        while (vs < ve && is_space((u8)*vs)) vs++;
        int imp = 0;
        for (const char *q = vs; q < ve; q++)
            if (*q == '!') imp = 1;
        int nl = (int)(ne - ns);
        int vl = (int)(ve - vs), val;
        u8 unit;
        const char *q = vs;
        if (imp) {  // the value without "!important"
            const char *bang = vs;
            while (bang < ve && *bang != '!') bang++;
            ve = bang;
            while (ve > vs && is_space((u8)ve[-1])) ve--;
            vl = (int)(ve - vs);
        }
        (void)vl;
        if (nl == 7 && iprefix(ns, 7, "display")) {
            d->disp = value_is(vs, ve, "none") ? 1 : 2;
            d->imp_disp = (u8)imp;
            static const char *const kinds[] = {"inline-flex", "inline-grid", "inline-block", "inline", "block", "flex", "grid", "list-item", "flow-root"};
            static const u8 as[] = {LD_FLEX, LD_GRID, LD_INLINE_BLOCK, LD_INLINE, LD_BLOCK, LD_FLEX, LD_GRID, LD_BLOCK, LD_BLOCK};
            int k = keyword(vs, ve, kinds, 9);
            d->ldisp = k ? as[k - 1] : LD_OTHER;
        } else if (nl == 5 && iprefix(ns, 5, "float")) {
            static const char *const w[] = {"none", "left", "right", "inline-start", "inline-end"};
            static const u8 as[] = {1, 2, 3, 2, 3};
            int k = keyword(vs, ve, w, 5);
            if (k) set_lp(d, LP_FLOAT, as[k - 1], U_NUM, imp);
        } else if (nl == 5 && iprefix(ns, 5, "clear")) {
            static const char *const w[] = {"none", "left", "right", "both", "inline-start", "inline-end"};
            static const u8 as[] = {1, 2, 3, 4, 2, 3};
            int k = keyword(vs, ve, w, 6);
            if (k) set_lp(d, LP_CLEAR, as[k - 1], U_NUM, imp);
        } else if ((nl == 5 && iprefix(ns, 5, "width")) || (nl == 9 && iprefix(ns, 9, "max-width")) || (nl == 10 && iprefix(ns, 10, "flex-basis"))) {
            if (read_len(&q, ve, &val, &unit)) set_lp(d, nl == 5 ? LP_WIDTH : nl == 9 ? LP_MAXW : LP_BASIS, val, unit, imp);
        } else if (nl == 9 && iprefix(ns, 9, "min-width")) {
            if (read_len(&q, ve, &val, &unit) && unit != U_PCT) set_lp(d, LP_MINW, val, unit, imp);
        } else if ((nl == 8 && iprefix(ns, 8, "overflow")) || (nl == 10 && iprefix(ns, 10, "overflow-x"))) {
            static const char *const w[] = {"visible", "hidden", "auto", "scroll", "clip"};
            int k = keyword(vs, ve, w, 5);  // (overflow: a b — the first is x)
            if (k) set_lp(d, LP_OVER, k == 1 ? 1 : 2, U_NUM, imp);
        } else if ((nl == 14 && iprefix(ns, 14, "flex-direction")) || (nl == 9 && iprefix(ns, 9, "flex-wrap")) || (nl == 9 && iprefix(ns, 9, "flex-flow"))) {
            for (const char *t = vs; t < ve; t++) {  // flex-flow has both, in any order
                if (t > vs && !is_space((u8)t[-1])) continue;
                if (value_is(t, ve, "column")) set_lp(d, LP_DIR, 2, U_NUM, imp);
                else if (value_is(t, ve, "row")) set_lp(d, LP_DIR, 1, U_NUM, imp);
                else if (value_is(t, ve, "nowrap")) set_lp(d, LP_WRAP, 1, U_NUM, imp);
                else if (value_is(t, ve, "wrap")) set_lp(d, LP_WRAP, 2, U_NUM, imp);
            }
        } else if (nl == 15 && iprefix(ns, 15, "justify-content")) {
            static const char *const w[] = {"flex-start", "start", "left", "normal", "stretch", "center", "flex-end", "end", "right",
                                            "space-between", "space-around", "space-evenly"};
            static const u8 as[] = {1, 1, 1, 1, 1, 2, 3, 3, 3, 4, 5, 6};
            int k = keyword(vs, ve, w, 12);
            if (k) set_lp(d, LP_JUSTIFY, as[k - 1], U_NUM, imp);
        } else if ((nl == 6 && iprefix(ns, 6, "margin")) || (nl == 7 && iprefix(ns, 7, "padding")) ||
                   (nl >= 10 && (iprefix(ns, 7, "margin-") || iprefix(ns, 8, "padding-")))) {
            box_sides(d, ns, nl, vs, ve, imp);
        } else if ((nl == 16 && iprefix(ns, 16, "background-color")) || (nl == 10 && iprefix(ns, 10, "background"))) {
            u32 col;
            if (value_is(vs, ve, "none") || value_is(vs, ve, "transparent")) set_lp(d, LP_BG, 0, U_NUM, imp);
            else if (background_colour(vs, ve, &col)) set_lp(d, LP_BG, (int)col, U_NUM, imp);
        } else if (nl == 5 && iprefix(ns, 5, "order")) {
            if (read_len(&q, ve, &val, &unit) && unit == U_NUM) set_lp(d, LP_ORDER, val / 100, U_NUM, imp);
        } else if (nl == 10 && iprefix(ns, 10, "align-self")) {
            static const char *const w[] = {"stretch", "normal", "flex-start", "start", "self-start", "baseline", "center", "flex-end", "end", "self-end"};
            static const u8 as[] = {1, 1, 2, 2, 2, 2, 3, 4, 4, 4};
            int k = keyword(vs, ve, w, 10);
            if (k) set_lp(d, LP_ASELF, as[k - 1], U_NUM, imp);
        } else if (nl == 11 && iprefix(ns, 11, "align-items")) {
            static const char *const w[] = {"stretch", "normal", "flex-start", "start", "self-start", "baseline", "center", "flex-end", "end", "self-end"};
            static const u8 as[] = {1, 1, 2, 2, 2, 2, 3, 4, 4, 4};
            int k = keyword(vs, ve, w, 10);
            if (k) set_lp(d, LP_ALIGN, as[k - 1], U_NUM, imp);
        } else if ((nl == 3 && iprefix(ns, 3, "gap")) || (nl == 8 && iprefix(ns, 8, "grid-gap"))) {
            if (read_len(&q, ve, &val, &unit)) {
                set_lp(d, LP_RGAP, val, unit, imp);
                set_lp(d, LP_CGAP, val, unit, imp);
                if (read_len(&q, ve, &val, &unit)) set_lp(d, LP_CGAP, val, unit, imp);
            }
        } else if ((nl == 7 && iprefix(ns, 7, "row-gap")) || (nl == 13 && iprefix(ns, 13, "grid-row-gap"))) {
            if (read_len(&q, ve, &val, &unit)) set_lp(d, LP_RGAP, val, unit, imp);
        } else if ((nl == 10 && iprefix(ns, 10, "column-gap")) || (nl == 15 && iprefix(ns, 15, "grid-column-gap"))) {
            if (read_len(&q, ve, &val, &unit)) set_lp(d, LP_CGAP, val, unit, imp);
        } else if (nl == 9 && iprefix(ns, 9, "flex-grow")) {
            if (read_len(&q, ve, &val, &unit)) set_lp(d, LP_GROW, val, U_NUM, imp);
        } else if (nl == 11 && iprefix(ns, 11, "flex-shrink")) {
            if (read_len(&q, ve, &val, &unit)) set_lp(d, LP_SHRINK, val, U_NUM, imp);
        } else if (nl == 4 && iprefix(ns, 4, "flex")) {  // none | auto | grow [shrink] [basis]
            if (value_is(vs, ve, "none")) { set_lp(d, LP_GROW, 0, U_NUM, imp); set_lp(d, LP_SHRINK, 0, U_NUM, imp); set_lp(d, LP_BASIS, 0, U_AUTO, imp); }
            else if (value_is(vs, ve, "auto")) { set_lp(d, LP_GROW, 100, U_NUM, imp); set_lp(d, LP_SHRINK, 100, U_NUM, imp); set_lp(d, LP_BASIS, 0, U_AUTO, imp); }
            else if (read_len(&q, ve, &val, &unit)) {
                if (unit != U_NUM) { set_lp(d, LP_BASIS, val, unit, imp); }
                else {
                    set_lp(d, LP_GROW, val, U_NUM, imp);
                    set_lp(d, LP_SHRINK, 100, U_NUM, imp);
                    set_lp(d, LP_BASIS, 0, U_PX, imp);  // "flex: 1" is basis 0
                    if (read_len(&q, ve, &val, &unit)) {
                        if (unit == U_NUM) {
                            set_lp(d, LP_SHRINK, val, U_NUM, imp);
                            if (read_len(&q, ve, &val, &unit)) set_lp(d, LP_BASIS, val, unit, imp);
                        } else set_lp(d, LP_BASIS, val, unit, imp);
                    }
                }
            }
        } else if (nl == 21 && iprefix(ns, 21, "grid-template-columns")) {
            if (!value_is(vs, ve, "none") && !value_is(vs, ve, "subgrid") && !value_is(vs, ve, "masonry"))
                set_lp(d, LP_COLS, (int)(unsigned long)arena_str(vs, (int)(ve - vs)), U_STR, imp);
        } else if ((nl == 11 && iprefix(ns, 11, "grid-column")) || (nl == 15 && iprefix(ns, 15, "grid-column-end"))) {
            const char *t = vs;
            while (t < ve && *t != '/') t++;
            int a = parse_int(vs, 0), span = 0;
            if (value_is(vs, ve, "span")) { q = vs + 4; if (read_len(&q, ve, &val, &unit)) span = val / 100; }
            else if (t < ve) {
                const char *b = t + 1;
                while (b < ve && is_space((u8)*b)) b++;
                if (value_is(b, ve, "span")) { q = b + 4; if (read_len(&q, ve, &val, &unit)) span = val / 100; }
                else {
                    int neg = *b == '-', end = parse_int(neg ? b + 1 : b, 0);
                    if (neg && end == 1) span = -1;  // to the last line: the whole row
                    else if (a > 0 && end > a) span = end - a;
                }
            }
            if (span) set_lp(d, LP_SPAN, span, U_NUM, imp);
        } else if (nl == 10 && iprefix(ns, 10, "visibility")) {
            d->vis = value_is(vs, ve, "hidden") || value_is(vs, ve, "collapse") ? 1 : 2;
            d->imp_vis = (u8)imp;
        } else if (nl == 8 && iprefix(ns, 8, "position")) {
            abs_pos = value_is(vs, ve, "absolute") || value_is(vs, ve, "fixed");
        } else if ((nl == 4 && iprefix(ns, 4, "left")) || (nl == 3 && iprefix(ns, 3, "top")) || (nl == 11 && iprefix(ns, 11, "text-indent"))) {
            char num[16];
            int k = 0;
            for (const char *q = vs + 1; q < ve && k < 15 && is_digit((u8)*q); q++) num[k++] = *q;
            num[k] = 0;
            if (*vs == '-' && k && parse_int(num, 0) >= 999) far_left = 1;
        } else if (nl == 4 && iprefix(ns, 4, "clip")) {
            const char *q = vs;
            while (q < ve && *q != '(') q++;
            if (q < ve && value_is(vs, ve, "rect")) {
                q++;
                while (q < ve && is_space((u8)*q)) q++;
                if (q < ve && (*q == '0' || *q == '1')) clipped = 1;
            }
        } else if (nl == 9 && iprefix(ns, 9, "clip-path")) {
            if (value_is(vs, ve, "inset(50%") || value_is(vs, ve, "inset(100%") || value_is(vs, ve, "rect(0")) clipped = 1;
        }
    }
    // (taken out of the flow and 1px wide: GitHub's .sr-only, with a clip we may not know)
    int speck = abs_pos && d->lhas[LP_WIDTH] && d->lunit[LP_WIDTH] == U_PX && d->lval[LP_WIDTH] <= 100;
    if ((abs_pos && clipped) || far_left || speck) d->sr = 1;
}

static int css_order;

static void parse_rules(const char *s, const char *e, int base_order) {
    const char *p = s;
    while (p < e) {
        while (p < e && (is_space((u8)*p) || *p == ';' || *p == '}')) p++;
        if (p >= e) break;
        if (*p == '@') {
            const char *ks = ++p;
            while (p < e && ident_char((u8)*p)) p++;
            int kl = (int)(p - ks);
            const char *prelude = p;
            while (p < e && *p != '{' && *p != ';') p++;
            if (p >= e) break;
            if (*p == ';') { p++; continue; }  // @import, @charset, @layer a, b;
            const char *be = block_end(p, e);
            if (kl == 5 && iprefix(ks, 5, "media")) {
                if (media_matches(prelude, p)) parse_rules(p + 1, be, base_order);
            } else if ((kl == 8 && iprefix(ks, 8, "supports")) || (kl == 5 && iprefix(ks, 5, "layer")) ||
                       (kl == 5 && iprefix(ks, 5, "scope"))) {
                parse_rules(p + 1, be, base_order);
            }  // @font-face, @keyframes, @container, @page...: skipped
            p = be + 1;
            continue;
        }
        const char *sel = p;
        while (p < e && *p != '{') {
            if (*p == '(' || *p == '[') {
                Cur c = {p, e};
                skip_balanced(&c);
                p = c.p;
            } else {
                p++;
            }
        }
        if (p >= e) break;
        const char *be = block_end(p, e);
        Decl d;
        scan_decls(p + 1, be, &d);
        int lay = 0;
        for (int i = 0; i < LP_COUNT; i++) lay |= d.lhas[i];
        if (d.disp || d.vis || d.sr || lay) {
            int order = base_order + css_order++;
            const char *a = sel;
            int depth = 0;
            for (const char *q = sel; q <= p; q++) {  // split the selector list at top-level commas
                if (q < p && (*q == '(' || *q == '[')) depth++;
                if (q < p && (*q == ')' || *q == ']')) depth--;
                if (q == p || (*q == ',' && depth == 0)) {
                    add_selector(a, q, &d, order);
                    a = q + 1;
                }
            }
        }
        p = be + 1;
    }
}

// ---- matching ----------------------------------------------------------------------------

static int mem_cmp(const void *a, const void *b, u32 n) {
    const u8 *x = (const u8 *)a, *y = (const u8 *)b;
    for (u32 i = 0; i < n; i++)
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    return 0;
}

static int has_class(Node *n, const char *cls) {
    const char *c = attr(n, "class");
    if (!c) return 0;
    int len = lw_strlen(cls);
    while (*c) {
        while (*c && is_space((u8)*c)) c++;
        const char *s = c;
        while (*c && !is_space((u8)*c)) c++;
        if (c - s == len) {
            int ok = 1;
            for (int i = 0; i < len && ok; i++) ok = s[i] == cls[i];
            if (ok) return 1;
        }
    }
    return 0;
}

static Node *prev_elem(Node *n) {
    Node *prev = 0;
    for (Node *c = n->parent ? n->parent->first : 0; c && c != n; c = c->next)
        if (c->type == N_ELEM) prev = c;
    return prev;
}

static Node *next_elem(Node *n) {
    for (Node *c = n->next; c; c = c->next)
        if (c->type == N_ELEM) return c;
    return 0;
}

static int attr_match(const char *v, const char *want, u8 op) {
    if (!v) return 0;
    if (!op) return 1;
    int vl = lw_strlen(v), wl = lw_strlen(want);
    switch (op) {
    case '=': return vl == wl && mem_cmp(v, want, (u32)vl) == 0;
    case '^': return wl <= vl && mem_cmp(v, want, (u32)wl) == 0;
    case '$': return wl <= vl && mem_cmp(v + vl - wl, want, (u32)wl) == 0;
    case '*':
        for (int i = 0; i + wl <= vl; i++)
            if (mem_cmp(v + i, want, (u32)wl) == 0) return 1;
        return 0;
    case '~':
        for (int i = 0; i + wl <= vl; i++)
            if ((i == 0 || is_space((u8)v[i - 1])) && mem_cmp(v + i, want, (u32)wl) == 0 &&
                (i + wl == vl || is_space((u8)v[i + wl])))
                return 1;
        return 0;
    case '|': return (vl == wl || (vl > wl && v[wl] == '-')) && mem_cmp(v, want, (u32)wl) == 0;
    }
    return 0;
}

static int match_compound(Node *n, const Compound *k) {
    if (k->impossible || n->type != N_ELEM) return 0;
    if (k->tag >= 0) {
        if (k->tag != n->tag) return 0;
        if (k->tag == T_UNKNOWN && !ieq(k->tag_name, n->name)) return 0;
    }
    if (k->id) {
        const char *id = attr(n, "id");
        if (!id || !ieq(id, k->id)) return 0;
    }
    for (int i = 0; i < k->nclasses; i++)
        if (!has_class(n, k->classes[i])) return 0;
    for (int i = 0; i < k->nattrs; i++)
        if (!attr_match(attr(n, k->attr_names[i]), k->attr_values[i], k->attr_ops[i])) return 0;
    if (k->first_child && prev_elem(n)) return 0;
    if (k->last_child && next_elem(n)) return 0;
    if (k->checked && !(n->tag == T_INPUT && attr(n, "checked"))) return 0;
    if (k->link && !(n->tag == T_A && attr(n, "href"))) return 0;
    for (int i = 0; i < k->nnots; i++)
        if (match_compound(n, &k->nots[i])) return 0;
    if (k->nises) {
        int any = 0;
        for (int i = 0; i < k->nises && !any; i++) any = match_compound(n, &k->ises[i]);
        if (!any) return 0;
    }
    return 1;
}

static int match_from(Node *n, const Rule *r, int i) {
    if (!match_compound(n, &r->parts[i])) return 0;
    if (i + 1 == r->nparts) return 1;
    switch (r->parts[i].comb) {
    case '>': return n->parent && n->parent->type == N_ELEM && match_from(n->parent, r, i + 1);
    case ' ':
        for (Node *p = n->parent; p && p->type == N_ELEM; p = p->parent)
            if (match_from(p, r, i + 1)) return 1;
        return 0;
    case '+': {
        Node *p = prev_elem(n);
        return p && match_from(p, r, i + 1);
    }
    case '~':
        for (Node *c = n->parent ? n->parent->first : 0; c && c != n; c = c->next)
            if (c->type == N_ELEM && match_from(c, r, i + 1)) return 1;
        return 0;
    }
    return 0;
}

#define PASS0_MASK ((1u << LP_FLOAT) | (1u << LP_CLEAR))

// Rules indexed by what their rightmost compound needs: an id, a class, a tag, or nothing.
typedef struct { const char *key; int rule, next; } Bucket;
#define CSS_HASH 4096
// Three indexes: [0] the rules every element needs (display, visibility, float, clear), [1]
// the rules with LP_ITEM_MASK properties (lay_items), [2] the rules with a box's spacing and
// colour (box_css). A rule with several kinds is in each of their indexes.
static int *css_heads[3];    // CSS_HASH heads for id/class keys
static int tag_heads[3][T_COUNT + 1], any_head[3] = {-1, -1, -1};
static Bucket *buckets;
static int nbuckets, buckets_cap;

static u32 key_hash(const char *s, int kind) {
    u32 h = 2166136261u ^ (u32)kind;
    while (*s) h = (h ^ (u8)lower(*s++)) * 16777619u;
    return h & (CSS_HASH - 1);
}

static void bucket_add(int *head, const char *key, int rule) {
    buckets = (Bucket *)grow_array(buckets, nbuckets, &buckets_cap, sizeof(Bucket));
    buckets[nbuckets].key = key;
    buckets[nbuckets].rule = rule;
    buckets[nbuckets].next = *head;
    *head = nbuckets++;
}

static void index_rule(int i, int set) {
    const Compound *k = &rules[i].parts[0];
    if (k->impossible) return;
    if (k->id) bucket_add(&css_heads[set][key_hash(k->id, 1)], k->id, i);
    else if (k->nclasses) bucket_add(&css_heads[set][key_hash(k->classes[0], 2)], k->classes[0], i);
    else if (k->tag >= 0) bucket_add(&tag_heads[set][k->tag], 0, i);
    else bucket_add(&any_head[set], 0, i);
}

static void index_both(int i) {  // into the index(es) of what it sets
    const Rule *r = &rules[i];
    if (r->d.disp || r->d.vis || r->d.sr || (r->lmask & PASS0_MASK)) index_rule(i, 0);
    if (r->lmask & LP_ITEM_MASK) index_rule(i, 1);
    if (r->lmask & LP_BOXPASS_MASK) index_rule(i, 2);
}

static void index_rules(void) {
    for (int set = 0; set < 3; set++) {
        if (!css_heads[set]) css_heads[set] = (int *)must_alloc(CSS_HASH * sizeof(int));
        for (int i = 0; i < CSS_HASH; i++) css_heads[set][i] = -1;
        for (int i = 0; i <= T_COUNT; i++) tag_heads[set][i] = -1;
        any_head[set] = -1;
    }
    nbuckets = 0;
    for (int i = 0; i < nrules; i++) index_both(i);
}

static int best_disp, best_vis, sr_hit, best_lp[LP_COUNT];
static int item_pass;  // which properties the cascade looks up: 0 display, float, clear; 1 LP_ITEM_MASK; 2 LP_BOXPASS_MASK
static const u32 pass_mask[3] = {PASS0_MASK, LP_ITEM_MASK, LP_BOXPASS_MASK};
static const u64 *cur_anc;  // anc_bloom of the element being styled

static int beats(const Rule *r, int imp, int best, int best_imp) {
    if (best < 0) return 1;
    const Rule *w = &rules[best];
    if (imp != best_imp) return imp > best_imp;
    return r->spec > w->spec || (r->spec == w->spec && r->order > w->order);
}

static void consider_rule(Node *n, int ri) {
    const Rule *r = &rules[ri];
    u32 lm = r->lmask & pass_mask[item_pass];
    if (!lm && (item_pass || (!r->d.disp && !r->d.vis && !r->d.sr))) return;  // nothing this pass looks for
    int want_disp = !item_pass && r->d.disp && beats(r, r->d.imp_disp, best_disp, best_disp >= 0 ? rules[best_disp].d.imp_disp : 0);
    int want_vis = !item_pass && r->d.vis && beats(r, r->d.imp_vis, best_vis, best_vis >= 0 ? rules[best_vis].d.imp_vis : 0);
    int want_sr = !item_pass && r->d.sr && !sr_hit;
    u32 want_lp = 0;
    for (int i = 0; lm; i++, lm >>= 1)
        if ((lm & 1) && beats(r, r->d.lhas[i] == 2, best_lp[i], best_lp[i] >= 0 ? rules[best_lp[i]].d.lhas[i] == 2 : 0)) want_lp |= 1u << i;
    if (!want_disp && !want_vis && !want_sr && !want_lp) return;
    if (r->has_need) {
        for (int i = 0; i < 4; i++)
            if ((cur_anc[i] & r->need[i]) != r->need[i]) return;
    }
    if (!match_from(n, r, 0)) return;
    if (want_disp) best_disp = ri;
    if (want_vis) best_vis = ri;
    if (want_sr) sr_hit = 1;
    for (int i = 0; i < LP_COUNT; i++)
        if (want_lp & (1u << i)) best_lp[i] = ri;
}

static void consider(Node *n, int b) {
    for (; b >= 0; b = buckets[b].next) consider_rule(n, buckets[b].rule);
}

static void consider_key(Node *n, const char *key, int kind) {
    for (int b = css_heads[item_pass][key_hash(key, kind)]; b >= 0; b = buckets[b].next) {
        const char *a = buckets[b].key, *c = key;
        if (kind == 1) { if (!ieq(a, c)) continue; }
        else {  // classes are case-sensitive
            while (*a && *a == *c) a++, c++;
            if (*a != *c) continue;
        }
        consider_rule(n, buckets[b].rule);
    }
}

// Runs the cascade for one element. Elements are styled lazily, when layout first asks
// about them (css_hidden below): matching every element up front costs more than the
// whole layout on big pages, and most of a page is often never laid out (reader view).
static void cascade(Node *c) {  // the rules that match c, by id, class, tag, any
        best_disp = best_vis = -1;
        for (int i = 0; i < LP_COUNT; i++) best_lp[i] = -1;
        cur_anc = any_need ? anc_bloom(c) : bloom_none;
        sr_hit = 0;
        const char *id = attr(c, "id");
        if (id) consider_key(c, id, 1);
        const char *cls = attr(c, "class");
        if (cls) {
            char one[128];
            const char *p = cls;
            while (*p) {
                while (*p && is_space((u8)*p)) p++;
                int k = 0;
                while (*p && !is_space((u8)*p)) { if (k < 127) one[k++] = *p; p++; }
                one[k] = 0;
                if (k) consider_key(c, one, 2);
            }
        }
        consider(c, tag_heads[item_pass][c->tag]);
        consider(c, any_head[item_pass]);
}

static int css_node(Node *c) {
        item_pass = 0;
        cascade(c);
        int disp = best_disp >= 0 ? rules[best_disp].d.disp : 0;
        int vis = best_vis >= 0 ? rules[best_vis].d.vis : 0;
        Lay lay;
        __builtin_memset(&lay, 0, sizeof lay);
        int any = 0;
        if (best_disp >= 0) lay.disp = rules[best_disp].d.ldisp;
        for (int i = 0; i < LP_COUNT; i++)
            if ((PASS0_MASK & (1u << i)) && best_lp[i] >= 0) {
                const Decl *d = &rules[best_lp[i]].d;
                lay.has[i] = 1; lay.unit[i] = d->lunit[i]; lay.val[i] = d->lval[i];
                any = 1;
            }
        const char *st = attr(c, "style");  // style="" beats stylesheet rules that aren't !important
        if (st) {
            Decl in;
            scan_decls(st, st + lw_strlen(st), &in);
            if (in.disp && !(best_disp >= 0 && rules[best_disp].d.imp_disp)) { disp = in.disp; lay.disp = in.ldisp; }
            if (in.vis && !(best_vis >= 0 && rules[best_vis].d.imp_vis)) vis = in.vis;
            if (in.sr) sr_hit = 1;
            for (int i = 0; i < LP_COUNT; i++)
                if ((PASS0_MASK & (1u << i)) && in.lhas[i] && !(best_lp[i] >= 0 && rules[best_lp[i]].d.lhas[i] == 2)) {
                    lay.has[i] = 1; lay.unit[i] = in.lunit[i]; lay.val[i] = in.lval[i];
                    any = 1;
                }
        }
        c->lay = 0;
        if (any || lay.disp) {
            c->lay = (Lay *)arena(sizeof(Lay));
            *c->lay = lay;
        }
        return disp == 1 || vis == 1 || sr_hit;
}

static u8 css_epoch;  // bumped whenever the rules change; nodes styled in an older epoch are stale
static int css_hidden(Node *n);

// c's LP_ITEM_MASK properties (width, max-width, flex and grid ones, overflow), for a float,
// a flex or grid container or item. Returns c's Lay (made if it had none), or 0 for a text node.
static Lay *lay_items(Node *c) {
    if (c->type != N_ELEM) return 0;
    css_hidden(c);  // (the rest of the cascade first: it makes c->lay anew when the rules change)
    if (c->lay && c->lay->items_done) return c->lay;
    if (!c->lay) {
        c->lay = (Lay *)arena(sizeof(Lay));
        __builtin_memset(c->lay, 0, sizeof(Lay));
    }
    Lay *l = c->lay;
    item_pass = 1;
    cascade(c);
    item_pass = 0;
    for (int i = 0; i < LP_COUNT; i++)
        if ((LP_ITEM_MASK & (1u << i)) && best_lp[i] >= 0) {
            const Decl *d = &rules[best_lp[i]].d;
            l->has[i] = 1; l->unit[i] = d->lunit[i]; l->val[i] = d->lval[i];
        }
    const char *st = attr(c, "style");
    if (st) {
        Decl in;
        scan_decls(st, st + lw_strlen(st), &in);
        for (int i = 0; i < LP_COUNT; i++)
            if ((LP_ITEM_MASK & (1u << i)) && in.lhas[i] && !(best_lp[i] >= 0 && rules[best_lp[i]].d.lhas[i] == 2)) {
                l->has[i] = 1; l->unit[i] = in.lunit[i]; l->val[i] = in.lval[i];
            }
    }
    l->items_done = 1;
    return l;
}

// c's margin, padding, background, width and max-width (LP_BOXPASS_MASK), for a block
// layout reaches; 0 if it has none (most elements: then nothing is kept for it).
static const Lay *box_css(Node *c) {
    if (c->type != N_ELEM) return 0;
    css_hidden(c);
    if (c->box_ep == css_epoch) return c->box;
    c->box_ep = css_epoch;
    c->box = 0;
    item_pass = 2;
    cascade(c);
    item_pass = 0;
    Lay l;
    __builtin_memset(&l, 0, sizeof l);
    int any = 0;
    for (int i = 0; i < LP_COUNT; i++)
        if ((LP_BOXPASS_MASK & (1u << i)) && best_lp[i] >= 0) {
            const Decl *d = &rules[best_lp[i]].d;
            l.has[i] = 1; l.unit[i] = d->lunit[i]; l.val[i] = d->lval[i];
            any = 1;
        }
    const char *st = attr(c, "style");
    if (st) {
        Decl in;
        scan_decls(st, st + lw_strlen(st), &in);
        for (int i = 0; i < LP_COUNT; i++)
            if ((LP_BOXPASS_MASK & (1u << i)) && in.lhas[i] && !(best_lp[i] >= 0 && rules[best_lp[i]].d.lhas[i] == 2)) {
                l.has[i] = 1; l.unit[i] = in.lunit[i]; l.val[i] = in.lval[i];
                any = 1;
            }
    }
    if (any) {
        c->box = (Lay *)arena(sizeof(Lay));
        *c->box = l;
    }
    return c->box;
}

static void css_reset(Node *n) {
    for (Node *c = n->first; c; c = c->next) {
        c->css_ep = 0;
        c->box_ep = 0;
        css_reset(c);
    }
}

static int css_hidden(Node *n) {
    if (n->css_ep != css_epoch) {
        n->css_hidden = (u8)css_node(n);
        n->css_ep = css_epoch;
    }
    return n->css_hidden;
}

// ---- loading -----------------------------------------------------------------------------

typedef struct { int fetch_id, state; int order; const char *media; } Sheet;  // state 0 loading, 1 done
static Sheet sheets[64];
static int nsheets, sheets_pending;
static int css_width = -1;       // view width (CSS px) the cascade was computed for
static char **sheet_text;        // kept to re-evaluate @media after a resize
static int *sheet_len;

static void keep_sheet(int slot, const char *s, int len) {
    if (!sheet_text) {
        sheet_text = (char **)must_alloc(64 * sizeof(char *));
        sheet_len = (int *)must_alloc(64 * sizeof(int));
        __builtin_memset(sheet_text, 0, 64 * sizeof(char *));
    }
    char *copy = (char *)must_alloc((u32)len + 1);
    __builtin_memcpy(copy, s, (u32)len);
    copy[len] = 0;
    sheet_text[slot] = copy;
    sheet_len[slot] = len;
}

static int css_built;      // compute_css has run; sheets found later are added to the rules
static int css_new_rules;  // rules were added since the viewer last looked (it relays out)
static double css_wait_until;  // the page isn't shown before this while stylesheets load

static void css_bump(void) {  // cached results (Node.css_hidden) are stale now
    if (++css_epoch == 0) {  // wrapped: forget every node's stamp
        if (root) css_reset(root);
        css_epoch = 1;
    }
}

// Adds one more sheet's rules (one that arrived or was found after compute_css). The
// cascade order comes from the sheet's position in the document, so adding late gives the
// same result as having had it from the start.
static void css_add_sheet(int i) {
    if (!sheet_text || !sheet_text[i]) return;
    const char *m = sheets[i].media;
    if (m && !media_matches(m, m + lw_strlen(m))) return;
    int first = nrules;
    parse_rules(sheet_text[i], sheet_text[i] + sheet_len[i], i << 20);
    for (int r = first; r < nrules; r++) index_both(r);
    if (nrules > first) {
        css_bump();
        css_new_rules = 1;
    }
}

// Re-parses every sheet (media queries depend on the width) and runs the cascade.
static void compute_css(void) {
    nrules = 0;
    css_order = 0;
    any_need = 0;
    for (int i = 0; i < nsheets; i++) {
        if (sheets[i].state != 1 || !sheet_text || !sheet_text[i]) continue;
        const char *m = sheets[i].media;
        if (m && !media_matches(m, m + lw_strlen(m))) continue;
        parse_rules(sheet_text[i], sheet_text[i] + sheet_len[i], i << 20);
    }
    index_rules();
    css_bump();
    css_width = (int)(W / S);
    css_built = 1;
}

// A <style> or <link rel=stylesheet> the parser has finished, in document order.
static void register_sheet(Node *c) {
    if (nsheets >= 64) return;
    if (c->tag == T_STYLE) {
        int slot = nsheets++;
        sheets[slot].state = 1;
        sheets[slot].media = attr(c, "media");
        int total = 0;
        for (Node *t = c->first; t; t = t->next)
            if (t->type == N_TEXT) total += (int)t->len;
        char *buf = (char *)must_alloc((u32)total + 1);
        int k = 0;
        for (Node *t = c->first; t; t = t->next)
            if (t->type == N_TEXT) { __builtin_memcpy(buf + k, t->text, t->len); k += (int)t->len; }
        keep_sheet(slot, buf, k);
        mem_free(buf);
        if (css_built) css_add_sheet(slot);
        return;
    }
    const char *rel = attr(c, "rel"), *href = attr(c, "href");
    if (rel && href && *href && attr_match(rel, "stylesheet", '~') && !attr_match(rel, "alternate", '~') && !attr(c, "disabled")) {
        const char *media = attr(c, "media");
        if (media && !media_matches(media, media + lw_strlen(media)) && !attr_match(media, "screen", '*')) return;
        int slot = nsheets++;
        sheets[slot].state = 0;
        sheets[slot].media = media;
        sheets[slot].fetch_id = lw_fetch(href, lw_strlen(href));
        sheets_pending++;
        if (!css_wait_until) css_wait_until = lw_now() + 2500;  // like browsers: wait a little for CSS
    }
}

// Returns the sheet's slot if this fetch was a stylesheet, else -1.
static int sheet_arrived(int id, int status, const u8 *data, int len) {
    for (int i = 0; i < nsheets; i++) {
        if (sheets[i].state != 0 || sheets[i].fetch_id != id) continue;
        sheets[i].state = 1;
        if (status == 200) keep_sheet(i, (const char *)data, len);
        sheets_pending--;
        return i;
    }
    return -1;
}
