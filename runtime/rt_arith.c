/* rt_arith.c - run-time arithmetic evaluator (for expressions the compiler could not lower) */
#include "rt.h"

/* ------------------------------------------------------------ arithmetic */
/* dry: a checking pass with no side effects (no assignments, no division messages), so that an expression
 * with a syntax error changes nothing, like in bash */
typedef struct { const char *p; int err; int depth; int dry; } AP;
static long ar_assign(AP *a);
static long ar_comma(AP *a);
static void ar_ws(AP *a) { while (*a->p == ' ' || *a->p == '\t' || *a->p == '\n') a->p++; }
static int ar_isid(int c) { return isalnum(c) || c == '_'; }
static long ar_value_of(const char *name, int depth, int dry) {
    const char *s = bash_rt_getvar(name);
    if (!s || !*s) return 0;
    char *e; long v = strtol(s, &e, 0);
    if (*e == 0) return v;
    if (depth > 8) return 0;
    char *c = xstrdup(s); AP sub = { c, 0, depth + 1, dry }; v = ar_comma(&sub); free(c); return v;
}
static char *ar_ident(AP *a) {
    const char *s = a->p; if (*s == '$') s++;
    int brace = 0; if (*s == '{') { brace = 1; s++; }
    const char *st = s;
    if (isdigit((unsigned char)*s) && a->p[0] == '$') while (isdigit((unsigned char)*s)) s++;
    else if ((*s == '#' || *s == '?' || *s == '@' || *s == '$' || *s == '*' || *s == '!') && (a->p[0] == '$' || brace)) s++;
    else { if (!(isalpha((unsigned char)*s) || *s == '_')) return NULL; while (ar_isid(*s)) s++; }
    if (*s == '[') { int d = 0; do { if (*s == '[') d++; else if (*s == ']') d--; s++; } while (*s && d > 0); }
    char *id = xstrndup(st, s - st);
    if (brace) { if (*s == '}') s++; }
    a->p = s; return id;
}
static long ar_primary(AP *a) {
    ar_ws(a);
    if (*a->p == '(') { a->p++; long v = ar_comma(a); ar_ws(a); if (*a->p == ')') a->p++; else a->err = 1; return v; }
    if (isdigit((unsigned char)*a->p)) {
        char *e; long v;
        const char *hash = a->p; while (isdigit((unsigned char)*hash)) hash++;
        if (*hash == '#') { int base = atoi(a->p); a->p = hash + 1; v = strtol(a->p, &e, base > 1 && base <= 36 ? base : 10); }
        else v = strtol(a->p, &e, 0);
        a->p = e; return v;
    }
    char *id = ar_ident(a);
    if (!id) { a->err = 1; return 0; }
    long v = ar_value_of(id, a->depth, a->dry);
    ar_ws(a);
    if ((a->p[0] == '+' && a->p[1] == '+') || (a->p[0] == '-' && a->p[1] == '-')) {
        long nv = v + (a->p[0] == '+' ? 1 : -1); char b[32]; rt_ltoa(b, nv);
        if (!a->dry) bash_rt_set(id, b, 0);
        a->p += 2;
    }
    free(id); return v;
}
static long ar_unary(AP *a) {
    ar_ws(a);
    char c = *a->p;
    if ((c == '+' && a->p[1] == '+') || (c == '-' && a->p[1] == '-')) {
        a->p += 2; ar_ws(a); char *id = ar_ident(a); if (!id) { a->err = 1; return 0; }
        long v = ar_value_of(id, a->depth, a->dry) + (c == '+' ? 1 : -1); char b[32]; rt_ltoa(b, v);
        if (!a->dry) bash_rt_set(id, b, 0);
        free(id); return v;
    }
    if (c == '!') { a->p++; return !ar_unary(a); }
    if (c == '~') { a->p++; return ~ar_unary(a); }
    if (c == '-') { a->p++; return -ar_unary(a); }
    if (c == '+') { a->p++; return ar_unary(a); }
    return ar_primary(a);
}
static long ar_pow(AP *a) {
    long l = ar_unary(a); ar_ws(a);
    if (a->p[0] == '*' && a->p[1] == '*') { a->p += 2; long r = ar_pow(a); return bash_rt_pow(l, r); }
    return l;
}
static long ar_mul(AP *a) {
    long l = ar_pow(a);
    for (;;) { ar_ws(a); char c = *a->p;
        if ((c == '*' && a->p[1] != '*' && a->p[1] != '=') || ((c == '/' || c == '%') && a->p[1] != '=')) {
            a->p++; long r = ar_pow(a);
            if (c == '*') l *= r;
            else if (r == 0) { if (a->dry) { l = 0; continue; } bash_rt_warn("division by 0"); a->err = 1; return 0; }
            else l = c == '/' ? bash_rt_div(l, r) : bash_rt_mod(l, r);
        } else return l; }
}
static long ar_add(AP *a) {
    long l = ar_mul(a);
    for (;;) { ar_ws(a); char c = *a->p;
        if ((c == '+' || c == '-') && a->p[1] != c && a->p[1] != '=') { a->p++; long r = ar_mul(a); l = c == '+' ? l + r : l - r; } else return l; }
}
static long ar_shift(AP *a) {
    long l = ar_add(a);
    for (;;) { ar_ws(a);
        if (a->p[0] == '<' && a->p[1] == '<' && a->p[2] != '=') { a->p += 2; l <<= (ar_add(a) & 63); }
        else if (a->p[0] == '>' && a->p[1] == '>' && a->p[2] != '=') { a->p += 2; l >>= (ar_add(a) & 63); }
        else return l; }
}
static long ar_rel(AP *a) {
    long l = ar_shift(a);
    for (;;) { ar_ws(a);
        if (a->p[0] == '<' && a->p[1] == '=') { a->p += 2; l = l <= ar_shift(a); }
        else if (a->p[0] == '>' && a->p[1] == '=') { a->p += 2; l = l >= ar_shift(a); }
        else if (a->p[0] == '<' && a->p[1] != '<') { a->p++; l = l < ar_shift(a); }
        else if (a->p[0] == '>' && a->p[1] != '>') { a->p++; l = l > ar_shift(a); }
        else return l; }
}
static long ar_eq(AP *a) {
    long l = ar_rel(a);
    for (;;) { ar_ws(a);
        if (a->p[0] == '=' && a->p[1] == '=') { a->p += 2; l = l == ar_rel(a); }
        else if (a->p[0] == '!' && a->p[1] == '=') { a->p += 2; l = l != ar_rel(a); }
        else return l; }
}
static long ar_band(AP *a) { long l = ar_eq(a); for (;;) { ar_ws(a); if (a->p[0] == '&' && a->p[1] != '&' && a->p[1] != '=') { a->p++; l &= ar_eq(a); } else return l; } }
static long ar_bxor(AP *a) { long l = ar_band(a); for (;;) { ar_ws(a); if (a->p[0] == '^' && a->p[1] != '=') { a->p++; l ^= ar_band(a); } else return l; } }
static long ar_bor(AP *a) { long l = ar_bxor(a); for (;;) { ar_ws(a); if (a->p[0] == '|' && a->p[1] != '|' && a->p[1] != '=') { a->p++; l |= ar_bxor(a); } else return l; } }
static long ar_land(AP *a) { long l = ar_bor(a); for (;;) { ar_ws(a); if (a->p[0] == '&' && a->p[1] == '&') { a->p += 2; long r = ar_bor(a); l = l && r; } else return l; } }
static long ar_lor(AP *a) { long l = ar_land(a); for (;;) { ar_ws(a); if (a->p[0] == '|' && a->p[1] == '|') { a->p += 2; long r = ar_land(a); l = l || r; } else return l; } }
static long ar_ternary(AP *a) {
    long c = ar_lor(a); ar_ws(a);
    if (*a->p == '?') { a->p++; long x = ar_assign(a); ar_ws(a); if (*a->p == ':') a->p++; else a->err = 1; long y = ar_ternary(a); return c ? x : y; }
    return c;
}
static long ar_assign(AP *a) {
    ar_ws(a);
    const char *save = a->p; char *id = ar_ident(a);
    if (id) {
        ar_ws(a);
        const char *p = a->p; char op = 0; int len = 0;
        if (p[0] == '=' && p[1] != '=') { op = '='; len = 1; }
        else if (strchr("+-*/%&|^", p[0]) && p[1] == '=') { op = p[0]; len = 2; }
        else if ((p[0] == '<' && p[1] == '<' && p[2] == '=') || (p[0] == '>' && p[1] == '>' && p[2] == '=')) { op = p[0] == '<' ? 'L' : 'R'; len = 3; }
        if (op && save[0] != '$') {
            a->p += len; long r = ar_assign(a), v = r;
            if (op != '=') { long o = ar_value_of(id, a->depth, a->dry);
                switch (op) { case '+': v = o + r; break; case '-': v = o - r; break; case '*': v = o * r; break;
                    case '/': v = a->dry && !r ? 0 : bash_rt_div(o, r); break; case '%': v = a->dry && !r ? 0 : bash_rt_mod(o, r); break;
                    case '&': v = o & r; break; case '|': v = o | r; break; case '^': v = o ^ r; break;
                    case 'L': v = o << (r & 63); break; case 'R': v = o >> (r & 63); break; } }
            char b[32]; rt_ltoa(b, v); if (!a->dry) bash_rt_set(id, b, 0);
            free(id); return v;
        }
        free(id);
    }
    a->p = save; return ar_ternary(a);
}
static long ar_comma(AP *a) { long v = ar_assign(a); for (;;) { ar_ws(a); if (*a->p != ',') return v; a->p++; v = ar_assign(a); } }   /* a, b: value of b */
long ar_eval_str(const char *s, int depth) {
    char *c = xstrdup(s); AP a = { c, 0, depth, 0 }; long v = ar_comma(&a); free(c); return v;
}
/* expansion context ($((...)), subscripts, ${s:off:len}): a syntax error ends the script (or the current
 * subshell) with status 1, like non-interactive bash */
long bash_rt_arith_eval(const char *expr, int *err) {
    int e; long v = bash_rt_arith_eval_nf(expr, &e);
    if (e) bash_rt_expansion_error();
    if (err) *err = 0; return v;
}
/* command context ((( )), let, [[ -eq ]]): an error only makes the command fail */
long bash_rt_arith_eval_nf(const char *expr, int *err) {
    AP d = { expr, 0, 0, 1 }; ar_comma(&d); ar_ws(&d);   /* check first: an error must change nothing */
    if (*d.p || d.err) { if (err) *err = 1; bash_rt_warn("%s: arithmetic syntax error", expr); return 0; }
    AP a = { expr, 0, 0, 0 }; long v = ar_comma(&a); ar_ws(&a);
    if (*a.p) a.err = 1;
    if (a.err) { if (err) *err = 1; return 0; }   /* (division by 0, already reported) */
    if (err) *err = 0; return v;
}
long bash_rt_arith_str(const char *expr) { int e; return bash_rt_arith_eval(expr, &e); }
int bash_rt_arith(const char *expr) { int e; long v = bash_rt_arith_eval_nf(expr, &e); return e ? 1 : (v != 0 ? 0 : 1); }
long bash_rt_div(long a, long b) { if (b == 0) { bash_rt_warn("division by 0"); return 0; } if (b == -1) return (long)(0UL - (unsigned long)a); return a / b; }
long bash_rt_mod(long a, long b) { if (b == 0) { bash_rt_warn("division by 0"); return 0; } if (b == -1) return 0; return a % b; }
long bash_rt_pow(long a, long b) { if (b < 0) return 0; long v = 1; while (b-- > 0) v = (long)((unsigned long)v * (unsigned long)a); return v; }
