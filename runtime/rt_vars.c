/* rt_vars.c - variables (scalars, indexed and associative arrays), positional and special parameters */
#include "rt.h"

unsigned long g_ver;   /* every scalar write gets a fresh version (substring position caches) */
static unsigned asc_hash(const char *s) { unsigned i = 2166136261u; for (; *s; s++) { i *= 16777619u; i ^= (unsigned)(int)(signed char)*s; } return i; }
Asc *asc_new(void) { Asc *a = malloc(sizeof *a); a->nb = 1024; a->n = 0; a->b = calloc(1024, sizeof *a->b); return a; }
static void asc_grow(Asc *a) {
    int nn = a->nb * 4; AItem **nb = calloc(nn, sizeof *nb);
    for (int i = 0; i < a->nb; i++) for (AItem *it = a->b[i], *nx; it; it = nx) { nx = it->next; unsigned k = it->h & (nn - 1); it->next = nb[k]; nb[k] = it; }
    free(a->b); a->b = nb; a->nb = nn;
}
AItem *asc_find(Asc *a, const char *k, int create) {
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
void asc_free(Asc *a) {
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
const char *ai_str(AItem *it) {
    if (it->vk & 2) return it->v;
    char b[24]; rt_ltoa(b, it->iv); it->v = strdup(b); it->vk |= 2; return it->v;
}
void ai_set(AItem *it, const char *val, int append) {
    char *n;
    if (append && it->vk) { const char *o = ai_str(it); size_t a = strlen(o), b = strlen(val); n = malloc(a + b + 1); memcpy(n, o, a); memcpy(n + a, val, b + 1); }
    else n = strdup(val);
    free(it->v); it->v = n; it->vk = 2;
}
void ai_seti(AItem *it, long x) { free(it->v); it->v = NULL; it->iv = x; it->vk = 4; }
int asc_list(const Asc *a, const char ***out, int keys) {   /* borrowed strings in bash order */
    const char **l = malloc((a->n + 1) * sizeof *l); int n = 0;
    for (int i = 0; i < a->nb; i++) for (AItem *it = a->b[i]; it; it = it->next) l[n++] = keys ? it->k : ai_str(it);
    *out = l; return n;
}
Var **vt; int nvt, capvt;
static int *ht; static int htcap;                 /* open-addressing name -> slot (stored +1) */
unsigned cur_gen;                          /* generation of the innermost subshell (0 = none) */

unsigned hash_str(const char *s) { unsigned h = 2166136261u; while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; } return h; }
void ht_rebuild(int ncap) {
    free(ht); ht = calloc(ncap, sizeof(int)); htcap = ncap;
    for (int i = 0; i < nvt; i++) { unsigned h = hash_str(vt[i]->name) & (ncap - 1); while (ht[h]) h = (h + 1) & (ncap - 1); ht[h] = i + 1; }
}
int vt_add(const char *name, int copy) {
    if (nvt == capvt) { capvt = capvt ? capvt * 2 : 256; vt = xrealloc(vt, capvt * sizeof *vt); }
    Var *v = calloc(1, sizeof *v); v->name = copy ? xstrdup(name) : name; vt[nvt] = v;
    if ((nvt + 1) * 2 > htcap) { nvt++; ht_rebuild(htcap ? htcap * 2 : 1024); return nvt - 1; }
    unsigned h = hash_str(name) & (htcap - 1); while (ht[h]) h = (h + 1) & (htcap - 1); ht[h] = nvt + 1;
    return nvt++;
}
int name_id(const char *name, int create) {
    if (htcap) { unsigned h = hash_str(name) & (htcap - 1); while (ht[h]) { Var *v = vt[ht[h] - 1]; if (!strcmp(v->name, name)) return ht[h] - 1; h = (h + 1) & (htcap - 1); } }
    return create ? vt_add(name, 1) : -1;
}

static const char *var_str_noarr(Var *v) {
    if (v->flags & VF_STR) return v->val;
    if (v->flags & VF_INT) return var_int_text(v);
    return NULL;
}
/* undo log: variables modified inside a subshell are restored when it ends */
Undo *ulog; int nulog; static int capulog;
static Var var_snapshot(const Var *o) {
    Var s = *o; s.val = o->val ? xstrdup(o->val) : NULL; s.vcap = 0; s.as = asc_copy(o->as);
    if (o->arr) {
        int c = o->arrn ? o->arrn : 1; s.arrcap = c;
        s.arr = xmalloc(c * sizeof(char *)); s.ai = xmalloc(c * sizeof(long)); s.ak = xmalloc(c);
        for (int i = 0; i < o->arrn; i++) { s.arr[i] = o->arr[i] ? xstrdup(o->arr[i]) : NULL; s.ai[i] = o->ai[i]; s.ak[i] = o->ak[i]; }
    }
    return s;
}
void var_release(Var *v) {
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
const char *el_str(Var *v, long i) {
    unsigned char k = v->ak[i];
    if (!(k & AK_SET)) return NULL;
    if (k & AK_STR) return v->arr[i];
    char b[24]; rt_ltoa(b, v->ai[i]); v->arr[i] = xstrdup(b); v->ak[i] = k | AK_STR; return v->arr[i];
}
static void el_take(Var *v, long i, char *owned) { if (i == 0) v->ver = ++g_ver; free(v->arr[i]); v->arr[i] = owned; v->ak[i] = AK_SET | AK_STR; }
void to_array(Var *v) {   /* scalar -> array with the old value as element 0 */
    const char *old = var_str_noarr(v); char *o = old ? xstrdup(old) : NULL; free(v->val); v->val = NULL;
    v->arr = NULL; v->ai = NULL; v->ak = NULL; v->arrn = v->arrcap = 0;
    v->flags = (v->flags & (VF_EXP | VF_RO | VF_LOCALE)) | VF_ARR;
    if (o) { arr_grow(v, 1); el_take(v, 0, o); }
}
__attribute__((noinline, cold)) void touch_slow(Var *v) {
    if (nulog == capulog) { capulog = capulog ? capulog * 2 : 64; ulog = xrealloc(ulog, capulog * sizeof *ulog); }
    ulog[nulog].v = v; ulog[nulog].snap = var_snapshot(v); nulog++; v->gen = cur_gen;
}
static void var_clear(Var *v) { touch(v); var_release(v); v->flags &= (VF_EXP | VF_RO | VF_LOCALE); v->ival = 0; if (v->flags & VF_LOCALE) bash_rt_locale_changed(); }

void asc_set_v(Var *v, const char *k, const char *val, int append) { touch(v); ai_set(asc_find(v->as, k, 1), val, append); if (!strcmp(k, "0")) v->ver = ++g_ver; }
void var_set_elem(Var *v, long idx, const char *val, int append) {
    if (v->flags & VF_ASC) { char k[24]; rt_ltoa(k, idx); asc_set_v(v, k, val, append); return; }
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
void var_set_elem_int(Var *v, long idx, long x) {
    if (v->flags & VF_ASC) { char k[24], b[24]; rt_ltoa(k, idx); rt_ltoa(b, x); asc_set_v(v, k, b, 0); return; }
    touch(v);
    if (!(v->flags & VF_ARR)) to_array(v);
    if (idx < 0) idx += v->arrn;
    if (idx < 0) { bash_rt_warn("bad array subscript"); return; }
    if (idx >= v->arrn) arr_grow(v, idx + 1);
    free(v->arr[idx]); v->arr[idx] = NULL; v->ai[idx] = x; v->ak[idx] = AK_SET | AK_INT; if (idx == 0) v->ver = ++g_ver;
}
void random_seed(const char *s);
Var *g_random_var, *g_seconds_var; time_t g_sec_base;   /* SECONDS == now - g_sec_base */
void var_set_str(Var *v, const char *val, int append) {
    if (v == g_random_var) { random_seed(val); return; }
    if (v == g_seconds_var) { g_sec_base = time(NULL) - strtol(val, NULL, 10); return; }
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
__attribute__((noinline)) static long geti_slow(Var *v) {
    const char *s = var_str(v);
    if (!s || !*s) return 0;
    char *e; long x = rt_strtol0(s, &e);
    if (*e == 0) { if (!(v->flags & (VF_ARR | VF_ASC))) { touch(v); v->ival = x; v->flags |= VF_INT; } return x; }
    return ar_eval_str(s, 1);
}
long bash_rt_geti(int id) { Var *v = vt[id]; if (v->flags & VF_INT) return v->ival; return geti_slow(v); }
int test_err;
long str_to_long_t(const char *s) {
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
    if (v->flags & (VF_ARR | VF_RO | VF_ASC | VF_LOCALE) || v == g_random_var || v == g_seconds_var) { char b[32]; rt_ltoa(b, x); var_set_str(v, b, 0); return; }
    touch(v);   /* the text buffer stays (see var_int_text) */
    v->ival = x; v->flags = (v->flags & (VF_EXP | VF_RO)) | VF_INT; v->ver = ++g_ver;
}
void bash_rt_seti(int id, long x) {   /* hot: plain integer variable, nothing to undo */
    Var *v = vt[id];
    if (__builtin_expect(!(v->flags & ~(unsigned)(VF_INT | VF_STR | VF_EXP)) && v != g_random_var && v != g_seconds_var && (!cur_gen || v->gen == cur_gen), 1)) {
        v->ival = x; v->flags = (v->flags & VF_EXP) | VF_INT; v->ver = ++g_ver; return; }
    seti_slow(v, x);
}
const char *bash_rt_gets(int id) { return var_str(vt[id]); }
const char *bash_rt_gets_e(int id) {
    const char *s = var_str(vt[id]);
    if (s) return s;
    if (opt_u) { bash_rt_warn("%s: unbound variable", vt[id]->name); bash_rt_exit(1); }
    return "";
}
const char *bash_rt_gets_elem_e(int id, long idx) {   /* ${a[idx]} of an indexed array, borrowed; "" when unset (honors set -u) */
    Var *v = vt[id];
    if ((v->flags & (VF_ARR | VF_ASC)) == VF_ARR && idx >= 0 && idx < v->arrn && (v->ak[idx] & AK_STR)) return v->arr[idx];
    Ref r; ref_from_id(id, 3, idx, &r); const char *s = ref_scalar(&r);
    if (s) return s;
    if (opt_u) { bash_rt_warn("%s[%ld]: unbound variable", v->name, idx); bash_rt_exit(1); }
    return "";
}
void bash_rt_set_id(int id, const char *s, int append) { var_set_str(vt[id], s, append); }
void bash_rt_set_elem_id(int id, long idx, const char *s, int append) { Var *v = vt[id]; if (v->flags & VF_RO) { bash_rt_warn("%s: readonly variable", v->name); return; } var_set_elem(v, idx, s, append); }
__attribute__((noinline)) static long geti_elem_slow(int id, long idx) {
    Var *v = vt[id]; const char *s = NULL;
    if (v->flags & VF_ASC) { char k[24]; rt_ltoa(k, idx); AItem *it = asc_find(v->as, k, 0); s = it ? ai_str(it) : NULL; }
    else if (v->flags & VF_ARR) {
        long i = idx < 0 ? idx + v->arrn : idx;
        if (i < 0 || i >= v->arrn) return 0;
        unsigned char k = v->ak[i];
        if (k & AK_INT) return v->ai[i];
        if (!(k & AK_SET)) return 0;
        s = v->arr[i]; if (!*s) return 0;
        char *e; long x = rt_strtol0(s, &e);
        if (*e == 0) { touch(v); v->ai[i] = x; v->ak[i] = k | AK_INT; return x; }
        return ar_eval_str(s, 1);
    }
    else if (idx == 0) s = var_str(v);
    if (!s || !*s) return 0;
    char *e; long x = rt_strtol0(s, &e); return *e == 0 ? x : ar_eval_str(s, 1);
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
static Tmp *tmps; static int captmp; int ntmp;
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
char **pos; int npos; void *pos_block;
static void *pos_build(int n, char **src, char ***out) {
    if (n <= 0) { *out = NULL; return NULL; }
    size_t sz = n * sizeof(char *); for (int i = 0; i < n; i++) sz += strlen(src[i]) + 1;
    char *blk = xmalloc(sz); char **p = (char **)blk; char *s = blk + n * sizeof(char *);
    for (int i = 0; i < n; i++) { size_t l = strlen(src[i]) + 1; memcpy(s, src[i], l); p[i] = s; s += l; }
    *out = p; return blk;
}
int bash_rt_npos(void) { return npos; }
const char *bash_rt_pos_s(int n) { return n >= 1 && n <= npos ? pos[n - 1] : ""; }
int bash_rt_shift(int n) { if (n < 0 || n > npos) return 1; pos += n; npos -= n; return 0; }
void bash_rt_pos_replace(int argc, char **argv) {
    char **np; void *nb = pos_build(argc, argv, &np);        /* build first: argv may point into the old block */
    pos_release_block(); pos = np; npos = argc > 0 ? argc : 0; pos_block = nb;
}

/* ------------------------------------------- references & specials */
static int is_special_name(const char *n) {
    if (!*n) return 0;
    if (isdigit((unsigned char)n[0])) return 1;
    if (!n[1] && strchr("?#$!-@*0", n[0])) return 1;
    return !strcmp(n, "RANDOM") || !strcmp(n, "PPID") || !strcmp(n, "SECONDS") || !strcmp(n, "EPOCHSECONDS") || !strcmp(n, "EPOCHREALTIME");
}
void ref_from_name(const char *name, Ref *r) {
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
void ref_from_key(int id, const char *key, Ref *r) {   /* compiled h[key]: assoc element, or an indexed one if h is not assoc at run time */
    r->vid = id; r->sname = (char *)vt[id]->name; r->owns = 0; r->idx = 0; r->key = NULL;
    if (vt[id]->flags & VF_ASC) { r->kind = 4; r->key = (char *)key; }
    else { int e; r->kind = 3; r->idx = bash_rt_arith_eval(key, &e); }
}
void ref_from_id(int id, int sub, long idx, Ref *r) { r->vid = id; r->sname = (char *)vt[id]->name; r->kind = sub; r->idx = idx; r->owns = 0; r->key = NULL; }
void ref_free(Ref *r) { if (r->owns) { free(r->sname); free(r->key); } }   /* key is owned exactly when sname is */
int ref_is_pos_list(const Ref *r) { return r->vid == -1; }
int ref_is_list(const Ref *r) { return r->kind == 1 || r->kind == 2 || r->vid == -1; }
int ref_is_star(const Ref *r) { return r->kind == 2 || (r->vid == -1 && r->sname[0] == '*'); }

/* $RANDOM: bash's generator (Park-Miller, folded to 15 bits, never the same value twice in a row), so
 * RANDOM=seed reproduces bash's sequence */
unsigned g_rseed = 1; static int g_rlast = -1;
static int bash_random(void) {
    int rv;
    do {
        unsigned r = g_rseed ? g_rseed : 123459876; int h = (int)(r / 127773), l = (int)(r - 127773u * (unsigned)h);
        int t = 16807 * l - 2836 * h; g_rseed = t < 0 ? (unsigned)t + 0x7fffffffu : (unsigned)t;
        rv = (int)(((g_rseed >> 16) ^ (g_rseed & 65535)) & 32767);
    } while (rv == g_rlast);
    return g_rlast = rv;
}
void random_seed(const char *s) { g_rseed = (unsigned)strtoul(s, NULL, 10); g_rlast = -1; }
const char *bash_rt_special_s(int kind) {
    static char bufs[8][32]; static int bi;
    char *buf = bufs[bi++ & 7];
    switch (kind) {
    case SP_STATUS: rt_ltoa(buf, g_status); return buf;
    case SP_NARGS: rt_ltoa(buf, npos); return buf;
    case SP_PID: rt_ltoa(buf, (int)g_shell_pid); return buf;
    case SP_BANG: if (!g_last_bg) return ""; rt_ltoa(buf, (int)g_last_bg); return buf;
    case SP_DASH: snprintf(buf, 32, "%s%s", opt_e ? "e" : "", opt_u ? "u" : ""); return buf;
    case SP_ZERO: return g_argv0;
    case SP_RANDOM: rt_ltoa(buf, bash_random()); return buf;
    case SP_PPID: rt_ltoa(buf, (int)getppid()); return buf;
    case SP_SECONDS: rt_ltoa(buf, (long)(time(NULL) - g_sec_base)); return buf;
    case SP_EPOCHSEC: rt_ltoa(buf, (long)time(NULL)); return buf;
    case SP_EPOCHREAL: { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); snprintf(buf, 32, "%ld.%06ld", (long)ts.tv_sec, ts.tv_nsec / 1000); return buf; }
    }
    return "";
}
long bash_rt_special_i(int kind) {
    switch (kind) {
    case SP_STATUS: return g_status; case SP_NARGS: return npos; case SP_PID: return g_shell_pid; case SP_PPID: return getppid(); case SP_BANG: return g_last_bg;
    case SP_RANDOM: return bash_random(); case SP_SECONDS: return (long)(time(NULL) - g_sec_base); case SP_EPOCHSEC: return (long)time(NULL);
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
    if (!strcmp(n, "EPOCHSECONDS")) return bash_rt_special_s(SP_EPOCHSEC);
    if (!strcmp(n, "EPOCHREALTIME")) return bash_rt_special_s(SP_EPOCHREAL);
    if (isdigit((unsigned char)n[0])) { char *e; long k = strtol(n, &e, 10); if (*e == 0 && k >= 1 && k <= npos) return pos[k - 1]; if (*e == 0) { *found = 0; return NULL; } }
    *found = 0; return NULL;
}
const char *fnstack_elem(int which, long idx) {   /* FUNCNAME (-3) / BASH_SOURCE (-4) */
    if (idx < 0 || idx > nframes) return NULL;
    if (which == -4) return g_argv0;
    if (nframes == 0) return NULL;
    return idx == nframes ? "main" : frames[nframes - 1 - idx].fname;
}
const char *ref_scalar(const Ref *r) {   /* NULL if unset */
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
int ref_list(const Ref *r, const char ***out) {   /* borrowed strings, malloc'd array */
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
void ref_assign(const Ref *r, const char *s) {
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
    if (!strchr(name, '[') && !is_special_name(name)) { var_set_str(vt[name_id(name, 1)], val, append); return; }   /* plain name */
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
const char *bash_rt_ifs(void) { return ifs_chars(); }
const char *ifs_chars(void) {
    if (id_ifs < 0) id_ifs = name_id("IFS", 1);
    const char *s = var_str(vt[id_ifs]); return s ? s : " \t\n";
}
