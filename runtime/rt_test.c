/* rt_test.c - test / [ / [[ evaluation */
#include "rt.h"

/* ------------------------------------------------------------- tests */
void bash_rt_test_err_clear(void) { test_err = 0; }
int bash_rt_test_err(void) { return test_err; }
int bash_rt_test_status(int ok) { int e = test_err; test_err = 0; return e ? 2 : (ok ? 0 : 1); }
int bash_rt_streq(const char *a, const char *b) { return strcmp(a, b) == 0; }
int bash_rt_fnm(const char *pat, const char *s) { return rt_fnm(pat, s, strlen(s)); }
static int id_rematch = -1;
static int regex_match(regex_t *re, const char *s) {   /* sets BASH_REMATCH like bash */
    regmatch_t m[32]; size_t nm = re->re_nsub + 1; if (nm > 32) nm = 32;
    int ok = regexec(re, s, nm, m, 0) == 0;
    if (id_rematch < 0) id_rematch = name_id("BASH_REMATCH", 1);
    if (!ok) { bash_rt_set_array_id(id_rematch, 0, NULL, 0); return 0; }
    char *v[32];
    for (size_t k = 0; k < nm; k++) v[k] = m[k].rm_so < 0 ? xstrdup("") : xstrndup(s + m[k].rm_so, m[k].rm_eo - m[k].rm_so);
    bash_rt_set_array_id(id_rematch, (int)nm, v, 0);
    for (size_t k = 0; k < nm; k++) free(v[k]);
    return 1;
}
/* compiled regexes, keyed on the pattern text (pattern buffers get reused, so never on the pointer) */
static regex_t *regex_cached(const char *pat) {
    static struct { char *txt; regex_t re; int ok; } cache[16]; static unsigned next;
    for (int i = 0; i < 16; i++) if (cache[i].txt && !strcmp(cache[i].txt, pat)) return cache[i].ok ? &cache[i].re : NULL;
    int i = next++ & 15;
    if (cache[i].txt) { if (cache[i].ok) regfree(&cache[i].re); free(cache[i].txt); }
    cache[i].txt = xstrdup(pat); cache[i].ok = regcomp(&cache[i].re, pat, REG_EXTENDED) == 0;
    return cache[i].ok ? &cache[i].re : NULL;
}
int bash_rt_regex(const char *s, const char *pat) {
    regex_t *re = regex_cached(pat);
    if (!re) { test_err = 1; return 0; }
    return regex_match(re, s);
}
int bash_rt_ftest(int op, const char *s) {
    struct stat st;
    switch (op) {
    case 'e': case 'a': return stat(s, &st) == 0;
    case 'f': return stat(s, &st) == 0 && S_ISREG(st.st_mode);
    case 'd': return stat(s, &st) == 0 && S_ISDIR(st.st_mode);
    case 'L': case 'h': return lstat(s, &st) == 0 && S_ISLNK(st.st_mode);
    case 'p': return stat(s, &st) == 0 && S_ISFIFO(st.st_mode);
    case 'S': return stat(s, &st) == 0 && S_ISSOCK(st.st_mode);
    case 'b': return stat(s, &st) == 0 && S_ISBLK(st.st_mode);
    case 'c': return stat(s, &st) == 0 && S_ISCHR(st.st_mode);
    case 's': return stat(s, &st) == 0 && st.st_size > 0;
    case 'r': return access(s, R_OK) == 0; case 'w': return access(s, W_OK) == 0; case 'x': return access(s, X_OK) == 0;
    case 'u': return stat(s, &st) == 0 && (st.st_mode & S_ISUID);
    case 'g': return stat(s, &st) == 0 && (st.st_mode & S_ISGID);
    case 't': return isatty(atoi(s));
    case 'v': return bash_rt_getvar(s) != NULL;
    }
    return 0;
}
typedef struct { char **a; int n, i, dbl; } TS;
static const char *unesc(const char *s) {
    static char *buf; static size_t cap; size_t L = strlen(s) + 1;
    if (L > cap) { cap = L * 2; buf = xrealloc(buf, cap); }
    char *o = buf; for (; *s; s++) { if (*s == '\\' && s[1]) s++; *o++ = *s; } *o = 0; return buf;
}
static char *sunesc(TS *t, const char *s) { return xstrdup(t->dbl ? unesc(s) : s); }
static int is_binop(TS *t, const char *s) {
    static const char *ops[] = { "=", "==", "!=", "-eq", "-ne", "-lt", "-le", "-gt", "-ge", "<", ">", "-nt", "-ot", "-ef", NULL };
    for (int i = 0; ops[i]; i++) if (!strcmp(s, ops[i])) return 1;
    return t->dbl && !strcmp(s, "=~");
}
static int is_unop(const char *s) { return s[0] == '-' && s[1] && !s[2] && strchr("zneEfdrwxsLhpSbcgukOGNtv", s[1]); }
static int ts_or(TS *t);
static long to_num(TS *t, const char *s) {
    if (t->dbl) { int e; return bash_rt_arith_eval_nf(unesc(s), &e); }
    char *e; long v = strtol(s, &e, 10); if (*e || !*s) { bash_rt_warn("[: %s: integer expression expected", s); test_err = 1; return 0; } return v;
}
static int ts_prim(TS *t) {
    if (t->i >= t->n) return -1;
    int rem = t->n - t->i;
    if (!strcmp(t->a[t->i], "(") && rem >= 2) { t->i++; int v = ts_or(t); if (t->i < t->n && !strcmp(t->a[t->i], ")")) t->i++; return v; }
    if (rem >= 3 && is_binop(t, t->a[t->i + 1])) {
        char *l = sunesc(t, t->a[t->i]); const char *op = t->a[t->i + 1]; const char *rr = t->a[t->i + 2]; t->i += 3; int res = 0;
        if (!strcmp(op, "=") || !strcmp(op, "==")) res = t->dbl ? rt_fnm(rr, l, strlen(l)) : !strcmp(l, rr);
        else if (!strcmp(op, "!=")) res = t->dbl ? !rt_fnm(rr, l, strlen(l)) : strcmp(l, rr) != 0;
        else if (!strcmp(op, "=~")) { regex_t *re = regex_cached(t->dbl ? rr : unesc(rr));   /* [[ ]] passes the regex ready-made (WB_REGEX) */ if (re) res = regex_match(re, l); else { res = 0; test_err = 1; } }
        else if (!strcmp(op, "<")) res = (t->dbl ? strcoll(l, unesc(rr)) : strcmp(l, rr)) < 0;
        else if (!strcmp(op, ">")) res = (t->dbl ? strcoll(l, unesc(rr)) : strcmp(l, rr)) > 0;
        else if (!strcmp(op, "-nt") || !strcmp(op, "-ot") || !strcmp(op, "-ef")) {
            struct stat a, b; int ea = stat(l, &a), eb = stat(t->dbl ? unesc(rr) : rr, &b);
            /* modification times to the nanosecond, like bash */
            int cmp = a.st_mtim.tv_sec != b.st_mtim.tv_sec ? (a.st_mtim.tv_sec < b.st_mtim.tv_sec ? -1 : 1)
                    : a.st_mtim.tv_nsec != b.st_mtim.tv_nsec ? (a.st_mtim.tv_nsec < b.st_mtim.tv_nsec ? -1 : 1) : 0;
            if (!strcmp(op, "-nt")) res = ea == 0 && (eb != 0 || cmp > 0);
            else if (!strcmp(op, "-ot")) res = eb == 0 && (ea != 0 || cmp < 0);
            else res = ea == 0 && eb == 0 && a.st_ino == b.st_ino && a.st_dev == b.st_dev;
        } else {
            long x = to_num(t, l), y = to_num(t, rr);
            res = !strcmp(op, "-eq") ? x == y : !strcmp(op, "-ne") ? x != y : !strcmp(op, "-lt") ? x < y : !strcmp(op, "-le") ? x <= y : !strcmp(op, "-gt") ? x > y : x >= y;
        }
        free(l); return res;
    }
    if (rem >= 2 && is_unop(t->a[t->i])) {
        char op = t->a[t->i][1]; char *s = sunesc(t, t->a[t->i + 1]); t->i += 2; int res;
        if (op == 'z') res = !*s; else if (op == 'n') res = *s != 0; else res = bash_rt_ftest(op, s);
        free(s); return res;
    }
    char *s = sunesc(t, t->a[t->i++]); int res = *s != 0; free(s); return res;
}
static int ts_not(TS *t) { if (t->i < t->n && !strcmp(t->a[t->i], "!") && t->n - t->i >= 2) { t->i++; int v = ts_not(t); return v < 0 ? v : !v; } return ts_prim(t); }
static int ts_and(TS *t) {
    int v = ts_not(t);
    while (v >= 0 && t->i < t->n && !strcmp(t->a[t->i], t->dbl ? "&&" : "-a")) { t->i++; int r = ts_not(t); if (r < 0) return r; v = v && r; }
    return v;
}
static int ts_or(TS *t) {
    int v = ts_and(t);
    while (v >= 0 && t->i < t->n && !strcmp(t->a[t->i], t->dbl ? "||" : "-o")) { t->i++; int r = ts_and(t); if (r < 0) return r; v = v || r; }
    return v;
}
char *bash_rt_pat_quote(char *s) {   /* escape \ * ? [ ] so that unescaping gives s back; frees s */
    size_t k = 0; for (const char *p = s; *p; p++) k += strchr("\\*?[]", *p) != NULL;
    if (!k) return s;
    char *o = xmalloc(strlen(s) + k + 1), *w = o;
    for (const char *p = s; *p; p++) { if (strchr("\\*?[]", *p)) *w++ = '\\'; *w++ = *p; }
    *w = 0; free(s); return o;
}
int bash_rt_test(int n, char **a, int dbl) {
    if (n == 0) return 1;
    TS t = { a, n, 0, dbl }; test_err = 0; int v = ts_or(&t);
    if (test_err) { test_err = 0; return 2; }
    if (v < 0 || t.i != t.n) { bash_rt_warn("test: syntax error"); return 2; }
    return v ? 0 : 1;
}
