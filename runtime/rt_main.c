/* rt_main.c - exit status and shell options, start-up, locale */
#include "rt.h"

/* --------------------------------------------------------------- status */
int g_status = 0, g_cs_status = 0;
int opt_e = 0, opt_u = 0, opt_pipefail = 0, opt_x = 0;
volatile sig_atomic_t g_trap_pending;
void bash_rt_set_status(int st) { g_status = st; if (__builtin_expect(g_trap_pending, 0)) run_pending_traps(); }
int  bash_rt_get_status(void) { return g_status; }
int  bash_rt_cmdsub_status(void) { return g_cs_status; }
void bash_rt_cmdsub_reset(void) { g_cs_status = 0; }
void bash_rt_errexit_check(void) { if (opt_e && g_status != 0) bash_rt_exit(g_status); }
int  bash_rt_opt_get(char o) { return o == 'e' ? opt_e : o == 'u' ? opt_u : o == 'x' ? opt_x : 0; }
void bash_rt_opt_set(char o, int v) { if (o == 'e') opt_e = v; else if (o == 'u') opt_u = v; else if (o == 'x') opt_x = v; }
int  bash_rt_pipefail_get(void) { return opt_pipefail; }
void bash_rt_pipefail_set(int v) { opt_pipefail = v; }

/* -------------------------------------------------------------- init */
/* interactive mode (bashful -i): BASHFUL_REPL names the file the shell state is written to on exit (see
 * repl_dump); with BASHFUL_REPL_NOENV the environment is not imported, the state prelude recreates it */
const char *g_repl_state;
int g_interactive;   /* BASHFUL_REPL_I: an interactive session, $- has i */
static time_t g_start_time;
long bash_rt_start_time(void) { return (long)g_start_time; }   /* printf %(...)T -2 */
void bash_rt_init(int argc, char **argv, const char **names, int nnames) {
    g_repl_state = getenv("BASHFUL_REPL");
    int noenv = g_repl_state && getenv("BASHFUL_REPL_NOENV");
    g_interactive = g_repl_state && getenv("BASHFUL_REPL_I");
    g_argv0 = argc > 0 ? argv[0] : "bash";
    o_line = isatty(1);
    ht_rebuild(1024);
    for (int i = 0; i < nnames; i++) vt_add(names[i], 0);        /* slot i == compile-time id i */
    for (char **e = environ; e && *e && !noenv; e++) {
        if (!strncmp(*e, "BASHFUL_REPL", 12)) continue;
        char *eq = strchr(*e, '='); if (!eq) continue;
        char *n = xstrndup(*e, eq - *e); int id = name_id(n, 1); free(n);
        Var *v = vt[id]; v->val = xstrdup(eq + 1); v->flags = VF_STR | VF_EXP;
    }
    char *cwd = getcwd(NULL, 0);
    if (cwd) { Var *v = vt[name_id("PWD", 1)]; free(v->val); v->val = cwd; v->flags = VF_STR | VF_EXP; }
    if (!var_isset(vt[name_id("HOME", 1)])) { Var *v = vt[name_id("HOME", 1)]; v->val = xstrdup("/"); v->flags = VF_STR; }
    static const char *one[] = { "OPTIND", "OPTERR", NULL };   /* bash sets these whatever the environment says */
    for (int i = 0; one[i]; i++) { Var *v = vt[name_id(one[i], 1)]; free(v->val); v->val = xstrdup("1"); v->vlen = 1; v->flags = VF_STR | (v->flags & VF_EXP); }
    pos = argv + 1; npos = argc > 1 ? argc - 1 : 0; pos_block = NULL;
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    g_rseed = (unsigned)time(NULL) ^ ((unsigned)getpid() << 8); g_random_var = vt[name_id("RANDOM", 1)];
    g_seconds_var = vt[name_id("SECONDS", 1)]; g_sec_base = time(NULL);
    g_shell_pid = getpid(); g_start_time = time(NULL);
    static const char *locvars[] = { "LC_ALL", "LC_CTYPE", "LC_COLLATE", "LC_TIME", "LANG", NULL };
    for (int i = 0; locvars[i]; i++) vt[name_id(locvars[i], 1)]->flags |= VF_LOCALE;
    bash_rt_locale_changed();
}


/* ===================================================================== locale
 * The shell's own locale follows LC_ALL / LC_CTYPE / LC_COLLATE / LANG like bash (even when not exported). */
int g_utf8;
static const char *locale_for(const char *cat) {
    const char *v = bash_rt_getvar("LC_ALL"); if (v && *v) return v;
    v = bash_rt_getvar(cat); if (v && *v) return v;
    v = bash_rt_getvar("LANG"); if (v && *v) return v;
    return "C";
}
void bash_rt_locale_changed(void) {
    if (!setlocale(LC_CTYPE, locale_for("LC_CTYPE"))) setlocale(LC_CTYPE, "C");
    if (!setlocale(LC_COLLATE, locale_for("LC_COLLATE"))) setlocale(LC_COLLATE, "C");
    if (!setlocale(LC_TIME, locale_for("LC_TIME"))) setlocale(LC_TIME, "C");   /* printf %(...)T */
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
size_t u8_len(const char *s) {
    if (!g_utf8) return strlen(s);
    size_t n = 0; mbstate_t st; memset(&st, 0, sizeof st); size_t L = strlen(s);
    while (L) { size_t k = mbrlen(s, L, &st); if (k == (size_t)-1 || k == (size_t)-2) { k = 1; memset(&st, 0, sizeof st); } else if (k == 0) k = 1; s += k; L -= k; n++; }
    return n;
}
size_t u8_off(const char *s, long chars) {   /* byte offset of the chars-th character */
    if (!g_utf8) { size_t L = strlen(s); return (size_t)chars < L ? (size_t)chars : L; }
    const char *p = s; mbstate_t st; memset(&st, 0, sizeof st);
    while (chars-- > 0 && *p) {
        if ((unsigned char)*p < 0x80) { p++; continue; }
        size_t k = mbrlen(p, strnlen(p, MB_LEN_MAX), &st); if (k == (size_t)-1 || k == (size_t)-2 || k == 0) { k = 1; memset(&st, 0, sizeof st); } p += k; }
    return p - s;
}
