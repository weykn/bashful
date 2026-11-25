/* rt.h - internal declarations shared by the runtime's source files (not part of the compiler ABI).
 *
 * Design: everything the compiler can decide is decided at compile time.  Variables the script names are
 * compiled to numbered slots (no lookup by name), arithmetic and tests are compiled to native code, and
 * only genuinely dynamic behaviour (IFS splitting, globbing, dynamic scoping, by-name access from builtins,
 * subshell state) lives here.
 *
 *   rt_main.c    status, options, init, locale        rt_vars.c    variables, arrays, specials
 *   rt_io.c      buffered stdout, redirections        rt_frames.c  locals, subshells, $(...), in-process pipes
 *   rt_arith.c   run-time arithmetic                  rt_expand.c  word expansion, parameter operators
 *   rt_test.c    test / [ / [[                         rt_exec.c    dispatch, processes, jobs, traps
 *   tool_*.c     embedded builtins and programs (see tools.def)
 */
#ifndef RT_H
#define RT_H
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
static inline void *xmalloc(size_t n) { void *p = malloc(n ? n : 1); if (!p) { write(2, "oom\n", 4); _exit(255); } return p; }
static inline void *xrealloc(void *p, size_t n) { p = realloc(p, n ? n : 1); if (!p) { write(2, "oom\n", 4); _exit(255); } return p; }
static inline char *xstrdup(const char *s) { size_t n = strlen(s) + 1; return memcpy(xmalloc(n), s, n); }
static inline char *xstrndup(const char *s, size_t n) { char *p = xmalloc(n + 1); memcpy(p, s, n); p[n] = 0; return p; }

typedef struct { char *s; size_t n, cap; } SB;
static inline void sb_putn(SB *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->s = xrealloc(b->s, b->cap); }
    memcpy(b->s + b->n, s, n); b->n += n; b->s[b->n] = 0;
}
static inline void sb_puts(SB *b, const char *s) { sb_putn(b, s, strlen(s)); }
static inline void sb_putc(SB *b, char c) { sb_putn(b, &c, 1); }
static inline char *sb_take(SB *b) { if (!b->s) return xstrdup(""); char *s = b->s; b->s = NULL; b->n = b->cap = 0; return s; }


/* decimal text of v into b (>= 21 bytes); returns the length -- snprintf("%ld") is a hot spot otherwise */
static inline int rt_ltoa(char *b, long v) {
    char t[24]; int n = 0; unsigned long u = v < 0 ? 0UL - (unsigned long)v : (unsigned long)v;
    do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    int k = 0; if (v < 0) b[k++] = '-';
    while (n) b[k++] = t[--n];
    b[k] = 0; return k;
}
/* strtol(s, e, 0) with a fast path for plain decimal text ("0", "123", "-45"): leading zeros (octal),
 * 0x, blanks, signs and long digit strings take the general path */
static inline long rt_strtol0(const char *s, char **e) {
    const char *p = s; int neg = *p == '-'; if (neg) p++;
    if (*p >= '1' && *p <= '9') {
        unsigned long v = 0; int n = 0;
        while (*p >= '0' && *p <= '9' && n < 18) { v = v * 10 + (unsigned long)(*p++ - '0'); n++; }
        if (!(*p >= '0' && *p <= '9')) { *e = (char *)p; return neg ? -(long)v : (long)v; }
    } else if (*p == '0' && !(p[1] >= '0' && p[1] <= '9') && p[1] != 'x' && p[1] != 'X') { *e = (char *)p + 1; return 0; }
    return strtol(s, e, 0);
}
typedef struct Cap { char *s; size_t n, cap; int memfd, saved, sinking; struct Cap *prev; } Cap;   /* $(...) in progress */
extern char obuf[1 << 16]; extern size_t on; extern int o_line;
extern int opt_e, opt_u, opt_pipefail, opt_x;

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
typedef struct { Var *v; Var snap; } Undo;          /* subshell undo log entry */
typedef struct { int id; Var snap; } Tmp;           /* VAR=x cmd prefix assignment */
extern Undo *ulog; extern int nulog, ntmp, nrs, redir_err; extern unsigned g_rseed;
extern Var **vt; extern int nvt, capvt;

typedef struct { char **pos; int npos; void *block; int loc_base; int redir_depth; const char *fname; } Frame;
extern Frame *frames; extern int nframes, capframes;
typedef struct Sub {
    jmp_buf jb; struct Sub *prev; int exit_code;
    int frame_depth, redir_depth, tmp_depth, log_base; unsigned saved_gen;
    char **spos; int snpos; void *sblock; char *cwd; int e, u, pf; int umask;   /* umask: -1 untouched */
    void *trapsave;
} Sub;
extern Sub *cur_sub; extern unsigned gen_counter;

typedef struct { int vid; char *sname; int kind; long idx; int owns; char *key; } Ref;   /* vid: >=0 slot, -1 positional list, -2 special; kind 4: assoc element `key` */
typedef struct { size_t end; unsigned char kind; } Seg;
struct WordBuilder {
    int mode; char *buf; size_t n, cap; Seg *seg; int nseg, capseg; int have, glob;
    char **argv; int argc, capargv;
    char *ab; size_t an, acap; int fixed;   /* argument strings live in one arena; argv holds offsets until wb_argv */
};

extern Cap *g_cap;
extern int bash_rt_sinking;
extern const char *g_argv0;
extern int g_lineno;
extern int g_status, g_cs_status;
extern volatile sig_atomic_t g_trap_pending;
extern int g_utf8;
extern unsigned long g_ver;
extern unsigned cur_gen;
extern Var *g_random_var, *g_seconds_var; extern time_t g_sec_base;
extern int test_err;
extern char **pos;
extern int npos;
extern void *pos_block;
extern pid_t g_last_bg, g_shell_pid;
extern int g_is_child;
extern const char *g_repl_state;
extern int g_interactive;
void rt_quote_q(SB *o, const char *s);
void rt_quote_dq(SB *o, const char *s);
void rt_quote_key(SB *o, const char *s);
char *rt_ansic_decode(const char *s);
int rt_var_is_local(int id);
extern bash_body_t trap_fn[65];
extern int trap_mode[65];

void raw_write(int fd, const char *s, size_t n);
int fd_dup_high(int fd);
int mkmem(void);
void write_all(int fd, const char *s, size_t n);
void raw_lseek_set(int fd);
void redir_save(int fd);
char *slurp_fd(int fd);
size_t u8_len(const char *s);
size_t u8_off(const char *s, long chars);
Asc *asc_new(void);
AItem *asc_find(Asc *a, const char *k, int create);
void asc_free(Asc *a);
void ai_set(AItem *it, const char *val, int append);
void ai_seti(AItem *it, long x);
int asc_list(const Asc *a, const char ***out, int keys);
unsigned hash_str(const char *s);
void ht_rebuild(int ncap);
int vt_add(const char *name, int copy);
int name_id(const char *name, int create);
void var_release(Var *v);
void to_array(Var *v);
void asc_set_v(Var *v, const char *k, const char *val, int append);
void var_set_elem(Var *v, long idx, const char *val, int append);
void var_set_elem_int(Var *v, long idx, long x);
void var_set_str(Var *v, const char *val, int append);
void ref_from_name(const char *name, Ref *r);
void ref_from_key(int id, const char *key, Ref *r);
void ref_from_id(int id, int sub, long idx, Ref *r);
void ref_free(Ref *r);
int ref_is_pos_list(const Ref *r);
int ref_is_list(const Ref *r);
int ref_is_star(const Ref *r);
const char *fnstack_elem(int which, long idx);
const char *ref_scalar(const Ref *r);
int ref_list(const Ref *r, const char ***out);
void ref_assign(const Ref *r, const char *s);
const char *ifs_chars(void);
void pos_release_block(void);
char *capture(bash_body_t body, int *status);
long ar_eval_str(const char *s, int depth);
void subshell_traps_end(Sub *s);
__attribute__((noinline, cold)) void run_pending_traps(void);

const char *el_str(Var *v, long i);
int rt_fnm(const char *pat, const char *s, size_t n);   /* bash pattern match of s[0..n) */
const char *ai_str(AItem *it);
__attribute__((noinline, cold)) void touch_slow(Var *v);
long str_to_long_t(const char *s);
void random_seed(const char *s);

static inline void touch(Var *v) { if (cur_gen && v->gen != cur_gen) touch_slow(v); }

/* text of an integer variable.  Without VF_STR, val may still hold an old buffer (capacity vcap, always
 * NUL-terminated): integer updates keep it so that `$i` after `((i++))` needs no malloc */
static inline const char *var_int_text(Var *v) {
    if (!v->val || v->vcap < 24) { free(v->val); v->val = xmalloc(24); v->vcap = 24; }
    v->vlen = (size_t)rt_ltoa(v->val, v->ival); v->flags |= VF_STR; return v->val;
}
static inline const char *var_str(Var *v) {     /* scalar view; NULL if unset */
    unsigned f = v->flags;
    if (f & VF_ARR) return v->arrn > 0 ? el_str(v, 0) : NULL;
    if (f & VF_STR) return v->val;
    if (f & VF_ASC) { AItem *it = asc_find(v->as, "0", 0); return it ? ai_str(it) : NULL; }
    if (f & VF_INT) return var_int_text(v);
    return NULL;
}
static inline int var_isset(const Var *v) { return v->flags & (VF_STR | VF_INT | VF_ARR | VF_ASC); }
#endif
