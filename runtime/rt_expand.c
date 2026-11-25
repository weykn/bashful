/* rt_expand.c - word expansion: IFS splitting, globbing, parameter expansion operators */
#include "rt.h"

/* ------------------------------------------------------- word builder */
static WordBuilder *wb_pool[32]; static int nwb_pool;
WordBuilder *bash_rt_wb_new(int mode) {
    WordBuilder *b;
    if (nwb_pool) b = wb_pool[--nwb_pool]; else { b = xmalloc(sizeof *b); memset(b, 0, sizeof *b); }
    b->mode = mode; b->n = 0; b->nseg = 0; b->have = 0; b->glob = 0; b->argc = 0; return b;
}
static void wb_recycle(WordBuilder *b) {
    b->argc = 0; b->an = 0; b->fixed = 0;
    if (nwb_pool < 32 && b->cap < 65536 && b->acap < 65536) wb_pool[nwb_pool++] = b;
    else { free(b->buf); free(b->seg); free(b->argv); free(b->ab); free(b); }
}
static void wb_pushcopy(WordBuilder *b, const char *s, size_t n) {
    if (b->fixed) { for (int i = 0; i < b->argc; i++) b->argv[i] = (char *)(uintptr_t)(b->argv[i] - b->ab); b->fixed = 0; }   /* pushing after wb_argv */
    if (b->argc + 2 > b->capargv) { b->capargv = b->capargv ? b->capargv * 2 : 8; b->argv = xrealloc(b->argv, b->capargv * sizeof(char *)); }
    if (b->an + n + 1 > b->acap) { b->acap = (b->an + n + 1) * 2 + 64; b->ab = xrealloc(b->ab, b->acap); }
    memcpy(b->ab + b->an, s, n); b->ab[b->an + n] = 0;
    b->argv[b->argc++] = (char *)(uintptr_t)b->an; b->argv[b->argc] = NULL; b->an += n + 1;
}
static int is_special_glob(char c) { return c == '*' || c == '?' || c == '[' || c == ']' || c == '\\'; }
static void wb_put(WordBuilder *b, const char *s, size_t n, int kind) {
    if (!n) return;
    if ((b->mode == WB_PATTERN || b->mode == WB_REGEX) && kind == LIT_QUOTED) {
        for (size_t i = 0; i < n; i++) { if (b->mode == WB_PATTERN ? is_special_glob(s[i]) : strchr("\\.[]()*+?{}|^$", s[i]) != NULL) { if (b->n + 2 > b->cap) { b->cap = (b->n + 2) * 2; b->buf = xrealloc(b->buf, b->cap); } b->buf[b->n++] = '\\'; }
            if (b->n + 1 > b->cap) { b->cap = (b->n + 2) * 2; b->buf = xrealloc(b->buf, b->cap); } b->buf[b->n++] = s[i]; }
        return;
    }
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 16; b->buf = xrealloc(b->buf, b->cap); }
    memcpy(b->buf + b->n, s, n); b->n += n;
    if (b->mode == WB_ARGV) {
        if (b->nseg && b->seg[b->nseg - 1].kind == kind) b->seg[b->nseg - 1].end = b->n;
        else { if (b->nseg == b->capseg) { b->capseg = b->capseg ? b->capseg * 2 : 8; b->seg = xrealloc(b->seg, b->capseg * sizeof(Seg)); } b->seg[b->nseg].end = b->n; b->seg[b->nseg].kind = kind; b->nseg++; }
        if (kind != LIT_QUOTED && !b->glob && memchr(s, '*', n) ? 1 : (kind != LIT_QUOTED && !b->glob && (memchr(s, '?', n) || memchr(s, '[', n)))) b->glob = 1;
    }
}
void bash_rt_wb_end(WordBuilder *b) {
    if (b->mode != WB_ARGV || !b->have) return;
    b->have = 0;
    if (b->glob) {
        SB pat = { 0 }; size_t st = 0;
        for (int k = 0; k < b->nseg; k++) {
            for (size_t i = st; i < b->seg[k].end; i++) { if (b->seg[k].kind == LIT_QUOTED && is_special_glob(b->buf[i])) sb_putc(&pat, '\\'); sb_putc(&pat, b->buf[i]); }
            st = b->seg[k].end;
        }
        glob_t g; int rc = glob(pat.s ? pat.s : "", 0, NULL, &g); free(pat.s);
        if (rc == 0 && g.gl_pathc > 0) {
            for (size_t i = 0; i < g.gl_pathc; i++) wb_pushcopy(b, g.gl_pathv[i], strlen(g.gl_pathv[i]));
            globfree(&g); b->n = 0; b->nseg = 0; b->glob = 0; return;
        }
        if (rc == 0) globfree(&g);
        b->glob = 0;
    }
    wb_pushcopy(b, b->buf ? b->buf : "", b->n); b->n = 0; b->nseg = 0;
}
void bash_rt_wb_lit(WordBuilder *b, const char *s, int kind) { wb_put(b, s, strlen(s), kind); if (kind == LIT_QUOTED || *s) b->have = 1; }
void bash_rt_wb_const(WordBuilder *b, const char *s) {
    if (b->mode == WB_ARGV) { wb_pushcopy(b, s, strlen(s)); return; }
    wb_put(b, s, strlen(s), LIT_QUOTED);
}
static void wb_append(WordBuilder *b, const char *s, int quoted) {
    if (b->mode != WB_ARGV) { wb_put(b, s, strlen(s), quoted ? LIT_QUOTED : LIT_EXPANDED); return; }
    if (quoted) { wb_put(b, s, strlen(s), LIT_QUOTED); b->have = 1; return; }
    const char *ifs = ifs_chars();
    int dflt = !strcmp(ifs, " \t\n");
    while (*s) {
        const char *st = s;
        if (dflt) while (*s && *s != ' ' && *s != '\t' && *s != '\n') s++;
        else while (*s && !strchr(ifs, *s)) s++;
        if (s > st) { wb_put(b, st, s - st, LIT_EXPANDED); b->have = 1; }
        if (!*s) break;
        char c = *s++;
        if (c == ' ' || c == '\t' || c == '\n') { if (b->have) bash_rt_wb_end(b); }
        else { b->have = 1; bash_rt_wb_end(b); }
    }
}
static void wb_emit_list(WordBuilder *b, const char **v, int n, int quoted, int star) {
    if (b->mode != WB_ARGV || (quoted && star)) {
        SB j = { 0 }; const char *ifs = ifs_chars();
        for (int i = 0; i < n; i++) { if (i) { if (star) { if (*ifs) sb_putc(&j, *ifs); } else sb_putc(&j, ' '); } sb_puts(&j, v[i]); }
        char *s = sb_take(&j); wb_append(b, s, b->mode != WB_ARGV ? quoted : 1); free(s); return;
    }
    for (int i = 0; i < n; i++) { if (i) bash_rt_wb_end(b); wb_append(b, v[i], quoted); }
}
void bash_rt_wb_var_id(WordBuilder *b, int id, int quoted) { wb_append(b, bash_rt_gets_e(id), quoted); }
const char *bash_rt_pos_u(int n) {   /* $n as a borrowed string, honoring set -u */
    if (n > npos && opt_u) { bash_rt_warn("$%d: unbound variable", n); bash_rt_exit(1); }
    return bash_rt_pos_s(n);
}
void bash_rt_copy_id(int dst, int src) {   /* dst=$src without going through text when src is an integer */
    Var *s = vt[src];
    if ((s->flags & (VF_INT | VF_STR | VF_ARR | VF_ASC)) == VF_INT) { bash_rt_seti(dst, s->ival); return; }
    const char *v = var_str(s);
    if (!v) { if (opt_u) { bash_rt_warn("%s: unbound variable", s->name); bash_rt_exit(1); } v = ""; }
    var_set_str(vt[dst], v, 0);
}
void bash_rt_wb_pos(WordBuilder *b, int n, int quoted) {
    if (n > npos && opt_u) { bash_rt_warn("$%d: unbound variable", n); bash_rt_exit(1); }
    wb_append(b, bash_rt_pos_s(n), quoted);
}
void bash_rt_wb_int(WordBuilder *b, long v, int quoted) { char t[32]; rt_ltoa(t, v); wb_append(b, t, quoted); }
void bash_rt_wb_special(WordBuilder *b, int kind, int quoted) {
    if (kind == SP_AT || kind == SP_STAR) { wb_emit_list(b, (const char **)pos, npos, quoted, kind == SP_STAR); return; }
    if (kind == SP_FUNCNAME || kind == SP_SOURCE) { const char *v = fnstack_elem(kind == SP_FUNCNAME ? -3 : -4, 0); wb_append(b, v ? v : "", quoted); return; }
    wb_append(b, bash_rt_special_s(kind), quoted);
}

/* ---- pattern matching ----
 * bash matches patterns with its own matcher (globasciiranges: [a-z] compares code points), not with the C
 * library's locale-dependent fnmatch.  rt_glob implements that for ASCII text (and for any bytes in a
 * single-byte locale); multibyte text and [=x=] / [.x.] fall back to fnmatch. */
static int glob_class(const char *nm, size_t len, unsigned char c) {
#define CL(n, e) if (len == sizeof n - 1 && !memcmp(nm, n, len)) return (e) != 0;
    CL("alpha", isalpha(c)) CL("digit", isdigit(c)) CL("alnum", isalnum(c)) CL("upper", isupper(c)) CL("lower", islower(c))
    CL("space", isspace(c)) CL("blank", c == ' ' || c == '\t') CL("punct", ispunct(c)) CL("print", isprint(c)) CL("graph", isgraph(c))
    CL("cntrl", iscntrl(c)) CL("xdigit", isxdigit(c)) CL("word", isalnum(c) || c == '_')
#undef CL
    return -1;
}
/* *pp at '[': 1/0 = c matches or not (and *pp moves past ']'), -2 = no closing ']' ('[' is literal), -1 = unsupported */
static int glob_bracket(const char **pp, unsigned char c) {
    const char *p = *pp + 1; int neg = 0, ok = 0, first = 1;
    if (*p == '!' || *p == '^') { neg = 1; p++; }
    for (;;) {
        if (!*p) return -2;
        if (*p == ']' && !first) { p++; break; }
        first = 0;
        if (p[0] == '[' && p[1] == ':') {
            const char *e = strstr(p + 2, ":]");
            if (e) { int r = glob_class(p + 2, (size_t)(e - p - 2), c); if (r < 0) return -1; ok |= r; p = e + 2; continue; }
        }
        if (p[0] == '[' && (p[1] == '=' || p[1] == '.')) return -1;
        if (*p == '\\' && p[1]) p++;
        unsigned char lo = (unsigned char)*p++;
        if (*p == '-' && p[1] && p[1] != ']') {
            p++; if (*p == '[' && (p[1] == '.' || p[1] == '=' || p[1] == ':')) return -1;
            if (*p == '\\' && p[1]) p++;
            unsigned char hi = (unsigned char)*p++;
            if (lo <= c && c <= hi) ok = 1;
        } else if (c == lo) ok = 1;
    }
    *pp = p; return ok ^ neg;
}
static int rt_glob(const char *p, const char *s, size_t n) {   /* 1 match, 0 no match, -1 unsupported */
    const char *star_p = NULL; size_t star_i = 0, i = 0;
    for (;;) {
        if (*p == '*') { while (*p == '*') p++; if (!*p) return 1; star_p = p; star_i = i; continue; }
        if (*p && i < n) {
            unsigned char c = (unsigned char)s[i];
            if (*p == '?') { p++; i++; continue; }
            if (*p == '[') {
                const char *q = p; int r = glob_bracket(&q, c);
                if (r == -1) return -1;
                if (r == -2) { if (c == '[') { p++; i++; continue; } }
                else if (r) { p = q; i++; continue; }
            } else if (*p == '\\' && p[1]) { if (c == (unsigned char)p[1]) { p += 2; i++; continue; } }
            else if (c == (unsigned char)*p) { p++; i++; continue; }
        } else if (!*p && i == n) return 1;
        if (!star_p || star_i >= n) return 0;     /* backtrack: let the last * absorb one more character */
        p = star_p; i = ++star_i;
    }
}
static int is_ascii(const char *s, size_t n) { for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] & 0x80) return 0; return 1; }
int rt_fnm(const char *pat, const char *s, size_t n) {   /* does s[0..n) match pat? */
    if (!g_utf8 || (is_ascii(pat, strlen(pat)) && is_ascii(s, n))) { int r = rt_glob(pat, s, n); if (r >= 0) return r; }
    if (s[n] == 0) return fnmatch(pat, s, 0) == 0;
    char *t = xstrndup(s, n); int r = fnmatch(pat, t, 0) == 0; free(t); return r;
}
/* number of characters every match of pat has (no '*'), or -1 */
static long glob_fixed_len(const char *p) {
    long k = 0;
    while (*p) {
        if (*p == '*') return -1;
        if (*p == '[') { const char *q = p; int r = glob_bracket(&q, 0); if (r == -1) return -1; if (r != -2) { p = q; k++; continue; } }
        if (*p == '\\' && p[1]) p++;
        p++; k++;
    }
    return k;
}

/* in a multibyte locale, positions inside a UTF-8 sequence are not character boundaries */
#define MID_CHAR(s, i) (g_utf8 && ((unsigned char)(s)[i] & 0xC0) == 0x80)

/* ---- parameter expansion ---- */
static int has_glob(const char *p) { return strpbrk(p, "*?[\\") != NULL; }
/* lit = the unescaped text of pat[0..n) if it has no glob characters (and is ASCII in a UTF-8 locale, so that a
 * match always starts and ends on a character boundary); 0 otherwise */
static int glob_literal(const char *pat, size_t n, char *lit) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        char c = pat[i];
        if (c == '*' || c == '?' || c == '[') return 0;
        if (c == '\\') { if (++i >= n) return 0; c = pat[i]; }
        if (g_utf8 && ((unsigned char)c & 0x80)) return 0;
        lit[k++] = c;
    }
    lit[k] = 0;
    return 1;
}
static const char *last_strstr(const char *s, const char *x) {
    const char *r = NULL;
    for (const char *f = strstr(s, x); f; f = strstr(f + 1, x)) r = f;
    return r;
}
/* does s[0..n) match pat; ascii: pattern and subject known to be ASCII (or the locale is single-byte) */
static int trim_fnm(const char *pat, const char *s, size_t n, int ascii) {
    if (ascii) { int r = rt_glob(pat, s, n); if (r >= 0) return r; }
    return rt_fnm(pat, s, n);
}
static char *trim_prefix(const char *s, const char *pat, int longest) {
    size_t L = strlen(s), pl = strlen(pat);
    if (!has_glob(pat)) return xstrdup(pl <= L && !strncmp(s, pat, pl) ? s + pl : s);
    if (pat[0] == '*' && pl < 256) {   /* *LIT: up to the first (#) / last (##) occurrence of LIT */
        char lit[256];
        if (glob_literal(pat + 1, pl - 1, lit)) {
            if (!*lit) return xstrdup(longest ? s + L : s);
            const char *f = longest ? last_strstr(s, lit) : strstr(s, lit);
            return xstrdup(f ? f + strlen(lit) : s);
        }
    }
    int ascii = !g_utf8 || (is_ascii(pat, pl) && is_ascii(s, L));
    for (size_t k = 0; k <= L; k++) {
        size_t i = longest ? L - k : k;
        if (!MID_CHAR(s, i) && trim_fnm(pat, s, i, ascii)) return xstrdup(s + i);
    }
    return xstrdup(s);
}
static char *trim_suffix(const char *s, const char *pat, int longest) {
    size_t L = strlen(s), pl = strlen(pat);
    if (!has_glob(pat)) return (pl <= L && !strcmp(s + L - pl, pat)) ? xstrndup(s, L - pl) : xstrdup(s);
    if (pl && pat[pl - 1] == '*' && pl < 256 && (pl < 2 || pat[pl - 2] != '\\')) {   /* LIT*: from the last (%) / first (%%) occurrence */
        char lit[256];
        if (glob_literal(pat, pl - 1, lit)) {
            if (!*lit) return longest ? xstrdup("") : xstrdup(s);
            const char *f = longest ? strstr(s, lit) : last_strstr(s, lit);
            return f ? xstrndup(s, (size_t)(f - s)) : xstrdup(s);
        }
    }
    int ascii = !g_utf8 || (is_ascii(pat, pl) && is_ascii(s, L));
    for (size_t k = 0; k <= L; k++) {
        size_t i = longest ? k : L - k;
        if (!MID_CHAR(s, i) && trim_fnm(pat, s + i, L - i, ascii)) return xstrndup(s, i);
    }
    return xstrdup(s);
}
static char *subst(const char *s, const char *pat, const char *rep, int op) {
    SB o = { 0 }; size_t L = strlen(s), pl = strlen(pat);
    if (!has_glob(pat)) {
        if (!pl) return xstrdup(s);
        if (op == P_SUBST_PRE) { if (!strncmp(s, pat, pl)) { sb_puts(&o, rep); sb_puts(&o, s + pl); return sb_take(&o); } return xstrdup(s); }
        if (op == P_SUBST_SUF) { if (pl <= L && !strcmp(s + L - pl, pat)) { sb_putn(&o, s, L - pl); sb_puts(&o, rep); return sb_take(&o); } return xstrdup(s); }
        const char *p = s;
        for (;;) {
            const char *f = strstr(p, pat);
            if (!f) break;
            sb_putn(&o, p, f - p); sb_puts(&o, rep); p = f + pl;
            if (op != P_SUBST_ALL) break;
        }
        sb_puts(&o, p); return sb_take(&o);
    }
    /* a pattern without '*' matches a fixed number of characters: only one end position to try (ASCII text) */
    long fixed = (!g_utf8 || is_ascii(s, L)) ? glob_fixed_len(pat) : -1;
    size_t p = 0; int done = 0;
    while (p <= L) {
        long mlen = -1;
        if (!(done && op != P_SUBST_ALL) && !(op == P_SUBST_PRE && p != 0) && !MID_CHAR(s, p)) {
            if (fixed >= 0) { if (fixed > 0 && p + (size_t)fixed <= L && (op != P_SUBST_SUF || p + (size_t)fixed == L) && rt_fnm(pat, s + p, (size_t)fixed)) mlen = fixed; }
            else for (size_t e = L; e > p; e--) {
                if ((op == P_SUBST_SUF && e != L) || (e < L && MID_CHAR(s, e))) continue;
                if (rt_fnm(pat, s + p, e - p)) { mlen = e - p; break; }
            }
        }
        if (mlen > 0) { sb_puts(&o, rep); p += mlen; done = 1; if (op == P_SUBST_PRE || op == P_SUBST_SUF) { sb_puts(&o, s + p); p = L + 1; } }
        else { do { if (p < L) sb_putc(&o, s[p]); p++; } while (p < L && MID_CHAR(s, p)); }
    }
    return sb_take(&o);
}
static char *casemod(const char *s, int op) {
    char *o = xstrdup(s); size_t L = strlen(o);
    for (size_t i = 0; i < L; i++) {
        if (op == P_UP1 || op == P_LOW1) { if (i) break; }
        o[i] = (op == P_UP1 || op == P_UPALL) ? toupper((unsigned char)o[i]) : tolower((unsigned char)o[i]);
    }
    return o;
}
/* ${x@a}: the attribute letters, in bash's order */
static void attr_letters(Var *v, char *o) {
    int f = v ? v->flags : 0; char *p = o;
    if (f & VF_ARR) *p++ = 'a';
    if (f & VF_ASC) *p++ = 'A';
    if (f & VF_INT) *p++ = 'i';
    if (f & VF_RO) *p++ = 'r';
    if (f & VF_EXP) *p++ = 'x';
    *p = 0;
}
/* ${x@A} ${x@K} ${x@k} ${x@a}: forms that need the whole variable */
static void at_var(WordBuilder *b, Ref *r, int c, int quoted) {
    Var *v = r->vid >= 0 ? vt[r->vid] : NULL;
    char fl[16]; attr_letters(v, fl);
    int list = ref_is_list(r);
    SB o = { 0 };
    if (c == 'a') {
        if (list && v && (v->flags & (VF_ARR | VF_ASC))) { const char **l; int n = ref_list(r, &l); const char **w = xmalloc((n + 1) * sizeof *w); for (int i = 0; i < n; i++) w[i] = fl; wb_emit_list(b, w, n, quoted, ref_is_star(r)); free(w); free(l); return; }
        wb_append(b, v && var_isset(v) ? fl : "", quoted);
        return;
    }
    if (r->vid == -1) {   /* "$@": set -- 'a' 'b' for @A, the quoted words for @K */
        const char **l; int n = ref_list(r, &l);
        if (c == 'k') { wb_emit_list(b, l, n, quoted, ref_is_star(r)); free(l); return; }
        if (c == 'A') sb_puts(&o, "set --");
        for (int i = 0; i < n; i++) { if (i || c == 'A') sb_putc(&o, ' '); rt_quote_q(&o, l[i]); }
        free(l);
    } else if (list && v && (v->flags & (VF_ARR | VF_ASC))) {   /* the whole array */
        int asc = v->flags & VF_ASC;
        const char **keys = NULL; int nk = 0; char **store = NULL;
        if (asc) nk = asc_list(v->as, &keys, 1);
        else {
            store = xmalloc((v->arrn + 1) * sizeof *store); keys = xmalloc((v->arrn + 1) * sizeof *keys);
            for (int i = 0; i < v->arrn; i++) if (v->ak[i] & AK_SET) { char t[32]; rt_ltoa(t, i); store[nk] = xstrdup(t); keys[nk] = store[nk]; nk++; }
        }
        if (c == 'k') {
            const char **w = xmalloc((2 * nk + 1) * sizeof *w);
            for (int i = 0; i < nk; i++) { w[2 * i] = keys[i]; w[2 * i + 1] = asc ? ai_str(asc_find(v->as, keys[i], 0)) : el_str(v, atol(keys[i])); }
            wb_emit_list(b, w, 2 * nk, quoted, ref_is_star(r)); free(w);
        } else {
            if (c == 'A') { sb_puts(&o, "declare -"); sb_puts(&o, fl); sb_putc(&o, ' '); sb_puts(&o, v->name); }
            if (c == 'A' && asc && !nk) { /* declare -A w */ }
            else {
                if (c == 'A') sb_puts(&o, "=(");
                for (int i = 0; i < nk; i++) {
                    const char *val = asc ? ai_str(asc_find(v->as, keys[i], 0)) : el_str(v, atol(keys[i]));
                    if (i) sb_putc(&o, ' ');
                    if (c == 'A') sb_putc(&o, '[');
                    if (asc) rt_quote_key(&o, keys[i]); else sb_puts(&o, keys[i]);
                    sb_puts(&o, c == 'A' ? "]=" : " ");
                    rt_quote_dq(&o, val ? val : "");
                }
                if (asc && nk) sb_putc(&o, ' ');
                if (c == 'A') sb_putc(&o, ')');
            }
        }
        for (int i = 0; store && i < nk; i++) free(store[i]);
        free(store); free(keys);
        if (c == 'k') return;
    } else {   /* one value */
        const char *s = ref_scalar(r);
        if (c == 'k' || c == 'K') { if (s) rt_quote_q(&o, s); }
        else if (v && (s || *fl)) {   /* @A */
            int loc = rt_var_is_local(r->vid);
            if (*fl || loc) { sb_puts(&o, "declare "); if (*fl) { sb_putc(&o, '-'); sb_puts(&o, fl); sb_putc(&o, ' '); } }
            sb_puts(&o, v->name);
            if (s) { sb_putc(&o, '='); rt_quote_q(&o, s); }
        }
    }
    char *t = sb_take(&o); wb_append(b, t, quoted); free(t);
}
/* ${!prefix*} ${!prefix@}: the names of the set variables that start with prefix, sorted */
static int cmp_names(const void *a, const void *b) { return strcmp(*(const char *const *)a, *(const char *const *)b); }
static void names_with_prefix(WordBuilder *b, const char *pfx, int star, int quoted) {
    size_t L = strlen(pfx); int n = 0;
    const char **l = xmalloc((nvt + 1) * sizeof *l);
    for (int i = 0; i < nvt; i++) {
        Var *v = vt[i];
        if (!strncmp(v->name, pfx, L) && var_isset(v) && (isalpha((unsigned char)v->name[0]) || v->name[0] == '_')) l[n++] = v->name;
    }
    qsort(l, n, sizeof *l, cmp_names);
    wb_emit_list(b, l, n, quoted, star);
    free(l);
}
typedef struct { bash_str_t f; const char *c; } Opd;
static char *opd_get(const Opd *o, int *owned) { if (o->f) { *owned = 1; return o->f(); } *owned = 0; return (char *)(o->c ? o->c : ""); }

static void param_core(WordBuilder *b, Ref *r, int op, Opd A, Opd C, int quoted) {
    int islist = ref_is_list(r);
    switch (op) {
    case P_LEN: {
        char buf[32];
        if (islist) { const char **l; int n = ref_list(r, &l); free(l); rt_ltoa(buf, n); }
        else { const char *s = ref_scalar(r); snprintf(buf, sizeof buf, "%zu", s ? u8_len(s) : (size_t)0); }
        wb_append(b, buf, quoted); break; }
    case P_KEYS: {
        Var *v = r->vid >= 0 ? vt[r->vid] : NULL; char buf[32];
        int cap = (v && (v->flags & VF_ARR) ? v->arrn : 1) + 1; const char **l = xmalloc(cap * sizeof *l); char **store = xmalloc(cap * sizeof *store); int n = 0;
        if (v && (v->flags & VF_ASC)) { const char **kl; int kn = asc_list(v->as, &kl, 1); wb_emit_list(b, kl, kn, quoted, r->kind == 2); free(kl); free(store); free(l); break; }
        if (v && (v->flags & VF_ARR)) { for (int i = 0; i < v->arrn; i++) if (v->ak[i] & AK_SET) { rt_ltoa(buf, i); store[n] = xstrdup(buf); l[n] = store[n]; n++; } }
        else if (v && var_isset(v)) { store[n] = xstrdup("0"); l[n] = store[n]; n++; }
        wb_emit_list(b, l, n, quoted, r->kind == 2);
        for (int i = 0; i < n; i++) free(store[i]);
        free(store); free(l); break; }
    case P_NAMES: case P_NAMES_STAR: names_with_prefix(b, r->sname, op == P_NAMES_STAR, quoted); break;
    case P_AT: {
        int ow; char *ls = opd_get(&A, &ow); int c = *ls; if (ow) free(ls);
        if (c == 'A' || c == 'K' || c == 'k' || c == 'a') { at_var(b, r, c, quoted); break; }
        const char **l; int n; const char *one = NULL;
        if (islist) n = ref_list(r, &l);
        else {
            one = ref_scalar(r);
            if (!one) { if (opt_u && r->vid != -2) { bash_rt_warn("%s: unbound variable", r->sname); bash_rt_exit(1); } wb_append(b, "", quoted); break; }
            l = xmalloc(2 * sizeof *l); l[0] = one; n = 1;
        }
        char **out = xmalloc((n + 1) * sizeof *out);
        for (int i = 0; i < n; i++) {
            SB o = { 0 };
            switch (c) {
            case 'Q': rt_quote_q(&o, l[i]); out[i] = sb_take(&o); break;
            case 'E': out[i] = rt_ansic_decode(l[i]); break;
            case 'U': out[i] = casemod(l[i], P_UPALL); break;
            case 'u': out[i] = casemod(l[i], P_UP1); break;
            case 'L': out[i] = casemod(l[i], P_LOWALL); break;
            default: out[i] = xstrdup(l[i]);
            }
        }
        wb_emit_list(b, (const char **)out, n, quoted, ref_is_star(r));
        for (int i = 0; i < n; i++) free(out[i]);
        free(out); free(l);
        break; }
    case P_INDIRECT: {
        const char *s = ref_scalar(r);
        if (s) { Ref r2; ref_from_name(s, &r2); if (ref_is_list(&r2)) { const char **l; int n = ref_list(&r2, &l); wb_emit_list(b, l, n, quoted, ref_is_star(&r2)); free(l); } else { const char *t = ref_scalar(&r2); wb_append(b, t ? t : "", quoted); } ref_free(&r2); }
        break; }
    case P_DEFAULT: case P_DEFAULT_NC: case P_ASSIGN: case P_ASSIGN_NC: case P_ALT: case P_ALT_NC: case P_ERR: case P_ERR_NC: {
        const char *s = NULL; char *joined = NULL;
        if (islist) { const char **l; int n = ref_list(r, &l); SB j = { 0 }; for (int i = 0; i < n; i++) { if (i) sb_putc(&j, ' '); sb_puts(&j, l[i]); } free(l); if (n) { joined = sb_take(&j); s = joined; } else free(j.s); }
        else s = ref_scalar(r);
        int colon = (op == P_DEFAULT || op == P_ASSIGN || op == P_ALT || op == P_ERR);
        int unset = !s || (colon && !*s); int ow; char *d;
        if (op == P_DEFAULT || op == P_DEFAULT_NC) { if (unset) { d = opd_get(&A, &ow); wb_append(b, d, quoted); if (ow) free(d); } else wb_append(b, s, quoted); }
        else if (op == P_ASSIGN || op == P_ASSIGN_NC) { if (unset) { d = opd_get(&A, &ow); ref_assign(r, d); wb_append(b, d, quoted); if (ow) free(d); } else wb_append(b, s, quoted); }
        else if (op == P_ALT || op == P_ALT_NC) { if (!unset) { d = opd_get(&A, &ow); wb_append(b, d, quoted); if (ow) free(d); } }
        else { if (unset) { d = opd_get(&A, &ow); bash_rt_warn("%s: %s", r->sname, *d ? d : "parameter null or not set"); bash_rt_exit(1); } else wb_append(b, s, quoted); }
        free(joined); break; }
    case P_SUBSTR: {
        int ow; char *spec0 = opd_get(&A, &ow); char *spec = xstrdup(spec0); if (ow) free(spec0);
        char *colon = strchr(spec, ':'); int err; long off, len = -1; int haslen = 0;
        if (colon) { *colon = 0; len = bash_rt_arith_eval(colon + 1, &err); haslen = 1; }
        off = bash_rt_arith_eval(spec, &err); free(spec);
        if (islist) {
            const char **l; int n = ref_list(r, &l);
            const char **full = l; int fn = n; const char **tmp = NULL;
            if (ref_is_pos_list(r) && off == 0) { tmp = xmalloc((n + 2) * sizeof *tmp); tmp[0] = g_argv0; for (int i = 0; i < n; i++) tmp[i + 1] = l[i]; full = tmp; fn = n + 1; }
            else if (ref_is_pos_list(r)) off -= 1;
            if (off < 0) off += fn;
            if (off < 0 || off > fn) off = fn;
            long cnt = haslen ? (len < 0 ? fn - off + len : len) : fn - off;
            if (cnt < 0) cnt = 0; if (off + cnt > fn) cnt = fn - off;
            wb_emit_list(b, full + off, (int)cnt, quoted, ref_is_star(r));
            free(l); free(tmp);
        } else {
            const char *s = ref_scalar(r); if (!s) s = ""; long L = (long)u8_len(s);
            if (off < 0) { off += L; if (off < 0) break; } if (off > L) off = L;
            long cnt = haslen ? (len < 0 ? L - off + len : len) : L - off;
            if (cnt < 0) cnt = 0; if (off + cnt > L) cnt = L - off;
            size_t bo = u8_off(s, off), bl = u8_off(s + bo, cnt);
            char *t = xstrndup(s + bo, bl); wb_append(b, t, quoted); free(t);
        }
        break; }
    default: {   /* P_PLAIN, trims, substitutions, case */
        int ow1 = 0, ow2 = 0;
        char *pa = (op >= P_TRIM_S && op <= P_SUBST_SUF) ? opd_get(&A, &ow1) : NULL;
        char *pc = (op >= P_SUBST && op <= P_SUBST_SUF) ? opd_get(&C, &ow2) : NULL;
        const char **l; int n; char *tmpv = NULL;
        if (islist) n = ref_list(r, &l);
        else { const char *one = ref_scalar(r); if (!one && opt_u && op == P_PLAIN && r->vid != -2) { bash_rt_warn("%s: unbound variable", r->sname); bash_rt_exit(1); } tmpv = xstrdup(one ? one : ""); l = xmalloc(2 * sizeof *l); l[0] = tmpv; n = 1; }
        char **out = xmalloc((n + 1) * sizeof *out);
        for (int i = 0; i < n; i++) {
            switch (op) {
            case P_TRIM_S: out[i] = trim_prefix(l[i], pa, 0); break;
            case P_TRIM_L: out[i] = trim_prefix(l[i], pa, 1); break;
            case P_TRIM_SE: out[i] = trim_suffix(l[i], pa, 0); break;
            case P_TRIM_LE: out[i] = trim_suffix(l[i], pa, 1); break;
            case P_SUBST: case P_SUBST_ALL: case P_SUBST_PRE: case P_SUBST_SUF: out[i] = subst(l[i], pa, pc, op); break;
            case P_UP1: case P_UPALL: case P_LOW1: case P_LOWALL: out[i] = casemod(l[i], op); break;
            default: out[i] = xstrdup(l[i]);
            }
        }
        wb_emit_list(b, (const char **)out, n, quoted, ref_is_star(r));
        for (int i = 0; i < n; i++) free(out[i]);
        free(out); free(l); free(tmpv); if (ow1) free(pa); if (ow2) free(pc);
    } }
}
void bash_rt_wb_param_id(WordBuilder *b, int id, int sub, long idx, int op, bash_str_t fa, bash_str_t fc, const char *ca, const char *cc, int quoted) {
    Ref r; ref_from_id(id, sub, idx, &r); Opd A = { fa, ca }, C = { fc, cc }; param_core(b, &r, op, A, C, quoted);
}
void bash_rt_wb_param(WordBuilder *b, const char *name, int op, bash_str_t fa, bash_str_t fc, int quoted) {
    Ref r; ref_from_name(name, &r); Opd A = { fa, NULL }, C = { fc, NULL }; param_core(b, &r, op, A, C, quoted); ref_free(&r);
}

/* ---- compiled associative-array access: h[key] with the key already expanded ---- */
void bash_rt_assoc_id(int id) {   /* declare -A */
    Var *v = vt[id]; if (v->flags & VF_ASC) return;
    if (v->flags & VF_ARR) { bash_rt_warn("%s: cannot convert indexed to associative array", v->name); return; }
    touch(v); const char *old = var_str(v); char *o = old ? xstrdup(old) : NULL;
    var_release(v); v->flags = (v->flags & (VF_EXP | VF_RO | VF_LOCALE)) | VF_ASC; v->as = asc_new();
    if (o) { ai_set(asc_find(v->as, "0", 1), o, 0); free(o); }
}
void bash_rt_aset(int id, const char *key, const char *val, int append) {
    Var *v = vt[id];
    if (v->flags & VF_RO) { bash_rt_warn("%s: readonly variable", v->name); return; }
    if (v->flags & VF_ASC) { asc_set_v(v, key, val, append); return; }
    int e; var_set_elem(v, bash_rt_arith_eval(key, &e), val, append);
}
long bash_rt_ageti(int id, const char *key) {
    Var *v = vt[id];
    if (!(v->flags & VF_ASC)) { int e; return bash_rt_geti_elem(id, bash_rt_arith_eval(key, &e)); }
    AItem *it = asc_find(v->as, key, 0); if (!it || !it->vk) return 0;
    if (it->vk & 4) return it->iv;
    const char *s = it->v; if (!*s) return 0;
    char *e; long x = rt_strtol0(s, &e);
    if (*e == 0) { touch(v); it = asc_find(v->as, key, 0); it->iv = x; it->vk |= 4; return x; }
    return ar_eval_str(s, 1);
}
void bash_rt_aseti(int id, const char *key, long x) {
    Var *v = vt[id];
    if (v->flags & VF_RO) { bash_rt_warn("%s: readonly variable", v->name); return; }
    if (!(v->flags & VF_ASC)) { int e; var_set_elem_int(v, bash_rt_arith_eval(key, &e), x); return; }
    touch(v); ai_seti(asc_find(v->as, key, 1), x); if (!strcmp(key, "0")) v->ver = ++g_ver;
}
/* keys computed inside compiled arithmetic: kept alive in a ring instead of being freed explicitly */
char *bash_rt_keytmp(char *k) { static char *ring[256]; static unsigned ri; unsigned i = ri++ & 255; free(ring[i]); ring[i] = k; return k; }
/* compound assignment with [key]=value elements:  begin, then kv / push per element, then end */
static long arr_cursor; static char *arr_pend; static int arr_ro;
void bash_rt_arr_begin(int id, int append) {
    Var *v = vt[id]; arr_ro = (v->flags & VF_RO) != 0; free(arr_pend); arr_pend = NULL;
    if (arr_ro) { bash_rt_warn("%s: readonly variable", v->name); return; }
    touch(v);
    if (v->flags & VF_ASC) { if (!append) { asc_free(v->as); v->as = asc_new(); } v->ver = ++g_ver; return; }
    if (!append) { var_release(v); v->flags &= (VF_EXP | VF_RO | VF_LOCALE); v->ival = 0; }
    if (!(v->flags & VF_ARR)) to_array(v);
    arr_cursor = 0; for (long i = v->arrn - 1; i >= 0; i--) if (v->ak[i] & AK_SET) { arr_cursor = i + 1; break; }
}
void bash_rt_arr_kv(int id, const char *key, const char *val, int append) {
    if (arr_ro) return;
    Var *v = vt[id];
    if (v->flags & VF_ASC) { ai_set(asc_find(v->as, key, 1), val, append); return; }
    int e; long i = bash_rt_arith_eval(key, &e); if (i < 0) i += v->arrn;
    if (i < 0) { bash_rt_warn("%s: bad array subscript", key); return; }
    var_set_elem(v, i, val, append); arr_cursor = i + 1;
}
void bash_rt_arr_push(int id, int argc, char **argv) {
    if (arr_ro) return;
    Var *v = vt[id];
    for (int k = 0; k < argc; k++) {
        if (v->flags & VF_ASC) { if (!arr_pend) arr_pend = xstrdup(argv[k]); else { ai_set(asc_find(v->as, arr_pend, 1), argv[k], 0); free(arr_pend); arr_pend = NULL; } }
        else var_set_elem(v, arr_cursor++, argv[k], 0);
    }
}
void bash_rt_arr_end(int id) {
    if (arr_pend) { if (!arr_ro && (vt[id]->flags & VF_ASC)) ai_set(asc_find(vt[id]->as, arr_pend, 1), "", 0); free(arr_pend); arr_pend = NULL; }
}
void bash_rt_wb_aparam(WordBuilder *b, int id, const char *key, int op, bash_str_t fa, bash_str_t fc, const char *ca, const char *cc, int quoted) {
    Ref r; ref_from_key(id, key, &r); Opd A = { fa, ca }, C = { fc, cc }; param_core(b, &r, op, A, C, quoted);
}
void bash_rt_make_assoc(const char *name) { bash_rt_assoc_id(name_id(name, 1)); }
int bash_rt_aisset(int id, const char *key) { Ref r; ref_from_key(id, key, &r); return ref_scalar(&r) != NULL; }

void bash_rt_wb_cmdsub(WordBuilder *b, bash_body_t body, int quoted) {
    int st; char *s = capture(body, &st); g_cs_status = st; wb_append(b, s, quoted); free(s);
}
void bash_rt_wb_procsub(WordBuilder *b, bash_body_t body) {
    int fd = mkmem(); bash_rt_redir_push(); redir_save(1); dup2(fd, 1);
    bash_rt_subshell(body); bash_rt_redir_pop(); lseek(fd, 0, SEEK_SET); fcntl(fd, F_SETFD, 0);
    char p[40]; snprintf(p, sizeof p, "/dev/fd/%d", fd); wb_append(b, p, 1);
}
void bash_rt_wb_arith(WordBuilder *b, const char *expr, int quoted) {
    int e; long v = bash_rt_arith_eval(expr, &e); bash_rt_wb_int(b, v, quoted);
}
int bash_rt_wb_argc(WordBuilder *b) { return b->argc; }
char **bash_rt_wb_argv(WordBuilder *b) {
    if (b->argc + 1 > b->capargv) { b->capargv = b->capargv ? b->capargv * 2 : 8; b->argv = xrealloc(b->argv, b->capargv * sizeof(char *)); }
    if (!b->fixed) { for (int i = 0; i < b->argc; i++) b->argv[i] = b->ab + (uintptr_t)b->argv[i]; b->fixed = 1; }
    b->argv[b->argc] = NULL; return b->argv;
}
char *bash_rt_wb_str(WordBuilder *b) { char *s = xstrndup(b->buf ? b->buf : "", b->n); wb_recycle(b); return s; }
void bash_rt_wb_free(WordBuilder *b) { wb_recycle(b); }

int bash_rt_case_match(const char *word, const char *pattern) { return rt_fnm(pattern, word, strlen(word)); }

/* ================================================================ fast helpers */
long bash_rt_arr_len(int id) {
    Var *v = vt[id];
    if (v->flags & VF_ARR) { long n = 0; for (int i = 0; i < v->arrn; i++) n += v->ak[i] & AK_SET; return n; }
    if (v->flags & VF_ASC) return v->as->n;
    return var_isset(v) ? 1 : 0;
}
/* per-variable string info cache, keyed on (id, version, buffer): byte length, char length, ASCII-ness,
 * and the last character->byte position, so ${#s} and ${s:i:1} loops are O(1) per step */
typedef struct { int id; unsigned long ver; const char *s; int ascii; size_t blen; long clen; long coff; size_t boff; } SInfo;
static SInfo sic[8]; static int sici;
static SInfo *sinfo(int id, const char *s) {
    Var *v = vt[id];
    for (int k = 0; k < 8; k++) if (sic[k].id == id && sic[k].ver == v->ver && sic[k].s == s) return &sic[k];
    SInfo *c = &sic[sici++ & 7]; c->id = id; c->ver = v->ver; c->s = s; c->coff = 0; c->boff = 0; c->clen = -1;
    const unsigned char *q = (const unsigned char *)s; int hi = 0;
    while (*q) { hi |= *q; q++; }
    c->blen = (size_t)(q - (const unsigned char *)s); c->ascii = !(hi & 0x80) || !g_utf8;
    if (c->ascii) c->clen = (long)c->blen;
    return c;
}
long bash_rt_strlen_id(int id) {
    const char *s = var_str(vt[id]); if (!s) return 0;
    SInfo *c = sinfo(id, s); if (c->clen < 0) c->clen = (long)u8_len(s);
    return c->clen;
}
long bash_rt_pos_i(int n) {
    const char *s = bash_rt_pos_s(n); if (!*s) return 0;
    char *e; long x = rt_strtol0(s, &e); return *e == 0 ? x : ar_eval_str(s, 1);
}
void bash_rt_wb_substr_id(WordBuilder *b, int id, long off, long len, int haslen, int quoted) {
    const char *s = var_str(vt[id]);
    if (!s) { if (opt_u) { bash_rt_warn("%s: unbound variable", vt[id]->name); bash_rt_exit(1); } s = ""; }
    if (off >= 0 && (!haslen || len >= 0)) {   /* common case: no need to measure the whole string */
        size_t bo, n; SInfo *c = sinfo(id, s);
        if (c->ascii) { if ((size_t)off > c->blen) { wb_append(b, "", quoted); return; } bo = (size_t)off; n = c->blen - bo; if (haslen && (size_t)len < n) n = (size_t)len; }
        else {
            if (off >= c->coff) bo = c->boff + u8_off(s + c->boff, off - c->coff); else bo = u8_off(s, off);
            c->coff = off; c->boff = bo;
            n = haslen ? u8_off(s + bo, len) : c->blen - bo;
        }
        char tmp[256];
        if (n < sizeof tmp) { memcpy(tmp, s + bo, n); tmp[n] = 0; wb_append(b, tmp, quoted); }
        else { char *t = xstrndup(s + bo, n); wb_append(b, t, quoted); free(t); }
        return;
    }
    long L = g_utf8 ? (long)u8_len(s) : (long)strlen(s);
    if (off < 0) off += L; if (off < 0 || off > L) { if (off > L) off = L; else off = L + 1; }
    if (off > L) { wb_append(b, "", quoted); return; }    /* negative offset before the start: empty ("" still counts as a word) */
    long cnt = haslen ? (len < 0 ? L - off + len : len) : L - off;
    if (cnt < 0) { if (haslen && len < 0) { bash_rt_warn("%ld: substring expression < 0", len); } cnt = 0; }
    if (off + cnt > L) cnt = L - off;
    size_t bo = u8_off(s, off), be = bo + u8_off(s + bo, cnt);
    char tmp[256]; size_t n = be - bo;
    if (n < sizeof tmp) { memcpy(tmp, s + bo, n); tmp[n] = 0; wb_append(b, tmp, quoted); }
    else { char *t = xstrndup(s + bo, n); wb_append(b, t, quoted); free(t); }
}
