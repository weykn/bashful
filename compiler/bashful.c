/* bashful.c - compile a bash script (a practical subset) to LLVM IR.
 *
 * Output IR calls the embedded runtime (runtime/bash_runtime.c, runtime/tools.c).  Known tools are called
 * directly as  @bash_external_<tool> / @bash_internal_<builtin>(i32 argc, ptr argv)  -- no process is ever spawned.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <setjmp.h>
#include <stdint.h>
#include "bash_runtime.h"   /* enums shared with the runtime: WB_*, LIT_*, R_*, P_* */

/* ================================================================ utils */
static void die(const char *fmt, ...) { va_list ap; va_start(ap, fmt); fprintf(stderr, "bashful: "); vfprintf(stderr, fmt, ap); fprintf(stderr, "\n"); va_end(ap); exit(1); }
/* ---- whole-script fallback: anything this compiler cannot translate exactly runs on an embedded bash ---- */
static jmp_buf g_fb_jb; static int g_fb_armed; static char g_unsup[512];
static void unsupported(const char *fmt, ...) {
    if (g_unsup[0]) return;
    va_list ap; va_start(ap, fmt); vsnprintf(g_unsup, sizeof g_unsup, fmt, ap); va_end(ap);
}
static void *xmalloc(size_t n) { void *p = calloc(1, n ? n : 1); if (!p) die("out of memory"); return p; }
static void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) die("out of memory"); return p; }
static char *xstrndup(const char *s, size_t n) { char *p = xmalloc(n + 1); memcpy(p, s, n); return p; }
static char *xstrdup(const char *s) { return xstrndup(s, strlen(s)); }
#define PUSH(a, n, x) do { (a) = xrealloc((a), sizeof(*(a)) * ((n) + 1)); (a)[(n)++] = (x); } while (0)

typedef struct { char *s; size_t n, cap; } SB;
static void sb_put(SB *b, const char *s, size_t n) { if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->s = xrealloc(b->s, b->cap); } memcpy(b->s + b->n, s, n); b->n += n; b->s[b->n] = 0; }
static void sb_puts(SB *b, const char *s) { sb_put(b, s, strlen(s)); }
static void sb_putc(SB *b, char c) { sb_put(b, &c, 1); }
static void sb_vprintf(SB *b, const char *fmt, va_list ap) {
    va_list c; va_copy(c, ap); int n = vsnprintf(NULL, 0, fmt, c); va_end(c);
    char *t = xmalloc(n + 1); vsnprintf(t, n + 1, fmt, ap); sb_put(b, t, n); free(t);
}
static void sb_printf(SB *b, const char *fmt, ...) { va_list ap; va_start(ap, fmt); sb_vprintf(b, fmt, ap); va_end(ap); }

/* tools known to the runtime (single source of truth: runtime/tools.def) */
static const struct { const char *id, *name, *prefix; } TOOLS[] = {
#define INTERNAL(id, name) { #id, name, "bash_internal_" },
#define EXTERNAL(id, name) { #id, name, "bash_external_" },
#include "tools.def"
#undef INTERNAL
#undef EXTERNAL
    { NULL, NULL, NULL } };
static int tool_index(const char *name) { for (int i = 0; TOOLS[i].name; i++) if (!strcmp(TOOLS[i].name, name)) return i; return -1; }
static char used_tool[256];
static int in_list(const char **l, const char *s) { for (int i = 0; l[i]; i++) if (!strcmp(l[i], s)) return 1; return 0; }

/* ================================================================== AST */
enum { PK_LIT, PK_VAR, PK_PARAM, PK_CMD, PK_ARITH, PK_PROC };
typedef struct Node Node;
typedef struct Word Word;
typedef struct { int kind; char *text; int quoted; int op; Word *a, *c; Node *body; } Part;
struct Word { Part *p; int n; };
typedef struct { int fd, op; Word *target; } Redir;
typedef struct { char *name; int append; Word *val; Word **elems; int nelems; int isarr; } Assign;
typedef struct { Word **pats; int np; Node *body; } CaseItem;
enum { N_SIMPLE, N_PIPE, N_AND, N_OR, N_NOT, N_LIST, N_IF, N_WHILE, N_UNTIL, N_FOR, N_CFOR, N_CASE, N_SUB, N_GROUP, N_FUNC, N_DBL, N_ARITH, N_BG };
struct Node {
    int type; Node **k; int nk; Word **w; int nw; Assign *as; int nas; Assign *arr; int narr;
    Redir *r; int nr; char *name; char *s1, *s2, *s3; CaseItem *ci; int nci; int flag; Word *aw[3];
};
static Node *mk(int t) { Node *n = xmalloc(sizeof *n); n->type = t; return n; }

/* ================================================================ parser */
typedef struct { Word *body; char *delim; int strip, quoted; } HD;
typedef struct { const char *s; size_t i; HD *pend; int npend; } Parser;
static const char *WSTOP = " \t\n|&;()<>";

static void perr(Parser *P, const char *msg) {
    int line = 1; for (size_t k = 0; k < P->i && P->s[k]; k++) if (P->s[k] == '\n') line++;
    if (g_fb_armed) { unsupported("line %d: %s", line, msg); longjmp(g_fb_jb, 1); }
    die("syntax error near line %d: %s", line, msg);
}
static int is_delim(char c) { return c == 0 || strchr(" \t\n|&;()<>", c) != NULL; }
static void flush_heredocs(Parser *P);
static void consume_newline(Parser *P) { P->i++; if (P->npend) flush_heredocs(P); }
static void skip_ws(Parser *P, int nl) {
    for (;;) {
        char c = P->s[P->i];
        if (c == ' ' || c == '\t') P->i++;
        else if (c == '\\' && P->s[P->i + 1] == '\n') P->i += 2;
        else if (c == '#') { while (P->s[P->i] && P->s[P->i] != '\n') P->i++; }
        else if (c == '\n' && nl) consume_newline(P);
        else return;
    }
}
static int at_kw(Parser *P, const char *kw) { size_t l = strlen(kw); return !strncmp(P->s + P->i, kw, l) && is_delim(P->s[P->i + l]); }
static int at_closer(Parser *P) {
    char c = P->s[P->i];
    if (c == ')' || (c == ';' && P->s[P->i + 1] == ';')) return 1;
    static const char *kws[] = { "then", "else", "elif", "fi", "do", "done", "esac", "}", NULL };
    for (int i = 0; kws[i]; i++) if (at_kw(P, kws[i])) return 1;
    return 0;
}

static Word *mkword(void) { return xmalloc(sizeof(Word)); }
static void add_part(Word *w, Part p) { PUSH(w->p, w->n, p); }
static void add_lit(Word *w, const char *t, size_t n, int quoted, int force) {
    if (w->n && w->p[w->n - 1].kind == PK_LIT && w->p[w->n - 1].quoted == quoted) {
        Part *l = &w->p[w->n - 1]; size_t o = strlen(l->text); l->text = xrealloc(l->text, o + n + 1); memcpy(l->text + o, t, n); l->text[o + n] = 0; return;
    }
    if (!n && !force) return;
    Part p = { 0 }; p.kind = PK_LIT; p.text = xstrndup(t, n); p.quoted = quoted; add_part(w, p);
}
static Word *parse_word(Parser *P, const char *stops);
static void scan_parts(Parser *P, Word *w, const char *stops, int dq);
static Node *parse_list(Parser *P);

static void flush_heredocs(Parser *P) {
    for (int h = 0; h < P->npend; h++) {
        HD *hd = &P->pend[h]; SB body = { 0 };
        while (P->s[P->i]) {
            size_t e = P->i; while (P->s[e] && P->s[e] != '\n') e++;
            const char *ls = P->s + P->i; size_t ll = e - P->i;
            if (hd->strip) while (ll && *ls == '\t') { ls++; ll--; }
            int end = ll == strlen(hd->delim) && !strncmp(ls, hd->delim, ll);
            P->i = P->s[e] ? e + 1 : e;
            if (end) break;
            sb_put(&body, ls, ll); sb_putc(&body, '\n');
        }
        char *txt = body.s ? body.s : xstrdup("");
        if (hd->quoted) add_lit(hd->body, txt, strlen(txt), 1, 1);
        else { Parser sub = { txt, 0, NULL, 0 }; scan_parts(&sub, hd->body, "", 2); }
    }
    free(P->pend); P->pend = NULL; P->npend = 0;
}

static void scan_dollar(Parser *P, Word *w, int quoted);
static void scan_backtick(Parser *P, Word *w, int quoted) {
    size_t i = P->i + 1; SB t = { 0 };
    while (P->s[i] && P->s[i] != '`') {
        if (P->s[i] == '\\' && strchr("`\\$", P->s[i + 1]) && P->s[i + 1]) i++;
        sb_putc(&t, P->s[i++]);
    }
    if (!P->s[i]) perr(P, "unterminated backtick");
    P->i = i + 1;
    Parser sub = { t.s ? t.s : xstrdup(""), 0, NULL, 0 };
    Part p = { 0 }; p.kind = PK_CMD; p.quoted = quoted; p.body = parse_list(&sub); add_part(w, p);
}
static void scan_parts(Parser *P, Word *w, const char *stops, int dq) {
    const char *s = P->s;
    for (;;) {
        char c = s[P->i];
        if (!c) return;
        if (dq == 0 && strchr(stops, c) && !(c == '<' && s[P->i + 1] == '(')) return;
        if (dq == 1 && c == '"') return;
        if (c == '\\') {
            char n = s[P->i + 1];
            if (dq == 0) { if (n == '\n') P->i += 2; else if (!n) { add_lit(w, "\\", 1, 1, 0); P->i++; } else { add_lit(w, &n, 1, 1, 0); P->i += 2; } }
            else if (n && (strchr("$`\\", n) || (dq == 1 && n == '"') || n == '\n')) { if (n != '\n') add_lit(w, &n, 1, 1, 0); P->i += 2; }
            else { add_lit(w, "\\", 1, 1, 0); P->i++; }
        } else if (dq == 0 && c == '\'') {
            size_t e = P->i + 1; while (s[e] && s[e] != '\'') e++;
            if (!s[e]) perr(P, "unterminated single quote");
            add_lit(w, s + P->i + 1, e - P->i - 1, 1, 1); P->i = e + 1;
        } else if (dq == 0 && c == '"') {
            P->i++; size_t qs = P->i;
            scan_parts(P, w, "", 1);
            if (P->i == qs) add_lit(w, "", 0, 1, 1);   /* "" is an empty word; "$@" is not */
            if (s[P->i] != '"') perr(P, "unterminated double quote");
            P->i++;
        } else if (c == '$') scan_dollar(P, w, dq != 0);
        else if (c == '`') scan_backtick(P, w, dq != 0);
        else if (dq == 0 && c == '<' && s[P->i + 1] == '(') {
            Parser sub = { s, P->i + 2, NULL, 0 }; Part p = { 0 }; p.kind = PK_PROC; p.body = parse_list(&sub);
            skip_ws(&sub, 1); if (s[sub.i] != ')') perr(&sub, "expected ) after <("); P->i = sub.i + 1; add_part(w, p);
        } else if (dq == 0 && c == '~' && w->n == 0 && s[P->i + 1] && (isalnum((unsigned char)s[P->i + 1]) || s[P->i + 1] == '+' || s[P->i + 1] == '-' || s[P->i + 1] == '_')) {
            unsupported("~user / ~+ / ~- tilde expansion"); add_lit(w, &c, 1, 0, 0); P->i++;
        } else if (dq == 0 && c == '~' && w->n == 0 && (!s[P->i + 1] || s[P->i + 1] == '/' || strchr(stops, s[P->i + 1]))) {
            Part p = { 0 }; p.kind = PK_VAR; p.text = xstrdup("HOME"); p.quoted = 1; add_part(w, p); P->i++;
        } else { add_lit(w, &c, 1, dq != 0, 0); P->i++; }
    }
}
static void ansi_c(Parser *P, Word *w) {   /* at $' */
    size_t i = P->i + 2; SB t = { 0 };
    while (P->s[i] && P->s[i] != '\'') {
        char c = P->s[i++];
        if (c != '\\' || !P->s[i]) { sb_putc(&t, c); continue; }
        c = P->s[i++];
        switch (c) {
        case 'n': sb_putc(&t, '\n'); break; case 't': sb_putc(&t, '\t'); break; case 'r': sb_putc(&t, '\r'); break;
        case 'a': sb_putc(&t, 7); break; case 'b': sb_putc(&t, 8); break; case 'e': case 'E': sb_putc(&t, 27); break;
        case 'f': sb_putc(&t, 12); break; case 'v': sb_putc(&t, 11); break;
        case 'x': { int v = 0, k = 0; while (k < 2 && isxdigit((unsigned char)P->s[i])) { v = v * 16 + (isdigit((unsigned char)P->s[i]) ? P->s[i] - '0' : (tolower(P->s[i]) - 'a' + 10)); i++; k++; } sb_putc(&t, (char)v); break; }
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': { int v = c - '0', k = 1; while (k < 3 && P->s[i] >= '0' && P->s[i] <= '7') { v = v * 8 + P->s[i++] - '0'; k++; } sb_putc(&t, (char)v); break; }
        default: sb_putc(&t, c);
        }
    }
    P->i = P->s[i] ? i + 1 : i; add_lit(w, t.s ? t.s : "", t.n, 1, 1);
}
static void scan_param(Parser *P, Word *w, int quoted) {
    const char *s = P->s; size_t i = P->i + 2; int len = 0, ind = 0;
    if (s[i] == '#' && s[i + 1] && s[i + 1] != '}') { len = 1; i++; }
    else if (s[i] == '!' && s[i + 1] && s[i + 1] != '}') { ind = 1; i++; }
    size_t st = i;
    if (isalpha((unsigned char)s[i]) || s[i] == '_') { while (isalnum((unsigned char)s[i]) || s[i] == '_') i++; }
    else if (isdigit((unsigned char)s[i])) { while (isdigit((unsigned char)s[i])) i++; }
    else if (s[i] && strchr("@*#?$!-", s[i])) i++;
    else perr(P, "bad substitution");
    if (s[i] == '[') { int d = 0; do { if (s[i] == '[') d++; else if (s[i] == ']') d--; i++; } while (s[i] && d > 0); }
    Part p = { 0 }; p.kind = PK_PARAM; p.quoted = quoted; p.text = xstrndup(s + st, i - st); p.op = P_PLAIN;
    int subst = 0, hasop = 1;
    if (s[i] == '}') hasop = 0;
    else if (s[i] == ':') {
        char c2 = s[i + 1];
        if (c2 == '-') { p.op = P_DEFAULT; i += 2; } else if (c2 == '=') { p.op = P_ASSIGN; i += 2; }
        else if (c2 == '+') { p.op = P_ALT; i += 2; } else if (c2 == '?') { p.op = P_ERR; i += 2; }
        else { p.op = P_SUBSTR; i++; }
    }
    else if (s[i] == '-') { p.op = P_DEFAULT_NC; i++; } else if (s[i] == '=') { p.op = P_ASSIGN_NC; i++; }
    else if (s[i] == '+') { p.op = P_ALT_NC; i++; } else if (s[i] == '?') { p.op = P_ERR_NC; i++; }
    else if (s[i] == '#') { if (s[i + 1] == '#') { p.op = P_TRIM_L; i += 2; } else { p.op = P_TRIM_S; i++; } }
    else if (s[i] == '%') { if (s[i + 1] == '%') { p.op = P_TRIM_LE; i += 2; } else { p.op = P_TRIM_SE; i++; } }
    else if (s[i] == '/') {
        subst = 1;
        if (s[i + 1] == '/') { p.op = P_SUBST_ALL; i += 2; } else if (s[i + 1] == '#') { p.op = P_SUBST_PRE; i += 2; }
        else if (s[i + 1] == '%') { p.op = P_SUBST_SUF; i += 2; } else { p.op = P_SUBST; i++; }
    }
    else if (s[i] == '^') { if (s[i + 1] == '^') { p.op = P_UPALL; i += 2; } else { p.op = P_UP1; i++; } }
    else if (s[i] == ',') { if (s[i + 1] == ',') { p.op = P_LOWALL; i += 2; } else { p.op = P_LOW1; i++; } }
    else perr(P, "bad parameter expansion");
    if (hasop) {
        P->i = i; p.a = mkword(); scan_parts(P, p.a, subst ? "/}" : "}", 0);
        if (subst && s[P->i] == '/') { P->i++; p.c = mkword(); scan_parts(P, p.c, "}", 0); }
        i = P->i;
    }
    if (s[i] != '}') perr(P, "missing } in parameter expansion");
    P->i = i + 1;
    if (len) p.op = P_LEN;
    else if (ind && p.op == P_PLAIN) p.op = (strstr(p.text, "[@]") || strstr(p.text, "[*]")) ? P_KEYS : P_INDIRECT;
    add_part(w, p);
}
static void scan_dollar(Parser *P, Word *w, int quoted) {
    const char *s = P->s; char n = s[P->i + 1];
    if (n == '(' && s[P->i + 2] == '(') {
        size_t j = P->i + 3; int d = 0;
        for (; s[j]; j++) { if (s[j] == '(') d++; else if (s[j] == ')') { if (d == 0 && s[j + 1] == ')') break; d--; } }
        if (!s[j]) perr(P, "unterminated $((");
        Part p = { 0 }; p.kind = PK_ARITH; p.quoted = quoted; p.text = xstrndup(s + P->i + 3, j - P->i - 3);
        if (strpbrk(p.text, "$`")) { Parser sub = { p.text, 0, NULL, 0 }; p.a = mkword(); scan_parts(&sub, p.a, "", 2); }   /* expand ${#x}, $(cmd), ... first */
        add_part(w, p); P->i = j + 2;
    } else if (n == '(') {
        Parser sub = { s, P->i + 2, NULL, 0 }; Part p = { 0 }; p.kind = PK_CMD; p.quoted = quoted; p.body = parse_list(&sub);
        skip_ws(&sub, 1); if (s[sub.i] != ')') perr(&sub, "expected ) after $("); P->i = sub.i + 1; add_part(w, p);
    } else if (n == '[') { unsupported("$[...] arithmetic"); add_lit(w, "$", 1, quoted, 0); P->i++; }
    else if (n == '{') scan_param(P, w, quoted);
    else if (!quoted && n == '\'') ansi_c(P, w);
    else if (isalpha((unsigned char)n) || n == '_') {
        size_t e = P->i + 1; while (isalnum((unsigned char)s[e]) || s[e] == '_') e++;
        Part p = { 0 }; p.kind = PK_VAR; p.quoted = quoted; p.text = xstrndup(s + P->i + 1, e - P->i - 1); add_part(w, p); P->i = e;
    } else if (isdigit((unsigned char)n) || (n && strchr("@*#?$!-", n))) {
        Part p = { 0 }; p.kind = PK_VAR; p.quoted = quoted; p.text = xstrndup(&n, 1); add_part(w, p); P->i += 2;
    } else { add_lit(w, "$", 1, quoted, 0); P->i++; }
}
static Word *parse_word(Parser *P, const char *stops) {
    Word *w = mkword(); size_t st = P->i; scan_parts(P, w, stops, 0);
    if (P->i == st) return NULL; return w;
}

/* ---- brace expansion (literal, unquoted parts only) ---- */
static int make_seq(const char *in, size_t len, char ***alts, int *na) {
    char *b = xstrndup(in, len); char *d1 = strstr(b, ".."); if (!d1) return 0;
    *d1 = 0; char *t2 = d1 + 2; char *d2 = strstr(t2, ".."); long step = 1;
    if (d2) { *d2 = 0; char *e; step = strtol(d2 + 2, &e, 10); if (*e || step == 0) return 0; if (step < 0) step = -step; }
    char *e1, *e2; long a = strtol(b, &e1, 10), z = strtol(t2, &e2, 10);
    if (*b && !*e1 && *t2 && !*e2) {
        int pad = 0; if ((strlen(b) > 1 && (b[0] == '0' || (b[0] == '-' && b[1] == '0'))) || (strlen(t2) > 1 && (t2[0] == '0' || (t2[0] == '-' && t2[1] == '0')))) pad = (int)(strlen(b) > strlen(t2) ? strlen(b) : strlen(t2));
        for (long v = a; a <= z ? v <= z : v >= z; v += (a <= z ? step : -step)) { char t[64]; snprintf(t, sizeof t, "%0*ld", pad, v); PUSH(*alts, *na, xstrdup(t)); }
        return 1;
    }
    if (strlen(b) == 1 && strlen(t2) == 1 && isalpha((unsigned char)b[0]) && isalpha((unsigned char)t2[0])) {
        for (int v = b[0]; b[0] <= t2[0] ? v <= t2[0] : v >= t2[0]; v += (b[0] <= t2[0] ? step : -step)) { char t[2] = { (char)v, 0 }; PUSH(*alts, *na, xstrdup(t)); }
        return 1;
    }
    return 0;
}
static void brace_expand(Word *w, Word ***out, int *n) {
    for (int k = 0; k < w->n; k++) {
        Part *p = &w->p[k]; if (p->kind != PK_LIT || p->quoted) continue;
        const char *t = p->text; size_t L = strlen(t);
        for (size_t a = 0; a < L; a++) {
            if (t[a] != '{') continue;
            int d = 0; size_t b = a;
            for (; b < L; b++) { if (t[b] == '{') d++; else if (t[b] == '}' && --d == 0) break; }
            if (b >= L) continue;
            char **alts = NULL; int na = 0; size_t st = a + 1; d = 0;
            for (size_t c = a + 1; c <= b; c++) {
                if (c == b || (t[c] == ',' && d == 0)) { PUSH(alts, na, xstrndup(t + st, c - st)); st = c + 1; }
                else if (t[c] == '{') d++; else if (t[c] == '}') d--;
            }
            if (na <= 1) { na = 0; alts = NULL; if (!make_seq(t + a + 1, b - a - 1, &alts, &na)) continue; }
            for (int i = 0; i < na; i++) {
                Word *nw = mkword();
                for (int j = 0; j < w->n; j++) {
                    if (j != k) { Part cp = w->p[j]; PUSH(nw->p, nw->n, cp); continue; }
                    SB sb = { 0 }; sb_put(&sb, t, a); sb_puts(&sb, alts[i]); sb_puts(&sb, t + b + 1);
                    Part cp = { 0 }; cp.kind = PK_LIT; cp.text = sb.s ? sb.s : xstrdup(""); PUSH(nw->p, nw->n, cp);
                }
                brace_expand(nw, out, n);
            }
            return;
        }
    }
    PUSH(*out, *n, w);
}
static void push_words(Node *nd, Word *w) { Word **ex = NULL; int n = 0; brace_expand(w, &ex, &n); for (int i = 0; i < n; i++) PUSH(nd->w, nd->nw, ex[i]); }

/* ---- redirections ---- */
static int try_redir(Parser *P, Node *nd) {
    const char *s = P->s; size_t j = P->i; int fd = -1;
    while (isdigit((unsigned char)s[j])) j++;
    if (j > P->i && (s[j] == '<' || s[j] == '>')) fd = atoi(s + P->i); else j = P->i;
    int op = -1, adv = 0, strip = 0, dflt = 1;
    if (fd < 0 && s[j] == '&' && s[j + 1] == '>') { if (s[j + 2] == '>') { op = R_APPEND_ERR; adv = 3; } else { op = R_OUT_ERR; adv = 2; } }
    else if (s[j] == '<') {
        dflt = 0;
        if (s[j + 1] == '<' && s[j + 2] == '<') { op = R_HERESTR; adv = 3; }
        else if (s[j + 1] == '<' && s[j + 2] == '-') { op = R_HEREDOC; adv = 3; strip = 1; }
        else if (s[j + 1] == '<') { op = R_HEREDOC; adv = 2; }
        else if (s[j + 1] == '&') { op = R_DUP_IN; adv = 2; }
        else if (s[j + 1] == '>') { op = R_RDWR; adv = 2; }
        else if (s[j + 1] == '(' && fd < 0) return 0;
        else { op = R_IN; adv = 1; }
    } else if (s[j] == '>') {
        if (s[j + 1] == '>') { op = R_APPEND; adv = 2; } else if (s[j + 1] == '&') { op = R_DUP_OUT; adv = 2; }
        else if (s[j + 1] == '|') { op = R_CLOBBER; adv = 2; } else { op = R_OUT; adv = 1; }
    }
    if (op < 0) return 0;
    P->i = j + adv; skip_ws(P, 0);
    Redir r = { fd >= 0 ? fd : (dflt ? 1 : 0), op, NULL };
    if (op == R_HEREDOC) {
        HD hd = { 0 }; hd.strip = strip; hd.body = mkword(); SB d = { 0 };
        while (!is_delim(s[P->i])) {
            char c = s[P->i];
            if (c == '\'' || c == '"') { hd.quoted = 1; char q = c; P->i++; while (s[P->i] && s[P->i] != q) sb_putc(&d, s[P->i++]); if (s[P->i]) P->i++; }
            else if (c == '\\') { hd.quoted = 1; P->i++; if (s[P->i]) sb_putc(&d, s[P->i++]); }
            else { sb_putc(&d, c); P->i++; }
        }
        hd.delim = d.s ? d.s : xstrdup(""); r.target = hd.body; PUSH(P->pend, P->npend, hd);
    } else { r.target = parse_word(P, WSTOP); if (!r.target) perr(P, "missing redirection target"); }
    PUSH(nd->r, nd->nr, r); return 1;
}
static void parse_redirs(Parser *P, Node *nd) { for (;;) { skip_ws(P, 0); if (!try_redir(P, nd)) break; } }

/* ---- commands ---- */
static Node *parse_command(Parser *P);
static int is_ident_start(char c) { return isalpha((unsigned char)c) || c == '_'; }
static int parse_assign_word(Parser *P, Word *w, Assign *a, int declare_arg) {
    if (!w->n || w->p[0].kind != PK_LIT || w->p[0].quoted) return 0;
    const char *t = w->p[0].text; size_t j = 0;
    if (!is_ident_start(t[0])) return 0;
    while (isalnum((unsigned char)t[j]) || t[j] == '_') j++;
    if (t[j] == '[') { int d = 0; do { if (t[j] == '[') d++; else if (t[j] == ']') d--; j++; } while (t[j] && d > 0); if (d) return 0; }
    int app = 0; if (t[j] == '+' && t[j + 1] == '=') { app = 1; j++; }
    if (t[j] != '=') return 0;
    memset(a, 0, sizeof *a);
    size_t nl = j - app; a->name = xstrndup(t, nl); a->append = app;
    const char *rest = t + j + 1;
    if (!*rest && w->n == 1 && P->s[P->i] == '(') {   /* name=( ... ) */
        P->i++; a->isarr = 1;
        for (;;) {
            skip_ws(P, 1); if (P->s[P->i] == ')') { P->i++; break; }
            Word *e = parse_word(P, WSTOP); if (!e) perr(P, "bad array literal");
            Word **ex = NULL; int n = 0; brace_expand(e, &ex, &n); for (int i = 0; i < n; i++) PUSH(a->elems, a->nelems, ex[i]);
        }
        return 1;
    }
    if (declare_arg) return 0;
    Word *v = mkword();
    if (rest[0] == '~' && (!rest[1] || rest[1] == '/' || rest[1] == ':')) { Part hp = { 0 }; hp.kind = PK_VAR; hp.text = xstrdup("HOME"); hp.quoted = 1; add_part(v, hp); rest++; }
    else if (rest[0] == '~' && rest[1]) unsupported("~user tilde expansion in assignment");
    if (*rest) add_lit(v, rest, strlen(rest), 0, 0);
    for (int k = 1; k < w->n; k++) PUSH(v->p, v->n, w->p[k]);
    a->val = v; return 1;
}
/* name[subscript]=value where the subscript holds expansions or quotes ($i, "a b", $(cmd)): recognized on the
 * raw source, since the word parser would split it into parts.  a->name keeps the raw "name[subscript]". */
static int parse_subscript_assign(Parser *P, Assign *a) {
    const char *s = P->s; size_t i = P->i;
    if (!is_ident_start(s[i])) return 0;
    while (isalnum((unsigned char)s[i]) || s[i] == '_') i++;
    if (s[i] != '[') return 0;
    size_t j = i + 1; int d = 1; char q = 0;
    for (; s[j]; j++) {
        if (q) { if (s[j] == q) q = 0; else if (s[j] == '\\' && q == '"' && s[j + 1]) j++; continue; }
        if (s[j] == '\\' && s[j + 1]) { j++; continue; }
        if (s[j] == '\'' || s[j] == '"') q = s[j]; else if (s[j] == '[') d++; else if (s[j] == ']' && --d == 0) break;
        else if (s[j] == '\n') return 0;
    }
    if (s[j] != ']') return 0;
    size_t e = j + 1; int app = 0;
    if (s[e] == '+' && s[e + 1] == '=') { app = 1; e++; }
    if (s[e] != '=') return 0;
    if (!strpbrk(xstrndup(s + i, j - i), "$`'\"\\ \t")) return 0;   /* plain subscripts: the word parser handles them */
    memset(a, 0, sizeof *a); a->name = xstrndup(s + P->i, j + 1 - P->i); a->append = app;
    P->i = e + 1;
    if (s[P->i] == '(') { unsupported("array literal assigned to an element"); return 0; }
    if (is_delim(s[P->i])) { a->val = mkword(); add_lit(a->val, "", 0, 1, 1); return 1; }
    Word *w = parse_word(P, WSTOP); Word *v = mkword();
    if (w && w->n && w->p[0].kind == PK_LIT && !w->p[0].quoted && w->p[0].text[0] == '~') {
        const char *rest = w->p[0].text;
        if (!rest[1] || rest[1] == '/' || rest[1] == ':') { Part hp = { 0 }; hp.kind = PK_VAR; hp.text = xstrdup("HOME"); hp.quoted = 1; add_part(v, hp); if (rest[1]) add_lit(v, rest + 1, strlen(rest + 1), 0, 0); for (int k = 1; k < w->n; k++) PUSH(v->p, v->n, w->p[k]); a->val = v; return 1; }
    }
    a->val = w ? w : v; if (!a->val->n) add_lit(a->val, "", 0, 1, 1);
    return 1;
}
static const char *lit_name(Word *w) { return (w && w->n == 1 && w->p[0].kind == PK_LIT) ? w->p[0].text : NULL; }
static Node *parse_simple(Parser *P) {
    Node *n = mk(N_SIMPLE); int seen = 0;
    for (;;) {
        skip_ws(P, 0);
        if (try_redir(P, n)) continue;
        char c = P->s[P->i];
        if (!c || strchr("\n;|)&(", c)) break;
        Assign a;
        if (!seen) { size_t save = P->i; if (parse_subscript_assign(P, &a)) { PUSH(n->as, n->nas, a); continue; } P->i = save; }
        Word *w = parse_word(P, WSTOP); if (!w) perr(P, "unexpected token");
        if (!seen && parse_assign_word(P, w, &a, 0)) { PUSH(n->as, n->nas, a); continue; }
        if (seen) {
            const char *cn = lit_name(n->w[0]);
            if (cn && (!strcmp(cn, "declare") || !strcmp(cn, "local") || !strcmp(cn, "typeset") || !strcmp(cn, "readonly") || !strcmp(cn, "export")) && parse_assign_word(P, w, &a, 1)) { PUSH(n->arr, n->narr, a); continue; }
        }
        seen = 1; push_words(n, w);
    }
    return n;
}
static void expect_kw(Parser *P, const char *kw) { skip_ws(P, 1); if (!at_kw(P, kw)) { char m[80]; snprintf(m, sizeof m, "expected '%s'", kw); perr(P, m); } P->i += strlen(kw); }
static Node *parse_cond_then(Parser *P, const char *kw) {   /* if/elif */
    Node *n = mk(N_IF); (void)kw;
    PUSH(n->k, n->nk, parse_list(P)); expect_kw(P, "then"); PUSH(n->k, n->nk, parse_list(P));
    skip_ws(P, 1);
    if (at_kw(P, "elif")) { P->i += 4; PUSH(n->k, n->nk, parse_cond_then(P, "elif")); }
    else { if (at_kw(P, "else")) { P->i += 4; PUSH(n->k, n->nk, parse_list(P)); } expect_kw(P, "fi"); }
    return n;
}
static char *read_raw_arith(Parser *P, char stop1, char stop2, int *closed) {   /* until stop char at depth 0 */
    size_t st = P->i; int d = 0; *closed = 0;
    for (; P->s[P->i]; P->i++) {
        char c = P->s[P->i];
        if (c == '(') d++; else if (c == ')') { if (d == 0) { if (stop1 == ')' && P->s[P->i + 1] == ')') { *closed = 1; break; } } else d--; }
        else if (d == 0 && c == stop1 && stop1 != ')') { *closed = 1; break; }
        (void)stop2;
    }
    return xstrndup(P->s + st, P->i - st);
}
static Word *arith_word(const char *text) {   /* (( )) text that needs $-expansion before evaluation */
    if (!strpbrk(text, "$`")) return NULL;
    Parser sub = { text, 0, NULL, 0 }; Word *w = mkword(); scan_parts(&sub, w, "", 2); return w;
}
static Node *parse_for(Parser *P) {
    skip_ws(P, 0);
    if (P->s[P->i] == '(' && P->s[P->i + 1] == '(') {
        P->i += 2; Node *n = mk(N_CFOR); int cl;
        n->s1 = read_raw_arith(P, ';', 0, &cl); P->i++; n->s2 = read_raw_arith(P, ';', 0, &cl); P->i++; n->s3 = read_raw_arith(P, ')', 0, &cl);
        if (!cl) perr(P, "bad for (( ))"); P->i += 2; skip_ws(P, 0); if (P->s[P->i] == ';') P->i++;
        n->aw[0] = arith_word(n->s1); n->aw[1] = arith_word(n->s2); n->aw[2] = arith_word(n->s3);
        expect_kw(P, "do"); PUSH(n->k, n->nk, parse_list(P)); expect_kw(P, "done"); return n;
    }
    Word *nm = parse_word(P, WSTOP); if (!nm || !lit_name(nm)) perr(P, "bad for variable");
    Node *n = mk(N_FOR); n->name = xstrdup(lit_name(nm)); skip_ws(P, 0);
    if (at_kw(P, "in")) {
        P->i += 2; n->flag = 1;
        for (;;) { skip_ws(P, 0); char c = P->s[P->i]; if (!c || c == ';' || c == '\n') break; Word *w = parse_word(P, WSTOP); if (!w) perr(P, "bad for list"); push_words(n, w); }
    }
    skip_ws(P, 0); if (P->s[P->i] == ';') P->i++;
    expect_kw(P, "do"); PUSH(n->k, n->nk, parse_list(P)); expect_kw(P, "done"); return n;
}
static Node *parse_case(Parser *P) {
    Node *n = mk(N_CASE); skip_ws(P, 0); Word *w = parse_word(P, WSTOP); if (!w) perr(P, "bad case word");
    PUSH(n->w, n->nw, w); expect_kw(P, "in");
    for (;;) {
        skip_ws(P, 1); while (P->s[P->i] == ';') { P->i++; skip_ws(P, 1); }
        if (at_kw(P, "esac")) { P->i += 4; break; }
        if (!P->s[P->i]) perr(P, "unterminated case");
        CaseItem it = { 0 }; if (P->s[P->i] == '(') { P->i++; skip_ws(P, 0); }
        for (;;) {
            skip_ws(P, 0); Word *pw = parse_word(P, " \t\n|&;()<>"); if (!pw) perr(P, "bad case pattern");
            PUSH(it.pats, it.np, pw); skip_ws(P, 0);
            if (P->s[P->i] == '|') { P->i++; continue; }
            break;
        }
        if (P->s[P->i] != ')') perr(P, "expected ) in case"); P->i++;
        it.body = parse_list(P); PUSH(n->ci, n->nci, it);
        skip_ws(P, 1); if (P->s[P->i] == ';' && P->s[P->i + 1] == ';') { P->i += 2; if (P->s[P->i] == '&') { unsupported("case ;;& fallthrough"); P->i++; } }
    }
    return n;
}
static Node *parse_dbl(Parser *P) {
    Node *n = mk(N_DBL); int after_re = 0;
    for (;;) {
        skip_ws(P, 1);
        if (!P->s[P->i]) perr(P, "unterminated [[");
        if (at_kw(P, "]]")) { P->i += 2; break; }
        Word *w;
        char c = P->s[P->i], c2 = P->s[P->i + 1];
        if (after_re) { w = parse_word(P, " \t\n"); after_re = 0; }
        else if ((c == '&' && c2 == '&') || (c == '|' && c2 == '|')) { w = mkword(); add_lit(w, P->s + P->i, 2, 0, 1); P->i += 2; }
        else if (c == '(' || c == ')' || c == '<' || c == '>') { w = mkword(); add_lit(w, &c, 1, 0, 1); P->i++; }
        else w = parse_word(P, " \t\n|&;()<>");
        if (!w) perr(P, "bad [[ expression");
        if (lit_name(w) && !strcmp(lit_name(w), "=~")) after_re = 1;
        PUSH(n->w, n->nw, w);
    }
    return n;
}
static Node *parse_command(Parser *P) {
    skip_ws(P, 0); Node *n; const char *s = P->s;
    if (s[P->i] == '(' && s[P->i + 1] == '(') {
        P->i += 2; int cl; char *e = read_raw_arith(P, ')', 0, &cl);
        if (cl) { P->i += 2; n = mk(N_ARITH); n->s1 = e; n->aw[0] = arith_word(e); parse_redirs(P, n); return n; }
        P->i -= 2 + strlen(e);   /* really a nested subshell */
    }
    if (s[P->i] == '(') {
        P->i++; n = mk(N_SUB); PUSH(n->k, n->nk, parse_list(P)); skip_ws(P, 1);
        if (s[P->i] != ')') perr(P, "expected )"); P->i++; parse_redirs(P, n); return n;
    }
    if (at_kw(P, "{")) { P->i++; n = mk(N_GROUP); PUSH(n->k, n->nk, parse_list(P)); expect_kw(P, "}"); parse_redirs(P, n); return n; }
    if (at_kw(P, "if")) { P->i += 2; n = parse_cond_then(P, "if"); parse_redirs(P, n); return n; }
    if (at_kw(P, "while") || at_kw(P, "until")) {
        int un = s[P->i] == 'u'; P->i += 5; n = mk(un ? N_UNTIL : N_WHILE); PUSH(n->k, n->nk, parse_list(P));
        expect_kw(P, "do"); PUSH(n->k, n->nk, parse_list(P)); expect_kw(P, "done"); parse_redirs(P, n); return n;
    }
    if (at_kw(P, "for")) { P->i += 3; n = parse_for(P); parse_redirs(P, n); return n; }
    if (at_kw(P, "case")) { P->i += 4; n = parse_case(P); parse_redirs(P, n); return n; }
    if (at_kw(P, "select") || at_kw(P, "coproc") || at_kw(P, "time")) unsupported("'%.6s' keyword", s + P->i);
    if (at_kw(P, "[[")) { P->i += 2; n = parse_dbl(P); parse_redirs(P, n); return n; }
    if (at_kw(P, "function")) {
        P->i += 8; skip_ws(P, 0); Word *nm = parse_word(P, WSTOP); if (!nm || !lit_name(nm)) perr(P, "bad function name");
        n = mk(N_FUNC); n->name = xstrdup(lit_name(nm)); skip_ws(P, 0);
        if (s[P->i] == '(') { P->i++; skip_ws(P, 0); if (s[P->i] != ')') perr(P, "expected )"); P->i++; }
        skip_ws(P, 1); PUSH(n->k, n->nk, parse_command(P)); return n;
    }
    /* name() compound */
    if (is_ident_start(s[P->i])) {
        size_t j = P->i; while (isalnum((unsigned char)s[j]) || strchr("_.:-", s[j]) ) { if (!s[j]) break; j++; }
        size_t e = j; while (s[j] == ' ' || s[j] == '\t') j++;
        if (s[j] == '(') { size_t k = j + 1; while (s[k] == ' ' || s[k] == '\t') k++; if (s[k] == ')') {
            n = mk(N_FUNC); n->name = xstrndup(s + P->i, e - P->i); P->i = k + 1; skip_ws(P, 1); PUSH(n->k, n->nk, parse_command(P)); return n; } }
    }
    return parse_simple(P);
}
static Node *parse_pipeline(Parser *P) {
    skip_ws(P, 0); int neg = 0;
    while (at_kw(P, "!")) { neg = !neg; P->i++; skip_ws(P, 0); }
    Node *first = parse_command(P); Node *res = first;
    skip_ws(P, 0);
    if (P->s[P->i] == '|' && P->s[P->i + 1] != '|') {
        res = mk(N_PIPE); PUSH(res->k, res->nk, first);
        while (P->s[P->i] == '|' && P->s[P->i + 1] != '|') {
            P->i++; if (P->s[P->i] == '&') P->i++;
            skip_ws(P, 1); PUSH(res->k, res->nk, parse_command(P)); skip_ws(P, 0);
        }
    }
    if (neg) { Node *n = mk(N_NOT); PUSH(n->k, n->nk, res); return n; }
    return res;
}
static Node *parse_andor(Parser *P) {
    Node *l = parse_pipeline(P);
    for (;;) {
        skip_ws(P, 0);
        int t = (P->s[P->i] == '&' && P->s[P->i + 1] == '&') ? N_AND : (P->s[P->i] == '|' && P->s[P->i + 1] == '|') ? N_OR : -1;
        if (t < 0) return l;
        P->i += 2; skip_ws(P, 1);
        Node *n = mk(t); PUSH(n->k, n->nk, l); PUSH(n->k, n->nk, parse_pipeline(P)); l = n;
    }
}
static Node *parse_list(Parser *P) {
    Node *l = mk(N_LIST);
    for (;;) {
        skip_ws(P, 1);
        if (P->s[P->i] == ';' && P->s[P->i + 1] == '&') { unsupported("case ;& fallthrough"); perr(P, "case ;&"); }
        while (P->s[P->i] == ';' && P->s[P->i + 1] != ';') { P->i++; skip_ws(P, 1); }
        if (!P->s[P->i] || at_closer(P)) break;
        PUSH(l->k, l->nk, parse_andor(P));
        skip_ws(P, 0); char c = P->s[P->i];
        if (c == '&' && P->s[P->i + 1] != '&') { P->i++; Node *bg = mk(N_BG); PUSH(bg->k, bg->nk, l->k[l->nk - 1]); l->k[l->nk - 1] = bg; }
        else if (c == ';' && P->s[P->i + 1] != ';') P->i++;
        else if (c == '\n' || !c) {}
        else if (!at_closer(P)) perr(P, "unexpected token");
    }
    return l;
}

static Node *parse_text(const char *text) {
    Parser sub = { xstrdup(text), 0, NULL, 0 }; Node *l = parse_list(&sub);
    skip_ws(&sub, 1); if (sub.s[sub.i]) perr(&sub, "unexpected token in eval/trap string");
    return l;
}

/* ================================================================ codegen */
typedef struct { char *brk, *cont; } Loop;
typedef struct { SB body, allocas; int terminated; Loop loops[64]; int nloops; const char *ret_label; int in_cond; } Fn;
static Fn *cur; static SB g_funcs, g_strs; static int g_tmp, g_lbl, g_blk;
static char **g_strtab; static int g_nstr;
static char **g_userfns; static int g_nuserfns; static int g_errexit;

static char **g_vn; static int g_nvn;
static int vid(const char *n) { for (int i = 0; i < g_nvn; i++) if (!strcmp(g_vn[i], n)) return i; PUSH(g_vn, g_nvn, xstrdup(n)); return g_nvn - 1; }
static int is_ident(const char *n) { if (!(isalpha((unsigned char)*n) || *n == '_')) return 0; for (; *n; n++) if (!(isalnum((unsigned char)*n) || *n == '_')) return 0; return 1; }
static int special_kind(const char *n) {   /* SP_* for special parameters, 0 otherwise */
    if (!n[1]) switch (n[0]) { case '?': return 1; case '#': return 2; case '$': return 3; case '!': return 4; case '-': return 5; case '0': return 6; case '@': return 10; case '*': return 11; }
    if (!strcmp(n, "RANDOM")) return 7; if (!strcmp(n, "PPID")) return 8; if (!strcmp(n, "SECONDS")) return 9;
    if (!strcmp(n, "FUNCNAME")) return SP_FUNCNAME; if (!strcmp(n, "BASH_SOURCE")) return SP_SOURCE;
    return 0;
}
static int newtmp(void) { return g_tmp++; }
static int newlbl(void) { return g_lbl++; }
static void emit(const char *fmt, ...) {
    if (cur->terminated) { sb_printf(&cur->body, "dead%d:\n", newlbl()); cur->terminated = 0; }
    va_list ap; va_start(ap, fmt); sb_puts(&cur->body, "  "); sb_vprintf(&cur->body, fmt, ap); sb_putc(&cur->body, '\n'); va_end(ap);
}
static void emit_term(const char *fmt, ...) {
    if (cur->terminated) { sb_printf(&cur->body, "dead%d:\n", newlbl()); cur->terminated = 0; }
    va_list ap; va_start(ap, fmt); sb_puts(&cur->body, "  "); sb_vprintf(&cur->body, fmt, ap); sb_putc(&cur->body, '\n'); va_end(ap);
    cur->terminated = 1;
}
static void label(const char *l) { if (!cur->terminated) emit_term("br label %%%s", l); sb_printf(&cur->body, "%s:\n", l); cur->terminated = 0; }
static char *lname(const char *pfx, int id) { char b[64]; snprintf(b, sizeof b, "%s.%d", pfx, id); return xstrdup(b); }
static void br(const char *l) { emit_term("br label %%%s", l); }
static void cbr(int cond, const char *a, const char *b) { emit_term("br i1 %%t%d, label %%%s, label %%%s", cond, a, b); }

static char *S(const char *s) {   /* global string constant */
    for (int i = 0; i < g_nstr; i++) if (!strcmp(g_strtab[i], s)) { char b[32]; snprintf(b, sizeof b, "@.str.%d", i); return xstrdup(b); }
    int id = g_nstr; PUSH(g_strtab, g_nstr, xstrdup(s));
    sb_printf(&g_strs, "@.str.%d = private unnamed_addr constant [%zu x i8] c\"", id, strlen(s) + 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) { if (*p >= 32 && *p < 127 && *p != '"' && *p != '\\') sb_putc(&g_strs, *p); else sb_printf(&g_strs, "\\%02X", *p); }
    sb_puts(&g_strs, "\\00\", align 1\n");
    char b[32]; snprintf(b, sizeof b, "@.str.%d", id); return xstrdup(b);
}
static int is_user_fn(const char *n) { for (int i = 0; i < g_nuserfns; i++) if (!strcmp(g_userfns[i], n)) return 1; return 0; }
/* names declared with declare/local/typeset -A anywhere in the script: their subscripts are string keys */
static char **g_assoc; static int g_nassoc;
static int is_assoc(const char *n) { for (int i = 0; i < g_nassoc; i++) if (!strcmp(g_assoc[i], n)) return 1; return 0; }
static void add_assoc(const char *n) { if (!is_assoc(n)) PUSH(g_assoc, g_nassoc, xstrdup(n)); }
/* "base[sub]" with base an assoc name and sub not @ / *: returns sub text (malloc'd), base in *basep */
static char *assoc_sub(const char *text, char **basep) {
    const char *br = strchr(text, '['); size_t L = strlen(text);
    if (!br || br == text || text[L - 1] != ']') return NULL;
    char *base = xstrndup(text, br - text); if (!is_assoc(base)) { free(base); return NULL; }
    char *sub = xstrndup(br + 1, L - (br - text) - 2);
    if (!strcmp(sub, "@") || !strcmp(sub, "*")) { free(base); free(sub); return NULL; }
    *basep = base; return sub;
}
static void scan_parts(Parser *P, Word *w, const char *stops, int dq);
static Word *key_word(const char *text) {   /* assoc subscript: expanded like a double-quoted word (no split/glob) */
    Parser sub = { text, 0, NULL, 0 }; Word *w = mkword(); scan_parts(&sub, w, "", 0); return w;
}
static int word_to_str(Word *w, int mode);
static int key_tmp(const char *sub) {   /* tmp holding the expanded key, owned by the runtime's key ring */
    int t = word_to_str(key_word(sub), WB_STR), r = newtmp(); emit("%%t%d = call ptr @bash_rt_keytmp(ptr %%t%d)", r, t); return r;
}

static int new_wb(int mode) { int t = newtmp(); emit("%%t%d = call ptr @bash_rt_wb_new(i32 %d)", t, mode); return t; }
static void gen_node(Node *n);

static void begin_fn(Fn *f) { memset(f, 0, sizeof *f); f->ret_label = "Lret"; }
static void flush_fn(Fn *f, const char *header) {
    sb_puts(&g_funcs, header); sb_puts(&g_funcs, "{\nentry:\n");
    if (f->allocas.s) sb_puts(&g_funcs, f->allocas.s);
    sb_puts(&g_funcs, "  br label %start\nstart:\n");
    sb_puts(&g_funcs, f->body.s ? f->body.s : ""); sb_puts(&g_funcs, "}\n\n");
}
static char *gen_block_fn(Node *body) {   /* i32 @bash_blk_N() */
    Fn f, *saved = cur; begin_fn(&f); cur = &f; int id = g_blk++;
    gen_node(body); br("Lret"); label("Lret");
    int t = newtmp(); emit("%%t%d = call i32 @bash_rt_get_status()", t); emit_term("ret i32 %%t%d", t);
    char name[64], hdr[128]; snprintf(name, sizeof name, "@bash_blk_%d", id); snprintf(hdr, sizeof hdr, "define internal i32 %s() ", name);
    flush_fn(&f, hdr); cur = saved; return xstrdup(name);
}
static void emit_word(int b, Word *w, int mode);
static int word_to_str(Word *w, int mode);
static char *gen_str_fn(Word *w, int mode) {   /* ptr @bash_str_N() -> malloc'd string; "null" if absent */
    if (!w) return xstrdup("null");
    Fn f, *saved = cur; begin_fn(&f); cur = &f; int id = g_blk++;
    int b = new_wb(mode); emit_word(b, w, mode);
    int t = newtmp(); emit("%%t%d = call ptr @bash_rt_wb_str(ptr %%t%d)", t, b); emit_term("ret ptr %%t%d", t);
    char name[64], hdr[128]; snprintf(name, sizeof name, "@bash_str_%d", id); snprintf(hdr, sizeof hdr, "define internal ptr %s() ", name);
    flush_fn(&f, hdr); cur = saved; return xstrdup(name);
}
static int word_const(Word *w, char **text) {
    if (!w->n) return 0; SB sb = { 0 };
    for (int i = 0; i < w->n; i++) {
        Part *p = &w->p[i]; if (p->kind != PK_LIT) return 0;
        if (!p->quoted && strpbrk(p->text, "*?[")) return 0;
        sb_puts(&sb, p->text);
    }
    *text = sb.s ? sb.s : xstrdup(""); return 1;
}
static int arith_native(const char *text, char **val);
static int opd_const(Word *w, int pat, char **text) {   /* operand that is a plain literal: no runtime evaluation needed */
    if (!w->n) { *text = xstrdup(""); return 1; }
    SB sb = { 0 };
    for (int i = 0; i < w->n; i++) { Part *p = &w->p[i]; if (p->kind != PK_LIT || (pat && strpbrk(p->text, "*?[]\\"))) { free(sb.s); return 0; } sb_puts(&sb, p->text); }
    *text = sb.s ? sb.s : xstrdup(""); return 1;
}
static void emit_word(int b, Word *w, int mode) {
    char *ct;
    if (mode == WB_ARGV && word_const(w, &ct)) { emit("call void @bash_rt_wb_const(ptr %%t%d, ptr %s)", b, S(ct)); return; }
    for (int i = 0; i < w->n; i++) {
        Part *p = &w->p[i];
        switch (p->kind) {
        case PK_LIT: emit("call void @bash_rt_wb_lit(ptr %%t%d, ptr %s, i32 %d)", b, S(p->text), p->quoted ? LIT_QUOTED : LIT_PLAIN); break;
        case PK_VAR: {
            const char *nm = p->text; int sk = special_kind(nm);
            if (sk >= 10 || (sk && sk != 6 && 0)) emit("call void @bash_rt_wb_special(ptr %%t%d, i32 %d, i32 %d)", b, sk, p->quoted);
            else if (sk) emit("call void @bash_rt_wb_special(ptr %%t%d, i32 %d, i32 %d)", b, sk, p->quoted);
            else if (isdigit((unsigned char)nm[0])) emit("call void @bash_rt_wb_pos(ptr %%t%d, i32 %d, i32 %d)", b, atoi(nm), p->quoted);
            else emit("call void @bash_rt_wb_var_id(ptr %%t%d, i32 %d, i32 %d)", b, vid(nm), p->quoted);
            break; }
        case PK_PARAM: {
            int pat = p->op >= P_TRIM_S && p->op <= P_SUBST_SUF;
            char *ca = NULL, *cc = NULL, *fa, *fc, *abase, *asub;
            if (p->op != P_KEYS && p->op != P_INDIRECT && (asub = assoc_sub(p->text, &abase))) {
                int k = key_tmp(asub);
                if (p->a && opd_const(p->a, pat, &ca)) fa = xstrdup("null"); else { ca = NULL; fa = gen_str_fn(p->a, pat ? WB_PATTERN : WB_STR); }
                if (p->c && opd_const(p->c, 0, &cc)) fc = xstrdup("null"); else { cc = NULL; fc = gen_str_fn(p->c, WB_STR); }
                emit("call void @bash_rt_wb_aparam(ptr %%t%d, i32 %d, ptr %%t%d, i32 %d, ptr %s, ptr %s, ptr %s, ptr %s, i32 %d)", b, vid(abase), k, p->op, fa, fc, ca ? S(ca) : "null", cc ? S(cc) : "null", p->quoted);
                break;
            }
            if (p->op == P_SUBSTR && is_ident(p->text) && !special_kind(p->text) && p->a && opd_const(p->a, 0, &ca) && !strchr(ca, '?')) {
                int d = 0; char *colon = NULL; for (char *q = ca; *q; q++) { if (*q == '(') d++; else if (*q == ')') d--; else if (*q == ':' && !d) { colon = q; break; } }
                char *offs = colon ? xstrndup(ca, colon - ca) : xstrdup(ca), *lens = colon ? xstrdup(colon + 1) : NULL, *ov, *lv = "0";
                size_t mark = cur->body.n; int term = cur->terminated;
                if (offs[strspn(offs, " \t")] && arith_native(offs, &ov) && (!lens || (lens[strspn(lens, " \t")] && arith_native(lens, &lv)))) {
                    emit("call void @bash_rt_wb_substr_id(ptr %%t%d, i32 %d, i64 %s, i64 %s, i32 %d, i32 %d)", b, vid(p->text), ov, lv, lens != NULL, p->quoted); break; }
                if (cur->body.s) { cur->body.n = mark; cur->body.s[mark] = 0; } cur->terminated = term;
            }
            if (is_ident(p->text) && !special_kind(p->text)) {
                if (p->a && opd_const(p->a, pat, &ca)) fa = xstrdup("null"); else { ca = NULL; fa = gen_str_fn(p->a, pat ? WB_PATTERN : WB_STR); }
                if (p->c && opd_const(p->c, 0, &cc)) fc = xstrdup("null"); else { cc = NULL; fc = gen_str_fn(p->c, WB_STR); }
                emit("call void @bash_rt_wb_param_id(ptr %%t%d, i32 %d, i32 0, i64 0, i32 %d, ptr %s, ptr %s, ptr %s, ptr %s, i32 %d)", b, vid(p->text), p->op, fa, fc, ca ? S(ca) : "null", cc ? S(cc) : "null", p->quoted);
                break;
            }
            fa = gen_str_fn(p->a, pat ? WB_PATTERN : WB_STR); fc = gen_str_fn(p->c, WB_STR);
            emit("call void @bash_rt_wb_param(ptr %%t%d, ptr %s, i32 %d, ptr %s, ptr %s, i32 %d)", b, S(p->text), p->op, fa, fc, p->quoted); break; }
        case PK_CMD: { char *f = gen_block_fn(p->body); emit("call void @bash_rt_wb_cmdsub(ptr %%t%d, ptr %s, i32 %d)", b, f, p->quoted); break; }
        case PK_PROC: { char *f = gen_block_fn(p->body); emit("call void @bash_rt_wb_procsub(ptr %%t%d, ptr %s)", b, f); break; }
        case PK_ARITH:
            { char *v; if (arith_native(p->text, &v)) emit("call void @bash_rt_wb_int(ptr %%t%d, i64 %s, i32 %d)", b, v, p->quoted);
              else if (p->a) { int s = word_to_str(p->a, WB_STR); emit("call void @bash_rt_wb_arith(ptr %%t%d, ptr %%t%d, i32 %d)", b, s, p->quoted); emit("call void @free(ptr %%t%d)", s); }
              else emit("call void @bash_rt_wb_arith(ptr %%t%d, ptr %s, i32 %d)", b, S(p->text), p->quoted); }
            break;
        }
    }
    if (mode == WB_ARGV) emit("call void @bash_rt_wb_end(ptr %%t%d)", b);
}
static int word_to_str(Word *w, int mode) {   /* returns tmp holding malloc'd string */
    int b = new_wb(mode); if (w) emit_word(b, w, mode);
    int s = newtmp(); emit("%%t%d = call ptr @bash_rt_wb_str(ptr %%t%d)", s, b); return s;
}
/* assignment value that needs no word builder: a constant, $var, $N or a special.  Writes an IR operand
 * (borrowed string) to *op; for a plain $var with copy_dst >= 0 emits a direct copy and sets *op = NULL. */
static int word_borrow(Word *w, int copy_dst, char **op) {
    char b[32];
    if (!w || !w->n) { *op = S(""); return 1; }
    int lit = 1; for (int i = 0; i < w->n; i++) if (w->p[i].kind != PK_LIT) lit = 0;
    if (lit) { SB sb = { 0 }; for (int i = 0; i < w->n; i++) sb_puts(&sb, w->p[i].text); *op = S(sb.s ? sb.s : ""); return 1; }
    if (w->n != 1 || w->p[0].kind != PK_VAR) return 0;
    const char *nm = w->p[0].text; int sk = special_kind(nm), t;
    if (sk) { if (sk == SP_AT || sk == SP_STAR || sk == SP_FUNCNAME || sk == SP_SOURCE) return 0;
        t = newtmp(); emit("%%t%d = call ptr @bash_rt_special_s(i32 %d)", t, sk); }
    else if (isdigit((unsigned char)nm[0])) { t = newtmp(); emit("%%t%d = call ptr @bash_rt_pos_u(i32 %d)", t, atoi(nm)); }
    else if (copy_dst >= 0) { emit("call void @bash_rt_copy_id(i32 %d, i32 %d)", copy_dst, vid(nm)); *op = NULL; return 1; }
    else { t = newtmp(); emit("%%t%d = call ptr @bash_rt_gets_e(i32 %d)", t, vid(nm)); }
    snprintf(b, sizeof b, "%%t%d", t); *op = xstrdup(b); return 1;
}
static void set_status_const(int v) { emit("call void @bash_rt_set_status(i32 %d)", v); }
static int get_status(void) { int t = newtmp(); emit("%%t%d = call i32 @bash_rt_get_status()", t); return t; }
static int alloca_i32(void) { int t = newtmp(); sb_printf(&cur->allocas, "  %%t%d = alloca i32\n", t); return t; }

/* [key]=value array-literal element: split at the first unquoted "]=" */
static int split_kv(Word *w, Word **kw, Word **vw) {
    if (!w->n || w->p[0].kind != PK_LIT || w->p[0].quoted || w->p[0].text[0] != '[') return 0;
    for (int i = 0; i < w->n; i++) {
        Part *p = &w->p[i]; if (p->kind != PK_LIT || p->quoted) continue;
        const char *from = i == 0 ? p->text + 1 : p->text, *e = strstr(from, "]=");
        if (!e) continue;
        Word *k = mkword(), *v = mkword();
        for (int j = 0; j < i; j++) { Part c = w->p[j]; if (j == 0) c.text = xstrdup(c.text + 1); if (c.kind == PK_LIT) c.quoted = 1; PUSH(k->p, k->n, c); }
        if (e > from) add_lit(k, from, e - from, 1, 0);
        if (e[2]) add_lit(v, e + 2, strlen(e + 2), 0, 0);
        for (int j = i + 1; j < w->n; j++) PUSH(v->p, v->n, w->p[j]);
        if (!v->n) add_lit(v, "", 0, 1, 1);
        if (!k->n) add_lit(k, "", 0, 1, 1);
        *kw = k; *vw = v; return 1;
    }
    return 0;
}
static void gen_assign(Assign *a, int tmp) {
    int keyed = 0;
    if (a->isarr) for (int i = 0; i < a->nelems; i++) { Word *k, *v; if (split_kv(a->elems[i], &k, &v)) keyed = 1; }
    if (a->isarr && (keyed || is_assoc(a->name))) {   /* begin / kv / push / end */
        int id = vid(a->name); emit("call void @bash_rt_arr_begin(i32 %d, i32 %d)", id, a->append);
        for (int i = 0; i < a->nelems; ) {
            Word *k, *v;
            if (split_kv(a->elems[i], &k, &v)) {
                int ks = word_to_str(k, WB_STR), vs = word_to_str(v, WB_STR);
                emit("call void @bash_rt_arr_kv(i32 %d, ptr %%t%d, ptr %%t%d, i32 0)", id, ks, vs); emit("call void @free(ptr %%t%d)", ks); emit("call void @free(ptr %%t%d)", vs); i++; continue;
            }
            int b = new_wb(WB_ARGV);
            for (; i < a->nelems && !split_kv(a->elems[i], &k, &v); i++) emit_word(b, a->elems[i], WB_ARGV);
            int c = newtmp(), av = newtmp();
            emit("%%t%d = call i32 @bash_rt_wb_argc(ptr %%t%d)", c, b); emit("%%t%d = call ptr @bash_rt_wb_argv(ptr %%t%d)", av, b);
            emit("call void @bash_rt_arr_push(i32 %d, i32 %%t%d, ptr %%t%d)", id, c, av); emit("call void @bash_rt_wb_free(ptr %%t%d)", b);
        }
        emit("call void @bash_rt_arr_end(i32 %d)", id); return;
    }
    { char *abase, *asub;
      if (!tmp && !a->isarr && (asub = assoc_sub(a->name, &abase))) {   /* h[key]=value */
          int ks = word_to_str(key_word(asub), WB_STR), sv = word_to_str(a->val, WB_STR);
          emit("call void @bash_rt_aset(i32 %d, ptr %%t%d, ptr %%t%d, i32 %d)", vid(abase), ks, sv, a->append);
          emit("call void @free(ptr %%t%d)", ks); emit("call void @free(ptr %%t%d)", sv); return; } }
    if (a->isarr) {
        int b = new_wb(WB_ARGV); for (int i = 0; i < a->nelems; i++) emit_word(b, a->elems[i], WB_ARGV);
        int c = newtmp(), v = newtmp();
        emit("%%t%d = call i32 @bash_rt_wb_argc(ptr %%t%d)", c, b); emit("%%t%d = call ptr @bash_rt_wb_argv(ptr %%t%d)", v, b);
        emit("call void @bash_rt_set_array_id(i32 %d, i32 %%t%d, ptr %%t%d, i32 %d)", vid(a->name), c, v, a->append);
        emit("call void @bash_rt_wb_free(ptr %%t%d)", b); return;
    }
    if (!tmp && !a->append && is_ident(a->name) && a->val && a->val->n == 1 && a->val->p[0].kind == PK_ARITH) {
        char *v; if (arith_native(a->val->p[0].text, &v)) { emit("call void @bash_rt_seti(i32 %d, i64 %s)", vid(a->name), v); return; }
    }
    if (!tmp && !is_ident(a->name)) {   /* name[expr]=value with a compile-time index expression */
        const char *br_ = strchr(a->name, '['); size_t L = strlen(a->name);
        if (br_ && a->name[L - 1] == ']') {
            char *base = xstrndup(a->name, br_ - a->name), *ix = xstrndup(br_ + 1, L - (br_ - a->name) - 2), *iv;
            if (is_ident(base) && !special_kind(base) && strcmp(ix, "@") && strcmp(ix, "*") && arith_native(ix, &iv)) {
                char *v;
                if (!a->append && a->val && a->val->n == 1 && a->val->p[0].kind == PK_ARITH && arith_native(a->val->p[0].text, &v)) {
                    emit("call void @bash_rt_seti_elem(i32 %d, i64 %s, i64 %s)", vid(base), iv, v); return; }
                int sv = word_to_str(a->val, WB_STR);
                emit("call void @bash_rt_set_elem_id(i32 %d, i64 %s, ptr %%t%d, i32 %d)", vid(base), iv, sv, a->append); emit("call void @free(ptr %%t%d)", sv); return;
            }
        }
    }
    if (!tmp && !is_ident(a->name)) {   /* name[sub]=value, sub needing expansion: expand, then index (arith) or key (assoc) at run time */
        const char *br_ = strchr(a->name, '['); size_t L = strlen(a->name);
        if (br_ && a->name[L - 1] == ']') {
            char *base = xstrndup(a->name, br_ - a->name), *ix = xstrndup(br_ + 1, L - (br_ - a->name) - 2);
            if (is_ident(base) && !special_kind(base) && strcmp(ix, "@") && strcmp(ix, "*")) {
                int ks = word_to_str(key_word(ix), WB_STR), sv = word_to_str(a->val, WB_STR);
                emit("call void @bash_rt_aset(i32 %d, ptr %%t%d, ptr %%t%d, i32 %d)", vid(base), ks, sv, a->append);
                emit("call void @free(ptr %%t%d)", ks); emit("call void @free(ptr %%t%d)", sv); return;
            }
        }
    }
    if (is_ident(a->name)) {   /* x=const / x=$y / x=$1: no temporary string */
        char *op; size_t mark = cur->body.n;
        if (word_borrow(a->val, (!tmp && !a->append) ? vid(a->name) : -1, &op)) {
            if (!op) return;
            if (tmp) emit("call void @bash_rt_tmp_set_id(i32 %d, ptr %s)", vid(a->name), op);
            else emit("call void @bash_rt_set_id(i32 %d, ptr %s, i32 %d)", vid(a->name), op, a->append);
            return;
        }
        (void)mark;
    }
    int s = word_to_str(a->val, WB_STR);
    if (!is_ident(a->name)) emit("call void @bash_rt_set(ptr %s, ptr %%t%d, i32 %d)", S(a->name), s, a->append);
    else if (tmp) emit("call void @bash_rt_tmp_set_id(i32 %d, ptr %%t%d)", vid(a->name), s);
    else emit("call void @bash_rt_set_id(i32 %d, ptr %%t%d, i32 %d)", vid(a->name), s, a->append);
    emit("call void @free(ptr %%t%d)", s);
}

/* redirections: returns label to continue at on failure (or NULL) */
typedef struct { char *fail, *done; int active; } RedirCtx;
static RedirCtx redir_begin(Redir *r, int nr) {
    RedirCtx c = { NULL, NULL, 0 }; if (!nr) return c;
    c.active = 1; int id = newlbl(); c.fail = lname("rd.fail", id); c.done = lname("rd.done", id); char *ok = lname("rd.ok", id);
    emit("call void @bash_rt_redir_push()");
    for (int i = 0; i < nr; i++) {
        int s = word_to_str(r[i].target, WB_STR);
        emit("call void @bash_rt_redir(i32 %d, i32 %d, ptr %%t%d)", r[i].fd, r[i].op, s); emit("call void @free(ptr %%t%d)", s);
    }
    int t = newtmp(), z = newtmp(); emit("%%t%d = call i32 @bash_rt_redir_ok()", t); emit("%%t%d = icmp ne i32 %%t%d, 0", z, t);
    cbr(z, ok, c.fail); label(ok); return c;
}
static void redir_end(RedirCtx *c) {
    if (!c->active) return;
    br(c->done); label(c->fail); set_status_const(1); label(c->done); emit("call void @bash_rt_redir_pop()");
}


/* ---------------------------------------------------------------- native arithmetic
 * Arithmetic that mentions no $-expansions is parsed here, at compile time, and turned into straight-line
 * i64 code; variables are read/written through their slot ids.  Anything unusual falls back to the runtime
 * evaluator (which also reports syntax errors at run time like bash does). */
typedef struct { const char *p; int err; } AC;
static char *tv(void) { char b[24]; snprintf(b, sizeof b, "%%t%d", newtmp()); return xstrdup(b); }
static int alloca_i64(void) { int t = newtmp(); sb_printf(&cur->allocas, "  %%t%d = alloca i64\n", t); return t; }
static void ac_ws(AC *a) { while (*a->p == ' ' || *a->p == '\t' || *a->p == '\n') a->p++; }
static char *ac_assign(AC *a);
typedef struct { int id, elem, special; char *idx; int key; } LV;   /* key >= 0: assoc element, tmp with the key */
static const char *ac_subscript_end(const char *s) {   /* s just after '[': matching ']' or NULL */
    int d = 1; char q = 0;
    for (; *s; s++) { if (q) { if (*s == q) q = 0; else if (*s == '\\' && q == '"' && s[1]) s++; continue; }
        if (*s == '\'' || *s == '"') q = *s; else if (*s == '[') d++; else if (*s == ']' && --d == 0) return s; }
    return NULL;
}
static int ac_lvalue(AC *a, LV *lv) {
    ac_ws(a); const char *s = a->p;
    if (!(isalpha((unsigned char)*s) || *s == '_')) return 0;
    const char *st = s; while (isalnum((unsigned char)*s) || *s == '_') s++;
    char *name = xstrndup(st, s - st); lv->elem = 0; lv->idx = NULL; lv->key = -1; lv->special = special_kind(name);
    if (lv->special >= 10) { free(name); return 0; }
    if (*s == '[' && !lv->special && is_assoc(name)) {
        const char *e = ac_subscript_end(s + 1); if (!e) { a->err = 1; free(name); return 1; }
        char *sub = xstrndup(s + 1, e - s - 1); lv->id = vid(name); free(name);
        lv->key = key_tmp(sub); a->p = e + 1; return 1;
    }
    lv->id = lv->special ? 0 : vid(name); free(name); a->p = s;
    if (*s == '[') { a->p++; lv->idx = ac_assign(a); ac_ws(a); if (*a->p != ']') { a->err = 1; return 1; } a->p++; lv->elem = 1; }
    return 1;
}
static char *lv_load(LV *lv) {
    char *r = tv();
    if (lv->special) emit("%s = call i64 @bash_rt_special_i(i32 %d)", r, lv->special);
    else if (lv->key >= 0) emit("%s = call i64 @bash_rt_ageti(i32 %d, ptr %%t%d)", r, lv->id, lv->key);
    else if (lv->elem) emit("%s = call i64 @bash_rt_geti_elem(i32 %d, i64 %s)", r, lv->id, lv->idx);
    else emit("%s = call i64 @bash_rt_geti(i32 %d)", r, lv->id);
    return r;
}
static void lv_store(AC *a, LV *lv, const char *v) {
    if (lv->special) { a->err = 1; return; }
    if (lv->key >= 0) emit("call void @bash_rt_aseti(i32 %d, ptr %%t%d, i64 %s)", lv->id, lv->key, v);
    else if (lv->elem) emit("call void @bash_rt_seti_elem(i32 %d, i64 %s, i64 %s)", lv->id, lv->idx, v);
    else emit("call void @bash_rt_seti(i32 %d, i64 %s)", lv->id, v);
}
static char *ac_bin(const char *op, const char *l, const char *r) { char *t = tv(); emit("%s = %s i64 %s, %s", t, op, l, r); return t; }
static char *ac_cmp(const char *cc, const char *l, const char *r) { char *c = tv(), *t = tv(); emit("%s = icmp %s i64 %s, %s", c, cc, l, r); emit("%s = zext i1 %s to i64", t, c); return t; }
static char *ac_dollar(AC *a) {   /* $name ${name} ${#name} ${#arr[@]} ${name[expr]} $N $# $? inside arithmetic */
    const char *s = a->p + 1; int brace = 0, len = 0;
    if (*s == '{') { brace = 1; s++; if (*s == '#' && s[1] != '}') { len = 1; s++; } }
    char *r = tv();
    if (!brace && (*s == '#' || *s == '?')) { emit("%s = call i64 @bash_rt_special_i(i32 %d)", r, *s == '#' ? 2 : 1); a->p = s + 1; return r; }
    if (isdigit((unsigned char)*s)) {
        int n = 0; if (brace) { while (isdigit((unsigned char)*s)) n = n * 10 + (*s++ - '0'); } else n = *s++ - '0';
        if (brace) { if (*s != '}') { a->err = 1; return r; } s++; }
        if (len) { a->err = 1; return r; }
        if (n == 0) { a->err = 1; return r; }
        emit("%s = call i64 @bash_rt_pos_i(i32 %d)", r, n); a->p = s; return r;
    }
    if (!(isalpha((unsigned char)*s) || *s == '_')) { a->err = 1; return r; }
    const char *st = s; while (isalnum((unsigned char)*s) || *s == '_') s++;
    char *name = xstrndup(st, s - st);
    if (special_kind(name)) { a->err = 1; return r; }
    int id = vid(name);
    if (!brace) { emit("%s = call i64 @bash_rt_geti(i32 %d)", r, id); a->p = s; return r; }
    if (*s == '[' && is_assoc(name) && !len && !((s[1] == '@' || s[1] == '*') && s[2] == ']')) {
        const char *e = ac_subscript_end(s + 1); if (!e || e[1] != '}') { a->err = 1; return r; }
        char *sub = xstrndup(s + 1, e - s - 1); int k = key_tmp(sub);
        emit("%s = call i64 @bash_rt_ageti(i32 %d, ptr %%t%d)", r, id, k); a->p = e + 2; return r;
    }
    if (*s == '[') {
        if ((s[1] == '@' || s[1] == '*') && s[2] == ']' && s[3] == '}' && len) { emit("%s = call i64 @bash_rt_arr_len(i32 %d)", r, id); a->p = s + 4; return r; }
        if (len) { a->err = 1; return r; }
        a->p = s + 1; char *ix = ac_assign(a); ac_ws(a); if (a->p[0] != ']' || a->p[1] != '}') { a->err = 1; return r; }
        a->p += 2; char *r2 = tv(); emit("%s = call i64 @bash_rt_geti_elem(i32 %d, i64 %s)", r2, id, ix); return r2;
    }
    if (*s != '}') { a->err = 1; return r; }
    if (len) emit("%s = call i64 @bash_rt_strlen_id(i32 %d)", r, id); else emit("%s = call i64 @bash_rt_geti(i32 %d)", r, id);
    a->p = s + 1; return r;
}
static char *ac_primary(AC *a) {
    ac_ws(a);
    if (*a->p == '$') return ac_dollar(a);
    if (*a->p == '(') { a->p++; char *v = ac_assign(a); ac_ws(a); if (*a->p == ')') a->p++; else a->err = 1; return v; }
    if (isdigit((unsigned char)*a->p)) {
        char *e; const char *h = a->p; while (isdigit((unsigned char)*h)) h++; long v;
        if (*h == '#') { int base = atoi(a->p); a->p = h + 1; v = strtol(a->p, &e, base > 1 && base <= 36 ? base : 10); }
        else v = strtol(a->p, &e, 0);
        if (e == a->p) { a->err = 1; return xstrdup("0"); }
        a->p = e; char b[32]; snprintf(b, sizeof b, "%ld", v); return xstrdup(b);
    }
    LV lv; if (!ac_lvalue(a, &lv)) { a->err = 1; return xstrdup("0"); }
    char *v = lv_load(&lv); ac_ws(a);
    if ((a->p[0] == '+' && a->p[1] == '+') || (a->p[0] == '-' && a->p[1] == '-')) {
        char *n = ac_bin(a->p[0] == '+' ? "add" : "sub", v, "1"); lv_store(a, &lv, n); a->p += 2;
    }
    return v;
}
static char *ac_unary(AC *a) {
    ac_ws(a); char c = *a->p;
    if ((c == '+' && a->p[1] == '+') || (c == '-' && a->p[1] == '-')) {
        a->p += 2; LV lv; if (!ac_lvalue(a, &lv)) { a->err = 1; return xstrdup("0"); }
        char *v = lv_load(&lv), *n = ac_bin(c == '+' ? "add" : "sub", v, "1"); lv_store(a, &lv, n); return n;
    }
    if (c == '!') { a->p++; char *v = ac_unary(a); return ac_cmp("eq", v, "0"); }
    if (c == '~') { a->p++; char *v = ac_unary(a); return ac_bin("xor", v, "-1"); }
    if (c == '-') { a->p++; char *v = ac_unary(a); return ac_bin("sub", "0", v); }
    if (c == '+') { a->p++; return ac_unary(a); }
    return ac_primary(a);
}
static char *ac_pow(AC *a) {
    char *l = ac_unary(a); ac_ws(a);
    if (a->p[0] == '*' && a->p[1] == '*') { a->p += 2; char *r = ac_pow(a), *t = tv(); emit("%s = call i64 @bash_rt_pow(i64 %s, i64 %s)", t, l, r); return t; }
    return l;
}
static char *ac_mul(AC *a) {
    char *l = ac_pow(a);
    for (;;) { ac_ws(a); char c = *a->p;
        if ((c == '*' && a->p[1] != '*' && a->p[1] != '=') || ((c == '/' || c == '%') && a->p[1] != '=')) {
            a->p++; char *r = ac_pow(a);
            if (c == '*') l = ac_bin("mul", l, r); else { char *t = tv(); emit("%s = call i64 @bash_rt_%s(i64 %s, i64 %s)", t, c == '/' ? "div" : "mod", l, r); l = t; }
        } else return l; }
}
static char *ac_add(AC *a) {
    char *l = ac_mul(a);
    for (;;) { ac_ws(a); char c = *a->p;
        if ((c == '+' || c == '-') && a->p[1] != c && a->p[1] != '=') { a->p++; char *r = ac_mul(a); l = ac_bin(c == '+' ? "add" : "sub", l, r); } else return l; }
}
static char *ac_shift(AC *a) {
    char *l = ac_add(a);
    for (;;) { ac_ws(a);
        int left = a->p[0] == '<' && a->p[1] == '<' && a->p[2] != '=', right = a->p[0] == '>' && a->p[1] == '>' && a->p[2] != '=';
        if (!left && !right) return l;
        a->p += 2; char *r = ac_add(a), *m = ac_bin("and", r, "63"); l = ac_bin(left ? "shl" : "ashr", l, m); }
}
static char *ac_rel(AC *a) {
    char *l = ac_shift(a);
    for (;;) { ac_ws(a); const char *cc = NULL; int n = 0;
        if (a->p[0] == '<' && a->p[1] == '=') { cc = "sle"; n = 2; } else if (a->p[0] == '>' && a->p[1] == '=') { cc = "sge"; n = 2; }
        else if (a->p[0] == '<' && a->p[1] != '<') { cc = "slt"; n = 1; } else if (a->p[0] == '>' && a->p[1] != '>') { cc = "sgt"; n = 1; }
        if (!cc) return l;
        a->p += n; char *r = ac_shift(a); l = ac_cmp(cc, l, r); }
}
static char *ac_eq(AC *a) {
    char *l = ac_rel(a);
    for (;;) { ac_ws(a);
        if (a->p[0] == '=' && a->p[1] == '=') { a->p += 2; char *r = ac_rel(a); l = ac_cmp("eq", l, r); }
        else if (a->p[0] == '!' && a->p[1] == '=') { a->p += 2; char *r = ac_rel(a); l = ac_cmp("ne", l, r); }
        else return l; }
}
static char *ac_band(AC *a) { char *l = ac_eq(a); for (;;) { ac_ws(a); if (a->p[0] == '&' && a->p[1] != '&' && a->p[1] != '=') { a->p++; char *r = ac_eq(a); l = ac_bin("and", l, r); } else return l; } }
static char *ac_bxor(AC *a) { char *l = ac_band(a); for (;;) { ac_ws(a); if (a->p[0] == '^' && a->p[1] != '=') { a->p++; char *r = ac_band(a); l = ac_bin("xor", l, r); } else return l; } }
static char *ac_bor(AC *a) { char *l = ac_bxor(a); for (;;) { ac_ws(a); if (a->p[0] == '|' && a->p[1] != '|' && a->p[1] != '=') { a->p++; char *r = ac_bxor(a); l = ac_bin("or", l, r); } else return l; } }
static char *ac_logic(AC *a, int is_and, char *(*sub)(AC *)) {
    char *l = sub(a);
    for (;;) { ac_ws(a);
        if (!(a->p[0] == (is_and ? '&' : '|') && a->p[1] == a->p[0] && a->p[2] != '=')) return l;
        a->p += 2; int id = newlbl(), res = alloca_i64(); char *rhs = lname("ac.rhs", id), *en = lname("ac.end", id);
        char *c = tv(); emit("%s = icmp ne i64 %s, 0", c, l); emit("store i64 %s, ptr %%t%d", is_and ? "0" : "1", res);
        int cc = atoi(c + 2); cbr(cc, is_and ? rhs : en, is_and ? en : rhs);
        label(rhs); char *r = sub(a), *c2 = tv(), *z = tv(); emit("%s = icmp ne i64 %s, 0", c2, r); emit("%s = zext i1 %s to i64", z, c2); emit("store i64 %s, ptr %%t%d", z, res); br(en);
        label(en); l = tv(); emit("%s = load i64, ptr %%t%d", l, res);
    }
}
static char *ac_land(AC *a) { return ac_logic(a, 1, ac_bor); }
static char *ac_lor(AC *a) { return ac_logic(a, 0, ac_land); }
static char *ac_ternary(AC *a) {
    char *c = ac_lor(a); ac_ws(a);
    if (*a->p != '?') return c;
    a->p++; int id = newlbl(), res = alloca_i64(); char *t1 = lname("ac.t", id), *t2 = lname("ac.f", id), *en = lname("ac.te", id);
    char *cv = tv(); emit("%s = icmp ne i64 %s, 0", cv, c); cbr(atoi(cv + 2), t1, t2);
    label(t1); char *x = ac_assign(a); emit("store i64 %s, ptr %%t%d", x, res); ac_ws(a); if (*a->p == ':') a->p++; else a->err = 1; br(en);
    label(t2); char *y = ac_ternary(a); emit("store i64 %s, ptr %%t%d", y, res); br(en);
    label(en); char *r = tv(); emit("%s = load i64, ptr %%t%d", r, res); return r;
}
static char *ac_assign(AC *a) {
    ac_ws(a); const char *save = a->p; size_t mark = cur->body.n; int term = cur->terminated; LV lv;
    if (ac_lvalue(a, &lv)) {
        ac_ws(a); const char *p = a->p; char op = 0; int len = 0;
        if (p[0] == '=' && p[1] != '=') { op = '='; len = 1; }
        else if (strchr("+-*/%&|^", p[0]) && p[1] == '=') { op = p[0]; len = 2; }
        else if ((p[0] == '<' && p[1] == '<' && p[2] == '=') || (p[0] == '>' && p[1] == '>' && p[2] == '=')) { op = p[0] == '<' ? 'L' : 'R'; len = 3; }
        if (op) {
            a->p += len; char *r = ac_assign(a), *v = r;
            if (op != '=') { char *o = lv_load(&lv);
                switch (op) { case '+': v = ac_bin("add", o, r); break; case '-': v = ac_bin("sub", o, r); break; case '*': v = ac_bin("mul", o, r); break;
                    case '/': case '%': v = tv(); emit("%s = call i64 @bash_rt_%s(i64 %s, i64 %s)", v, op == '/' ? "div" : "mod", o, r); break;
                    case '&': v = ac_bin("and", o, r); break; case '|': v = ac_bin("or", o, r); break; case '^': v = ac_bin("xor", o, r); break;
                    case 'L': { char *m = ac_bin("and", r, "63"); v = ac_bin("shl", o, m); break; }
                    default: { char *m = ac_bin("and", r, "63"); v = ac_bin("ashr", o, m); } } }
            lv_store(a, &lv, v); return v;
        }
    }
    if (cur->body.s) { cur->body.n = mark; cur->body.s[mark] = 0; } cur->terminated = term;   /* not an assignment: rewind */
    a->p = save; a->err = 0; return ac_ternary(a);
}
static int arith_native(const char *text, char **val) {
    if (strpbrk(text, "`,\"'\\")) return 0;
    for (const char *q = strchr(text, '$'); q; q = strchr(q + 1, '$')) if (q[1] == '(' ) return 0;
    size_t mark = cur->body.n; int term = cur->terminated;
    AC a = { text, 0 }; char *v = ac_assign(&a); ac_ws(&a);
    if (a.err || *a.p || !text[strspn(text, " \t\n")]) {
        if (cur->body.s) { cur->body.n = mark; cur->body.s[mark] = 0; } cur->terminated = term; return 0;
    }
    *val = v; return 1;
}


/* ---------------------------------------------------------------- native [ ] / [[ ]]
 * Common tests become direct comparisons: integers are read from the variable's cached integer,
 * strings are compared via borrowed pointers.  Anything exotic returns 0 and the generic path is used. */
typedef struct { Word **t; int n, i, dbl, fail, uses_int; } TC;
static const char *tc_lit(TC *c, int k) { return k < c->n ? lit_name(c->t[k]) : NULL; }
static int is_cmp_int(const char *s) { return s && (!strcmp(s, "-eq") || !strcmp(s, "-ne") || !strcmp(s, "-lt") || !strcmp(s, "-le") || !strcmp(s, "-gt") || !strcmp(s, "-ge")); }
static int is_cmp_str(TC *c, const char *s) { return s && (!strcmp(s, "=") || !strcmp(s, "==") || !strcmp(s, "!=")) ; (void)c; }
static int is_bin_any(TC *c, const char *s) {
    static const char *o[] = { "=", "==", "!=", "-eq", "-ne", "-lt", "-le", "-gt", "-ge", "<", ">", "-nt", "-ot", "-ef", NULL };
    if (!s) return 0; for (int i = 0; o[i]; i++) if (!strcmp(s, o[i])) return 1; return c->dbl && !strcmp(s, "=~");
}
static int is_file_un(const char *s) { return s && s[0] == '-' && s[1] && !s[2] && strchr("efdrwxsLhpSbcgukOGNtv", s[1]); }
/* string operand: returns pointer operand; *own set when it must be freed */
static char *tc_str(TC *c, Word *w, int pat, int *own) {
    *own = 0; char *ct;
    if (w->n == 1 && w->p[0].kind == PK_VAR && is_ident(w->p[0].text) && !special_kind(w->p[0].text) && (w->p[0].quoted || c->dbl)) {
        char *t = tv(); emit("%s = call ptr @bash_rt_gets_e(i32 %d)", t, vid(w->p[0].text)); return t; }
    int allq = 1; for (int i = 0; i < w->n; i++) if (w->p[i].kind != PK_LIT) allq = 0;
    if (allq && w->n && !pat && word_const(w, &ct)) return xstrdup(S(ct));
    if (allq && w->n && pat && opd_const(w, 1, &ct)) return xstrdup(S(ct));
    if (!c->dbl) { for (int i = 0; i < w->n; i++) if (!w->p[i].quoted && w->p[i].kind != PK_LIT) { c->fail = 1; return xstrdup("null"); } }
    int sv = word_to_str(w, pat ? WB_PATTERN : WB_STR); *own = 1; char *t = xmalloc(24); snprintf(t, 24, "%%t%d", sv); return t;
}
static char *tc_int(TC *c, Word *w) {
    char *ct;
    if (word_const(w, &ct)) { char *e; long v = strtol(ct, &e, 10); if (*e || !*ct) { c->fail = 1; return xstrdup("0"); } char b[32]; snprintf(b, sizeof b, "%ld", v); return xstrdup(b); }
    if (w->n == 1 && w->p[0].kind == PK_VAR) {
        const char *nm = w->p[0].text; char *t = tv();
        if (isdigit((unsigned char)nm[0]) && atoi(nm) > 0) { emit("%s = call i64 @bash_rt_pos_i%s(i32 %d)", t, c->dbl ? "" : "_t", atoi(nm)); if (c->dbl) c->fail = 1; return t; }
        if (is_ident(nm) && !special_kind(nm)) { emit("%s = call i64 @bash_rt_geti%s(i32 %d)", t, c->dbl ? "" : "_t", vid(nm)); return t; }
        if (!strcmp(nm, "#")) { emit("%s = call i64 @bash_rt_special_i(i32 2)", t); return t; }
        if (!strcmp(nm, "?")) { emit("%s = call i64 @bash_rt_special_i(i32 1)", t); return t; }
    }
    if (w->n == 1 && w->p[0].kind == PK_ARITH) { char *v; if (arith_native(w->p[0].text, &v)) return v; }
    c->fail = 1; return xstrdup("0");
}
static char *tc_or(TC *c);
static char *tc_atom(TC *c) {
    int rem = c->n - c->i; const char *a0 = tc_lit(c, c->i);
    if (rem <= 0) { c->fail = 1; return xstrdup("0"); }
    if (a0 && !strcmp(a0, "(") && rem >= 2 && c->dbl) { c->i++; char *v = tc_or(c); if (tc_lit(c, c->i) && !strcmp(tc_lit(c, c->i), ")")) c->i++; else c->fail = 1; return v; }
    const char *a1 = rem >= 2 ? tc_lit(c, c->i + 1) : NULL;
    if (rem >= 3 && is_bin_any(c, a1)) {
        Word *lw = c->t[c->i], *rw = c->t[c->i + 2]; c->i += 3; char *r = tv();
        if (is_cmp_int(a1)) {
            if (!c->dbl) c->uses_int = 1;
            char *x = tc_int(c, lw), *y = tc_int(c, rw);
            const char *cc = !strcmp(a1, "-eq") ? "eq" : !strcmp(a1, "-ne") ? "ne" : !strcmp(a1, "-lt") ? "slt" : !strcmp(a1, "-le") ? "sle" : !strcmp(a1, "-gt") ? "sgt" : "sge";
            emit("%s = icmp %s i64 %s, %s", r, cc, x, y); return r;
        }
        if (is_cmp_str(c, a1)) {
            int neg = a1[0] == '!'; int own1, own2; char *x = tc_str(c, lw, 0, &own1);
            int glob = 0; if (c->dbl) for (int i = 0; i < rw->n; i++) if (rw->p[i].kind != PK_LIT || (!rw->p[i].quoted && strpbrk(rw->p[i].text, "*?[]\\"))) glob = 1;
            char *y = tc_str(c, rw, c->dbl && glob, &own2); char *m = tv();
            if (c->dbl && glob) emit("%s = call i32 @bash_rt_fnm(ptr %s, ptr %s)", m, y, x); else emit("%s = call i32 @bash_rt_streq(ptr %s, ptr %s)", m, x, y);
            if (own1) emit("call void @free(ptr %s)", x); if (own2) emit("call void @free(ptr %s)", y);
            emit("%s = icmp %s i32 %s, 0", r, neg ? "eq" : "ne", m); return r;
        }
        if (c->dbl && !strcmp(a1, "=~")) {
            char *pat; if (!(rw->n && word_const(rw, &pat))) { c->fail = 1; return xstrdup("0"); }
            for (int i = 0; i < rw->n; i++) if (rw->p[i].quoted) c->fail = 1;
            int own; char *x = tc_str(c, lw, 0, &own); char *m = tv();
            emit("%s = call i32 @bash_rt_regex(ptr %s, ptr %s)", m, x, S(pat)); if (own) emit("call void @free(ptr %s)", x);
            emit("%s = icmp ne i32 %s, 0", r, m); return r;
        }
        c->fail = 1; return xstrdup("0");
    }
    if (rem >= 2 && is_file_un(a0)) {
        Word *w = c->t[c->i + 1]; char op = a0[1]; c->i += 2; int own; char *x = tc_str(c, w, 0, &own); char *r = tv();
        if (op == 'v') { c->fail = 1; return r; }
        char *m = tv(); emit("%s = call i32 @bash_rt_ftest(i32 %d, ptr %s)", m, op, x); if (own) emit("call void @free(ptr %s)", x);
        emit("%s = icmp ne i32 %s, 0", r, m); return r;
    }
    if (rem >= 2 && a0 && (!strcmp(a0, "-z") || !strcmp(a0, "-n"))) {
        Word *w = c->t[c->i + 1]; c->i += 2; int own; char *x = tc_str(c, w, 0, &own); char *b = tv(), *r = tv();
        emit("%s = load i8, ptr %s", b, x); emit("%s = icmp %s i8 %s, 0", r, a0[1] == 'z' ? "eq" : "ne", b);
        if (own) emit("call void @free(ptr %s)", x); return r;
    }
    if (rem == 1 || c->dbl) {   /* lone string: true when non-empty */
        if (a0 && (is_bin_any(c, a0) || is_file_un(a0) || !strcmp(a0, "!") || !strcmp(a0, "(") || !strcmp(a0, "-z") || !strcmp(a0, "-n"))) { c->fail = 1; return xstrdup("0"); }
        Word *w = c->t[c->i++]; int own; char *x = tc_str(c, w, 0, &own); char *b = tv(), *r = tv();
        emit("%s = load i8, ptr %s", b, x); emit("%s = icmp ne i8 %s, 0", r, b); if (own) emit("call void @free(ptr %s)", x); return r;
    }
    c->fail = 1; return xstrdup("0");
}
static char *tc_not(TC *c) {
    const char *a0 = tc_lit(c, c->i);
    if (a0 && !strcmp(a0, "!") && c->n - c->i >= 2) { c->i++; char *v = tc_not(c), *r = tv(); emit("%s = xor i1 %s, true", r, v); return r; }
    return tc_atom(c);
}
static char *tc_and(TC *c) {
    char *l = tc_not(c);
    for (;;) { const char *o = tc_lit(c, c->i); if (!o || strcmp(o, c->dbl ? "&&" : "-a")) return l;
        c->i++; int id = newlbl(), res = alloca_i32(); char *rhs = lname("tc.rhs", id), *en = lname("tc.end", id), *lz = tv();
        emit("%s = zext i1 %s to i32", lz, l); emit("store i32 %s, ptr %%t%d", lz, res); cbr(atoi(l + 2), rhs, en);
        label(rhs); char *r = tc_not(c), *rz = tv(); emit("%s = zext i1 %s to i32", rz, r); emit("store i32 %s, ptr %%t%d", rz, res); br(en);
        label(en); char *ld = tv(), *b = tv(); emit("%s = load i32, ptr %%t%d", ld, res); emit("%s = icmp ne i32 %s, 0", b, ld); l = b; }
}
static char *tc_or(TC *c) {
    char *l = tc_and(c);
    for (;;) { const char *o = tc_lit(c, c->i); if (!o || strcmp(o, c->dbl ? "||" : "-o")) return l;
        c->i++; int id = newlbl(), res = alloca_i32(); char *rhs = lname("tc.orhs", id), *en = lname("tc.oend", id), *lz = tv();
        emit("%s = zext i1 %s to i32", lz, l); emit("store i32 %s, ptr %%t%d", lz, res); cbr(atoi(l + 2), en, rhs);
        label(rhs); char *r = tc_and(c), *rz = tv(); emit("%s = zext i1 %s to i32", rz, r); emit("store i32 %s, ptr %%t%d", rz, res); br(en);
        label(en); char *ld = tv(), *b = tv(); emit("%s = load i32, ptr %%t%d", ld, res); emit("%s = icmp ne i32 %s, 0", b, ld); l = b; }
}
/* emits the test, sets $?; *cond receives an i1 temp number (true == success) */
static int native_test(Node *n, int *cond) {
    Word **t; int cnt; int dbl = 0;
    if (n->type == N_DBL) { t = n->w; cnt = n->nw; dbl = 1; }
    else if (n->type == N_SIMPLE && n->nw >= 1 && !n->nr && !n->nas && !n->narr && lit_name(n->w[0]) && !is_user_fn(lit_name(n->w[0])) &&
             (!strcmp(lit_name(n->w[0]), "[") || !strcmp(lit_name(n->w[0]), "test"))) {
        int br_ = !strcmp(lit_name(n->w[0]), "["); if (br_) { if (n->nw < 2 || !lit_name(n->w[n->nw - 1]) || strcmp(lit_name(n->w[n->nw - 1]), "]")) return 0; }
        t = n->w + 1; cnt = n->nw - 1 - (br_ ? 1 : 0);
    } else return 0;
    if (cnt == 0 || n->nr) return 0;
    if (dbl && n->nr) return 0;
    size_t mark = cur->body.n; int term = cur->terminated;
    TC c = { t, cnt, 0, dbl, 0, 0 };
    for (int i = 0; i < cnt && !dbl; i++) if (is_cmp_int(lit_name(t[i]))) c.uses_int = 1;
    if (c.uses_int) emit("call void @bash_rt_test_err_clear()");
    char *v = tc_or(&c);
    if (c.fail || c.i != cnt) { if (cur->body.s) { cur->body.n = mark; cur->body.s[mark] = 0; } cur->terminated = term; return 0; }
    int st = newtmp(), cd = newtmp();
    if (c.uses_int) { emit("%%t%d = call i32 @bash_rt_test_status(i1 %s)", st, v); emit("call void @bash_rt_set_status(i32 %%t%d)", st); emit("%%t%d = icmp eq i32 %%t%d, 0", cd, st); }
    else { emit("%%t%d = select i1 %s, i32 0, i32 1", st, v); emit("call void @bash_rt_set_status(i32 %%t%d)", st); cd = atoi(v + 2); }
    *cond = cd; return 1;
}

static void errexit_check(void) { if (g_errexit && cur->in_cond == 0) emit("call void @bash_rt_errexit_check()"); }

static int g_in_func;
/* functions defined exactly once, at the top level of the script: called directly once defined */
static Node **g_dnode; static char **g_dname; static int g_ndirect;
static int direct_idx_node(Node *n) { for (int i = 0; i < g_ndirect; i++) if (g_dnode[i] == n) return i; return -1; }
static int direct_idx_name(const char *nm) { for (int i = 0; i < g_ndirect; i++) if (!strcmp(g_dname[i], nm)) return i; return -1; }
static void gen_trap(Node *n) {
    int first = (int)(intptr_t)n->s2; int mode = !strcmp(n->s1, "reset") ? 0 : !strcmp(n->s1, "ignore") ? 1 : 2;
    char *f = mode == 2 ? gen_block_fn(n->k[0]) : xstrdup("null");
    for (int i = first; i < n->nw; i++) { char *sg; word_const(n->w[i], &sg); emit("call void @bash_rt_trap(ptr %s, i32 %d, ptr %s)", S(sg), mode, f); }
    set_status_const(0);
}
/* local/declare/typeset/export/readonly whose arguments are all plain names or name=value words */
static int decl_assign_word(Word *w, char **name, Word **val, int *app) {
    if (!w->n || w->p[0].kind != PK_LIT || w->p[0].quoted) return 0;
    const char *t = w->p[0].text; size_t j = 0;
    if (!is_ident_start(t[0])) return 0;
    while (isalnum((unsigned char)t[j]) || t[j] == '_') j++;
    if (!t[j] && w->n == 1) { *name = xstrndup(t, j); *val = NULL; *app = 0; return 1; }
    int ap = 0; if (t[j] == '+' && t[j + 1] == '=') { ap = 1; j++; }
    if (t[j] != '=') return 0;
    *name = xstrndup(t, j - ap); *app = ap;
    Word *v = mkword(); const char *rest = t + j + 1;
    if (rest[0] == '~' && (!rest[1] || rest[1] == '/' || rest[1] == ':')) { Part hp = { 0 }; hp.kind = PK_VAR; hp.text = xstrdup("HOME"); hp.quoted = 1; add_part(v, hp); rest++; }
    if (*rest) add_lit(v, rest, strlen(rest), 1, 0);
    for (int k = 1; k < w->n; k++) { Part cp = w->p[k]; if (cp.kind == PK_LIT) cp.quoted = 1; PUSH(v->p, v->n, cp); }
    if (!v->n) add_lit(v, "", 0, 1, 1);
    *val = v; return 1;
}
static int gen_native_decl(Node *n, const char *name) {
    int loc = !strcmp(name, "local") || !strcmp(name, "declare") || !strcmp(name, "typeset"), ex = !strcmp(name, "export"), ro = !strcmp(name, "readonly");
    if (!loc && !ex && !ro) return 0;
    if (!strcmp(name, "local") && !g_in_func) return 0;
    if (n->nr || n->nas || n->nw + n->narr < 2) return 0;
    char **names = xmalloc(n->nw * sizeof *names); Word **vals = xmalloc(n->nw * sizeof *vals); int *apps = xmalloc(n->nw * sizeof(int));
    int asc = 0, first = 1;   /* leading literal options: -a -A -g -x -r (declare/local/typeset only) */
    for (; first < n->nw && loc; first++) {
        const char *t = lit_name(n->w[first]); if (!t || t[0] != '-' || !t[1] || n->w[first]->p[0].quoted) break;
        if (!strcmp(t, "--")) { first++; break; }
        for (const char *c = t + 1; *c; c++) { if (*c == 'A') asc = 1; else if (*c == 'g') { if (strcmp(name, "local")) loc = 0; else return 0; } else if (*c == 'x') ex = 1; else if (*c == 'r') ro = 1; else if (*c != 'a') return 0; }
    }
    if (!loc && !ex && !ro && !asc && first > 1) { loc = 0; }
    if (first >= n->nw && !n->narr) return 0;
    for (int i = first; i < n->nw; i++) if (!decl_assign_word(n->w[i], &names[i], &vals[i], &apps[i]) || special_kind(names[i])) return 0;
    for (int i = 0; i < n->narr; i++) if (!is_ident(n->arr[i].name)) return 0;
    int dolocal = loc && g_in_func;   /* declare at top level is just a global */
    for (int i = first; i < n->nw; i++) {
        int id = vid(names[i]);
        if (asc) { if (dolocal) emit("call void @bash_rt_local_id(i32 %d)", id); emit("call void @bash_rt_assoc_id(i32 %d)", id); }
        if (vals[i]) {
            Word *vw = vals[i]; int borrow = vw->n == 1 && vw->p[0].kind == PK_VAR && !apps[i];
            if (borrow) {   /* local x=$1 / local x=$y: the value is read before the local shadows anything */
                const char *nm = vw->p[0].text; int sk = special_kind(nm); char *op; int t = -1;
                if (sk) borrow = 0;
                else if (isdigit((unsigned char)nm[0])) { t = newtmp(); emit("%%t%d = call ptr @bash_rt_pos_u(i32 %d)", t, atoi(nm)); }
                else if (vid(nm) == id) borrow = 0;   /* local x=$x */
                else { t = newtmp(); emit("%%t%d = call ptr @bash_rt_gets_e(i32 %d)", t, vid(nm)); }
                if (borrow) {
                    if (dolocal && !asc) emit("call void @bash_rt_local_id(i32 %d)", id);
                    if (isdigit((unsigned char)nm[0])) emit("call void @bash_rt_set_id(i32 %d, ptr %%t%d, i32 0)", id, t);
                    else { (void)op; emit("call void @bash_rt_copy_id(i32 %d, i32 %d)", id, vid(nm)); }
                    goto decl_done;
                }
            }
            int sv = word_to_str(vals[i], WB_STR);
            if (dolocal && !asc) emit("call void @bash_rt_local_id(i32 %d)", id);
            emit("call void @bash_rt_set_id(i32 %d, ptr %%t%d, i32 %d)", id, sv, apps[i]); emit("call void @free(ptr %%t%d)", sv);
        }
        else if (dolocal && !asc) emit("call void @bash_rt_local_id(i32 %d)", id);
      decl_done:
        if (ex) emit("call void @bash_rt_export_id(i32 %d)", id);
        if (ro) emit("call void @bash_rt_readonly_id(i32 %d)", id);
    }
    for (int i = 0; i < n->narr; i++) { if (dolocal) emit("call void @bash_rt_local_id(i32 %d)", vid(n->arr[i].name));
        if (asc) emit("call void @bash_rt_assoc_id(i32 %d)", vid(n->arr[i].name)); gen_assign(&n->arr[i], 0);
        if (ex) emit("call void @bash_rt_export_id(i32 %d)", vid(n->arr[i].name)); if (ro) emit("call void @bash_rt_readonly_id(i32 %d)", vid(n->arr[i].name)); }
    set_status_const(0); return 1;
}
static void gen_simple(Node *n) {
    if (n->flag == 1) { RedirCtx rc = redir_begin(n->r, n->nr); gen_node(n->k[0]); redir_end(&rc); return; }   /* eval 'literal' */
    if (n->flag == 2) { gen_trap(n); return; }
    if (n->nw && lit_name(n->w[0]) && !is_user_fn(lit_name(n->w[0])) && gen_native_decl(n, lit_name(n->w[0]))) return;
    if (!n->nw) {
        if (!n->nas && !n->narr) { RedirCtx rc = redir_begin(n->r, n->nr); redir_end(&rc); set_status_const(0); return; }
        emit("call void @bash_rt_cmdsub_reset()");
        for (int i = 0; i < n->nas; i++) gen_assign(&n->as[i], 0);
        int t = newtmp(); emit("%%t%d = call i32 @bash_rt_cmdsub_status()", t); emit("call void @bash_rt_set_status(i32 %%t%d)", t);
        return;
    }
    const char *name = lit_name(n->w[0]);
    if (name && !strcmp(name, "exec") && n->nw == 1) {   /* exec >file : permanent redirections */
        if (n->nr) { emit("call void @bash_rt_redir_push()"); RedirCtx dummy = { 0 };
            for (int i = 0; i < n->nr; i++) { int s = word_to_str(n->r[i].target, WB_STR); emit("call void @bash_rt_redir(i32 %d, i32 %d, ptr %%t%d)", n->r[i].fd, n->r[i].op, s); emit("call void @free(ptr %%t%d)", s); }
            emit("call void @bash_rt_redir_commit()"); (void)dummy; }
        set_status_const(0); return;
    }
    if (name && (!strcmp(name, "break") || !strcmp(name, "continue")) && cur->nloops) {
        int lvl = 1; if (n->nw > 1 && lit_name(n->w[1])) lvl = atoi(lit_name(n->w[1])); if (lvl < 1) lvl = 1; if (lvl > cur->nloops) lvl = cur->nloops;
        Loop *l = &cur->loops[cur->nloops - lvl]; br(name[0] == 'b' ? l->brk : l->cont); return;
    }
    int mark = -1;
    if (n->nas) { mark = newtmp(); emit("%%t%d = call i32 @bash_rt_tmp_mark()", mark); for (int i = 0; i < n->nas; i++) gen_assign(&n->as[i], 1); }
    int b = new_wb(WB_ARGV); for (int i = 0; i < n->nw; i++) emit_word(b, n->w[i], WB_ARGV);
    int argc = newtmp(), argv = newtmp();
    emit("%%t%d = call i32 @bash_rt_wb_argc(ptr %%t%d)", argc, b); emit("%%t%d = call ptr @bash_rt_wb_argv(ptr %%t%d)", argv, b);
    if (n->narr) {
        int loc = name && (!strcmp(name, "local") || !strcmp(name, "declare") || !strcmp(name, "typeset"));
        for (int i = 0; i < n->narr; i++) { if (loc) emit("call void @bash_rt_declare_local(ptr %s)", S(n->arr[i].name)); gen_assign(&n->arr[i], 0); }
    }
    if (name && !strcmp(name, "return")) {
        int r = newtmp(); emit("%%t%d = call i32 @bash_rt_return_status(i32 %%t%d, ptr %%t%d)", r, argc, argv);
        emit("call void @bash_rt_wb_free(ptr %%t%d)", b); br(cur->ret_label); return;
    }
    /* read ... <<< word: hand the string to read directly (no memfd / dup2 / seek) */
    int hs = name && !strcmp(name, "read") && !is_user_fn(name) && n->nr == 1 && n->r[0].op == R_HERESTR && n->r[0].fd == 0;
    if (hs) { int s = word_to_str(n->r[0].target, WB_STR); emit("call void @bash_rt_read_src(ptr %%t%d)", s); }
    RedirCtx rc = redir_begin(n->r, hs ? 0 : n->nr);
    int r = newtmp(); int ti = (name && !is_user_fn(name)) ? tool_index(name) : -1;
    if (ti >= 0 && !strcmp(TOOLS[ti].prefix, "bash_external_")) {   /* embedded tool; unsupported options re-run the real program */
        used_tool[ti] = 1; emit("%%t%d = call i32 @bash_rt_tool_call(i32 %d, ptr @%s%s, i32 %%t%d, ptr %%t%d)", r, ti, TOOLS[ti].prefix, TOOLS[ti].id, argc, argv); }
    else if (ti >= 0) { used_tool[ti] = 1; emit("%%t%d = call i32 @%s%s(i32 %%t%d, ptr %%t%d)", r, TOOLS[ti].prefix, TOOLS[ti].id, argc, argv); }
    else if (name && is_user_fn(name) && direct_idx_name(name) >= 0) {
        int di = direct_idx_name(name), d = newtmp(), a = newtmp(), b2 = newtmp(), id = newlbl();
        char *ld = lname("uf.d", id), *ls = lname("uf.s", id), *le = lname("uf.e", id);
        emit("%%t%d = load i1, ptr @bash_ufd_%d", d, di); cbr(d, ld, ls);
        label(ld); emit("%%t%d = call i32 @bash_ufn_%d(i32 %%t%d, ptr %%t%d)", a, di, argc, argv); br(le);
        label(ls); emit("%%t%d = call i32 @bash_rt_run(i32 %%t%d, ptr %%t%d)", b2, argc, argv); br(le);
        label(le); emit("%%t%d = phi i32 [ %%t%d, %%%s ], [ %%t%d, %%%s ]", r, a, ld, b2, ls);
    }
    else emit("%%t%d = call i32 @bash_rt_run(i32 %%t%d, ptr %%t%d)", r, argc, argv);
    emit("call void @bash_rt_set_status(i32 %%t%d)", r);
    if (hs) emit("call void @bash_rt_read_src(ptr null)");
    redir_end(&rc);
    emit("call void @bash_rt_wb_free(ptr %%t%d)", b);
    if (mark >= 0) emit("call void @bash_rt_tmp_restore(i32 %%t%d)", mark);
}

static void gen_node(Node *n);
static int arith_cond(Node *n, int *c);
/* evaluates n as a condition: returns an i1 temp (true == status 0); $? is set as a side effect */
static int gen_cond(Node *n) {
    int c;
    cur->in_cond++;
    if (native_test(n, &c) || arith_cond(n, &c)) { cur->in_cond--; return c; }
    if (n->type == N_NOT && n->nk == 1) { int x = gen_cond(n->k[0]), r = newtmp(), st = newtmp(); cur->in_cond--;
        emit("%%t%d = xor i1 %%t%d, true", r, x); emit("%%t%d = select i1 %%t%d, i32 0, i32 1", st, r); emit("call void @bash_rt_set_status(i32 %%t%d)", st); return r; }
    gen_node(n); cur->in_cond--;
    int s = get_status(); c = newtmp(); emit("%%t%d = icmp eq i32 %%t%d, 0", c, s); return c;
}
static int arith_cond(Node *n, int *c) {
    if (n->type != N_ARITH || n->nr) return 0;
    char *nv; if (!arith_native(n->s1, &nv)) return 0;
    int z = newtmp(), st = newtmp(); emit("%%t%d = icmp ne i64 %s, 0", z, nv); emit("%%t%d = select i1 %%t%d, i32 0, i32 1", st, z); emit("call void @bash_rt_set_status(i32 %%t%d)", st); *c = z; return 1;
}
static Node **g_fnbodies; static char **g_fnnames; static int g_nfnb;
static int fn_body_idx(const char *nm) { for (int i = g_nfnb - 1; i >= 0; i--) if (!strcmp(g_fnnames[i], nm)) return i; return -1; }
static int word_safe(Word *w, int depth);
/* Can this pipeline stage run to completion on its own (in-process, output buffered for the next stage)?
 * Requires: every command known at compile time (builtin, embedded tool, or such a function), no loops that
 * could be infinite, and -- for the first stage -- nothing that reads the script's own stdin. */
static const char *STDIN_READERS[] = { "cat", "grep", "sort", "uniq", "tr", "cut", "head", "tail", "wc", "tee", "read", "mapfile", "readarray", NULL };
static int node_safe(Node *n, int first, int depth) {
    if (!n || depth > 8) return !n;
    switch (n->type) {
    case N_WHILE: case N_UNTIL: case N_CFOR: case N_BG: return 0;
    case N_SIMPLE: {
        for (int i = 0; i < n->nw; i++) if (!word_safe(n->w[i], depth)) return 0;
        for (int i = 0; i < n->nas; i++) if (!word_safe(n->as[i].val, depth)) return 0;
        if (n->flag == 1) return node_safe(n->k[0], first, depth + 1);
        if (!n->nw) return 1;
        const char *nm = lit_name(n->w[0]); if (!nm) return 0;
        if (is_user_fn(nm)) { int bi = fn_body_idx(nm); return bi >= 0 && node_safe(g_fnbodies[bi], first, depth + 1); }
        int ti = tool_index(nm); if (ti < 0) return 0;
        if (!strcmp(nm, "exec") || !strcmp(nm, "command") || !strcmp(nm, "builtin") || !strcmp(nm, "env") || !strcmp(nm, "sleep") || !strcmp(nm, "eval") || !strcmp(nm, "wait")) return 0;
        if (first && in_list(STDIN_READERS, nm)) {
            int files = 0; for (int i = 1; i < n->nw; i++) { const char *a = lit_name(n->w[i]); if (!a || a[0] != '-') files++; }
            int has_in = 0; for (int i = 0; i < n->nr; i++) if (n->r[i].fd == 0) has_in = 1;
            if (!has_in && (files == 0 || !strcmp(nm, "tr") || !strcmp(nm, "read") || !strcmp(nm, "mapfile") || !strcmp(nm, "readarray") || (!strcmp(nm, "grep") && files < 2))) return 0;
        }
        return 1; }
    default:
        for (int i = 0; i < n->nk; i++) if (!node_safe(n->k[i], first, depth)) return 0;
        for (int i = 0; i < n->nci; i++) if (!node_safe(n->ci[i].body, first, depth)) return 0;
        for (int i = 0; i < n->nw; i++) if (!word_safe(n->w[i], depth)) return 0;
        return 1;
    }
}
static int word_safe(Word *w, int depth) {
    if (!w) return 1;
    for (int i = 0; i < w->n; i++) { Part *p = &w->p[i];
        if ((p->kind == PK_CMD || p->kind == PK_PROC) && !node_safe(p->body, 0, depth + 1)) return 0;
        if (!word_safe(p->a, depth) || !word_safe(p->c, depth)) return 0; }
    return 1;
}
static void gen_pipe(Node *n) {
    int k = n->nk; char **names = xmalloc(k * sizeof *names);
    int inproc = 1; for (int i = 0; i < k - 1; i++) if (!node_safe(n->k[i], i == 0, 0)) inproc = 0;
    for (int i = 0; i < k; i++) names[i] = gen_block_fn(n->k[i]);
    int arr = newtmp(); sb_printf(&cur->allocas, "  %%t%d = alloca [%d x ptr]\n", arr, k);
    for (int i = 0; i < k; i++) { int p = newtmp(); emit("%%t%d = getelementptr inbounds [%d x ptr], ptr %%t%d, i64 0, i64 %d", p, k, arr, i); emit("store ptr %s, ptr %%t%d", names[i], p); }
    int r = newtmp(); emit("%%t%d = call i32 @bash_rt_pipeline%s(i32 %d, ptr %%t%d)", r, inproc ? "" : "_fork", k, arr); emit("call void @bash_rt_set_status(i32 %%t%d)", r);
}
static void gen_if(Node *n) {
    int id = newlbl(); char *th = lname("if.then", id), *el = lname("if.else", id), *en = lname("if.end", id);
    int c = gen_cond(n->k[0]); cbr(c, th, el);
    label(th); gen_node(n->k[1]); br(en);
    label(el); if (n->nk > 2) gen_node(n->k[2]); else set_status_const(0);
    label(en);
}
static void loop_exit_labels(int id, char **exit, char **brk, char **done) { *exit = lname("lp.exit", id); *brk = lname("lp.brk", id); *done = lname("lp.done", id); }
static void gen_while(Node *n) {
    int id = newlbl(), until = n->type == N_UNTIL; char *cond = lname("lp.cond", id), *body = lname("lp.body", id), *ex, *brk, *done; loop_exit_labels(id, &ex, &brk, &done);
    int last = alloca_i32(); emit("store i32 0, ptr %%t%d", last); br(cond);
    label(cond); int c = gen_cond(n->k[0]);
    if (until) { int c2 = newtmp(); emit("%%t%d = xor i1 %%t%d, true", c2, c); c = c2; }
    cbr(c, body, ex);
    label(body); cur->loops[cur->nloops].brk = brk; cur->loops[cur->nloops++].cont = cond;
    gen_node(n->k[1]); cur->nloops--;
    int ls = get_status(); emit("store i32 %%t%d, ptr %%t%d", ls, last); br(cond);
    label(ex); int v = newtmp(); emit("%%t%d = load i32, ptr %%t%d", v, last); emit("call void @bash_rt_set_status(i32 %%t%d)", v); br(done);
    label(brk); set_status_const(0); br(done); label(done);
}
static void gen_for(Node *n) {
    int id = newlbl(); char *head = lname("for.head", id), *body = lname("for.body", id), *incr = lname("for.incr", id), *ex, *brk, *done; loop_exit_labels(id, &ex, &brk, &done);
    int b = new_wb(WB_ARGV);
    if (n->flag) for (int i = 0; i < n->nw; i++) emit_word(b, n->w[i], WB_ARGV);
    else { Word *w = mkword(); Part p = { 0 }; p.kind = PK_VAR; p.text = xstrdup("@"); p.quoted = 1; add_part(w, p); emit_word(b, w, WB_ARGV); }
    int cnt = newtmp(), argv = newtmp(); emit("%%t%d = call i32 @bash_rt_wb_argc(ptr %%t%d)", cnt, b); emit("%%t%d = call ptr @bash_rt_wb_argv(ptr %%t%d)", argv, b);
    int idx = alloca_i32(), last = alloca_i32(); emit("store i32 0, ptr %%t%d", idx); emit("store i32 0, ptr %%t%d", last); br(head);
    label(head); int i0 = newtmp(), c = newtmp(); emit("%%t%d = load i32, ptr %%t%d", i0, idx); emit("%%t%d = icmp slt i32 %%t%d, %%t%d", c, i0, cnt); cbr(c, body, ex);
    label(body); int i1 = newtmp(), p = newtmp(), sv = newtmp();
    emit("%%t%d = load i32, ptr %%t%d", i1, idx); emit("%%t%d = getelementptr inbounds ptr, ptr %%t%d, i32 %%t%d", p, argv, i1); emit("%%t%d = load ptr, ptr %%t%d", sv, p);
    emit("call void @bash_rt_set_id(i32 %d, ptr %%t%d, i32 0)", vid(n->name), sv);
    cur->loops[cur->nloops].brk = brk; cur->loops[cur->nloops++].cont = incr;
    gen_node(n->k[0]); cur->nloops--;
    int ls = get_status(); emit("store i32 %%t%d, ptr %%t%d", ls, last); br(incr);
    label(incr); int i2 = newtmp(), i3 = newtmp(); emit("%%t%d = load i32, ptr %%t%d", i2, idx); emit("%%t%d = add i32 %%t%d, 1", i3, i2); emit("store i32 %%t%d, ptr %%t%d", i3, idx); br(head);
    label(ex); int v = newtmp(); emit("%%t%d = load i32, ptr %%t%d", v, last); emit("call void @bash_rt_set_status(i32 %%t%d)", v); br(done);
    label(brk); set_status_const(0); br(done);
    label(done); emit("call void @bash_rt_wb_free(ptr %%t%d)", b);
}
/* runtime (( )) evaluation: expands $-forms first when the text has them; returns i32 status temp */
static int arith_rt(const char *e, Word *w) {
    int t = newtmp();
    if (w) { int sv = word_to_str(w, WB_STR); emit("%%t%d = call i32 @bash_rt_arith(ptr %%t%d)", t, sv); emit("call void @free(ptr %%t%d)", sv); }
    else emit("%%t%d = call i32 @bash_rt_arith(ptr %s)", t, S(e));
    return t;
}
static void cfor_expr(const char *e, Word *w) { char *v; if (!arith_native(e, &v)) arith_rt(e, w); }
static int cfor_cond(const char *e, Word *w) {   /* i1: continue looping? */
    char *v;
    if (arith_native(e, &v)) { int c = newtmp(); emit("%%t%d = icmp ne i64 %s, 0", c, v); return c; }
    int t = arith_rt(e, w), c = newtmp(); emit("%%t%d = icmp eq i32 %%t%d, 0", c, t);
    return c;
}
static void gen_cfor(Node *n) {
    int id = newlbl(); char *head = lname("cf.head", id), *body = lname("cf.body", id), *incr = lname("cf.incr", id), *ex, *brk, *done; loop_exit_labels(id, &ex, &brk, &done);
    if (n->s1[strspn(n->s1, " \t")]) cfor_expr(n->s1, n->aw[0]);
    int last = alloca_i32(); emit("store i32 0, ptr %%t%d", last); br(head);
    label(head);
    if (n->s2[strspn(n->s2, " \t")]) { int c = cfor_cond(n->s2, n->aw[1]); cbr(c, body, ex); }
    else br(body);
    label(body); cur->loops[cur->nloops].brk = brk; cur->loops[cur->nloops++].cont = incr;
    gen_node(n->k[0]); cur->nloops--; int ls = get_status(); emit("store i32 %%t%d, ptr %%t%d", ls, last); br(incr);
    label(incr); if (n->s3[strspn(n->s3, " \t")]) cfor_expr(n->s3, n->aw[2]); br(head);
    label(ex); int v = newtmp(); emit("%%t%d = load i32, ptr %%t%d", v, last); emit("call void @bash_rt_set_status(i32 %%t%d)", v); br(done);
    label(brk); set_status_const(0); br(done); label(done);
}
static void gen_case(Node *n) {
    int id = newlbl(); char *end = lname("cs.end", id);
    int w = word_to_str(n->w[0], WB_STR);
    for (int i = 0; i < n->nci; i++) {
        CaseItem *it = &n->ci[i]; char *body = lname("cs.body", id * 1000 + i);
        char *next = i + 1 < n->nci ? lname("cs.item", id * 1000 + i + 1) : lname("cs.none", id);
        if (i == 0) { char *f = lname("cs.item", id * 1000); br(f); label(f); }
        for (int j = 0; j < it->np; j++) {
            int p = word_to_str(it->pats[j], WB_PATTERN), m = newtmp(), c = newtmp();
            emit("%%t%d = call i32 @bash_rt_case_match(ptr %%t%d, ptr %%t%d)", m, w, p); emit("call void @free(ptr %%t%d)", p); emit("%%t%d = icmp ne i32 %%t%d, 0", c, m);
            char *nx = j + 1 < it->np ? lname("cs.pat", id * 1000 + i * 100 + j + 1) : next; cbr(c, body, nx); if (j + 1 < it->np) label(nx);
        }
        label(body); gen_node(it->body); br(end);
        label(next);
    }
    if (!n->nci) { char *none = lname("cs.none", id); br(none); label(none); }
    set_status_const(0); br(end); label(end); emit("call void @free(ptr %%t%d)", w);
}
static void gen_dbl(Node *n) {
    int k = n->nw; int arr = newtmp(); sb_printf(&cur->allocas, "  %%t%d = alloca [%d x ptr]\n", arr, k + 2);
    int p0 = newtmp(); emit("%%t%d = getelementptr inbounds [%d x ptr], ptr %%t%d, i64 0, i64 0", p0, k + 2, arr); emit("store ptr %s, ptr %%t%d", S("[["), p0);
    int *strs = xmalloc((k + 1) * sizeof(int));
    for (int i = 0; i < k; i++) { strs[i] = word_to_str(n->w[i], WB_PATTERN); int p = newtmp(); emit("%%t%d = getelementptr inbounds [%d x ptr], ptr %%t%d, i64 0, i64 %d", p, k + 2, arr, i + 1); emit("store ptr %%t%d, ptr %%t%d", strs[i], p); }
    int pn = newtmp(); emit("%%t%d = getelementptr inbounds [%d x ptr], ptr %%t%d, i64 0, i64 %d", pn, k + 2, arr, k + 1); emit("store ptr null, ptr %%t%d", pn);
    used_tool[tool_index("[[")] = 1;
    RedirCtx rc = redir_begin(n->r, n->nr);
    int r = newtmp(); emit("%%t%d = call i32 @bash_internal_dblbracket(i32 %d, ptr %%t%d)", r, k + 1, arr); emit("call void @bash_rt_set_status(i32 %%t%d)", r);
    redir_end(&rc);
    for (int i = 0; i < k; i++) emit("call void @free(ptr %%t%d)", strs[i]);
}
static void gen_func(Node *n) {
    Fn f, *saved = cur; begin_fn(&f); cur = &f; int id = g_blk++;
    emit("call void @bash_rt_fn_enter(i32 %%argc, ptr %%argv)");
    g_in_func++; gen_node(n->k[0]); g_in_func--; br("Lret"); label("Lret");
    emit("call void @bash_rt_fn_leave()"); int t = newtmp(); emit("%%t%d = call i32 @bash_rt_get_status()", t); emit_term("ret i32 %%t%d", t);
    char name[256], hdr[300]; int di = direct_idx_node(n);
    if (di >= 0) snprintf(name, sizeof name, "@bash_ufn_%d", di);
    else { snprintf(name, sizeof name, "@bash_fn_%d_", id);
           for (const char *p = n->name; *p && strlen(name) < 200; p++) { char c[2] = { isalnum((unsigned char)*p) ? *p : '_', 0 }; strcat(name, c); } }
    snprintf(hdr, sizeof hdr, "define internal i32 %s(i32 %%argc, ptr %%argv) ", name);
    flush_fn(&f, hdr); cur = saved;
    emit("call void @bash_rt_defun(ptr %s, ptr %s)", S(n->name), name);
    if (di >= 0) emit("store i1 true, ptr @bash_ufd_%d", di);
    set_status_const(0);
}
static void gen_inner(Node *n) {
    switch (n->type) {
    case N_SIMPLE: { int c; if (native_test(n, &c)) break; gen_simple(n); break; }
    case N_LIST: for (int i = 0; i < n->nk; i++) gen_node(n->k[i]); if (!n->nk) set_status_const(0); break;
    case N_PIPE: gen_pipe(n); break;
    case N_AND: case N_OR: {
        int id = newlbl(); char *rhs = lname("ao.rhs", id), *en = lname("ao.end", id);
        int c = gen_cond(n->k[0]);
        if (n->type == N_OR) { int c2 = newtmp(); emit("%%t%d = xor i1 %%t%d, true", c2, c); c = c2; }
        cbr(c, rhs, en);
        label(rhs); gen_node(n->k[1]); br(en); label(en); break; }
    case N_NOT: { cur->in_cond++; gen_node(n->k[0]); cur->in_cond--; int s = get_status(), z = newtmp(), v = newtmp();
        emit("%%t%d = icmp eq i32 %%t%d, 0", z, s); emit("%%t%d = select i1 %%t%d, i32 1, i32 0", v, z); emit("call void @bash_rt_set_status(i32 %%t%d)", v); break; }
    case N_IF: { RedirCtx rc = redir_begin(n->r, n->nr); gen_if(n); redir_end(&rc); break; }
    case N_WHILE: case N_UNTIL: { RedirCtx rc = redir_begin(n->r, n->nr); gen_while(n); redir_end(&rc); break; }
    case N_FOR: { RedirCtx rc = redir_begin(n->r, n->nr); gen_for(n); redir_end(&rc); break; }
    case N_CFOR: { RedirCtx rc = redir_begin(n->r, n->nr); gen_cfor(n); redir_end(&rc); break; }
    case N_CASE: { RedirCtx rc = redir_begin(n->r, n->nr); gen_case(n); redir_end(&rc); break; }
    case N_SUB: { char *f = gen_block_fn(n->k[0]); RedirCtx rc = redir_begin(n->r, n->nr); int r = newtmp();
        emit("%%t%d = call i32 @bash_rt_subshell(ptr %s)", r, f); emit("call void @bash_rt_set_status(i32 %%t%d)", r); redir_end(&rc); break; }
    case N_GROUP: { RedirCtx rc = redir_begin(n->r, n->nr); gen_node(n->k[0]); redir_end(&rc); break; }
    case N_FUNC: gen_func(n); break;
    case N_BG: { char *f = gen_block_fn(n->k[0]); emit("call void @bash_rt_background(ptr %s)", f); set_status_const(0); break; }
    case N_DBL: { int c; if (native_test(n, &c)) break; gen_dbl(n); break; }
    case N_ARITH: { RedirCtx rc = redir_begin(n->r, n->nr); int r = newtmp(); char *nv;
        if (arith_native(n->s1, &nv)) { int z = newtmp(); emit("%%t%d = icmp ne i64 %s, 0", z, nv); emit("%%t%d = select i1 %%t%d, i32 0, i32 1", r, z); }
        else { int t = arith_rt(n->s1, n->aw[0]); emit("%%t%d = add i32 %%t%d, 0", r, t); } emit("call void @bash_rt_set_status(i32 %%t%d)", r); redir_end(&rc); break; }
    }
}
static void gen_node(Node *n) {
    gen_inner(n);
    if (n->type == N_SIMPLE || n->type == N_PIPE || n->type == N_SUB || n->type == N_DBL || n->type == N_ARITH) errexit_check();
}

/* pre-pass: user functions, set -e, literal eval/trap bodies, and detection of anything that cannot be
 * compiled exactly (-> whole-script fallback to an embedded bash) */
static const char *FALLBACK_CMDS[] = { "source", ".", "jobs", "fg", "bg", "disown", "suspend", "shopt", "alias", "unalias",
    "getopts", "type", "hash", "compgen", "complete", "compopt", "enable", "caller", "pushd", "popd", "dirs", "ulimit", "umask",
    "times", "history", "fc", "bind", "logout", "help", "coproc", "select", NULL };
static const char *FALLBACK_VARS[] = { "BASH_LINENO", "LINENO", "PIPESTATUS", "BASHPID", "EPOCHSECONDS",
    "EPOCHREALTIME", "BASH_VERSINFO", "BASH_VERSION", "GROUPS", "DIRSTACK", "BASH_ARGV", "BASH_ARGC", "BASH_COMMAND", "BASH_SUBSHELL",
    "HISTCMD", "SHELLOPTS", "BASHOPTS", "OPTIND", "OPTARG", "COPROC", "BASH_ALIASES", "BASH_CMDS", "BASH_EXECUTION_STRING",
    "FUNCNEST", "PROMPT_COMMAND", "BASH_XTRACEFD", "TIMEFORMAT", "SRANDOM", "BASH_ARGV0", "BASH_COMPAT", "BASH_ENV", NULL };
static void check_var(const char *name) {
    size_t n = strcspn(name, "["); char b[128]; if (n >= sizeof b) return; memcpy(b, name, n); b[n] = 0;
    if (in_list(FALLBACK_VARS, b)) unsupported("$%s", b);
}
static void check_arith_text(const char *t) {   /* identifiers mentioned inside (( )) / $(( )) */
    for (const char *p = t; *p; ) {
        if (isalpha((unsigned char)*p) || *p == '_') { const char *st = p; while (isalnum((unsigned char)*p) || *p == '_') p++;
            char b[128]; size_t n = p - st; if (n < sizeof b) { memcpy(b, st, n); b[n] = 0; if (in_list(FALLBACK_VARS, b)) unsupported("$%s", b); } }
        else p++;
    }
}
static void walk(Node *n);
static void walk_word(Word *w) {
    if (!w) return;
    for (int i = 0; i < w->n; i++) {
        Part *p = &w->p[i];
        if (p->kind == PK_VAR || p->kind == PK_PARAM) check_var(p->text);
        if (p->kind == PK_CMD || p->kind == PK_PROC) walk(p->body);
        if (p->kind == PK_ARITH) { check_arith_text(p->text); walk_word(p->a); }
        if (p->kind == PK_PARAM) { walk_word(p->a); walk_word(p->c); if (strchr(p->text, '[')) check_arith_text(strchr(p->text, '[')); }
    }
}
static int sig_ok(const char *s) {
    static const char *ok[] = { "EXIT", "INT", "TERM", "HUP", "QUIT", "USR1", "USR2", "PIPE", "ALRM", "CHLD", "WINCH", "CONT", "TSTP", NULL };
    if (!strncmp(s, "SIG", 3)) s += 3;
    if (!strcmp(s, "0")) return 1;
    if (isdigit((unsigned char)s[0])) { int v = atoi(s); return v > 0 && v < 32 && v != 9 && v != 19; }
    return in_list(ok, s);
}
static Node *parse_text(const char *text);
static int words_const(Node *n, int from, int to) { char *t; for (int i = from; i < to; i++) if (!word_const(n->w[i], &t)) return 0; return 1; }
static void walk_simple(Node *n) {
    const char *name = lit_name(n->w[0]);
    if (!name) return;
    if (is_user_fn(name)) return;
    if (in_list(FALLBACK_CMDS, name)) { unsupported("'%s' builtin", name); return; }
    char *t;
    if (!strcmp(name, "eval")) {
        if (n->nas || !words_const(n, 1, n->nw)) { unsupported("eval of a non-literal string"); return; }
        SB code = { 0 }; for (int i = 1; i < n->nw; i++) { word_const(n->w[i], &t); if (i > 1) sb_putc(&code, ' '); sb_puts(&code, t); }
        n->flag = 1; Node *body = parse_text(code.s ? code.s : ""); n->k = NULL; n->nk = 0; PUSH(n->k, n->nk, body); walk(body); return;
    }
    if (!strcmp(name, "trap")) {
        if (n->nw < 2 || !words_const(n, 1, n->nw)) { unsupported("trap without a literal action"); return; }
        word_const(n->w[1], &t);
        int first_sig = 2; Node *body = NULL; int reset = 0;
        if (!strcmp(t, "-")) reset = 1;
        else if (n->nw == 2 && sig_ok(t)) { reset = 1; first_sig = 1; }
        else if (t[0] == '-') { unsupported("trap %s", t); return; }
        else if (*t) body = parse_text(t);
        if (first_sig >= n->nw) { unsupported("trap without signals"); return; }
        for (int i = first_sig; i < n->nw; i++) { char *sg; word_const(n->w[i], &sg); if (!sig_ok(sg)) { unsupported("trap on %s", sg); return; } }
        n->flag = 2; n->k = NULL; n->nk = 0; PUSH(n->k, n->nk, body); n->s1 = (char *)(reset ? "reset" : body ? "run" : "ignore"); n->s2 = (char *)(intptr_t)first_sig;
        if (body) walk(body); return;
    }
    if (!strcmp(name, "declare") || !strcmp(name, "local") || !strcmp(name, "typeset") || !strcmp(name, "readonly") || !strcmp(name, "export")) {
        int ex = !strcmp(name, "export"), any = 0;
        for (int i = 1; i < n->nw; i++) {
            if (!word_const(n->w[i], &t)) { any = 1; continue; }
            if ((t[0] == '-' || t[0] == '+') && t[1]) { if (!strcmp(t, "--")) continue; for (const char *c = t + 1; *c; c++) if (!strchr(ex ? "n" : "agxrA", *c)) { unsupported("%s %s", name, t); return; } }
            else any = 1;
        }
        int asc = 0;   /* declare -A names (must be literal) */
        for (int i = 1; i < n->nw; i++) {
            if (!word_const(n->w[i], &t)) { if (asc) { unsupported("%s -A with a computed name", name); return; } continue; }
            if (t[0] == '-' && t[1]) { if (strchr(t, 'A')) asc = 1; continue; }
            if (t[0] == '+' && strchr(t, 'A')) { unsupported("%s +A", name); return; }
            if (asc) { size_t l = strcspn(t, "=+["); char *nm = xstrndup(t, l); add_assoc(nm); free(nm); }
        }
        if (asc) for (int i = 0; i < n->narr; i++) add_assoc(n->arr[i].name);
        if (!any && !n->narr) unsupported("%s without names (prints variables)", name);
        return;
    }
    if (!strcmp(name, "set")) {
        for (int i = 1; i < n->nw; i++) {
            if (!word_const(n->w[i], &t)) break;
            if (!strcmp(t, "--") || !strcmp(t, "-")) break;
            if ((t[0] != '-' && t[0] != '+') || !t[1]) break;
            int wants_o = 0;
            for (const char *c = t + 1; *c; c++) { if (*c == 'o') { wants_o = 1; continue; } if (!strchr("eu", *c)) { unsupported("set %c%c", t[0], *c); return; } }
            if (wants_o) { char *on; if (i + 1 < n->nw && word_const(n->w[i + 1], &on) && (!strcmp(on, "pipefail") || !strcmp(on, "errexit") || !strcmp(on, "nounset"))) { i++; continue; } unsupported("set -o option"); return; }
        }
        if (n->nw == 1) unsupported("set without arguments");
        return;
    }
    if (!strcmp(name, "read")) {
        for (int i = 1; i < n->nw; i++) { if (!word_const(n->w[i], &t)) break; if (t[0] != '-' || !t[1]) break; if (!strcmp(t, "--")) break;
            for (const char *c = t + 1; *c; c++) { if (!strchr("rsadpnNtu", *c)) { unsupported("read -%c", *c); return; } if (strchr("adpnNtu", *c)) { if (!c[1]) i++; break; } } }
        return;
    }
    if (!strcmp(name, "mapfile") || !strcmp(name, "readarray")) {
        for (int i = 1; i < n->nw; i++) { if (!word_const(n->w[i], &t)) break; if (t[0] != '-' || !t[1]) break;
            for (const char *c = t + 1; *c; c++) { if (!strchr("tdns", *c)) { unsupported("%s -%c", name, *c); return; } if (strchr("dns", *c)) { if (!c[1]) i++; break; } } }
        return;
    }
    if (!strcmp(name, "unset")) { for (int i = 1; i < n->nw; i++) if (word_const(n->w[i], &t) && t[0] == '-' && strcmp(t, "-v")) { unsupported("unset %s", t); return; } return; }
    if (!strcmp(name, "printf") && n->nw > 1) { for (int i = 1; i < n->nw; i++) if (word_const(n->w[i], &t) && strstr(t, "%(")) { unsupported("printf %%(...)T"); return; } return; }
    if (!strcmp(name, "wait")) { for (int i = 1; i < n->nw; i++) if (word_const(n->w[i], &t) && t[0] == '-') { unsupported("wait %s", t); return; } return; }
}
static void walk(Node *n) {
    if (!n) return;
    if (n->type == N_FUNC) { if (!is_user_fn(n->name)) PUSH(g_userfns, g_nuserfns, n->name); PUSH(g_fnbodies, g_nfnb, n->k[0]); g_nfnb--; PUSH(g_fnnames, g_nfnb, n->name); }
    if (n->type == N_SIMPLE && n->nw && lit_name(n->w[0]) && !strcmp(lit_name(n->w[0]), "set"))
        for (int i = 1; i < n->nw; i++) { const char *t = lit_name(n->w[i]); if (t && ((t[0] == '-' && t[1] != '-' && strchr(t, 'e')) || !strcmp(t, "errexit"))) g_errexit = 1; }
    for (int i = 0; i < n->nw; i++) walk_word(n->w[i]);
    for (int i = 0; i < n->nas; i++) { walk_word(n->as[i].val); for (int j = 0; j < n->as[i].nelems; j++) walk_word(n->as[i].elems[j]); }
    for (int i = 0; i < n->narr; i++) { walk_word(n->arr[i].val); for (int j = 0; j < n->arr[i].nelems; j++) walk_word(n->arr[i].elems[j]); }
    for (int i = 0; i < n->nr; i++) walk_word(n->r[i].target);
    for (int i = 0; i < n->nci; i++) for (int j = 0; j < n->ci[i].np; j++) walk_word(n->ci[i].pats[j]);
    for (int i = 0; i < 3; i++) walk_word(n->aw[i]);
    if (n->type == N_ARITH || n->type == N_CFOR) { if (n->s1) check_arith_text(n->s1); if (n->s2) check_arith_text(n->s2); if (n->s3) check_arith_text(n->s3); }
    if (n->type == N_SIMPLE && n->nw && n->flag == 0) walk_simple(n);
    if (n->type == N_SIMPLE && n->flag) return;    /* eval/trap bodies were walked when parsed */
    for (int i = 0; i < n->nk; i++) walk(n->k[i]);
    for (int i = 0; i < n->nci; i++) walk(n->ci[i].body);
}

static const char *DECLS =
"declare i64 @bash_rt_geti_t(i32)\ndeclare i64 @bash_rt_pos_i_t(i32)\ndeclare ptr @bash_rt_gets_e(i32)\ndeclare i32 @bash_rt_streq(ptr, ptr)\ndeclare i32 @bash_rt_fnm(ptr, ptr)\ndeclare i32 @bash_rt_regex(ptr, ptr)\ndeclare i32 @bash_rt_ftest(i32, ptr)\ndeclare void @bash_rt_test_err_clear()\ndeclare i32 @bash_rt_test_status(i1)\ndeclare i64 @bash_rt_geti(i32)\ndeclare void @bash_rt_seti(i32, i64)\ndeclare i64 @bash_rt_geti_elem(i32, i64)\ndeclare void @bash_rt_seti_elem(i32, i64, i64)\ndeclare i64 @bash_rt_special_i(i32)\ndeclare i64 @bash_rt_div(i64, i64)\ndeclare i64 @bash_rt_mod(i64, i64)\ndeclare i64 @bash_rt_pow(i64, i64)\ndeclare void @bash_rt_wb_int(ptr, i64, i32)\ndeclare void @bash_rt_init(i32, ptr, ptr, i32)\ndeclare void @bash_rt_wb_var_id(ptr, i32, i32)\ndeclare void @bash_rt_wb_pos(ptr, i32, i32)\ndeclare void @bash_rt_wb_special(ptr, i32, i32)\ndeclare void @bash_rt_wb_param_id(ptr, i32, i32, i64, i32, ptr, ptr, ptr, ptr, i32)\ndeclare void @bash_rt_set_id(i32, ptr, i32)\ndeclare void @bash_rt_set_array_id(i32, i32, ptr, i32)\ndeclare void @bash_rt_tmp_set_id(i32, ptr)\ndeclare void @bash_rt_exit(i32)\ndeclare ptr @bash_rt_wb_new(i32)\n"
"declare void @bash_rt_wb_lit(ptr, ptr, i32)\ndeclare void @bash_rt_wb_const(ptr, ptr)\ndeclare void @bash_rt_wb_var(ptr, ptr, i32)\n"
"declare void @bash_rt_wb_param(ptr, ptr, i32, ptr, ptr, i32)\ndeclare void @bash_rt_wb_cmdsub(ptr, ptr, i32)\ndeclare void @bash_rt_wb_procsub(ptr, ptr)\n"
"declare void @bash_rt_wb_arith(ptr, ptr, i32)\ndeclare void @bash_rt_wb_end(ptr)\ndeclare i32 @bash_rt_wb_argc(ptr)\ndeclare ptr @bash_rt_wb_argv(ptr)\n"
"declare ptr @bash_rt_wb_str(ptr)\ndeclare void @bash_rt_wb_free(ptr)\ndeclare i32 @bash_rt_cmdsub_status()\ndeclare void @bash_rt_cmdsub_reset()\n"
"declare void @bash_rt_set_status(i32)\ndeclare i32 @bash_rt_get_status()\ndeclare void @bash_rt_errexit_check()\n"
"declare void @bash_rt_set(ptr, ptr, i32)\ndeclare void @bash_rt_set_array(ptr, i32, ptr, i32)\ndeclare void @bash_rt_tmp_set(ptr, ptr)\n"
"declare i32 @bash_rt_tmp_mark()\ndeclare void @bash_rt_tmp_restore(i32)\ndeclare i32 @bash_rt_arith(ptr)\ndeclare void @bash_rt_defun(ptr, ptr)\n"
"declare i32 @bash_rt_run(i32, ptr)\ndeclare void @bash_rt_fn_enter(i32, ptr)\ndeclare void @bash_rt_fn_leave()\ndeclare i32 @bash_rt_return_status(i32, ptr)\n"
"declare void @bash_rt_redir_push()\ndeclare void @bash_rt_redir(i32, i32, ptr)\ndeclare i32 @bash_rt_redir_ok()\ndeclare void @bash_rt_redir_pop()\n"
"declare void @bash_rt_redir_commit()\ndeclare i32 @bash_rt_subshell(ptr)\ndeclare i32 @bash_rt_pipeline(i32, ptr)\n"
"declare i32 @bash_rt_case_match(ptr, ptr)\ndeclare void @bash_rt_declare_local(ptr)\ndeclare void @free(ptr)\n"
"declare i32 @bash_rt_pipeline_fork(i32, ptr)\ndeclare void @bash_rt_background(ptr)\ndeclare void @bash_rt_trap(ptr, i32, ptr)\n"
"declare i32 @bash_rt_tool_ret(i32, i32, ptr)\ndeclare i32 @bash_rt_tool_call(i32, ptr, i32, ptr)\ndeclare void @bash_rt_read_src(ptr)\ndeclare ptr @bash_rt_pos_u(i32)\ndeclare void @bash_rt_copy_id(i32, i32)\ndeclare ptr @bash_rt_special_s(i32)\ndeclare void @bash_rt_assoc_id(i32)\ndeclare void @bash_rt_aset(i32, ptr, ptr, i32)\ndeclare i64 @bash_rt_ageti(i32, ptr)\ndeclare void @bash_rt_aseti(i32, ptr, i64)\ndeclare void @bash_rt_wb_aparam(ptr, i32, ptr, i32, ptr, ptr, ptr, ptr, i32)\ndeclare ptr @bash_rt_keytmp(ptr)\ndeclare void @bash_rt_arr_begin(i32, i32)\ndeclare void @bash_rt_arr_kv(i32, ptr, ptr, i32)\ndeclare void @bash_rt_arr_push(i32, i32, ptr)\ndeclare void @bash_rt_arr_end(i32)\ndeclare i64 @bash_rt_arr_len(i32)\ndeclare i64 @bash_rt_strlen_id(i32)\ndeclare i64 @bash_rt_pos_i(i32)\n"
"declare void @bash_rt_set_elem_id(i32, i64, ptr, i32)\ndeclare void @bash_rt_local_id(i32)\ndeclare void @bash_rt_export_id(i32)\ndeclare void @bash_rt_readonly_id(i32)\n"
"declare void @bash_rt_wb_substr_id(ptr, i32, i64, i64, i32, i32)\n";

static void ir_bytes(SB *o, const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) { if (*p >= 32 && *p < 127 && *p != '"' && *p != '\\') sb_putc(o, *p); else sb_printf(o, "\\%02X", *p); }
}
/* the script runs on an embedded copy of its source through the system bash: exact semantics, no native speedup */
static char *compile_fallback(const char *src, const char *srcname) {
    SB out = { 0 };
    sb_printf(&out, "; generated by bashful from %s (bash fallback)\nsource_filename = \"%s\"\n\n", srcname, srcname);
    sb_printf(&out, "@script = private unnamed_addr constant [%zu x i8] c\"", strlen(src) + 1); ir_bytes(&out, src); sb_puts(&out, "\\00\", align 1\n");
    sb_puts(&out, "declare i32 @bash_rt_fallback(i32, ptr, ptr)\n\ndefine i32 @main(i32 %argc, ptr %argv) {\n  %r = call i32 @bash_rt_fallback(i32 %argc, ptr %argv, ptr @script)\n  ret i32 %r\n}\n");
    return out.s;
}
static char *compile(const char *src, const char *srcname) {
    g_fb_armed = 1;
    if (setjmp(g_fb_jb)) return NULL;
    Parser P = { src, 0, NULL, 0 }; Node *root = parse_list(&P);
    skip_ws(&P, 1); if (P.s[P.i]) perr(&P, "unexpected token");
    walk(root);
    g_fb_armed = 0;
    for (int i = 0; i < root->nk; i++) if (root->k[i]->type == N_FUNC) {
        int defs = 0; for (int j = 0; j < g_nfnb; j++) if (!strcmp(g_fnnames[j], root->k[i]->name)) defs++;
        if (defs == 1) { PUSH(g_dnode, g_ndirect, root->k[i]); g_ndirect--; PUSH(g_dname, g_ndirect, root->k[i]->name); }
    }
    if (g_unsup[0]) return NULL;
    Fn f; begin_fn(&f); cur = &f;
    emit("call void @bash_rt_init(i32 %%argc, ptr %%argv, ptr @bash_names, i32 NNAMES__)");
    gen_node(root); br("Lret"); label("Lret");
    int t = newtmp(); emit("%%t%d = call i32 @bash_rt_get_status()", t); emit("call void @bash_rt_exit(i32 %%t%d)", t); emit_term("ret i32 %%t%d", t);
    flush_fn(&f, "define i32 @main(i32 %argc, ptr %argv) ");
    { char *p = strstr(g_funcs.s, "NNAMES__"); char num[16]; snprintf(num, sizeof num, "%-8d", g_nvn); memcpy(p, num, 8); }
    SB out = { 0 };
    sb_printf(&out, "; generated by bashful from %s\nsource_filename = \"%s\"\n\n", srcname, srcname);
    sb_puts(&out, DECLS);
    for (int i = 0; TOOLS[i].name; i++) if (used_tool[i]) sb_printf(&out, "declare i32 @%s%s(i32, ptr)\n", TOOLS[i].prefix, TOOLS[i].id);
    sb_putc(&out, '\n');
    { SB nm = { 0 }; sb_printf(&nm, "@bash_names = private constant [%d x ptr] [", g_nvn ? g_nvn : 1);
      for (int i = 0; i < g_nvn; i++) sb_printf(&nm, "%sptr %s", i ? ", " : "", S(g_vn[i])); if (!g_nvn) sb_puts(&nm, "ptr null");
      sb_puts(&nm, "]\n"); sb_puts(&out, g_strs.s); sb_puts(&out, "\n"); sb_puts(&out, nm.s); }
    for (int i = 0; i < g_ndirect; i++) sb_printf(&out, "@bash_ufd_%d = internal global i1 false\n", i);
    sb_puts(&out, g_funcs.s); return out.s;
}

/* ================================================================== driver */
static char *slurp(const char *path) {
    FILE *f = strcmp(path, "-") ? fopen(path, "rb") : stdin; if (!f) die("cannot open %s", path);
    SB b = { 0 }; char buf[65536]; size_t n; while ((n = fread(buf, 1, sizeof buf, f)) > 0) sb_put(&b, buf, n);
    if (f != stdin) fclose(f); return b.s ? b.s : xstrdup("");
}
static int run(const char *fmt, ...) { char cmd[8192]; va_list ap; va_start(ap, fmt); vsnprintf(cmd, sizeof cmd, fmt, ap); va_end(ap); return system(cmd); }
#ifndef RUNTIME_DIR
#define RUNTIME_DIR "runtime"
#endif
/* Prebuilt runtime: ThinLTO objects cached under $XDG_CACHE_HOME/bashful, keyed on the runtime sources.
 * A compile then only optimizes the script itself; ThinLTO still imports/inlines the hot runtime helpers. */
#include <sys/stat.h>
static unsigned long fnv(unsigned long h, const void *p, size_t n) { const unsigned char *c = p; while (n--) { h ^= *c++; h *= 1099511628211UL; } return h; }
static int rt_cached(char *rt_o, char *tl_o, char *ltodir, size_t cap) {   /* cap <= 4200 */
    static const char *srcs[] = { "bash_runtime.c", "tools.c", "bash_runtime.h", "tools.def", "rt_fd.h" };
    unsigned long h = fnv(1469598103934665603UL, RUNTIME_DIR, strlen(RUNTIME_DIR));
    for (int i = 0; i < 5; i++) {
        char p[4096]; struct stat st; snprintf(p, sizeof p, "%s/%s", RUNTIME_DIR, srcs[i]);
        if (stat(p, &st)) return 0;
        h = fnv(h, &st.st_mtim, sizeof st.st_mtim); h = fnv(h, &st.st_size, sizeof st.st_size);
    }
    if (run("command -v ld.lld >/dev/null 2>&1")) return 0;
    const char *xc = getenv("XDG_CACHE_HOME"), *home = getenv("HOME"); char base[3000];
    if (xc && *xc) snprintf(base, sizeof base, "%s/bashful", xc); else if (home && *home) snprintf(base, sizeof base, "%s/.cache/bashful", home); else return 0;
    char dir[3100]; snprintf(dir, sizeof dir, "%s/rt-%016lx", base, h);
    snprintf(rt_o, cap, "%s/bash_runtime.o", dir); snprintf(tl_o, cap, "%s/tools.o", dir); snprintf(ltodir, cap, "%s/thinlto", dir);
    if (!access(rt_o, R_OK) && !access(tl_o, R_OK)) return 1;
    if (run("mkdir -p '%s' '%s'", dir, ltodir)) return 0;
    int pid = (int)getpid();
    if (run("clang -c -flto=thin -O2 -w -I'%s' '%s/bash_runtime.c' -o '%s.%d' && clang -c -flto=thin -O2 -w -I'%s' '%s/tools.c' -o '%s.%d'",
            RUNTIME_DIR, RUNTIME_DIR, rt_o, pid, RUNTIME_DIR, RUNTIME_DIR, tl_o, pid)) return 0;
    char a[4300], b[4300]; snprintf(a, sizeof a, "%s.%d", rt_o, pid); snprintf(b, sizeof b, "%s.%d", tl_o, pid);
    if (rename(a, rt_o) || rename(b, tl_o)) return 0;
    run("find '%s' -maxdepth 1 -name 'rt-*' ! -name 'rt-%016lx' -mmin +1440 -exec rm -rf {} + 2>/dev/null", base, h);   /* old runtimes */
    return 1;
}
static void usage(FILE *f) { fprintf(f, "usage: bashful [options] script.sh [args...]\n"
    "  -o, --output FILE   output file\n"
    "  -r, --run           compile and run\n"
    "  -c, --command STR   compile and run STR\n"
    "  --emit-ir           write LLVM IR\n"
    "  --emit-bc           write LLVM bitcode\n"
    "  --strict            fail instead of falling back to embedded bash for unsupported features\n"
    "  -q, --quiet         no fallback note\n"
    "  -h, --help          show this help\n"); }
enum { OUT_EXE, OUT_IR, OUT_BC };
int main(int argc, char **argv) {
    const char *in = NULL, *out = NULL; int mode = OUT_EXE, runflag = 0, nsargs = 0, nofallback = 0, quiet = 0; const char *cmdtext = NULL; char **sargs = xmalloc((argc + 2) * sizeof *sargs);
    for (int i = 1; i < argc; i++) {
        if ((in || cmdtext) && (runflag || cmdtext)) { sargs[nsargs++] = argv[i]; continue; }       /* everything after the script goes to the script */
        if ((!strcmp(argv[i], "-o") || !strcmp(argv[i], "--output")) && i + 1 < argc) out = argv[++i];
        else if ((!strcmp(argv[i], "-c") || !strcmp(argv[i], "--command") || !strcmp(argv[i], "--inline")) && i + 1 < argc) { cmdtext = argv[++i]; in = "bash"; }
        else if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "--run")) runflag = 1;
        else if (!strcmp(argv[i], "--emit-ir")) mode = OUT_IR;
        else if (!strcmp(argv[i], "--emit-bc")) mode = OUT_BC;
        else if (!strcmp(argv[i], "--no-fallback") || !strcmp(argv[i], "--strict")) nofallback = 1;
        else if (!strcmp(argv[i], "-q") || !strcmp(argv[i], "--quiet")) quiet = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) { usage(stdout); return 0; }
        else if (argv[i][0] == '-' && argv[i][1]) { usage(stderr); die("unknown option %s", argv[i]); }
        else if (!in) in = argv[i]; else die("unexpected argument %s", argv[i]);
    }
    if (!in) { usage(stderr); return 1; }
    if (cmdtext && mode == OUT_EXE) runflag = 1;
    if (cmdtext && nsargs > 0) { in = sargs[0]; sargs++; nsargs--; }      /* like bash -c: first extra argument is $0 */
    if (runflag && mode != OUT_EXE) die("--run cannot be combined with --emit-ir/--emit-bc");
    if (mode == OUT_EXE && !runflag && !out) { usage(stderr); die("no output file: use -o/--output FILE to build an executable, or -r/--run to run the script"); }
    if (out && !strcmp(out, "-") && mode != OUT_IR) die("only --emit-ir can write to stdout");
    char *src = cmdtext ? xstrdup(cmdtext) : slurp(in);
    char *ll = compile(src, cmdtext ? "-c" : in);
    if (!ll) {
        if (nofallback) die("%s: cannot compile natively: %s", cmdtext ? "-c" : in, g_unsup);
        if (!quiet) fprintf(stderr, "bashful: note: %s: %s -> using the embedded-bash fallback for this script (exact bash semantics, no native speedup)\n", cmdtext ? "-c" : in, g_unsup);
        ll = compile_fallback(src, cmdtext ? "-c" : in);
    }

    /* default output name: input basename without extension (never overwrite the input) */
    char defname[4096], runname[64]; const char *final = out;
    if (!final && runflag) { snprintf(runname, sizeof runname, "/tmp/bashful.%d.run", (int)getpid()); final = runname; }
    if (!final) {
        const char *b = strrchr(in, '/'); b = b ? b + 1 : in; if (!strcmp(in, "-")) b = "a";
        const char *dot = strrchr(b, '.'); size_t l = dot && dot != b ? (size_t)(dot - b) : strlen(b);
        if (mode == OUT_EXE) { if (dot && dot != b) snprintf(defname, sizeof defname, "%.*s", (int)l, b); else snprintf(defname, sizeof defname, "a.out"); }
        else snprintf(defname, sizeof defname, "%.*s%s", (int)l, b, mode == OUT_BC ? ".bc" : ".ll");
        final = defname;
    }
    int pid = (int)getpid(); char tmp_ll[64], tmp_rt[64], tmp_tools[64], tmp_out[64];
    snprintf(tmp_ll, sizeof tmp_ll, "/tmp/bashful.%d.ll", pid); snprintf(tmp_rt, sizeof tmp_rt, "/tmp/bashful.%d.rt.bc", pid);
    snprintf(tmp_tools, sizeof tmp_tools, "/tmp/bashful.%d.tools.bc", pid);
    snprintf(tmp_out, sizeof tmp_out, mode == OUT_IR ? "/tmp/bashful.%d.out.ll" : "/tmp/bashful.%d.out.bc", pid);
    FILE *f = fopen(tmp_ll, "w"); if (!f) die("cannot write %s", tmp_ll); fputs(ll, f); fclose(f);
    int stdout_mode = !strcmp(final, "-"); int link_to_final = mode != OUT_EXE && !stdout_mode;
    const char *dst = link_to_final ? final : tmp_out;
    char rto[4200], tlo[4200], ltod[4200]; int rc;
    if (mode == OUT_EXE && rt_cached(rto, tlo, ltod, sizeof rto)) {   /* fast path: script-only optimization + cached ThinLTO runtime */
        rc = run("clang -flto=thin -fuse-ld=lld -O2 -w -x ir '%s' -x none '%s' '%s' -Wl,--thinlto-cache-dir='%s' -Wl,--thinlto-cache-policy=cache_size_bytes=256m -o '%s'",
                 tmp_ll, rto, tlo, ltod, final);
        unlink(tmp_ll); if (rc) die("clang failed to build the executable");
        goto built;
    }
    rc = run("clang -c -emit-llvm -O2 -w -I'%s' '%s/bash_runtime.c' -o '%s' && clang -c -emit-llvm -O2 -w -I'%s' '%s/tools.c' -o '%s' && llvm-link %s'%s' '%s' '%s' -o '%s'",
                 RUNTIME_DIR, RUNTIME_DIR, tmp_rt, RUNTIME_DIR, RUNTIME_DIR, tmp_tools, mode == OUT_IR ? "-S " : "", tmp_ll, tmp_rt, tmp_tools, dst);
    unlink(tmp_ll); unlink(tmp_rt); unlink(tmp_tools);
    if (rc) die("could not link the embedded runtime (need clang and llvm-link in PATH)");
    if (mode == OUT_EXE) { rc = run("clang -O2 -w '%s' -o '%s'", tmp_out, final); unlink(tmp_out); if (rc) die("clang failed to build the executable"); }
built:
    if (runflag) {
        int keep = out != NULL; char *path = (char *)final; char rp[4200];
        if (keep && !strchr(path, '/')) { snprintf(rp, sizeof rp, "./%s", path); path = rp; }
        char **av = xmalloc((nsargs + 2) * sizeof *av); av[0] = (char *)in; for (int k = 0; k < nsargs; k++) av[k + 1] = sargs[k]; av[nsargs + 1] = NULL;
        fflush(NULL); pid_t c = fork(); int st = 0;
        if (c == 0) { execv(path, av); perror("bashful: exec"); _exit(127); }
        signal(SIGINT, SIG_IGN); if (c > 0) waitpid(c, &st, 0);
        if (!keep) unlink(final);
        return c < 0 ? 1 : WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }
    else if (stdout_mode) { char *s = slurp(tmp_out); fputs(s, stdout); unlink(tmp_out); }
    return 0;
}
