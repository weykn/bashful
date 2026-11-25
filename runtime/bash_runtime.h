/* bash_runtime.h - ABI between LLVM IR emitted by bashful and the embedded runtime.
 *
 * Builtins and embedded programs (cat, grep, sort, ...) are ordinary C functions named
 * bash_internal_<builtin> / bash_external_<tool>(argc, argv) that run inside the compiled program's
 * own process.  Programs that are not embedded are run for real (posix_spawn / fork+exec).
 */
#ifndef BASH_RUNTIME_H
#define BASH_RUNTIME_H
#include <stddef.h>

typedef int (*bash_fn_t)(int argc, char **argv);   /* tools and shell functions */
typedef int (*bash_body_t)(void);                  /* compiled blocks (subshell, pipeline stage, $(...)) */
typedef char *(*bash_str_t)(void);                 /* compiled word (param-expansion operand), malloc'd result */

/* word builder modes */
enum { WB_ARGV = 0, WB_STR = 1, WB_PATTERN = 2, WB_REGEX = 3 };   /* PATTERN/REGEX: quoted text is escaped for fnmatch/ERE */
/* literal kinds */
enum { LIT_EXPANDED = 0, LIT_QUOTED = 1, LIT_PLAIN = 2 };

/* redirection ops */
enum { R_IN, R_OUT, R_APPEND, R_DUP_IN, R_DUP_OUT, R_RDWR, R_HEREDOC, R_CLOBBER,
       R_HERESTR, R_OUT_ERR, R_APPEND_ERR };

/* parameter-expansion ops */
enum { P_PLAIN, P_DEFAULT, P_DEFAULT_NC, P_ASSIGN, P_ASSIGN_NC, P_ALT, P_ALT_NC,
       P_ERR, P_ERR_NC, P_LEN, P_TRIM_S, P_TRIM_L, P_TRIM_SE, P_TRIM_LE,
       P_SUBST, P_SUBST_ALL, P_SUBST_PRE, P_SUBST_SUF, P_SUBSTR,
       P_UP1, P_UPALL, P_LOW1, P_LOWALL, P_KEYS, P_INDIRECT, P_AT, P_NAMES, P_NAMES_STAR };

typedef struct WordBuilder WordBuilder;

/* special parameter kinds (compile-time classified) */
enum { SP_STATUS = 1, SP_NARGS, SP_PID, SP_BANG, SP_DASH, SP_ZERO, SP_RANDOM, SP_PPID, SP_SECONDS, SP_AT, SP_STAR, SP_FUNCNAME, SP_SOURCE, SP_EPOCHSEC, SP_EPOCHREAL, SP_LINENO };

/* Defined by the compiled script: 1 = embedded programs (cat, grep, sort, ...) run in-process,
 * 0 = built with --no-embed, every external command runs the program installed on the system. */
extern const int bash_rt_embed;

/* ---- lifecycle.  `names` is the compile-time variable table: variable i has slot i. ---- */
void  bash_rt_init(int argc, char **argv, const char **names, int nnames);
void  bash_rt_exit(int code);
int   bash_rt_line(bash_body_t body);    /* one top-level line of a script */
void  bash_rt_expansion_error(void);     /* fatal expansion error: abandon the line / exit the subshell */

/* ---- variables by compile-time slot (hot path) ---- */
long        bash_rt_geti(int id);                 /* arithmetic value (cached integer) */
long        bash_rt_geti_t(int id);               /* strict integer for [ ] / [[ ]] */
void        bash_rt_seti(int id, long v);
const char *bash_rt_gets(int id);                 /* NULL when unset */
const char *bash_rt_gets_e(int id);
const char *bash_rt_gets_elem_e(int id, long idx);               /* "" when unset (honors set -u) */
void        bash_rt_set_id(int id, const char *s, int append);
void        bash_rt_set_elem_id(int id, long idx, const char *s, int append);
void        bash_rt_set_array_id(int id, int argc, char **argv, int append);
long        bash_rt_geti_elem(int id, long idx);
void        bash_rt_seti_elem(int id, long idx, long v);
void        bash_rt_local_id(int id);
void        bash_rt_export_id(int id);
void        bash_rt_readonly_id(int id);
void        bash_rt_tmp_set_id(int id, const char *s);
int         bash_rt_tmp_mark(void);
void        bash_rt_tmp_restore(int mark);
const char *bash_rt_pos_s(int n);                 /* "" when out of range */
long        bash_rt_pos_i_t(int n);
const char *bash_rt_special_s(int kind);
long        bash_rt_special_i(int kind);
void        bash_rt_unset_id(int id);

/* ---- word builder (expansion with IFS splitting / globbing) ---- */
WordBuilder *bash_rt_wb_new(int mode);
void  bash_rt_wb_lit(WordBuilder *b, const char *s, int kind);
void  bash_rt_wb_const(WordBuilder *b, const char *s);       /* literal + end word, no glob */
void  bash_rt_wb_var_id(WordBuilder *b, int id, int quoted);
void  bash_rt_wb_pos(WordBuilder *b, int n, int quoted);
void  bash_rt_wb_special(WordBuilder *b, int kind, int quoted);
void  bash_rt_wb_int(WordBuilder *b, long v, int quoted);
void  bash_rt_wb_param_id(WordBuilder *b, int id, int sub, long idx, int op, bash_str_t fa, bash_str_t fc,
                          const char *ca, const char *cc, int quoted);
void  bash_rt_wb_param(WordBuilder *b, const char *name, int op, bash_str_t a, bash_str_t c, int quoted);
void  bash_rt_wb_cmdsub(WordBuilder *b, bash_body_t body, int quoted);
void  bash_rt_wb_procsub(WordBuilder *b, bash_body_t body);
void  bash_rt_wb_arith(WordBuilder *b, const char *expr, int quoted);   /* runtime-evaluated expression */
void  bash_rt_wb_end(WordBuilder *b);
int   bash_rt_wb_argc(WordBuilder *b);
char **bash_rt_wb_argv(WordBuilder *b);
char *bash_rt_wb_str(WordBuilder *b);          /* WB_STR / WB_PATTERN result (malloc'd); recycles b */
void  bash_rt_wb_free(WordBuilder *b);
int   bash_rt_cmdsub_status(void);
void  bash_rt_cmdsub_reset(void);

/* ---- status ---- */
void  bash_rt_set_status(int st);
int   bash_rt_get_status(void);
void  bash_rt_errexit_check(void);

/* ---- name based access (builtins, dynamic cases) ---- */
void  bash_rt_set(const char *name, const char *val, int append);
void  bash_rt_set_array(const char *name, int argc, char **argv, int append);
void  bash_rt_tmp_set(const char *name, const char *val);
int   bash_rt_arith(const char *expr);          /* (( )) evaluated at run time: status 0 if non-zero */
long  bash_rt_arith_eval(const char *expr, int *err);      /* syntax error: exit 1 (expansion context) */
long  bash_rt_arith_eval_nf(const char *expr, int *err);   /* syntax error: *err = 1, warning only */
long  bash_rt_arith_str(const char *expr);
long  bash_rt_div(long a, long b);
long  bash_rt_mod(long a, long b);
long  bash_rt_pow(long a, long b);

/* ---- native test helpers ---- */
int   bash_rt_ftest(int op, const char *s);
int   bash_rt_streq(const char *a, const char *b);
int   bash_rt_fnm(const char *pat, const char *s);
int   bash_rt_regex(const char *s, const char *pat);
void  bash_rt_test_err_clear(void);
int   bash_rt_test_err(void);
int   bash_rt_test_status(int ok);

/* ---- functions & dispatch ---- */
void  bash_rt_defun(const char *name, bash_fn_t fn);
int   bash_rt_run(int argc, char **argv);
int   bash_rt_run_nofunc(int argc, char **argv);
int   bash_rt_kind(const char *name);   /* 1 function, 2 builtin, 3 embedded program, 0 unknown */
void  bash_rt_fn_enter(int argc, char **argv);
void  bash_rt_fn_leave(void);
int   bash_rt_return_status(int argc, char **argv);
int   bash_rt_shift(int n);

/* ---- redirections ---- */
void  bash_rt_redir_push(void);
void  bash_rt_redir(int fd, int op, const char *target);
int   bash_rt_redir_ok(void);
void  bash_rt_redir_pop(void);
void  bash_rt_redir_commit(void);   /* keep redirections (exec >file) */

/* ---- blocks ---- */
int   bash_rt_subshell(bash_body_t body);
void  bash_rt_lineno(int line);
char *bash_rt_printf_q(const char *s);
void  bash_rt_umask_changing(void);
long  bash_rt_start_time(void);
int   bash_rt_time_start(void);
void  bash_rt_time_end(int slot, int posix);
int   bash_rt_pipeline(int n, bash_body_t *stages);
int   bash_rt_case_match(const char *word, const char *pattern);

/* ---- buffered stdout (tools write through this) ---- */
void  bash_rt_out(const char *s, size_t n);
void  bash_rt_flush(void);
void  bash_rt_cwd_changing(void);

/* ---- helpers for builtins ---- */
const char *bash_rt_ifs(void);                     /* current IFS (default when unset) */
const char *bash_rt_getvar(const char *name);        /* NULL when unset */
void  bash_rt_setvar(const char *name, const char *val);
void  bash_rt_export(const char *name);
void  bash_rt_mark_readonly(const char *name);
int   bash_rt_is_known(const char *name);
void  bash_rt_pos_replace(int argc, char **argv);   /* set -- args */
void  bash_rt_unsetvar(const char *name);
void  bash_rt_declare_local(const char *name);
char **bash_rt_exported_env(int *n);
int   bash_rt_npos(void);
int   bash_rt_opt_get(char o);
void  bash_rt_opt_set(char o, int v);
int   bash_rt_pipefail_get(void);
void  bash_rt_pipefail_set(int v);
void  bash_rt_warn(const char *fmt, ...);
int   bash_rt_test(int n, char **a, int dbl);
char *bash_rt_pat_quote(char *s);   /* operands only, no name / closing bracket */

/* ---- processes (real programs, forked pipelines, jobs, traps) ---- */
#define BASH_TOOL_FALLBACK (-1000)                  /* embedded tool: "run the real program instead" */
char *bash_rt_path_search(const char *name);
int   bash_rt_exec_external(int argc, char **argv);
int   bash_rt_exec_builtin(int argc, char **argv);
int   bash_rt_tool_gate(int idx, int argc, char **argv);   /* 1 = the embedded tool implements these args */
int   bash_rt_tool_call(int idx, bash_fn_t fn, int argc, char **argv);
int   bash_rt_tool_ret(int r, int argc, char **argv);
int   bash_rt_pipeline_fork(int n, bash_body_t *stages);
void  bash_rt_background(bash_body_t body);
int   bash_rt_wait(int argc, char **argv);
void  bash_rt_trap(const char *sig, int mode, bash_body_t fn);
int   bash_rt_fallback(int argc, char **argv, const char *script);
void  bash_rt_locale_changed(void);
void  bash_rt_assoc_id(int id);                    /* declare -A */
void  bash_rt_make_assoc(const char *name);
void  bash_rt_aset(int id, const char *key, const char *val, int append);
long  bash_rt_ageti(int id, const char *key);
void  bash_rt_aseti(int id, const char *key, long v);
void  bash_rt_wb_aparam(WordBuilder *b, int id, const char *key, int op, bash_str_t fa, bash_str_t fc, const char *ca, const char *cc, int quoted);
int   bash_rt_aisset(int id, const char *key);
void  bash_rt_arr_begin(int id, int append);
void  bash_rt_arr_kv(int id, const char *key, const char *val, int append);
void  bash_rt_arr_push(int id, int argc, char **argv);
void  bash_rt_arr_end(int id);
char *bash_rt_keytmp(char *k);
const char *bash_rt_pos_u(int n);
void  bash_rt_copy_id(int dst, int src);
void  bash_rt_read_src(char *s);                  /* next `read` on fd 0 takes this (malloc'd) here-string; NULL drops it */
int   bash_rt_utf8(void);
int   bash_rt_child_c_locale(void);
long  bash_rt_arr_len(int id);
long  bash_rt_strlen_id(int id);
long  bash_rt_pos_i(int n);
void  bash_rt_wb_substr_id(WordBuilder *b, int id, long off, long len, int haslen, int quoted);

/* commands: one prototype per entry of tools.def */
#define INTERNAL(id, name) int bash_internal_##id(int argc, char **argv);
#define EXTERNAL(id, name) int bash_external_##id(int argc, char **argv);
#include "tools.def"
#undef INTERNAL
#undef EXTERNAL

#endif
