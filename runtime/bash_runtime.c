/* bash_runtime.c - core of the embedded bash runtime.
 *
 * Builtins, embedded tools, subshells, $(...) and pipelines of known commands run inside the process.
 * Programs that are not embedded (or embedded tools called with options they do not implement) are run
 * for real with posix_spawn, pipelines that contain such programs (or could stream forever) are real
 * forked pipelines, and `cmd &` is a real background job.
 *
 * Design: everything the compiler can decide is decided at compile time.  Variables the script names are
 * compiled to numbered slots (no lookup by name), arithmetic and tests are compiled to native code, and
 * only genuinely dynamic behaviour (IFS splitting, globbing, dynamic scoping, by-name access from builtins,
 * subshell state) lives here.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <setjmp.h>
#include <glob.h>
#include <fnmatch.h>
#include <regex.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <time.h>
#include <signal.h>
#include <spawn.h>
#include <poll.h>
#include <locale.h>
#include <wchar.h>
#include <sys/wait.h>
#include "bash_runtime.h"
#include "rt_fd.h"

extern char **environ;

/* ---------------------------------------------------------------- utils */
static void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) { write(2, "oom\n", 4); _exit(255); } return p; }
static void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) { write(2, "oom\n", 4); _exit(255); } return p; }
static char *xstrdup(const char *s) { size_t n = strlen(s) + 1; return memcpy(xmalloc(n), s, n); }
static char *xstrndup(const char *s, size_t n) { char *p = xmalloc(n + 1); memcpy(p, s, n); p[n] = 0; return p; }

typedef struct { char *s; size_t n, cap; } SB;
static void sb_putn(SB *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->s = xrealloc(b->s, b->cap); }
    memcpy(b->s + b->n, s, n); b->n += n; b->s[b->n] = 0;
}
static void sb_puts(SB *b, const char *s) { sb_putn(b, s, strlen(s)); }
static void sb_putc(SB *b, char c) { sb_putn(b, &c, 1); }
static char *sb_take(SB *b) { if (!b->s) return xstrdup(""); char *s = b->s; b->s = NULL; b->n = b->cap = 0; return s; }

/* ------------------------------------------------- buffered stdout */
static char obuf[1 << 16]; static size_t on; static int o_line;
static void raw_write(int fd, const char *s, size_t n) {
    while (n) { ssize_t w = write(fd, s, n); if (w < 0) { if (errno == EINTR) continue; return; } s += w; n -= w; }
}
typedef struct Cap { char *s; size_t n, cap; int memfd, saved, sinking; struct Cap *prev; } Cap;   /* $(...) in progress */
static Cap *g_cap; int bash_rt_sinking;
void bash_rt_flush(void) { if (on && !bash_rt_sinking) { size_t n = on; on = 0; raw_write(1, obuf, n); } }
void bash_rt_out(const char *s, size_t n) {
    if (bash_rt_sinking) {
        Cap *c = g_cap; if (c->n + n + 1 > c->cap) { c->cap = (c->n + n + 1) * 2; c->s = xrealloc(c->s, c->cap); }
        memcpy(c->s + c->n, s, n); c->n += n; return;
    }
    if (n >= sizeof obuf) { bash_rt_flush(); raw_write(1, s, n); return; }
    if (on + n > sizeof obuf) bash_rt_flush();
    memcpy(obuf + on, s, n); on += n;
    if (o_line && memchr(s, '\n', n)) bash_rt_flush();
}

static const char *g_argv0 = "bash";
void bash_rt_warn(const char *fmt, ...) {
    char buf[1024]; int n = snprintf(buf, sizeof buf, "%s: ", g_argv0);
    va_list ap; va_start(ap, fmt); n += vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap); va_end(ap);
    if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
    buf[n++] = '\n'; bash_rt_flush(); raw_write(2, buf, n);
}

/* --------------------------------------------------------------- status */
static int g_status = 0, g_cs_status = 0;
static int opt_e = 0, opt_u = 0, opt_pipefail = 0, opt_x = 0;
static volatile sig_atomic_t g_trap_pending;
static void run_pending_traps(void);
void bash_rt_set_status(int st) { g_status = st; if (__builtin_expect(g_trap_pending, 0)) run_pending_traps(); }
int  bash_rt_get_status(void) { return g_status; }
int  bash_rt_cmdsub_status(void) { return g_cs_status; }
void bash_rt_cmdsub_reset(void) { g_cs_status = 0; }
void bash_rt_errexit_check(void) { if (opt_e && g_status != 0) bash_rt_exit(g_status); }
int  bash_rt_opt_get(char o) { return o == 'e' ? opt_e : o == 'u' ? opt_u : o == 'x' ? opt_x : 0; }
void bash_rt_opt_set(char o, int v) { if (o == 'e') opt_e = v; else if (o == 'u') opt_u = v; else if (o == 'x') opt_x = v; }
int  bash_rt_pipefail_get(void) { return opt_pipefail; }
void bash_rt_pipefail_set(int v) { opt_pipefail = v; }

/* ------------------------------------------------------------ variables */
enum { VF_STR = 1, VF_INT = 2, VF_ARR = 4, VF_EXP = 8, VF_RO = 16, VF_LOCALE = 32, VF_ASC = 64 };
/* arrays: element i exists when ak[i] & AK_SET; its text (arr[i]) and/or integer (ai[i]) are cached lazily,
 * so arithmetic on array elements never goes through strings */
enum { AK_SET = 1, AK_STR = 2, AK_INT = 4 };
/* associative arrays: bash's own table layout (FNV-1 hash, 1024 buckets, insert at chain head, x4 growth at
 * 2 entries/bucket), so "${!h[@]}" lists keys in exactly the order bash does */
typedef struct AItem { char *k, *v; long iv; unsigned h; unsigned char vk; struct AItem *next; } AItem;   /* vk: AK_STR/AK_INT */
typedef struct Asc { int nb, n; AItem **b; } Asc;
typedef struct Var { char *val; char **arr; long *ai; unsigned char *ak; int arrn, arrcap; long ival; unsigned flags; unsigned gen; const char *name; unsigned long ver; size_t vlen, vcap; Asc *as; } Var;   /* vcap != 0: val has room for appends */
static unsigned long g_ver;   /* every scalar write gets a fresh version (substring position caches) */
static unsigned asc_hash(const char *s) { unsigned i = 2166136261u; for (; *s; s++) { i *= 16777619u; i ^= (unsigned)(int)(signed char)*s; } return i; }
static Asc *asc_new(void) { Asc *a = malloc(sizeof *a); a->nb = 1024; a->n = 0; a->b = calloc(1024, sizeof *a->b); return a; }
static void asc_grow(Asc *a) {
    int nn = a->nb * 4; AItem **nb = calloc(nn, sizeof *nb);
    for (int i = 0; i < a->nb; i++) for (AItem *it = a->b[i], *nx; it; it = nx) { nx = it->next; unsigned k = it->h & (nn - 1); it->next = nb[k]; nb[k] = it; }
    free(a->b); a->b = nb; a->nb = nn;
}
static AItem *asc_find(Asc *a, const char *k, int create) {
    unsigned h = asc_hash(k);
    for (AItem *it = a->b[h & (a->nb - 1)]; it; it = it->next) if (it->h == h && !strcmp(it->k, k)) return it;
    if (!create) return NULL;
    if (a->n >= a->nb * 2) asc_grow(a);
    AItem *it = malloc(sizeof *it); size_t L = strlen(k); it->k = malloc(L + 1); memcpy(it->k, k, L + 1);
    it->v = NULL; it->iv = 0; it->vk = 0; it->h = h; unsigned b = h & (a->nb - 1); it->next = a->b[b]; a->b[b] = it; a->n++;
    return it;
}
static void asc_del(Asc *a, const char *k) {
    unsigned h = asc_hash(k); AItem **pp = &a->b[h & (a->nb - 1)];
    for (; *pp; pp = &(*pp)->next) if ((*pp)->h == h && !strcmp((*pp)->k, k)) { AItem *it = *pp; *pp = it->next; free(it->k); free(it->v); free(it); a->n--; return; }
}
static void asc_free(Asc *a) {
    if (!a) return;
    for (int i = 0; i < a->nb; i++) for (AItem *it = a->b[i], *nx; it; it = nx) { nx = it->next; free(it->k); free(it->v); free(it); }
    free(a->b); free(a);
}
static Asc *asc_copy(const Asc *o) {
    if (!o) return NULL;
    Asc *a = malloc(sizeof *a); a->nb = o->nb; a->n = o->n; a->b = calloc(o->nb, sizeof *a->b);
    for (int i = 0; i < o->nb; i++) { AItem **tail = &a->b[i];
        for (const AItem *it = o->b[i]; it; it = it->next) { AItem *c = malloc(sizeof *c); *c = *it; c->k = strdup(it->k); c->v = it->v ? strdup(it->v) : NULL; c->next = NULL; *tail = c; tail = &c->next; } }
    return a;
}
static const char *ai_str(AItem *it) {
    if (it->vk & 2) return it->v;
    char b[24]; snprintf(b, sizeof b, "%ld", it->iv); it->v = strdup(b); it->vk |= 2; return it->v;
}
static void ai_set(AItem *it, const char *val, int append) {
    char *n;
    if (append && it->vk) { const char *o = ai_str(it); size_t a = strlen(o), b = strlen(val); n = malloc(a + b + 1); memcpy(n, o, a); memcpy(n + a, val, b + 1); }
    else n = strdup(val);
    free(it->v); it->v = n; it->vk = 2;
}
static void ai_seti(AItem *it, long x) { free(it->v); it->v = NULL; it->iv = x; it->vk = 4; }
static int asc_list(const Asc *a, const char ***out, int keys) {   /* borrowed strings in bash order */
    const char **l = malloc((a->n + 1) * sizeof *l); int n = 0;
    for (int i = 0; i < a->nb; i++) for (AItem *it = a->b[i]; it; it = it->next) l[n++] = keys ? it->k : ai_str(it);
    *out = l; return n;
}
static Var **vt; static int nvt, capvt;
static int *ht; static int htcap;                 /* open-addressing name -> slot (stored +1) */
static unsigned cur_gen;                          /* generation of the innermost subshell (0 = none) */

static unsigned hash_str(const char *s) { unsigned h = 2166136261u; while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; } return h; }
static void ht_rebuild(int ncap) {
    free(ht); ht = calloc(ncap, sizeof(int)); htcap = ncap;
    for (int i = 0; i < nvt; i++) { unsigned h = hash_str(vt[i]->name) & (ncap - 1); while (ht[h]) h = (h + 1) & (ncap - 1); ht[h] = i + 1; }
}
static int vt_add(const char *name, int copy) {
    if (nvt == capvt) { capvt = capvt ? capvt * 2 : 256; vt = xrealloc(vt, capvt * sizeof *vt); }
    Var *v = calloc(1, sizeof *v); v->name = copy ? xstrdup(name) : name; vt[nvt] = v;
    if ((nvt + 1) * 2 > htcap) { nvt++; ht_rebuild(htcap ? htcap * 2 : 1024); return nvt - 1; }
    unsigned h = hash_str(name) & (htcap - 1); while (ht[h]) h = (h + 1) & (htcap - 1); ht[h] = nvt + 1;
    return nvt++;
}
static int name_id(const char *name, int create) {
    if (htcap) { unsigned h = hash_str(name) & (htcap - 1); while (ht[h]) { Var *v = vt[ht[h] - 1]; if (!strcmp(v->name, name)) return ht[h] - 1; h = (h + 1) & (htcap - 1); } }
    return create ? vt_add(name, 1) : -1;
}

static const char *var_str_noarr(Var *v) {
    if (v->flags & VF_STR) return v->val;
    if (v->flags & VF_INT) { v->val = xmalloc(24); snprintf(v->val, 24, "%ld", v->ival); v->flags |= VF_STR; v->vcap = 0; return v->val; }
    return NULL;
}
/* undo log: variables modified inside a subshell are restored when it ends */
typedef struct { Var *v; Var snap; } Undo;
static Undo *ulog; static int nulog, capulog;
static Var var_snapshot(const Var *o) {
    Var s = *o; s.val = o->val ? xstrdup(o->val) : NULL; s.vcap = 0; s.as = asc_copy(o->as);
    if (o->arr) {
        int c = o->arrn ? o->arrn : 1; s.arrcap = c;
        s.arr = xmalloc(c * sizeof(char *)); s.ai = xmalloc(c * sizeof(long)); s.ak = xmalloc(c);
        for (int i = 0; i < o->arrn; i++) { s.arr[i] = o->arr[i] ? xstrdup(o->arr[i]) : NULL; s.ai[i] = o->ai[i]; s.ak[i] = o->ak[i]; }
    }
    return s;
}
static void var_release(Var *v) {
    free(v->val); v->val = NULL; v->ver = ++g_ver; v->vcap = 0; asc_free(v->as); v->as = NULL;
    for (int i = 0; i < v->arrn; i++) free(v->arr[i]);
    free(v->arr); free(v->ai); free(v->ak); v->arr = NULL; v->ai = NULL; v->ak = NULL; v->arrn = v->arrcap = 0;
}
static void arr_grow(Var *v, long n) {   /* make indices < n valid (new ones unset) */
    if (n > v->arrcap) {
        long c = v->arrcap ? v->arrcap : 8; while (c < n) c *= 2;
        v->arr = xrealloc(v->arr, c * sizeof(char *)); v->ai = xrealloc(v->ai, c * sizeof(long)); v->ak = xrealloc(v->ak, c); v->arrcap = (int)c;
    }
    for (long i = v->arrn; i < n; i++) { v->arr[i] = NULL; v->ak[i] = 0; }
    if (n > v->arrn) v->arrn = (int)n;
}
static const char *el_str(Var *v, long i) {
    unsigned char k = v->ak[i];
    if (!(k & AK_SET)) return NULL;
    if (k & AK_STR) return v->arr[i];
    char b[24]; snprintf(b, sizeof b, "%ld", v->ai[i]); v->arr[i] = xstrdup(b); v->ak[i] = k | AK_STR; return v->arr[i];
}
static void el_take(Var *v, long i, char *owned) { if (i == 0) v->ver = ++g_ver; free(v->arr[i]); v->arr[i] = owned; v->ak[i] = AK_SET | AK_STR; }
static void to_array(Var *v) {   /* scalar -> array with the old value as element 0 */
    const char *old = var_str_noarr(v); char *o = old ? xstrdup(old) : NULL; free(v->val); v->val = NULL;
    v->arr = NULL; v->ai = NULL; v->ak = NULL; v->arrn = v->arrcap = 0;
    v->flags = (v->flags & (VF_EXP | VF_RO | VF_LOCALE)) | VF_ARR;
    if (o) { arr_grow(v, 1); el_take(v, 0, o); }
}
__attribute__((noinline, cold)) static void touch_slow(Var *v) {
    if (nulog == capulog) { capulog = capulog ? capulog * 2 : 64; ulog = xrealloc(ulog, capulog * sizeof *ulog); }
    ulog[nulog].v = v; ulog[nulog].snap = var_snapshot(v); nulog++; v->gen = cur_gen;
}
static inline void touch(Var *v) { if (cur_gen && v->gen != cur_gen) touch_slow(v); }

static inline const char *var_str(Var *v) {     /* scalar view; NULL if unset */
    unsigned f = v->flags;
    if (f & VF_ARR) return v->arrn > 0 ? el_str(v, 0) : NULL;
    if (f & VF_STR) return v->val;
    if (f & VF_ASC) { AItem *it = asc_find(v->as, "0", 0); return it ? ai_str(it) : NULL; }
    if (f & VF_INT) { v->val = xmalloc(24); snprintf(v->val, 24, "%ld", v->ival); v->flags |= VF_STR; v->vcap = 0; return v->val; }
    return NULL;
}
static inline int var_isset(const Var *v) { return v->flags & (VF_STR | VF_INT | VF_ARR | VF_ASC); }
static void var_clear(Var *v) { touch(v); var_release(v); v->flags &= (VF_EXP | VF_RO | VF_LOCALE); v->ival = 0; if (v->flags & VF_LOCALE) bash_rt_locale_changed(); }

static void asc_set_v(Var *v, const char *k, const char *val, int append) { touch(v); ai_set(asc_find(v->as, k, 1), val, append); if (!strcmp(k, "0")) v->ver = ++g_ver; }
static void var_set_elem(Var *v, long idx, const char *val, int append) {
    if (v->flags & VF_ASC) { char k[24]; snprintf(k, sizeof k, "%ld", idx); asc_set_v(v, k, val, append); return; }
    touch(v);
    if (!(v->flags & VF_ARR)) to_array(v);
    if (idx < 0) idx += v->arrn;
    if (idx < 0) { bash_rt_warn("bad array subscript"); return; }
    if (idx >= v->arrn) arr_grow(v, idx + 1);
    char *n; const char *old = append ? el_str(v, idx) : NULL;
    if (old) { size_t a = strlen(old), b = strlen(val); n = xmalloc(a + b + 1); memcpy(n, old, a); memcpy(n + a, val, b + 1); }
    else n = xstrdup(val);
    el_take(v, idx, n);
}
static void var_set_elem_int(Var *v, long idx, long x) {
    if (v->flags & VF_ASC) { char k[24], b[24]; snprintf(k, sizeof k, "%ld", idx); snprintf(b, sizeof b, "%ld", x); asc_set_v(v, k, b, 0); return; }
    touch(v);
    if (!(v->flags & VF_ARR)) to_array(v);
    if (idx < 0) idx += v->arrn;
    if (idx < 0) { bash_rt_warn("bad array subscript"); return; }
    if (idx >= v->arrn) arr_grow(v, idx + 1);
    free(v->arr[idx]); v->arr[idx] = NULL; v->ai[idx] = x; v->ak[idx] = AK_SET | AK_INT; if (idx == 0) v->ver = ++g_ver;
}
static void random_seed(const char *s);
static Var *g_random_var;
static void var_set_str(Var *v, const char *val, int append) {
    if (v == g_random_var) { random_seed(val); return; }
    if (v->flags & VF_RO) { bash_rt_warn("%s: readonly variable", v->name); g_status = 1; return; }
    if (v->flags & (VF_ARR | VF_ASC)) { var_set_elem(v, 0, val, append); return; }
    touch(v);
    size_t bl = strlen(val);
    if (append && (v->flags & VF_STR) && v->vcap && v->val && (val < v->val || val > v->val + v->vcap)) {   /* amortized in-place append */
        if (v->vlen + bl + 1 > v->vcap) { size_t c = (v->vlen + bl + 1) * 2; v->val = xrealloc(v->val, c); v->vcap = c; }
        memcpy(v->val + v->vlen, val, bl + 1); v->vlen += bl;
        v->flags = (v->flags & (VF_EXP | VF_RO | VF_LOCALE)) | VF_STR; v->ver = ++g_ver;
        if (v->flags & VF_LOCALE) bash_rt_locale_changed();
        return;
    }
    char *n; size_t nl;
    if (append) { const char *old = var_str(v); if (old) { size_t ol = strlen(old); nl = ol + bl; n = xmalloc(nl * 2 + 16); memcpy(n, old, ol); memcpy(n + ol, val, bl + 1); v->vcap = nl * 2 + 16; } else { nl = bl; n = xstrdup(val); v->vcap = 0; } }
    else { nl = bl; n = xmalloc(bl + 1); memcpy(n, val, bl + 1); v->vcap = bl + 1; }   /* copy first: val may point into the old value */
    free(v->val); v->val = n; v->vlen = nl; v->flags = (v->flags & (VF_EXP | VF_RO | VF_LOCALE)) | VF_STR; v->ver = ++g_ver;
    if (v->flags & VF_LOCALE) bash_rt_locale_changed();
}

/* ---- slot API ---- */
static long ar_eval_str(const char *s, int depth);
__attribute__((noinline)) static long geti_slow(Var *v) {
    const char *s = var_str(v);
    if (!s || !*s) return 0;
    char *e; long x = strtol(s, &e, 0);
    if (*e == 0) { if (!(v->flags & (VF_ARR | VF_ASC))) { touch(v); v->ival = x; v->flags |= VF_INT; } return x; }
    return ar_eval_str(s, 1);
}
long bash_rt_geti(int id) { Var *v = vt[id]; if (v->flags & VF_INT) return v->ival; return geti_slow(v); }
static int test_err;
static long str_to_long_t(const char *s) {
    if (!s || !*s) { test_err = 1; bash_rt_warn("[: : integer expression expected"); return 0; }
    char *e; errno = 0; long x = strtol(s, &e, 10);
    while (*e == ' ' || *e == '\t') e++;
    if (*e || errno) { test_err = 1; bash_rt_warn("[: %s: integer expression expected", s); return 0; }
    return x;
}
long bash_rt_geti_t(int id) {
    Var *v = vt[id];
    if ((v->flags & VF_INT) && !(v->flags & VF_STR) && !(v->flags & (VF_ARR | VF_ASC))) return v->ival;
    const char *s = var_str(v);
    if (!s) { test_err = 1; bash_rt_warn("[: : integer expression expected"); return 0; }
    return str_to_long_t(s);
}
__attribute__((noinline)) static void seti_slow(Var *v, long x) {
    if (v->flags & (VF_ARR | VF_RO | VF_ASC | VF_LOCALE) || v == g_random_var) { char b[32]; snprintf(b, sizeof b, "%ld", x); var_set_str(v, b, 0); return; }
    touch(v); if (v->flags & VF_STR) { free(v->val); v->val = NULL; }
    v->ival = x; v->flags = (v->flags & (VF_EXP | VF_RO)) | VF_INT; v->ver = ++g_ver;
}
void bash_rt_seti(int id, long x) {   /* hot: plain integer variable, nothing to undo */
    Var *v = vt[id];
    if (__builtin_expect(!(v->flags & ~(unsigned)(VF_INT | VF_EXP)) && v != g_random_var && (!cur_gen || v->gen == cur_gen), 1)) {
        v->ival = x; v->flags |= VF_INT; v->ver = ++g_ver; return; }
    seti_slow(v, x);
}
const char *bash_rt_gets(int id) { return var_str(vt[id]); }
const char *bash_rt_gets_e(int id) {
    const char *s = var_str(vt[id]);
    if (s) return s;
    if (opt_u) { bash_rt_warn("%s: unbound variable", vt[id]->name); bash_rt_exit(1); }
    return "";
}
void bash_rt_set_id(int id, const char *s, int append) { var_set_str(vt[id], s, append); }
void bash_rt_set_elem_id(int id, long idx, const char *s, int append) { Var *v = vt[id]; if (v->flags & VF_RO) { bash_rt_warn("%s: readonly variable", v->name); return; } var_set_elem(v, idx, s, append); }
__attribute__((noinline)) static long geti_elem_slow(int id, long idx) {
    Var *v = vt[id]; const char *s = NULL;
    if (v->flags & VF_ASC) { char k[24]; snprintf(k, sizeof k, "%ld", idx); AItem *it = asc_find(v->as, k, 0); s = it ? ai_str(it) : NULL; }
    else if (v->flags & VF_ARR) {
        long i = idx < 0 ? idx + v->arrn : idx;
        if (i < 0 || i >= v->arrn) return 0;
        unsigned char k = v->ak[i];
        if (k & AK_INT) return v->ai[i];
        if (!(k & AK_SET)) return 0;
        s = v->arr[i]; if (!*s) return 0;
        char *e; long x = strtol(s, &e, 0);
        if (*e == 0) { touch(v); v->ai[i] = x; v->ak[i] = k | AK_INT; return x; }
        return ar_eval_str(s, 1);
    }
    else if (idx == 0) s = var_str(v);
    if (!s || !*s) return 0;
    char *e; long x = strtol(s, &e, 0); return *e == 0 ? x : ar_eval_str(s, 1);
}
long bash_rt_geti_elem(int id, long idx) {
    Var *v = vt[id];
    if (__builtin_expect((v->flags & (VF_ARR | VF_ASC)) == VF_ARR && idx >= 0 && idx < v->arrn && (v->ak[idx] & AK_INT), 1)) return v->ai[idx];
    return geti_elem_slow(id, idx);
}
__attribute__((noinline)) static void seti_elem_slow(Var *v, long idx, long x) { if (v->flags & VF_RO) { bash_rt_warn("%s: readonly variable", v->name); return; } var_set_elem_int(v, idx, x); }
void bash_rt_seti_elem(int id, long idx, long x) {
    Var *v = vt[id];
    if (__builtin_expect((v->flags & (VF_ARR | VF_ASC | VF_RO)) == VF_ARR && idx > 0 && idx < v->arrn && !v->arr[idx] && (!cur_gen || v->gen == cur_gen), 1)) {
        v->ai[idx] = x; v->ak[idx] = AK_SET | AK_INT; return; }
    seti_elem_slow(v, idx, x);
}
void bash_rt_set_array_id(int id, int argc, char **argv, int append) {
    Var *v = vt[id];
    if (v->flags & VF_RO) { bash_rt_warn("%s: readonly variable", v->name); return; }
    touch(v);
    if (v->flags & VF_ASC) {   /* h=([k]=v ...) or h=(k v ...) arriving as words */
        if (!append) { asc_free(v->as); v->as = asc_new(); }
        for (int i = 0; i < argc; i++) {
            const char *a = argv[i], *e;
            if (a[0] == '[' && (e = strstr(a, "]="))) { char *k = xstrndup(a + 1, e - a - 1); ai_set(asc_find(v->as, k, 1), e + 2, 0); free(k); }
            else { ai_set(asc_find(v->as, a, 1), i + 1 < argc ? argv[i + 1] : "", 0); i++; }
        }
        v->ver = ++g_ver; return;
    }
    if (!append) { var_release(v); v->flags = (v->flags & (VF_EXP | VF_RO)); v->ival = 0; }
    if (!(v->flags & VF_ARR)) to_array(v);
    int base = v->arrn;
    arr_grow(v, base + argc);
    for (int i = 0; i < argc; i++) { v->arr[base + i] = xstrdup(argv[i]); v->ak[base + i] = AK_SET | AK_STR; }
}
void bash_rt_export_id(int id) { touch(vt[id]); vt[id]->flags |= VF_EXP; }
void bash_rt_readonly_id(int id) { touch(vt[id]); vt[id]->flags |= VF_RO; }
void bash_rt_unset_id(int id) {
    Var *v = vt[id];
    if (v->flags & VF_RO) { bash_rt_warn("%s: readonly variable", v->name); return; }
    var_clear(v);
}

/* temporary (prefix) assignments: VAR=x cmd */
typedef struct { int id; Var snap; } Tmp;
static Tmp *tmps; static int ntmp, captmp;
int bash_rt_tmp_mark(void) { return ntmp; }
void bash_rt_tmp_set_id(int id, const char *s) {
    if (ntmp == captmp) { captmp = captmp ? captmp * 2 : 8; tmps = xrealloc(tmps, captmp * sizeof *tmps); }
    Var *v = vt[id]; touch(v);
    tmps[ntmp].id = id; tmps[ntmp].snap = var_snapshot(v); ntmp++;
    var_set_str(v, s, 0); v->flags |= VF_EXP;
}
void bash_rt_tmp_set(const char *name, const char *val) { bash_rt_tmp_set_id(name_id(name, 1), val); }
void bash_rt_tmp_restore(int mark) {
    while (ntmp > mark) {
        ntmp--; Var *v = vt[tmps[ntmp].id]; touch(v); var_release(v);
        const char *nm = v->name; unsigned g = v->gen; *v = tmps[ntmp].snap; v->name = nm; v->gen = g; v->ver = ++g_ver;
        if (v->flags & VF_LOCALE) bash_rt_locale_changed();
    }
}

/* --------------------------------------------- positional parameters */
static char **pos; static int npos; static void *pos_block;
static void *pos_build(int n, char **src, char ***out) {
    if (n <= 0) { *out = NULL; return NULL; }
    size_t sz = n * sizeof(char *); for (int i = 0; i < n; i++) sz += strlen(src[i]) + 1;
    char *blk = xmalloc(sz); char **p = (char **)blk; char *s = blk + n * sizeof(char *);
    for (int i = 0; i < n; i++) { size_t l = strlen(src[i]) + 1; memcpy(s, src[i], l); p[i] = s; s += l; }
    *out = p; return blk;
}
int bash_rt_npos(void) { return npos; }
const char *bash_rt_pos_s(int n) { return n >= 1 && n <= npos ? pos[n - 1] : ""; }
static void pos_release_block(void);
int bash_rt_shift(int n) { if (n < 0 || n > npos) return 1; pos += n; npos -= n; return 0; }
void bash_rt_pos_replace(int argc, char **argv) {
    char **np; void *nb = pos_build(argc, argv, &np);        /* build first: argv may point into the old block */
    pos_release_block(); pos = np; npos = argc > 0 ? argc : 0; pos_block = nb;
}

/* ------------------------------------------------------- fds & capture */
static int fd_dup_high(int fd) { return fcntl(fd, F_DUPFD_CLOEXEC, 100); }
static int mkmem(void) {
    int fd = memfd_create("bashrt", MFD_CLOEXEC);
    if (fd >= 0) return fd;
    char t[] = "/tmp/bashrtXXXXXX"; fd = mkstemp(t); if (fd >= 0) unlink(t); return fd;
}
static void write_all(int fd, const char *s, size_t n) { raw_write(fd, s, n); }

typedef struct { int fd, saved; } RS;
static RS *rstack; static int nrs, caprs; static int redir_err;
static void rs_push(int fd, int saved) {
    if (nrs == caprs) { caprs = caprs ? caprs * 2 : 32; rstack = xrealloc(rstack, caprs * sizeof *rstack); }
    rstack[nrs].fd = fd; rstack[nrs].saved = saved; nrs++;
}
void bash_rt_redir_push(void) { rs_push(-1, -1); redir_err = 0; }
static void raw_lseek_set(int fd) { lseek(fd, 0, SEEK_SET); }
static void redir_save(int fd) { if (fd == 1) RT_NEED_FD1(); if (fd == 0) RT_NEED_FD0(); if (fd == 1 || fd == 2) bash_rt_flush(); rs_push(fd, fd_dup_high(fd)); }
void bash_rt_redir_pop(void) {
    bash_rt_flush();
    while (nrs > 0 && rstack[nrs - 1].fd != -1) {
        RS r = rstack[--nrs];
        if (r.saved >= 0) { dup2(r.saved, r.fd); close(r.saved); } else close(r.fd);
    }
    if (nrs > 0) nrs--;
}
void bash_rt_redir_commit(void) {
    while (nrs > 0 && rstack[nrs - 1].fd != -1) { RS r = rstack[--nrs]; if (r.saved >= 0) close(r.saved); }
    if (nrs > 0) nrs--;
}
int bash_rt_redir_ok(void) { return !redir_err; }
static int is_number(const char *s) { if (!*s) return 0; for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0; return 1; }
static void redir_install(int fd, int nfd) { if (nfd != fd) { dup2(nfd, fd); close(nfd); } }
void bash_rt_redir(int fd, int op, const char *t) {
    int nfd, flags;
    switch (op) {
    case R_IN: case R_OUT: case R_APPEND: case R_CLOBBER: case R_RDWR:
        redir_save(fd);
        flags = op == R_IN ? O_RDONLY : op == R_RDWR ? (O_RDWR | O_CREAT) : op == R_APPEND ? (O_WRONLY | O_CREAT | O_APPEND) : (O_WRONLY | O_CREAT | O_TRUNC);
        nfd = open(t, flags, 0666);
        if (nfd < 0) { bash_rt_warn("%s: %s", t, strerror(errno)); redir_err = 1; return; }
        redir_install(fd, nfd); return;
    case R_OUT_ERR: case R_APPEND_ERR:
        redir_save(1); redir_save(2);
        nfd = open(t, O_WRONLY | O_CREAT | (op == R_APPEND_ERR ? O_APPEND : O_TRUNC), 0666);
        if (nfd < 0) { bash_rt_warn("%s: %s", t, strerror(errno)); redir_err = 1; return; }
        dup2(nfd, 1); dup2(nfd, 2); close(nfd); return;
    case R_DUP_IN: case R_DUP_OUT:
        redir_save(fd);
        if (!strcmp(t, "-")) { close(fd); return; }
        if (is_number(t)) {
            int src = atoi(t);
            if (fcntl(src, F_GETFD) < 0) { bash_rt_warn("%s: Bad file descriptor", t); redir_err = 1; return; }
            if (src != fd) dup2(src, fd); return;
        }
        if (op == R_DUP_OUT) { /* >&file == &>file */
            redir_save(1); redir_save(2);
            nfd = open(t, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (nfd < 0) { bash_rt_warn("%s: %s", t, strerror(errno)); redir_err = 1; return; }
            dup2(nfd, 1); dup2(nfd, 2); close(nfd); return;
        }
        bash_rt_warn("%s: ambiguous redirect", t); redir_err = 1; return;
    case R_HEREDOC: case R_HERESTR:
        redir_save(fd);
        nfd = mkmem(); write_all(nfd, t, strlen(t)); if (op == R_HERESTR) write_all(nfd, "\n", 1);
        lseek(nfd, 0, SEEK_SET); redir_install(fd, nfd); return;
    }
}
static char *slurp_fd(int fd) {
    SB b = { 0 }; char buf[4096]; lseek(fd, 0, SEEK_SET);
    for (;;) { ssize_t n = read(fd, buf, sizeof buf); if (n < 0 && errno == EINTR) continue; if (n <= 0) break; sb_putn(&b, buf, n); }
    return sb_take(&b);
}

/* ------------------------------------------- locals, frames, subshells */
typedef struct { int id; Var saved; } Loc;
static Loc *locs; static int nloc, caploc;
typedef struct { char **pos; int npos; void *block; int loc_base; int redir_depth; const char *fname; } Frame;
static Frame *frames; static int nframes, capframes;

typedef struct Sub {
    jmp_buf jb; struct Sub *prev; int exit_code;
    int frame_depth, redir_depth, tmp_depth, log_base; unsigned saved_gen;
    char **spos; int snpos; void *sblock; char *cwd; int e, u, pf;
    void *trapsave;
} Sub;
static Sub *cur_sub; static unsigned gen_counter;
static pid_t g_last_bg, g_shell_pid;

static void pos_release_block(void) { if (!(cur_sub && pos_block == cur_sub->sblock)) free(pos_block); pos_block = NULL; }
void bash_rt_cwd_changing(void) { if (cur_sub && !cur_sub->cwd) cur_sub->cwd = getcwd(NULL, 0); }

static void frame_pop(void) {
    Frame *f = &frames[--nframes];
    while (nrs > f->redir_depth) bash_rt_redir_pop();
    while (nloc > f->loc_base) {
        Loc *l = &locs[--nloc]; Var *v = vt[l->id]; touch(v); var_release(v);
        const char *nm = v->name; unsigned g = v->gen; *v = l->saved; v->name = nm; v->gen = g;
        if (v->flags & VF_LOCALE) bash_rt_locale_changed();
    }
    if (!(cur_sub && pos_block == cur_sub->sblock)) free(pos_block);
    pos = f->pos; npos = f->npos; pos_block = f->block;
}
void bash_rt_fn_enter(int argc, char **argv) {
    if (nframes == capframes) { capframes = capframes ? capframes * 2 : 32; frames = xrealloc(frames, capframes * sizeof *frames); }
    Frame *f = &frames[nframes++]; f->pos = pos; f->npos = npos; f->block = pos_block; f->loc_base = nloc; f->redir_depth = nrs; f->fname = argc > 0 ? argv[0] : "";
    pos = argc > 1 ? argv + 1 : NULL; npos = argc > 1 ? argc - 1 : 0; pos_block = NULL;   /* borrowed: the caller's argv outlives the call */
}
void bash_rt_fn_leave(void) { frame_pop(); }
void bash_rt_local_id(int id) {
    if (nframes == 0) return;
    int base = frames[nframes - 1].loc_base;
    for (int i = nloc - 1; i >= base; i--) if (locs[i].id == id) return;
    if (nloc == caploc) { caploc = caploc ? caploc * 2 : 64; locs = xrealloc(locs, caploc * sizeof *locs); }
    Var *v = vt[id]; touch(v);
    locs[nloc].id = id; locs[nloc].saved = *v; nloc++;        /* move the old contents; the variable starts unset */
    unsigned lf = v->flags & VF_LOCALE; v->val = NULL; v->vcap = 0; v->as = NULL; v->arr = NULL; v->ai = NULL; v->ak = NULL; v->arrn = v->arrcap = 0; v->ival = 0; v->flags = lf;
    if (lf) bash_rt_locale_changed();
}
void bash_rt_declare_local(const char *name) {
    const char *br = strchr(name, '['); char *base = br ? xstrndup(name, br - name) : (char *)name;
    bash_rt_local_id(name_id(base, 1)); if (br) free(base);
}

static void subshell_traps_end(Sub *s);
int bash_rt_subshell(bash_body_t body) {
    Sub s; volatile int st = 0;
    s.prev = cur_sub; s.frame_depth = nframes; s.redir_depth = nrs; s.tmp_depth = ntmp; s.log_base = nulog; s.saved_gen = cur_gen;
    s.spos = pos; s.snpos = npos; s.sblock = pos_block; s.cwd = NULL; s.e = opt_e; s.u = opt_u; s.pf = opt_pipefail; s.trapsave = NULL;
    cur_gen = ++gen_counter; cur_sub = &s;
    if (_setjmp(s.jb) == 0) st = body();
    else {
        st = s.exit_code;
        while (nframes > s.frame_depth) frame_pop();
        while (nrs > s.redir_depth) bash_rt_redir_pop();
        ntmp = s.tmp_depth;
    }
    if (s.trapsave) subshell_traps_end(&s);
    cur_sub = s.prev;
    while (nulog > s.log_base) {       /* undo variable changes */
        Undo *u = &ulog[--nulog]; Var *v = u->v; var_release(v);
        const char *nm = v->name; *v = u->snap; v->name = nm; v->ver = ++g_ver;
        if (v->flags & VF_LOCALE) bash_rt_locale_changed();
    }
    cur_gen = s.saved_gen;
    if (pos_block != s.sblock) free(pos_block);
    pos = s.spos; npos = s.snpos; pos_block = s.sblock;
    if (s.cwd) { if (chdir(s.cwd) != 0) {} free(s.cwd); }
    opt_e = s.e; opt_u = s.u; opt_pipefail = s.pf;
    g_status = st; return st;
}
static int g_is_child;
static bash_body_t trap_fn[65]; static int trap_mode[65];   /* 0 default, 1 ignore, 2 run */
void bash_rt_exit(int code) {
    if (cur_sub) { cur_sub->exit_code = code & 255; _longjmp(cur_sub->jb, 1); }
    if (!g_is_child && trap_mode[0] == 2) { bash_body_t f = trap_fn[0]; trap_mode[0] = 0; g_status = code; f(); }
    bash_rt_flush();
    bash_rt_fd_sync();          /* leave the shared input offset where bash would */
    if (g_is_child) _exit(code & 255);
    exit(code & 255);
}

void bash_rt_cap_materialize(void) {   /* the capture needs a real fd 1 from here on */
    Cap *c = g_cap; if (!c || !c->sinking) return;
    c->sinking = 0; bash_rt_sinking = 0;
    bash_rt_flush();                     /* output still pending from before the capture began */
    c->memfd = mkmem(); c->saved = fd_dup_high(1); dup2(c->memfd, 1);
    if (c->n) raw_write(1, c->s, c->n);
    free(c->s); c->s = NULL; c->n = c->cap = 0;
}
static char *capture(bash_body_t body, int *status) {
    Cap c; memset(&c, 0, sizeof c); c.memfd = c.saved = -1; c.sinking = 1; c.prev = g_cap; redir_err = 0;
    g_cap = &c; bash_rt_sinking = 1;
    int st = bash_rt_subshell(body);
    char *s;
    if (c.sinking) { if (!c.s) s = xstrdup(""); else { c.s[c.n] = 0; s = c.s; } }
    else {
        bash_rt_flush();
        if (c.saved >= 0) { dup2(c.saved, 1); close(c.saved); } else close(1);
        s = slurp_fd(c.memfd); close(c.memfd);
    }
    g_cap = c.prev; bash_rt_sinking = g_cap ? g_cap->sinking : 0;
    size_t n = strlen(s); while (n > 0 && s[n - 1] == '\n') s[--n] = 0;
    if (status) *status = st; return s;
}
RtMin *bash_rt_min;
typedef struct { RtMin m; int saved0; int mat; pid_t feeder; } MinStage;
static MinStage *g_minstage;
void bash_rt_min_materialize(void) {   /* somebody needs a real fd 0: hand over the unread rest through a pipe */
    MinStage *st = g_minstage; RtMin *m = bash_rt_min; if (!m || !st) return;
    bash_rt_min = NULL; st->mat = 1; st->feeder = 0;
    const char *d = m->s + m->p; size_t len = m->n > m->p ? m->n - m->p : 0;
    int pfd[2]; if (pipe2(pfd, O_CLOEXEC) != 0) return;
    if (len > 65536) fcntl(pfd[1], F_SETPIPE_SZ, (int)(len < (1 << 20) ? len : (1 << 20)));
    long cap = fcntl(pfd[1], F_GETPIPE_SZ);
    if (cap > 0 && (size_t)cap >= len) { if (len) raw_write(pfd[1], d, len); }
    else {   /* more than a pipe holds: a small feeder process writes it while the stage reads */
        pid_t pid = (fork)();
        if (pid == 0) { (close)(pfd[0]); signal(SIGPIPE, SIG_DFL); raw_write(pfd[1], d, len); _exit(0); }
        st->feeder = pid;
    }
    (close)(pfd[1]);
    st->saved0 = fd_dup_high(0); (dup2)(pfd[0], 0); (close)(pfd[0]);
}
/* in-process pipeline: each stage's output is collected in memory (like $(...)) and becomes the next stage's
 * stdin, served from memory to the embedded tools and builtins; a real fd appears only if something needs one */
int bash_rt_pipeline(int n, bash_body_t *stages) {
    int st = 0, pf = 0; char *in = NULL; size_t inlen = 0;
    for (int i = 0; i < n; i++) {
        MinStage ms, *prevst = g_minstage; RtMin *prevmin = bash_rt_min;
        if (i > 0) {
            if (bash_rt_rdc_fd >= 0) bash_rt_fd_sync();
            ms.m.s = in ? in : ""; ms.m.n = inlen; ms.m.p = 0; ms.saved0 = -1; ms.mat = 0;
            g_minstage = &ms; bash_rt_min = &ms.m;
        }
        Cap c; int capping = i < n - 1;
        if (capping) { memset(&c, 0, sizeof c); c.memfd = c.saved = -1; c.sinking = 1; c.prev = g_cap; g_cap = &c; bash_rt_sinking = 1; }
        st = bash_rt_subshell(stages[i]);
        if (st) pf = st;
        char *outb = NULL; size_t outn = 0;
        if (capping) {
            if (c.sinking) { outb = c.s; outn = c.n; }
            else {
                bash_rt_flush();
                if (c.saved >= 0) { dup2(c.saved, 1); close(c.saved); } else close(1);
                SB b = { 0 }; char buf[65536]; raw_lseek_set(c.memfd);
                for (;;) { ssize_t k = read(c.memfd, buf, sizeof buf); if (k < 0 && errno == EINTR) continue; if (k <= 0) break; sb_putn(&b, buf, k); }
                close(c.memfd); outb = b.s; outn = b.n;
            }
            g_cap = c.prev; bash_rt_sinking = g_cap ? g_cap->sinking : 0;
        }
        if (i > 0) {
            if (ms.mat) { if (ms.saved0 >= 0) { dup2(ms.saved0, 0); close(ms.saved0); } else close(0); if (ms.feeder > 0) waitpid(ms.feeder, NULL, 0); }
            g_minstage = prevst; bash_rt_min = prevmin;
        }
        free(in); in = outb; inlen = outn;
    }
    free(in);
    if (opt_pipefail && pf) st = pf;
    return st;
}

/* ------------------------------------------------------------ arithmetic */
typedef struct { const char *p; int err; int depth; } AP;
static long ar_assign(AP *a);
static void ar_ws(AP *a) { while (*a->p == ' ' || *a->p == '\t' || *a->p == '\n') a->p++; }
static int ar_isid(int c) { return isalnum(c) || c == '_'; }
static long ar_value_of(const char *name, int depth) {
    const char *s = bash_rt_getvar(name);
    if (!s || !*s) return 0;
    char *e; long v = strtol(s, &e, 0);
    if (*e == 0) return v;
    if (depth > 8) return 0;
    char *c = xstrdup(s); AP sub = { c, 0, depth + 1 }; v = ar_assign(&sub); free(c); return v;
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
    if (*a->p == '(') { a->p++; long v = ar_assign(a); ar_ws(a); if (*a->p == ')') a->p++; else a->err = 1; return v; }
    if (isdigit((unsigned char)*a->p)) {
        char *e; long v;
        const char *hash = a->p; while (isdigit((unsigned char)*hash)) hash++;
        if (*hash == '#') { int base = atoi(a->p); a->p = hash + 1; v = strtol(a->p, &e, base > 1 && base <= 36 ? base : 10); }
        else v = strtol(a->p, &e, 0);
        a->p = e; return v;
    }
    char *id = ar_ident(a);
    if (!id) { a->err = 1; return 0; }
    long v = ar_value_of(id, a->depth);
    ar_ws(a);
    if ((a->p[0] == '+' && a->p[1] == '+') || (a->p[0] == '-' && a->p[1] == '-')) {
        long nv = v + (a->p[0] == '+' ? 1 : -1); char b[32]; snprintf(b, sizeof b, "%ld", nv);
        bash_rt_set(id, b, 0); a->p += 2;
    }
    free(id); return v;
}
static long ar_unary(AP *a) {
    ar_ws(a);
    char c = *a->p;
    if ((c == '+' && a->p[1] == '+') || (c == '-' && a->p[1] == '-')) {
        a->p += 2; ar_ws(a); char *id = ar_ident(a); if (!id) { a->err = 1; return 0; }
        long v = ar_value_of(id, a->depth) + (c == '+' ? 1 : -1); char b[32]; snprintf(b, sizeof b, "%ld", v);
        bash_rt_set(id, b, 0); free(id); return v;
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
            if (c == '*') l *= r; else if (r == 0) { bash_rt_warn("division by 0"); a->err = 1; return 0; } else l = c == '/' ? bash_rt_div(l, r) : bash_rt_mod(l, r);
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
            if (op != '=') { long o = ar_value_of(id, a->depth);
                switch (op) { case '+': v = o + r; break; case '-': v = o - r; break; case '*': v = o * r; break;
                    case '/': v = bash_rt_div(o, r); break; case '%': v = bash_rt_mod(o, r); break;
                    case '&': v = o & r; break; case '|': v = o | r; break; case '^': v = o ^ r; break;
                    case 'L': v = o << (r & 63); break; case 'R': v = o >> (r & 63); break; } }
            char b[32]; snprintf(b, sizeof b, "%ld", v); bash_rt_set(id, b, 0); free(id); return v;
        }
        free(id);
    }
    a->p = save; return ar_ternary(a);
}
static long ar_eval_str(const char *s, int depth) {
    char *c = xstrdup(s); AP a = { c, 0, depth }; long v = ar_assign(&a); free(c); return v;
}
long bash_rt_arith_eval(const char *expr, int *err) {
    AP a = { expr, 0, 0 }; long v = ar_assign(&a); ar_ws(&a);
    if (*a.p) a.err = 1;
    if (a.err) { if (err) *err = 1; bash_rt_warn("%s: arithmetic syntax error", expr); return 0; }
    if (err) *err = 0; return v;
}
long bash_rt_arith_str(const char *expr) { int e; return bash_rt_arith_eval(expr, &e); }
int bash_rt_arith(const char *expr) { int e; long v = bash_rt_arith_eval(expr, &e); return e ? 1 : (v != 0 ? 0 : 1); }
long bash_rt_div(long a, long b) { if (b == 0) { bash_rt_warn("division by 0"); return 0; } if (b == -1) return (long)(0UL - (unsigned long)a); return a / b; }
long bash_rt_mod(long a, long b) { if (b == 0) { bash_rt_warn("division by 0"); return 0; } if (b == -1) return 0; return a % b; }
long bash_rt_pow(long a, long b) { if (b < 0) return 0; long v = 1; while (b-- > 0) v = (long)((unsigned long)v * (unsigned long)a); return v; }

/* ------------------------------------------- references & specials */
typedef struct { int vid; char *sname; int kind; long idx; int owns; char *key; } Ref;   /* vid: >=0 slot, -1 positional list, -2 special; kind 4: assoc element `key` */
static int is_special_name(const char *n) {
    if (!*n) return 0;
    if (isdigit((unsigned char)n[0])) return 1;
    if (!n[1] && strchr("?#$!-@*0", n[0])) return 1;
    return !strcmp(n, "RANDOM") || !strcmp(n, "PPID") || !strcmp(n, "SECONDS");
}
static void ref_from_name(const char *name, Ref *r) {
    size_t L = strlen(name); const char *br = strchr(name, '['); char *inner = NULL;
    r->kind = 0; r->idx = 0; r->owns = 1; r->key = NULL;
    if (br && br != name && name[L - 1] == ']') { r->sname = xstrndup(name, br - name); inner = xstrndup(br + 1, L - (br - name) - 2); }
    else { r->sname = (char *)name; r->owns = 0; }   /* plain name: borrowed */
    if (!strcmp(r->sname, "@") || !strcmp(r->sname, "*")) r->vid = -1;
    else if (!strcmp(r->sname, "FUNCNAME")) r->vid = -3;
    else if (!strcmp(r->sname, "BASH_SOURCE")) r->vid = -4;
    else if (is_special_name(r->sname)) r->vid = -2;
    else r->vid = name_id(r->sname, 1);
    if (inner) {
        if (!strcmp(inner, "@")) r->kind = 1; else if (!strcmp(inner, "*")) r->kind = 2;
        else if (r->vid >= 0 && (vt[r->vid]->flags & VF_ASC)) { r->kind = 4; r->key = inner; return; }
        else { int e; r->kind = 3; r->idx = bash_rt_arith_eval(inner, &e); }
        free(inner);
    }
}
static void ref_from_key(int id, const char *key, Ref *r) {   /* compiled h[key]: assoc element, or an indexed one if h is not assoc at run time */
    r->vid = id; r->sname = (char *)vt[id]->name; r->owns = 0; r->idx = 0; r->key = NULL;
    if (vt[id]->flags & VF_ASC) { r->kind = 4; r->key = (char *)key; }
    else { int e; r->kind = 3; r->idx = bash_rt_arith_eval(key, &e); }
}
static void ref_from_id(int id, int sub, long idx, Ref *r) { r->vid = id; r->sname = (char *)vt[id]->name; r->kind = sub; r->idx = idx; r->owns = 0; r->key = NULL; }
static void ref_free(Ref *r) { if (r->owns) { free(r->sname); free(r->key); } }   /* key is owned exactly when sname is */
static int ref_is_pos_list(const Ref *r) { return r->vid == -1; }
static int ref_is_list(const Ref *r) { return r->kind == 1 || r->kind == 2 || r->vid == -1; }
static int ref_is_star(const Ref *r) { return r->kind == 2 || (r->vid == -1 && r->sname[0] == '*'); }

/* $RANDOM: bash's generator (Park-Miller, folded to 15 bits, never the same value twice in a row), so
 * RANDOM=seed reproduces bash's sequence */
static unsigned g_rseed = 1; static int g_rlast = -1;
static int bash_random(void) {
    int rv;
    do {
        unsigned r = g_rseed ? g_rseed : 123459876; int h = (int)(r / 127773), l = (int)(r - 127773u * (unsigned)h);
        int t = 16807 * l - 2836 * h; g_rseed = t < 0 ? (unsigned)t + 0x7fffffffu : (unsigned)t;
        rv = (int)(((g_rseed >> 16) ^ (g_rseed & 65535)) & 32767);
    } while (rv == g_rlast);
    return g_rlast = rv;
}
static void random_seed(const char *s) { g_rseed = (unsigned)strtoul(s, NULL, 10); g_rlast = -1; }
const char *bash_rt_special_s(int kind) {
    static char bufs[8][32]; static int bi;
    char *buf = bufs[bi++ & 7];
    switch (kind) {
    case SP_STATUS: snprintf(buf, 32, "%d", g_status); return buf;
    case SP_NARGS: snprintf(buf, 32, "%d", npos); return buf;
    case SP_PID: snprintf(buf, 32, "%d", (int)g_shell_pid); return buf;
    case SP_BANG: if (!g_last_bg) return ""; snprintf(buf, 32, "%d", (int)g_last_bg); return buf;
    case SP_DASH: snprintf(buf, 32, "%s%s", opt_e ? "e" : "", opt_u ? "u" : ""); return buf;
    case SP_ZERO: return g_argv0;
    case SP_RANDOM: snprintf(buf, 32, "%d", bash_random()); return buf;
    case SP_PPID: snprintf(buf, 32, "%d", (int)getppid()); return buf;
    case SP_SECONDS: snprintf(buf, 32, "%ld", (long)(time(NULL) % 1000000)); return buf;
    }
    return "";
}
long bash_rt_special_i(int kind) {
    switch (kind) {
    case SP_STATUS: return g_status; case SP_NARGS: return npos; case SP_PID: return g_shell_pid; case SP_PPID: return getppid(); case SP_BANG: return g_last_bg;
    case SP_RANDOM: return bash_random(); case SP_SECONDS: return (long)(time(NULL) % 1000000);
    }
    return 0;
}
long bash_rt_pos_i_t(int n) { return str_to_long_t(bash_rt_pos_s(n)); }
static const char *special_var(const char *n, int *found) {
    *found = 1;
    if (!strcmp(n, "?")) return bash_rt_special_s(SP_STATUS);
    if (!strcmp(n, "#")) return bash_rt_special_s(SP_NARGS);
    if (!strcmp(n, "$")) return bash_rt_special_s(SP_PID);
    if (!strcmp(n, "!")) return bash_rt_special_s(SP_BANG);
    if (!strcmp(n, "-")) return bash_rt_special_s(SP_DASH);
    if (!strcmp(n, "0")) return g_argv0;
    if (!strcmp(n, "RANDOM")) return bash_rt_special_s(SP_RANDOM);
    if (!strcmp(n, "PPID")) return bash_rt_special_s(SP_PPID);
    if (!strcmp(n, "SECONDS")) return bash_rt_special_s(SP_SECONDS);
    if (isdigit((unsigned char)n[0])) { char *e; long k = strtol(n, &e, 10); if (*e == 0 && k >= 1 && k <= npos) return pos[k - 1]; if (*e == 0) { *found = 0; return NULL; } }
    *found = 0; return NULL;
}
static const char *fnstack_elem(int which, long idx) {   /* FUNCNAME (-3) / BASH_SOURCE (-4) */
    if (idx < 0 || idx > nframes) return NULL;
    if (which == -4) return g_argv0;
    if (nframes == 0) return NULL;
    return idx == nframes ? "main" : frames[nframes - 1 - idx].fname;
}
static const char *ref_scalar(const Ref *r) {   /* NULL if unset */
    if (r->vid == -2) { int f; return special_var(r->sname, &f); }
    if (r->vid <= -3) return fnstack_elem(r->vid, r->kind == 3 ? r->idx : 0);
    if (r->vid == -1) return NULL;
    Var *v = vt[r->vid];
    if (r->kind == 4) { if (!(v->flags & VF_ASC)) return NULL; AItem *it = asc_find(v->as, r->key, 0); return it ? ai_str(it) : NULL; }
    if (r->kind == 3) {
        if (!var_isset(v)) return NULL;
        if (!(v->flags & VF_ARR)) return r->idx == 0 ? var_str(v) : NULL;
        long i = r->idx < 0 ? r->idx + v->arrn : r->idx;
        return (i >= 0 && i < v->arrn) ? el_str(v, i) : NULL;
    }
    return var_str(v);
}
static int ref_list(const Ref *r, const char ***out) {   /* borrowed strings, malloc'd array */
    const char **l; int n = 0;
    if (r->vid == -1) { l = xmalloc((npos + 1) * sizeof *l); for (int i = 0; i < npos; i++) l[n++] = pos[i]; }
    else if (r->vid <= -3) { l = xmalloc((nframes + 2) * sizeof *l); for (int i = 0; (nframes || r->vid == -4) && i <= nframes; i++) l[n++] = fnstack_elem(r->vid, i); }
    else {
        Var *v = vt[r->vid];
        if (!var_isset(v)) l = xmalloc(sizeof *l);
        else if (v->flags & VF_ASC) { *out = NULL; return asc_list(v->as, out, 0); }
        else if (v->flags & VF_ARR) { l = xmalloc((v->arrn + 1) * sizeof *l); for (int i = 0; i < v->arrn; i++) if (v->ak[i] & AK_SET) l[n++] = el_str(v, i); }
        else { l = xmalloc(2 * sizeof *l); l[n++] = var_str(v); }
    }
    *out = l; return n;
}
static void ref_assign(const Ref *r, const char *s) {
    if (r->vid < 0) return;
    if (r->kind == 4) { if (vt[r->vid]->flags & VF_RO) { bash_rt_warn("%s: readonly variable", r->sname); return; } asc_set_v(vt[r->vid], r->key, s, 0); return; }
    if (r->kind == 3) bash_rt_set_elem_id(r->vid, r->idx, s, 0); else var_set_str(vt[r->vid], s, 0);
}

const char *bash_rt_getvar(const char *name) { Ref r; ref_from_name(name, &r); const char *s = ref_scalar(&r); ref_free(&r); return s; }
void bash_rt_setvar(const char *name, const char *val) { bash_rt_set(name, val, 0); }
void bash_rt_export(const char *name) { bash_rt_export_id(name_id(name, 1)); }
void bash_rt_mark_readonly(const char *name) { bash_rt_readonly_id(name_id(name, 1)); }
void bash_rt_unsetvar(const char *name) {
    Ref r; ref_from_name(name, &r);
    if (r.vid >= 0) {
        Var *v = vt[r.vid];
        if (r.kind == 4) { if (v->flags & VF_ASC) { touch(v); asc_del(v->as, r.key); if (!strcmp(r.key, "0")) v->ver = ++g_ver; } }
        else if (r.kind == 3 && (v->flags & VF_ARR)) { long i = r.idx < 0 ? r.idx + v->arrn : r.idx; if (i >= 0 && i < v->arrn) { touch(v); free(v->arr[i]); v->arr[i] = NULL; v->ak[i] = 0; } }
        else bash_rt_unset_id(r.vid);
    }
    ref_free(&r);
}
void bash_rt_set(const char *name, const char *val, int append) {
    Ref r; ref_from_name(name, &r);
    if (r.vid >= 0) { if (r.kind == 4) { if (vt[r.vid]->flags & VF_RO) bash_rt_warn("%s: readonly variable", r.sname); else asc_set_v(vt[r.vid], r.key, val, append); } else if (r.kind == 3) bash_rt_set_elem_id(r.vid, r.idx, val, append); else var_set_str(vt[r.vid], val, append); }
    ref_free(&r);
}
void bash_rt_set_array(const char *name, int argc, char **argv, int append) { bash_rt_set_array_id(name_id(name, 1), argc, argv, append); }
char **bash_rt_exported_env(int *n) {
    char **out = xmalloc((nvt + 1) * sizeof *out); int k = 0;
    for (int i = 0; i < nvt; i++) {
        Var *v = vt[i]; const char *s = var_str(v);
        if ((v->flags & VF_EXP) && s) { out[k] = xmalloc(strlen(v->name) + strlen(s) + 2); sprintf(out[k], "%s=%s", v->name, s); k++; }
    }
    *n = k; return out;
}
static int id_ifs = -1;
static const char *ifs_chars(void) {
    if (id_ifs < 0) id_ifs = name_id("IFS", 1);
    const char *s = var_str(vt[id_ifs]); return s ? s : " \t\n";
}

/* ------------------------------------------------------- word builder */
typedef struct { size_t end; unsigned char kind; } Seg;
struct WordBuilder {
    int mode; char *buf; size_t n, cap; Seg *seg; int nseg, capseg; int have, glob;
    char **argv; int argc, capargv;
    char *ab; size_t an, acap; int fixed;   /* argument strings live in one arena; argv holds offsets until wb_argv */
};
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
    if (b->mode == WB_PATTERN && kind == LIT_QUOTED) {
        for (size_t i = 0; i < n; i++) { if (is_special_glob(s[i])) { if (b->n + 2 > b->cap) { b->cap = (b->n + 2) * 2; b->buf = xrealloc(b->buf, b->cap); } b->buf[b->n++] = '\\'; }
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
void bash_rt_wb_int(WordBuilder *b, long v, int quoted) { char t[32]; snprintf(t, sizeof t, "%ld", v); wb_append(b, t, quoted); }
void bash_rt_wb_special(WordBuilder *b, int kind, int quoted) {
    if (kind == SP_AT || kind == SP_STAR) { wb_emit_list(b, (const char **)pos, npos, quoted, kind == SP_STAR); return; }
    if (kind == SP_FUNCNAME || kind == SP_SOURCE) { const char *v = fnstack_elem(kind == SP_FUNCNAME ? -3 : -4, 0); wb_append(b, v ? v : "", quoted); return; }
    wb_append(b, bash_rt_special_s(kind), quoted);
}

/* ---- parameter expansion ---- */
static int has_glob(const char *p) { return strpbrk(p, "*?[\\") != NULL; }
static char *trim_prefix(const char *s, const char *pat, int longest) {
    size_t L = strlen(s);
    if (!has_glob(pat)) { size_t pl = strlen(pat); return xstrdup(pl <= L && !strncmp(s, pat, pl) ? s + pl : s); }
    char *t = xstrdup(s);
    for (size_t k = 0; k <= L; k++) {
        size_t i = longest ? L - k : k; char save = t[i]; t[i] = 0; int m = fnmatch(pat, t, 0) == 0; t[i] = save;
        if (m) { free(t); return xstrdup(s + i); }
    }
    free(t); return xstrdup(s);
}
static char *trim_suffix(const char *s, const char *pat, int longest) {
    size_t L = strlen(s);
    if (!has_glob(pat)) { size_t pl = strlen(pat); return (pl <= L && !strcmp(s + L - pl, pat)) ? xstrndup(s, L - pl) : xstrdup(s); }
    for (size_t k = 0; k <= L; k++) {
        size_t i = longest ? k : L - k;
        if (fnmatch(pat, s + i, 0) == 0) return xstrndup(s, i);
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
    size_t p = 0; int done = 0; char *t = xmalloc(L + 1);
    while (p <= L) {
        long mlen = -1;
        if (!(done && op != P_SUBST_ALL) && !(op == P_SUBST_PRE && p != 0)) {
            for (size_t e = L; e > p; e--) {
                if (op == P_SUBST_SUF && e != L) continue;
                memcpy(t, s + p, e - p); t[e - p] = 0;
                if (fnmatch(pat, t, 0) == 0) { mlen = e - p; break; }
            }
        }
        if (mlen > 0) { sb_puts(&o, rep); p += mlen; done = 1; if (op == P_SUBST_PRE || op == P_SUBST_SUF) { sb_puts(&o, s + p); p = L + 1; } }
        else { if (p < L) sb_putc(&o, s[p]); p++; }
    }
    free(t); return sb_take(&o);
}
static char *casemod(const char *s, int op) {
    char *o = xstrdup(s); size_t L = strlen(o);
    for (size_t i = 0; i < L; i++) {
        if (op == P_UP1 || op == P_LOW1) { if (i) break; }
        o[i] = (op == P_UP1 || op == P_UPALL) ? toupper((unsigned char)o[i]) : tolower((unsigned char)o[i]);
    }
    return o;
}
typedef struct { bash_str_t f; const char *c; } Opd;
static char *opd_get(const Opd *o, int *owned) { if (o->f) { *owned = 1; return o->f(); } *owned = 0; return (char *)(o->c ? o->c : ""); }

static size_t u8_len(const char *s); static size_t u8_off(const char *s, long chars);
static void param_core(WordBuilder *b, Ref *r, int op, Opd A, Opd C, int quoted) {
    int islist = ref_is_list(r);
    switch (op) {
    case P_LEN: {
        char buf[32];
        if (islist) { const char **l; int n = ref_list(r, &l); free(l); snprintf(buf, sizeof buf, "%d", n); }
        else { const char *s = ref_scalar(r); snprintf(buf, sizeof buf, "%zu", s ? u8_len(s) : (size_t)0); }
        wb_append(b, buf, quoted); break; }
    case P_KEYS: {
        Var *v = r->vid >= 0 ? vt[r->vid] : NULL; char buf[32];
        int cap = (v && (v->flags & VF_ARR) ? v->arrn : 1) + 1; const char **l = xmalloc(cap * sizeof *l); char **store = xmalloc(cap * sizeof *store); int n = 0;
        if (v && (v->flags & VF_ASC)) { const char **kl; int kn = asc_list(v->as, &kl, 1); wb_emit_list(b, kl, kn, quoted, r->kind == 2); free(kl); free(store); free(l); break; }
        if (v && (v->flags & VF_ARR)) { for (int i = 0; i < v->arrn; i++) if (v->ak[i] & AK_SET) { snprintf(buf, sizeof buf, "%d", i); store[n] = xstrdup(buf); l[n] = store[n]; n++; } }
        else if (v && var_isset(v)) { store[n] = xstrdup("0"); l[n] = store[n]; n++; }
        wb_emit_list(b, l, n, quoted, r->kind == 2);
        for (int i = 0; i < n; i++) free(store[i]);
        free(store); free(l); break; }
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
    char *e; long x = strtol(s, &e, 0);
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

int bash_rt_case_match(const char *word, const char *pattern) { return fnmatch(pattern, word, 0) == 0; }

/* ---------------------------------------------- functions & dispatch */
typedef struct { char *name; bash_fn_t fn; } FnEnt;
static FnEnt *funcs; static int nfuncs, capfuncs;
void bash_rt_defun(const char *name, bash_fn_t fn) {
    for (int i = 0; i < nfuncs; i++) if (!strcmp(funcs[i].name, name)) { funcs[i].fn = fn; return; }
    if (nfuncs == capfuncs) { capfuncs = capfuncs ? capfuncs * 2 : 16; funcs = xrealloc(funcs, capfuncs * sizeof *funcs); }
    funcs[nfuncs].name = xstrdup(name); funcs[nfuncs].fn = fn; nfuncs++;
}
static const struct { const char *name; bash_fn_t fn; int ext; } tool_table[] = {
#define INTERNAL(id, name) { name, bash_internal_##id, 0 },
#define EXTERNAL(id, name) { name, bash_external_##id, 1 },
#include "tools.def"
#undef INTERNAL
#undef EXTERNAL
    { NULL, NULL, 0 }
};
static int tool_is_external(int i) { return tool_table[i].ext; }
static int run_impl(int argc, char **argv, int fns) {
    if (argc == 0) return g_cs_status;
    if (fns) for (int i = 0; i < nfuncs; i++) if (!strcmp(funcs[i].name, argv[0])) return funcs[i].fn(argc, argv);
    for (int i = 0; tool_table[i].name; i++) if (!strcmp(tool_table[i].name, argv[0])) {
        if (tool_is_external(i)) return bash_rt_tool_call(i, tool_table[i].fn, argc, argv);
        return tool_table[i].fn(argc, argv);
    }
    return bash_rt_exec_external(argc, argv);
}
int bash_rt_run(int argc, char **argv) { return run_impl(argc, argv, 1); }
int bash_rt_run_nofunc(int argc, char **argv) { return run_impl(argc, argv, 0); }
int bash_rt_kind(const char *name) {
    for (int i = 0; i < nfuncs; i++) if (!strcmp(funcs[i].name, name)) return 1;
    for (int i = 0; tool_table[i].name; i++) if (!strcmp(tool_table[i].name, name)) return tool_table[i].ext ? 3 : 2;
    return 0;
}
int bash_rt_is_known(const char *name) {
    for (int i = 0; i < nfuncs; i++) if (!strcmp(funcs[i].name, name)) return 1;
    for (int i = 0; tool_table[i].name; i++) if (!strcmp(tool_table[i].name, name)) return 1;
    return 0;
}
int bash_rt_return_status(int argc, char **argv) {
    if (argc > 1) g_status = atoi(argv[1]) & 255;
    return g_status;
}

/* ------------------------------------------------------------- tests */
void bash_rt_test_err_clear(void) { test_err = 0; }
int bash_rt_test_err(void) { return test_err; }
int bash_rt_test_status(int ok) { int e = test_err; test_err = 0; return e ? 2 : (ok ? 0 : 1); }
int bash_rt_streq(const char *a, const char *b) { return strcmp(a, b) == 0; }
int bash_rt_fnm(const char *pat, const char *s) { return fnmatch(pat, s, 0) == 0; }
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
int bash_rt_regex(const char *s, const char *pat) {
    static struct { const char *pat; regex_t re; int ok; } cache[8]; static int next;
    int i; for (i = 0; i < 8; i++) if (cache[i].pat == pat) break;
    if (i == 8) { i = next++ & 7; if (cache[i].pat && cache[i].ok) regfree(&cache[i].re); cache[i].pat = pat; cache[i].ok = regcomp(&cache[i].re, pat, REG_EXTENDED) == 0; }
    if (!cache[i].ok) return 0;
    return regex_match(&cache[i].re, s);
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
    if (t->dbl) { int e; return bash_rt_arith_eval(unesc(s), &e); }
    char *e; long v = strtol(s, &e, 10); if (*e || !*s) { bash_rt_warn("[: %s: integer expression expected", s); test_err = 1; return 0; } return v;
}
static int ts_prim(TS *t) {
    if (t->i >= t->n) return -1;
    int rem = t->n - t->i;
    if (!strcmp(t->a[t->i], "(") && rem >= 2) { t->i++; int v = ts_or(t); if (t->i < t->n && !strcmp(t->a[t->i], ")")) t->i++; return v; }
    if (rem >= 3 && is_binop(t, t->a[t->i + 1])) {
        char *l = sunesc(t, t->a[t->i]); const char *op = t->a[t->i + 1]; const char *rr = t->a[t->i + 2]; t->i += 3; int res = 0;
        if (!strcmp(op, "=") || !strcmp(op, "==")) res = t->dbl ? fnmatch(rr, l, 0) == 0 : !strcmp(l, rr);
        else if (!strcmp(op, "!=")) res = t->dbl ? fnmatch(rr, l, 0) != 0 : strcmp(l, rr) != 0;
        else if (!strcmp(op, "=~")) { regex_t re; if (regcomp(&re, unesc(rr), REG_EXTENDED) == 0) { res = regex_match(&re, l); regfree(&re); } else { res = 0; test_err = 1; } }
        else if (!strcmp(op, "<")) res = (t->dbl ? strcoll(l, unesc(rr)) : strcmp(l, rr)) < 0;
        else if (!strcmp(op, ">")) res = (t->dbl ? strcoll(l, unesc(rr)) : strcmp(l, rr)) > 0;
        else if (!strcmp(op, "-nt") || !strcmp(op, "-ot") || !strcmp(op, "-ef")) {
            struct stat a, b; int ea = stat(l, &a), eb = stat(t->dbl ? unesc(rr) : rr, &b);
            if (!strcmp(op, "-nt")) res = ea == 0 && (eb != 0 || a.st_mtime > b.st_mtime);
            else if (!strcmp(op, "-ot")) res = eb == 0 && (ea != 0 || a.st_mtime < b.st_mtime);
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
int bash_rt_test(int n, char **a, int dbl) {
    if (n == 0) return 1;
    TS t = { a, n, 0, dbl }; test_err = 0; int v = ts_or(&t);
    if (test_err) { test_err = 0; return 2; }
    if (v < 0 || t.i != t.n) { bash_rt_warn("test: syntax error"); return 2; }
    return v ? 0 : 1;
}

/* -------------------------------------------------------------- init */
void bash_rt_init(int argc, char **argv, const char **names, int nnames) {
    g_argv0 = argc > 0 ? argv[0] : "bash";
    o_line = isatty(1);
    ht_rebuild(1024);
    for (int i = 0; i < nnames; i++) vt_add(names[i], 0);        /* slot i == compile-time id i */
    for (char **e = environ; e && *e; e++) {
        char *eq = strchr(*e, '='); if (!eq) continue;
        char *n = xstrndup(*e, eq - *e); int id = name_id(n, 1); free(n);
        Var *v = vt[id]; v->val = xstrdup(eq + 1); v->flags = VF_STR | VF_EXP;
    }
    char *cwd = getcwd(NULL, 0);
    if (cwd) { Var *v = vt[name_id("PWD", 1)]; free(v->val); v->val = cwd; v->flags = VF_STR | VF_EXP; }
    if (!var_isset(vt[name_id("HOME", 1)])) { Var *v = vt[name_id("HOME", 1)]; v->val = xstrdup("/"); v->flags = VF_STR; }
    pos = argv + 1; npos = argc > 1 ? argc - 1 : 0; pos_block = NULL;
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    g_rseed = (unsigned)time(NULL) ^ ((unsigned)getpid() << 8); g_random_var = vt[name_id("RANDOM", 1)];
    g_shell_pid = getpid();
    static const char *locvars[] = { "LC_ALL", "LC_CTYPE", "LC_COLLATE", "LANG", NULL };
    for (int i = 0; locvars[i]; i++) vt[name_id(locvars[i], 1)]->flags |= VF_LOCALE;
    bash_rt_locale_changed();
}


/* ===================================================================== locale
 * The shell's own locale follows LC_ALL / LC_CTYPE / LC_COLLATE / LANG like bash (even when not exported). */
static int g_utf8;
static const char *locale_for(const char *cat) {
    const char *v = bash_rt_getvar("LC_ALL"); if (v && *v) return v;
    v = bash_rt_getvar(cat); if (v && *v) return v;
    v = bash_rt_getvar("LANG"); if (v && *v) return v;
    return "C";
}
void bash_rt_locale_changed(void) {
    if (!setlocale(LC_CTYPE, locale_for("LC_CTYPE"))) setlocale(LC_CTYPE, "C");
    if (!setlocale(LC_COLLATE, locale_for("LC_COLLATE"))) setlocale(LC_COLLATE, "C");
    g_utf8 = MB_CUR_MAX > 1;
}
int bash_rt_utf8(void) { return g_utf8; }
/* locale that a *child program* would see: only exported variables count */
static const char *exported_val(const char *name) { int id = name_id(name, 0); if (id < 0) return NULL; Var *v = vt[id]; return (v->flags & VF_EXP) ? var_str(v) : NULL; }
static int is_c_name(const char *v) { return !v || !*v || !strcmp(v, "C") || !strcmp(v, "POSIX") || !strncmp(v, "C.", 2); }
int bash_rt_child_c_locale(void) {
    const char *v = exported_val("LC_ALL"); if (v && *v) return is_c_name(v);
    v = exported_val("LC_COLLATE"); if (v && *v) { if (!is_c_name(v)) return 0; }
    else { v = exported_val("LANG"); if (v && *v && !is_c_name(v)) return 0; }
    v = exported_val("LC_CTYPE"); if (v && *v && !is_c_name(v)) return 0;
    return 1;
}
static size_t u8_len(const char *s) {
    if (!g_utf8) return strlen(s);
    size_t n = 0; mbstate_t st; memset(&st, 0, sizeof st); size_t L = strlen(s);
    while (L) { size_t k = mbrlen(s, L, &st); if (k == (size_t)-1 || k == (size_t)-2) { k = 1; memset(&st, 0, sizeof st); } else if (k == 0) k = 1; s += k; L -= k; n++; }
    return n;
}
static size_t u8_off(const char *s, long chars) {   /* byte offset of the chars-th character */
    if (!g_utf8) { size_t L = strlen(s); return (size_t)chars < L ? (size_t)chars : L; }
    const char *p = s; mbstate_t st; memset(&st, 0, sizeof st);
    while (chars-- > 0 && *p) {
        if ((unsigned char)*p < 0x80) { p++; continue; }
        size_t k = mbrlen(p, strnlen(p, MB_LEN_MAX), &st); if (k == (size_t)-1 || k == (size_t)-2 || k == 0) { k = 1; memset(&st, 0, sizeof st); } p += k; }
    return p - s;
}

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
    char *e; long x = strtol(s, &e, 0); return *e == 0 ? x : ar_eval_str(s, 1);
}
void bash_rt_wb_substr_id(WordBuilder *b, int id, long off, long len, int haslen, int quoted) {
    const char *s = var_str(vt[id]);
    if (!s) { if (opt_u) { bash_rt_warn("%s: unbound variable", vt[id]->name); bash_rt_exit(1); } s = ""; }
    if (off >= 0 && (!haslen || len >= 0)) {   /* common case: no need to measure the whole string */
        size_t bo, n; SInfo *c = sinfo(id, s);
        if (c->ascii) { if ((size_t)off > c->blen) return; bo = (size_t)off; n = c->blen - bo; if (haslen && (size_t)len < n) n = (size_t)len; }
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
    if (off > L) return;    /* negative offset before the start: empty */
    long cnt = haslen ? (len < 0 ? L - off + len : len) : L - off;
    if (cnt < 0) { if (haslen && len < 0) { bash_rt_warn("%ld: substring expression < 0", len); } cnt = 0; }
    if (off + cnt > L) cnt = L - off;
    size_t bo = u8_off(s, off), be = bo + u8_off(s + bo, cnt);
    char tmp[256]; size_t n = be - bo;
    if (n < sizeof tmp) { memcpy(tmp, s + bo, n); tmp[n] = 0; wb_append(b, tmp, quoted); }
    else { char *t = xstrndup(s + bo, n); wb_append(b, t, quoted); free(t); }
}

/* ================================================================== processes */
static struct { char *name, *full, *path; } pcache[64]; static int npcache;
char *bash_rt_path_search(const char *name) {      /* malloc'd absolute path or NULL */
    if (!*name) return NULL;
    if (strchr(name, '/')) return xstrdup(name);
    const char *path = bash_rt_getvar("PATH"); if (!path) path = "/usr/local/bin:/usr/bin:/bin";
    for (int i = 0; i < npcache; i++) if (!strcmp(pcache[i].name, name) && !strcmp(pcache[i].path, path)) {
        if (access(pcache[i].full, X_OK) == 0) return xstrdup(pcache[i].full); break; }
    const char *p = path; char buf[4096];
    for (;;) {
        const char *e = strchr(p, ':'); size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l == 0) snprintf(buf, sizeof buf, "%s", name); else snprintf(buf, sizeof buf, "%.*s/%s", (int)l, p, name);
        struct stat st;
        if (stat(buf, &st) == 0 && S_ISREG(st.st_mode) && access(buf, X_OK) == 0) {
            int slot = npcache < 64 ? npcache++ : (int)(hash_str(name) & 63);
            if (slot < npcache && pcache[slot].name) { free(pcache[slot].name); free(pcache[slot].full); free(pcache[slot].path); }
            pcache[slot].name = xstrdup(name); pcache[slot].full = xstrdup(buf); pcache[slot].path = xstrdup(path);
            return xstrdup(buf);
        }
        if (!e) break; p = e + 1;
    }
    return NULL;
}
static char **build_envp(void) { int n; char **e = bash_rt_exported_env(&n); e[n] = NULL; return e; }
static void free_envp(char **e) { for (char **p = e; *p; p++) free(*p); free(e); }
static int wait_status(pid_t pid) {
    int st;
    for (;;) { if (waitpid(pid, &st, 0) >= 0) break; if (errno == EINTR) continue; return 127; }
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    if (WIFSIGNALED(st)) { if (WTERMSIG(st) == SIGINT && !g_is_child) { /* like bash: die the same way */ } return 128 + WTERMSIG(st); }
    return 1;
}
static int find_bash(char *out, size_t n);
static int spawn_wait(const char *path, char **argv) {
    char **envp = build_envp(); pid_t pid;
    int rc = posix_spawn(&pid, path, NULL, NULL, argv, envp);
    if (rc == ENOEXEC) {      /* script without #!: run it with bash, like bash does */
        char bash[4096]; int argc = 0; while (argv[argc]) argc++;
        if (find_bash(bash, sizeof bash)) {
            char **av = xmalloc((argc + 2) * sizeof *av); av[0] = "bash"; av[1] = (char *)path; for (int i = 1; i <= argc; i++) av[i + 1] = argv[i];
            rc = posix_spawn(&pid, bash, NULL, NULL, av, envp); free(av);
        }
    }
    free_envp(envp);
    if (rc != 0) {
        bash_rt_warn("%s: %s", argv[0], strerror(rc));
        return rc == ENOENT ? 127 : 126;
    }
    return wait_status(pid);
}
int bash_rt_exec_external(int argc, char **argv) {
    if (argc == 0) return 0;
    char *path = bash_rt_path_search(argv[0]);
    if (!path) { bash_rt_warn("%s: command not found", argv[0]); return 127; }
    struct stat st;
    if (stat(path, &st) != 0) { bash_rt_warn("%s: No such file or directory", argv[0]); free(path); return 127; }
    if (S_ISDIR(st.st_mode)) { bash_rt_warn("%s: Is a directory", argv[0]); free(path); return 126; }
    bash_rt_flush();
    int r = spawn_wait(path, argv); free(path); return r;
}
int bash_rt_tool_ret(int r, int argc, char **argv) { return r == BASH_TOOL_FALLBACK ? bash_rt_exec_external(argc, argv) : r; }
int bash_rt_tool_call(int idx, bash_fn_t fn, int argc, char **argv) {
    if (bash_rt_rdc_fd >= 0) bash_rt_fd_sync();   /* embedded tools may read or seek that fd */
    if (!bash_rt_tool_gate(idx, argc, argv)) return bash_rt_exec_external(argc, argv);
    int r = fn(argc, argv);
    return r == BASH_TOOL_FALLBACK ? bash_rt_exec_external(argc, argv) : r;
}
/* exec builtin: replace the shell (or, inside an in-process subshell, the subshell) */
int bash_rt_exec_builtin(int argc, char **argv) {
    int i = 1; while (i < argc && argv[i][0] == '-' && argv[i][1]) { if (!strcmp(argv[i], "--")) { i++; break; } i++; }
    if (i >= argc) return 0;
    argc -= i; argv += i;
    int known = bash_rt_is_known(argv[0]);
    if (known) { int st = bash_rt_run(argc, argv); bash_rt_exit(st); return st; }
    char *path = bash_rt_path_search(argv[0]);
    if (!path) { bash_rt_warn("%s: not found", argv[0]); bash_rt_exit(127); return 127; }
    bash_rt_flush();
    if (cur_sub) { int st = spawn_wait(path, argv); free(path); bash_rt_exit(st); return st; }
    char **envp = build_envp(); execve(path, argv, envp);
    int e = errno;
    if (e == ENOEXEC) { char bash[4096]; if (find_bash(bash, sizeof bash)) { int n = 0; while (argv[n]) n++; char **av = xmalloc((n + 2) * sizeof *av); av[0] = "bash"; av[1] = path; for (int k = 1; k <= n; k++) av[k + 1] = argv[k]; execve(bash, av, envp); } }
    bash_rt_warn("%s: %s", argv[0], strerror(e)); free_envp(envp); free(path);
    bash_rt_exit(e == ENOENT ? 127 : 126); return 126;
}

/* ---- forked children: pipelines with real programs, background jobs ---- */
static void child_init(void) {
    g_is_child = 1; cur_sub = NULL;
    for (int s = 1; s < 65; s++) if (trap_mode[s] == 2) { signal(s, SIG_DFL); trap_mode[s] = 0; }
    trap_mode[0] = 0; g_trap_pending = 0;
}
static void run_child(bash_body_t body) { child_init(); int st = body(); bash_rt_exit(st); _exit(st & 255); }
int bash_rt_pipeline_fork(int n, bash_body_t *stages) {
    bash_rt_flush();
    pid_t pids[n]; int in = -1;
    for (int i = 0; i < n; i++) {
        int pfd[2] = { -1, -1 };
        if (i < n - 1 && pipe2(pfd, O_CLOEXEC) != 0) { bash_rt_warn("pipe: %s", strerror(errno)); return 1; }
        pid_t pid = fork();
        if (pid == 0) {
            if (in >= 0) { dup2(in, 0); close(in); }
            if (i < n - 1) { dup2(pfd[1], 1); close(pfd[1]); close(pfd[0]); }
            run_child(stages[i]);
        }
        if (pid < 0) { bash_rt_warn("fork: %s", strerror(errno)); return 1; }
        if (in >= 0) close(in);
        if (i < n - 1) { close(pfd[1]); in = pfd[0]; }
        pids[i] = pid;
    }
    int st = 0, pf = 0;
    for (int i = 0; i < n; i++) { st = wait_status(pids[i]); if (st) pf = st; }
    if (opt_pipefail && pf) st = pf;
    return st;
}
typedef struct { pid_t pid; int done, status; } Job;
static Job *jobs; static int njobs, capjobs;
void bash_rt_background(bash_body_t body) {
    bash_rt_flush();
    pid_t pid = fork();
    if (pid == 0) { int fd = open("/dev/null", O_RDONLY); if (fd >= 0) { dup2(fd, 0); if (fd) close(fd); } run_child(body); }
    if (pid < 0) { bash_rt_warn("fork: %s", strerror(errno)); body(); return; }
    if (njobs == capjobs) { capjobs = capjobs ? capjobs * 2 : 16; jobs = xrealloc(jobs, capjobs * sizeof *jobs); }
    jobs[njobs].pid = pid; jobs[njobs].done = 0; jobs[njobs].status = 0; njobs++;
    g_last_bg = pid;
}
int bash_rt_wait(int argc, char **argv) {
    bash_rt_flush();
    if (argc <= 1) { for (int i = 0; i < njobs; i++) if (!jobs[i].done) { jobs[i].status = wait_status(jobs[i].pid); jobs[i].done = 1; } return 0; }
    int st = 0;
    for (int a = 1; a < argc; a++) {
        const char *s = argv[a]; Job *j = NULL;
        if (s[0] == '%') { long k = atol(s + 1); if (k >= 1 && k <= njobs) j = &jobs[k - 1]; }
        else { pid_t p = (pid_t)atol(s); for (int i = njobs - 1; i >= 0; i--) if (jobs[i].pid == p) { j = &jobs[i]; break; } }
        if (!j) { bash_rt_warn("wait: pid %s is not a child of this shell", s); st = 127; continue; }
        if (!j->done) { j->status = wait_status(j->pid); j->done = 1; }
        st = j->status;
    }
    return st;
}

/* ---- traps ---- */
static volatile sig_atomic_t trap_hit[65];
static const struct { const char *n; int s; } rt_sigs[] = {
    {"EXIT",0},{"HUP",SIGHUP},{"INT",SIGINT},{"QUIT",SIGQUIT},{"USR1",SIGUSR1},{"USR2",SIGUSR2},{"PIPE",SIGPIPE},{"ALRM",SIGALRM},
    {"TERM",SIGTERM},{"CHLD",SIGCHLD},{"CONT",SIGCONT},{"TSTP",SIGTSTP},{"WINCH",SIGWINCH},{NULL,0} };
static int sig_from_name(const char *s) {
    if (isdigit((unsigned char)*s)) { int v = atoi(s); return v >= 0 && v < 65 ? v : -1; }
    if (!strncasecmp(s, "SIG", 3)) s += 3;
    for (int i = 0; rt_sigs[i].n; i++) if (!strcasecmp(rt_sigs[i].n, s)) return rt_sigs[i].s;
    return -1;
}
static void on_trap_sig(int s) { trap_hit[s] = 1; g_trap_pending = 1; }
static void install_sig(int s) {
    if (s <= 0) return;
    struct sigaction sa; memset(&sa, 0, sizeof sa); sigemptyset(&sa.sa_mask);
    sa.sa_handler = trap_mode[s] == 2 ? on_trap_sig : trap_mode[s] == 1 ? SIG_IGN : SIG_DFL;
    sigaction(s, &sa, NULL);
}
typedef struct { int mode[65]; bash_body_t fn[65]; } TrapSave;
void bash_rt_trap(const char *sig, int mode, bash_body_t fn) {
    int s = sig_from_name(sig); if (s < 0) { bash_rt_warn("trap: %s: invalid signal specification", sig); return; }
    if (cur_sub && !cur_sub->trapsave) { TrapSave *t = xmalloc(sizeof *t); memcpy(t->mode, trap_mode, sizeof trap_mode); memcpy(t->fn, trap_fn, sizeof trap_fn); cur_sub->trapsave = t; }
    trap_mode[s] = mode; trap_fn[s] = fn; install_sig(s);
}
static void subshell_traps_end(Sub *s) {
    TrapSave *t = s->trapsave; s->trapsave = NULL;
    if (trap_mode[0] == 2 && trap_fn[0] != t->fn[0]) { bash_body_t f = trap_fn[0]; trap_mode[0] = 0; if (_setjmp(s->jb) == 0) f(); }
    memcpy(trap_mode, t->mode, sizeof trap_mode); memcpy(trap_fn, t->fn, sizeof trap_fn);
    for (int k = 1; k < 65; k++) install_sig(k);
    free(t);
}
__attribute__((noinline, cold)) static void run_pending_traps(void) {
    g_trap_pending = 0; int saved = g_status;
    for (int s = 1; s < 65; s++) if (trap_hit[s]) { trap_hit[s] = 0; if (trap_mode[s] == 2 && trap_fn[s]) trap_fn[s](); }
    g_status = saved;
}

/* ---- whole-script fallback: run the embedded source with the system bash ---- */
static int find_bash(char *out, size_t n) {
    const char *env = getenv("BASHFUL_BASH");
    const char *cands[] = { env, "/bin/bash", "/usr/bin/bash", "/usr/local/bin/bash", NULL };
    for (int i = 0; i < 4; i++) if (cands[i] && *cands[i] && access(cands[i], X_OK) == 0) { snprintf(out, n, "%s", cands[i]); return 1; }
    const char *path = getenv("PATH");
    for (const char *p = path; p && *p; ) {
        const char *e = strchr(p, ':'); size_t l = e ? (size_t)(e - p) : strlen(p);
        snprintf(out, n, "%.*s/bash", (int)l, p); if (l && access(out, X_OK) == 0) return 1;
        if (!e) break; p = e + 1;
    }
    return 0;
}
int bash_rt_fallback(int argc, char **argv, const char *script) {
    char bash[4096];
    if (!find_bash(bash, sizeof bash)) { fprintf(stderr, "%s: this program needs bash at run time (set BASHFUL_BASH)\n", argc ? argv[0] : "bashful"); return 127; }
    char **av = malloc((argc + 5) * sizeof *av); int k = 0; char cmd[64];
    av[k++] = "bash";
    if (strlen(script) < 100000) { av[k++] = "-c"; av[k++] = (char *)script; }
    else {   /* too long for one argument: source it from a memfd */
        int fd = memfd_create("bashful-script", 0);
        if (fd < 0) { perror("memfd_create"); return 127; }
        write_all(fd, script, strlen(script)); lseek(fd, 0, SEEK_SET);
        snprintf(cmd, sizeof cmd, ". /dev/fd/%d", fd); av[k++] = "-c"; av[k++] = cmd;
    }
    av[k++] = argc > 0 ? argv[0] : "bash";
    for (int i = 1; i < argc; i++) av[k++] = argv[i];
    av[k] = NULL;
    execv(bash, av);
    perror(bash); return 127;
}

int bash_tools_gate(const char *name, int argc, char **argv);
int bash_rt_tool_gate(int idx, int argc, char **argv) { return bash_tools_gate(tool_table[idx].name, argc, argv); }
