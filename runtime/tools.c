/* tools.c - embedded commands.  bash_internal_* are bash builtins; bash_external_* behave like external
 * programs but run inside the host process:
 * bash_external_cat() reads and writes fds directly, bash_internal_kill() calls kill(2), etc. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <ctype.h>
#include <time.h>
#include <pwd.h>
#include <grp.h>
#include <ftw.h>
#include <regex.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <wchar.h>
#include <stdint.h>
#include <poll.h>
#include "bash_runtime.h"
#include "rt_fd.h"

/* ---------------------------------------------------------------- helpers */
typedef struct { char *s; size_t n, cap; } Buf;
static void bput(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->s = realloc(b->s, b->cap); }
    memcpy(b->s + b->n, s, n); b->n += n; b->s[b->n] = 0;
}
static void bputs(Buf *b, const char *s) { bput(b, s, strlen(s)); }
static void bputc(Buf *b, char c) { bput(b, &c, 1); }
static void wr(int fd, const char *s, size_t n) { if (fd == 1) { bash_rt_out(s, n); return; } if (fd == 2) bash_rt_flush(); while (n) { ssize_t w = write(fd, s, n); if (w < 0) { if (errno == EINTR) continue; return; } s += w; n -= w; } }
static void out(const char *s, size_t n) { wr(1, s, n); }
static void outs(const char *s) { out(s, strlen(s)); }
static void errs(const char *name, const char *what) { bash_rt_warn("%s: %s: %s", name, what, strerror(errno)); }
static int slurp(const char *path, Buf *b) {     /* "-" or NULL = stdin */
    int fd = (!path || !strcmp(path, "-")) ? 0 : open(path, O_RDONLY);
    if (fd < 0) return -1;
    char buf[65536]; ssize_t n;
    for (;;) { n = read(fd, buf, sizeof buf); if (n < 0 && errno == EINTR) continue; if (n <= 0) break; bput(b, buf, n); }
    int e = n < 0 ? errno : 0; if (fd) close(fd); if (e) { errno = e; return -1; }
    if (!b->s) b->s = calloc(1, 1);
    return 0;
}
static int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static int str_to_int(const char *s, long *v) { char *e; errno = 0; *v = strtol(s, &e, 10); return *s && !*e && !errno; }

/* ---------------------------------------------------------------- trivial */
int bash_internal_true(int argc, char **argv) { return 0; }
int bash_internal_false(int argc, char **argv) { return 1; }
int bash_internal_colon(int argc, char **argv) { return 0; }
int bash_internal_wait(int argc, char **argv) { return bash_rt_wait(argc, argv); }
/* reached only through a run-time command name (e.g. "$cmd"); literal uses are compiled or trigger the bash fallback */
int bash_internal_trap(int argc, char **argv) { bash_rt_warn("trap: cannot be invoked through a dynamic command name in a compiled script"); return 1; }
int bash_internal_eval(int argc, char **argv) { bash_rt_warn("eval: cannot be invoked through a dynamic command name in a compiled script"); return 1; }
int bash_internal_source(int argc, char **argv) { bash_rt_warn("%s: cannot be invoked through a dynamic command name in a compiled script", argv[0]); return 1; }
int bash_internal_dot(int argc, char **argv) { return bash_internal_source(argc, argv); }
int bash_internal_break(int argc, char **argv) { bash_rt_warn("break: only meaningful in a loop"); return 0; }
int bash_internal_continue(int argc, char **argv) { bash_rt_warn("continue: only meaningful in a loop"); return 0; }
int bash_internal_return(int argc, char **argv) { return bash_rt_return_status(argc, argv); }
int bash_internal_exit(int argc, char **argv) { bash_rt_exit(argc > 1 ? atoi(argv[1]) : bash_rt_get_status()); return 0; }
int bash_internal_exec(int argc, char **argv) { return bash_rt_exec_builtin(argc, argv); }
int bash_internal_builtin(int argc, char **argv) { return argc > 1 ? bash_rt_run(argc - 1, argv + 1) : 0; }
int bash_external_whoami(int argc, char **argv) { struct passwd *p = getpwuid(geteuid()); outs(p ? p->pw_name : "?"); outs("\n"); return 0; }
int bash_internal_command(int argc, char **argv) {
    int i = 1, v = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) { if (!strcmp(argv[i], "--")) { i++; break; } if (strchr(argv[i], 'v') || strchr(argv[i], 'V')) v = 1; }
    if (i >= argc) return 0;
    if (v) { int rc = 0; for (; i < argc; i++) {
        int k = bash_rt_kind(argv[i]);
        if (k == 1 || k == 2) { outs(argv[i]); outs("\n"); continue; }
        char *p = bash_rt_path_search(argv[i]);
        if (p) { outs(p); outs("\n"); free(p); } else rc = 1;
      } return rc; }
    return bash_rt_run_nofunc(argc - i, argv + i);
}

/* ------------------------------------------------------------------- echo */
static int octal_val(const char *s, int maxd, int *used) { int v = 0, k = 0; while (k < maxd && s[k] >= '0' && s[k] <= '7') v = v * 8 + (s[k++] - '0'); *used = k; return v; }
static int hexv(int c) { return isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10); }
/* interpret backslash escapes; returns 1 if \c seen (stop output) */
static int unescape_into(Buf *b, const char *s, int octal_needs_zero) {
    for (; *s; s++) {
        if (*s != '\\' || !s[1]) { bputc(b, *s); continue; }
        s++;
        switch (*s) {
        case 'a': bputc(b, 7); break; case 'b': bputc(b, 8); break; case 'e': case 'E': bputc(b, 27); break;
        case 'f': bputc(b, 12); break; case 'n': bputc(b, 10); break; case 'r': bputc(b, 13); break;
        case 't': bputc(b, 9); break; case 'v': bputc(b, 11); break; case '\\': bputc(b, '\\'); break;
        case 'c': return 1;
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
            int u; const char *p = s; if (octal_needs_zero) { if (*p == '0') p++; else { bputc(b, '\\'); bputc(b, *s); break; } }
            int v = octal_val(p, octal_needs_zero ? 3 : 3, &u); bputc(b, (char)v); s = p + u - 1; break; }
        case 'x': if (isxdigit((unsigned char)s[1])) { int v = hexv(s[1]); s++; if (isxdigit((unsigned char)s[1])) { v = v * 16 + hexv(s[1]); s++; } bputc(b, (char)v); } else { bputc(b, '\\'); bputc(b, 'x'); } break;
        default: bputc(b, '\\'); bputc(b, *s);
        }
    }
    return 0;
}
int bash_internal_echo(int argc, char **argv) {
    int nl = 1, esc = 0, i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i]; if (a[0] != '-' || !a[1]) break;
        int ok = 1; for (const char *p = a + 1; *p; p++) if (!strchr("neE", *p)) ok = 0;
        if (!ok) break;
        for (const char *p = a + 1; *p; p++) { if (*p == 'n') nl = 0; else if (*p == 'e') esc = 1; else esc = 0; }
    }
    Buf b = { 0 };
    for (; i < argc; i++) {
        if (esc) { if (unescape_into(&b, argv[i], 1)) { nl = 0; break; } } else bputs(&b, argv[i]);
        if (i + 1 < argc) bputc(&b, ' ');
    }
    if (nl) bputc(&b, '\n');
    if (b.s) out(b.s, b.n); free(b.s); return 0;
}

/* ----------------------------------------------------------------- printf */
static long long arg_int(const char *s) {
    if (!s || !*s) return 0;
    if (s[0] == '\'' || s[0] == '"') { if (!s[1]) return 0; if (bash_rt_utf8() && (unsigned char)s[1] >= 0x80) { wchar_t wc; mbstate_t st; memset(&st, 0, sizeof st); size_t k = mbrtowc(&wc, s + 1, strlen(s + 1), &st); if (k != (size_t)-1 && k != (size_t)-2 && k) return wc; } return (unsigned char)s[1]; }
    char *e; long long v = strtoll(s, &e, 0); if (*e) bash_rt_warn("printf: %s: invalid number", s); return v;
}
/* formatted conversion of any width (no fixed buffer: printf '%0200000d' works) */
#define PFMT(...) do { n = snprintf(tmp, sizeof tmp, spec, __VA_ARGS__); if (n < (int)sizeof tmp) bput(o, tmp, n); \
    else { char *big = malloc(n + 1); snprintf(big, n + 1, spec, __VA_ARGS__); bput(o, big, n); free(big); } } while (0)
static int printf_once(Buf *o, const char *fmt, int argc, char **argv, int *ai) {
    int used = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p == '\\') {
            char tmp[8]; int k = 0; tmp[k++] = *p++; if (!*p) { bputc(o, '\\'); break; }
            tmp[k++] = *p;
            if (*p >= '0' && *p <= '7') { int u; int v = octal_val(p, 3, &u); bputc(o, (char)v); p += u - 1; continue; }
            if (*p == 'x') { Buf t = { 0 }; char two[4] = { '\\', 'x', p[1], 0 }; if (isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) { two[3] = 0; char e3[6] = { '\\', 'x', p[1], p[2], 0 }; unescape_into(&t, e3, 0); p += 2; } else unescape_into(&t, two, 0); if (t.s) bput(o, t.s, t.n); free(t.s); continue; }
            tmp[k] = 0; Buf t = { 0 }; unescape_into(&t, tmp, 0); if (t.s) bput(o, t.s, t.n); free(t.s); continue;
        }
        if (*p != '%') { bputc(o, *p); continue; }
        p++; if (*p == '%') { bputc(o, '%'); continue; }
        char spec[64]; int k = 0; spec[k++] = '%';
        while (*p && strchr("-+ #0", *p) && k < 20) spec[k++] = *p++;
        if (*p == '*') { char *a = *ai < argc ? argv[(*ai)++] : "0"; k += snprintf(spec + k, 20, "%lld", arg_int(a)); p++; used = 1; }
        else while (isdigit((unsigned char)*p) && k < 30) spec[k++] = *p++;
        if (*p == '.') { spec[k++] = *p++; if (*p == '*') { char *a = *ai < argc ? argv[(*ai)++] : "0"; k += snprintf(spec + k, 20, "%lld", arg_int(a)); p++; } else while (isdigit((unsigned char)*p) && k < 50) spec[k++] = *p++; }
        char conv = *p; if (!conv) break;
        const char *arg = *ai < argc ? argv[(*ai)++] : NULL; if (arg) used = 1;
        char tmp[512]; int n = 0;
        switch (conv) {
        case 'd': case 'i': spec[k++] = 'l'; spec[k++] = 'l'; spec[k++] = 'd'; spec[k] = 0; PFMT(arg_int(arg)); break;
        case 'u': case 'o': case 'x': case 'X': spec[k++] = 'l'; spec[k++] = 'l'; spec[k++] = conv; spec[k] = 0; PFMT((unsigned long long)arg_int(arg)); break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': spec[k++] = conv; spec[k] = 0; PFMT(arg ? strtod(arg, NULL) : 0.0); break;
        case 'c': spec[k++] = 'c'; spec[k] = 0; PFMT(arg && *arg ? *arg : 0); break;
        case 's': case 'b': case 'q': {
            Buf t = { 0 }; const char *s = arg ? arg : "";
            if (conv == 'b') { unescape_into(&t, s, 1); s = t.s ? t.s : ""; }
            if (conv == 'q') { Buf q = { 0 }; for (const char *c = s; *c; c++) { if (!isalnum((unsigned char)*c) && !strchr("_-./,:@%+=", *c)) bputc(&q, '\\'); bputc(&q, *c); } t = q; s = t.s ? t.s : "''"; }
            spec[k++] = 's'; spec[k] = 0;
            int need = snprintf(NULL, 0, spec, s); char *buf = malloc(need + 1); snprintf(buf, need + 1, spec, s); bput(o, buf, need); free(buf); free(t.s); break; }
        default: bputs(o, spec); bputc(o, conv);
        }
    }
    return used;
}
int bash_internal_printf(int argc, char **argv) {
    int i = 1; const char *var = NULL;
    if (i + 1 < argc && !strcmp(argv[i], "-v")) { var = argv[i + 1]; i += 2; }
    if (i < argc && !strcmp(argv[i], "--")) i++;
    if (i >= argc) { bash_rt_warn("printf: usage: printf [-v var] format [arguments]"); return 2; }
    const char *fmt = argv[i++]; int ai = i; Buf o = { 0 };
    if (var && i == argc - 1 && fmt[0] == '%' && fmt[1] == 'd' && !fmt[2]) {   /* printf -v c %d "'$ch": the ord() idiom */
        char nb[32]; snprintf(nb, sizeof nb, "%lld", arg_int(argv[i])); bash_rt_setvar(var, nb); return 0;
    }
    do { if (!printf_once(&o, fmt, argc, argv, &ai) && ai >= argc) break; } while (ai < argc);
    if (var) { bash_rt_setvar(var, o.s ? o.s : ""); } else if (o.s) out(o.s, o.n);
    free(o.s); return 0;
}

/* -------------------------------------------------------------------- cat */
int bash_external_cat(int argc, char **argv) {
    int rc = 0, number = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) { if (!strcmp(argv[i], "--")) { i++; break; } if (!strcmp(argv[i], "-n")) number = 1; else if (!strcmp(argv[i], "-u")) {} else { bash_rt_warn("cat: invalid option -- '%s'", argv[i] + 1); return 1; } }
    static char buf[65536]; long lineno = 0; int atbol = 1;
    int nfiles = argc - i;
    for (int f = 0; f < (nfiles ? nfiles : 1); f++) {
        const char *path = nfiles ? argv[i + f] : "-";
        int fd = !strcmp(path, "-") ? 0 : open(path, O_RDONLY);
        if (fd < 0) { errs("cat", path); rc = 1; continue; }
        if (!number) {
            ssize_t n; while ((n = read(fd, buf, sizeof buf)) != 0) { if (n < 0) { if (errno == EINTR) continue; errs("cat", path); rc = 1; break; } wr(1, buf, n); }
        } else {
            Buf o = { 0 }; ssize_t n;
            while ((n = read(fd, buf, sizeof buf)) > 0) for (ssize_t k = 0; k < n; k++) {
                if (atbol) { char t[32]; int l = snprintf(t, sizeof t, "%6ld\t", ++lineno); bput(&o, t, l); atbol = 0; }
                bputc(&o, buf[k]); if (buf[k] == '\n') atbol = 1;
            }
            if (o.s) out(o.s, o.n); free(o.s);
        }
        if (fd) close(fd);
    }
    return rc;
}

/* ------------------------------------------------------------------- kill */
static const struct { const char *n; int s; } sigtab[] = {
    {"HUP",SIGHUP},{"INT",SIGINT},{"QUIT",SIGQUIT},{"ILL",SIGILL},{"TRAP",SIGTRAP},{"ABRT",SIGABRT},{"BUS",SIGBUS},{"FPE",SIGFPE},
    {"KILL",SIGKILL},{"USR1",SIGUSR1},{"SEGV",SIGSEGV},{"USR2",SIGUSR2},{"PIPE",SIGPIPE},{"ALRM",SIGALRM},{"TERM",SIGTERM},
    {"CHLD",SIGCHLD},{"CONT",SIGCONT},{"STOP",SIGSTOP},{"TSTP",SIGTSTP},{"TTIN",SIGTTIN},{"TTOU",SIGTTOU},{"URG",SIGURG},
    {"XCPU",SIGXCPU},{"XFSZ",SIGXFSZ},{"VTALRM",SIGVTALRM},{"PROF",SIGPROF},{"WINCH",SIGWINCH},{"IO",SIGIO},{"SYS",SIGSYS},{"STKFLT",16},{"PWR",30},{NULL,0} };
static int sig_parse(const char *s) {
    long v; if (str_to_int(s, &v)) return (int)v;
    if (!strncasecmp(s, "SIG", 3)) s += 3;
    if (!strcmp(s, "EXIT") || !strcmp(s, "0")) return 0;
    for (int i = 0; sigtab[i].n; i++) if (!strcasecmp(sigtab[i].n, s)) return sigtab[i].s;
    return -1;
}
int bash_internal_kill(int argc, char **argv) {
    int sig = SIGTERM, i = 1;
    if (argc == 1) { bash_rt_warn("kill: usage: kill [-s sigspec | -n signum | -sigspec] pid | jobspec ... or kill -l [sigspec]"); return 2; }
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if (!strcmp(a, "-l") || !strcmp(a, "-L")) {
            if (i + 1 < argc) { for (i++; i < argc; i++) { int s = sig_parse(argv[i]); const char *nm = NULL; for (int k = 0; sigtab[k].n; k++) if (sigtab[k].s == s) nm = sigtab[k].n;
                if (nm) { long v; if (str_to_int(argv[i], &v)) { outs(nm); outs("\n"); } else { char t[16]; int l = snprintf(t, sizeof t, "%d\n", s); out(t, l); } } else { bash_rt_warn("kill: %s: invalid signal specification", argv[i]); return 1; } } return 0; }
            { int col = 0; for (int sn = 1; sn < 32; sn++) { const char *nm = NULL; for (int k = 0; sigtab[k].n; k++) if (sigtab[k].s == sn) nm = sigtab[k].n; if (!nm) continue;
                char t[64]; int l = snprintf(t, sizeof t, "%2d) SIG%s%s", sn, nm, (++col % 5 == 0) ? "\n" : "\t"); out(t, l); } if (col % 5) outs("\n"); }
            return 0;
        }
        if (!strcmp(a, "-s") || !strcmp(a, "-n")) { if (i + 1 >= argc) { bash_rt_warn("kill: %s: option requires an argument", a); return 2; } sig = sig_parse(argv[++i]); if (sig < 0) { bash_rt_warn("kill: %s: invalid signal specification", argv[i]); return 1; } continue; }
        long v; if (str_to_int(a + 1, &v) && v < 0) break;   /* -PID handled below */
        sig = sig_parse(a + 1); if (sig < 0) { bash_rt_warn("kill: %s: invalid signal specification", a + 1); return 1; }
    }
    if (i >= argc) { bash_rt_warn("kill: usage: kill [-s sigspec | -n signum | -sigspec] pid | jobspec ... or kill -l [sigspec]"); return 2; }
    int rc = 0;
    for (; i < argc; i++) {
        long pid; if (!str_to_int(argv[i], &pid)) { bash_rt_warn("kill: %s: arguments must be process or job IDs", argv[i]); rc = 1; continue; }
        if (kill((pid_t)pid, sig) != 0) { bash_rt_warn("kill: (%ld) - %s", pid, strerror(errno)); rc = 1; }
    }
    return rc;
}

/* ---------------------------------------------------------- shell builtins */
int bash_internal_pwd(int argc, char **argv) { char *c = getcwd(NULL, 0); if (!c) return 1; outs(c); outs("\n"); free(c); return 0; }
int bash_internal_cd(int argc, char **argv) {
    const char *t = NULL; int pr = 0;
    if (argc > 1 && !strcmp(argv[1], "--")) { argv++; argc--; }
    if (argc > 2) { bash_rt_warn("cd: too many arguments"); return 1; }
    if (argc < 2) { t = bash_rt_getvar("HOME"); if (!t) { bash_rt_warn("cd: HOME not set"); return 1; } }
    else if (!strcmp(argv[1], "-")) { t = bash_rt_getvar("OLDPWD"); if (!t) { bash_rt_warn("cd: OLDPWD not set"); return 1; } pr = 1; }
    else t = argv[1];
    char *old = getcwd(NULL, 0);
    if (chdir(t) != 0) { errs("cd", t); free(old); return 1; }
    char *nw = getcwd(NULL, 0);
    if (old) { bash_rt_setvar("OLDPWD", old); bash_rt_export("OLDPWD"); }
    if (nw) { bash_rt_setvar("PWD", nw); if (pr) { outs(nw); outs("\n"); } }
    free(old); free(nw); return 0;
}
int bash_internal_shift(int argc, char **argv) {
    long n = 1; if (argc > 1 && !str_to_int(argv[1], &n)) { bash_rt_warn("shift: %s: numeric argument required", argv[1]); return 1; }
    if (bash_rt_shift((int)n)) { bash_rt_warn("shift: shift count out of range"); return 1; }
    return 0;
}
int bash_internal_unset(int argc, char **argv) {
    for (int i = 1; i < argc; i++) { if (argv[i][0] == '-' && argv[i][1]) continue; bash_rt_unsetvar(argv[i]); }
    return 0;
}
static void assign_arg(const char *a, int local, int exp, int ro, int assoc) {
    const char *eq = strchr(a, '='); char *name; int app = 0;
    if (eq) { size_t l = eq - a; if (l && a[l - 1] == '+') { app = 1; l--; } name = strndup(a, l); } else name = strdup(a);
    if (local) bash_rt_declare_local(name);
    if (assoc) bash_rt_make_assoc(name);
    if (eq) bash_rt_set(name, eq + 1, app);
    else if (local && 0) bash_rt_setvar(name, "");
    if (exp) bash_rt_export(name);
    if (ro) bash_rt_mark_readonly(name);
    free(name);
}
static int declare_common(int argc, char **argv, int dflt_local, int dflt_exp, int dflt_ro) {
    int local = dflt_local, exp = dflt_exp, ro = dflt_ro, print = 0, i = 1, assoc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'g') local = 0; else if (*p == 'x') exp = 1; else if (*p == 'r') ro = 1; else if (*p == 'p') print = 1;
            else if (*p == 'A') assoc = 1;
            else if (*p == 'n' && dflt_exp) exp = 0;
        }
    }
    if (print && i >= argc) { int n; char **e = bash_rt_exported_env(&n); for (int k = 0; k < n; k++) { outs("declare -x "); outs(e[k]); outs("\n"); free(e[k]); } free(e); return 0; }
    if (i >= argc && dflt_exp) { int n; char **e = bash_rt_exported_env(&n); for (int k = 0; k < n; k++) { outs("declare -x "); outs(e[k]); outs("\n"); free(e[k]); } free(e); return 0; }
    for (; i < argc; i++) assign_arg(argv[i], local, exp, ro, assoc);
    return 0;
}
int bash_internal_local(int argc, char **argv) { return declare_common(argc, argv, 1, 0, 0); }
int bash_internal_declare(int argc, char **argv) { return declare_common(argc, argv, 1, 0, 0); }
int bash_internal_typeset(int argc, char **argv) { return declare_common(argc, argv, 1, 0, 0); }
int bash_internal_export(int argc, char **argv) { return declare_common(argc, argv, 0, 1, 0); }
int bash_internal_readonly(int argc, char **argv) { return declare_common(argc, argv, 0, 0, 1); }
int bash_internal_set(int argc, char **argv) {
    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) { i++; break; }
        if ((a[0] != '-' && a[0] != '+') || !a[1]) break;
        int on = a[0] == '-';
        int wants_o = 0;
        for (const char *p = a + 1; *p; p++) { if (*p == 'o') wants_o = 1; else bash_rt_opt_set(*p, on); }
        if (wants_o && i + 1 < argc) { i++; if (!strcmp(argv[i], "pipefail")) bash_rt_pipefail_set(on); else if (!strcmp(argv[i], "errexit")) bash_rt_opt_set('e', on); else if (!strcmp(argv[i], "nounset")) bash_rt_opt_set('u', on); }
    }
    if (i < argc || (i > 1 && !strcmp(argv[i - 1], "--"))) {
        /* set -- args : replace positional parameters */
        int n = argc - i; int cur = bash_rt_npos(); bash_rt_shift(cur);
        /* re-add through a function-frame-free path */
        bash_rt_pos_replace(n, argv + i);
    }
    return 0;
}
int bash_internal_let(int argc, char **argv) {
    long v = 0; for (int i = 1; i < argc; i++) { int e; v = bash_rt_arith_eval(argv[i], &e); if (e) return 1; }
    return v != 0 ? 0 : 1;
}
int bash_internal_lbracket(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[argc - 1], "]")) { bash_rt_warn("[: missing `]'"); return 2; }
    return bash_rt_test(argc - 2, argv + 1, 0);
}
int bash_internal_test(int argc, char **argv) { return bash_rt_test(argc - 1, argv + 1, 0); }
int bash_internal_dblbracket(int argc, char **argv) { return bash_rt_test(argc - 1, argv + 1, 1); }

/* input reader shared by read/mapfile: block reads on seekable fds (seeking back what was not consumed),
 * byte reads otherwise, so the rest of the input stays available to the next command -- like bash */
typedef struct { int fd, seekable, cached, mem; const char *src; char buf[8192]; size_t n, p; long timeout_ms; int timed_out; } Rd;
static char *g_rd_src;
void bash_rt_read_src(char *s) { free(g_rd_src); g_rd_src = s; }
/* shared read-ahead buffer (see rt_fd.h): persists across read/mapfile calls on the same seekable fd */
int bash_rt_rdc_fd = -1;
static struct { char buf[65536]; size_t n, p; } rdc;
void bash_rt_fd_sync(void) {
    if (bash_rt_rdc_fd < 0) return;
    int fd = bash_rt_rdc_fd; bash_rt_rdc_fd = -1;
    if (rdc.p < rdc.n) lseek(fd, -(off_t)(rdc.n - rdc.p), SEEK_CUR);
    rdc.n = rdc.p = 0;
}
static void rd_init(Rd *r, int fd, long tmo) {
    r->fd = fd; r->n = r->p = 0; r->timeout_ms = tmo; r->timed_out = 0; r->cached = 0; r->src = NULL; r->mem = 0;
    if (fd == 0 && g_rd_src) { r->src = g_rd_src; r->seekable = 0; return; }   /* here-string: "<text>\n" from memory */
    if (tmo < 0 && fd == bash_rt_rdc_fd) { r->cached = 1; r->seekable = 1; return; }
    bash_rt_fd_sync();
    if (fd == 0 && bash_rt_min) { r->mem = 1; r->seekable = 0; return; }   /* pipeline stage input in memory: consume exactly */
    r->seekable = lseek(fd, 0, SEEK_CUR) >= 0;
    if (r->seekable && tmo < 0) { struct stat st; if (!fstat(fd, &st) && S_ISREG(st.st_mode)) { r->cached = 1; bash_rt_rdc_fd = fd; rdc.n = rdc.p = 0; } }
}
static int rd_get(Rd *r) {
    if (r->mem) { RtMin *m = bash_rt_min; if (!m) { r->mem = 0; return rd_get(r); } return m->p < m->n ? (unsigned char)m->s[m->p++] : -1; }
    if (r->src) { if (*r->src) return (unsigned char)*r->src++; if (r->n == 0) { r->n = 1; return '\n'; } return -1; }
    if (r->cached) {
        if (rdc.p < rdc.n) return (unsigned char)rdc.buf[rdc.p++];
        for (;;) {
            ssize_t k = read(r->fd, rdc.buf, sizeof rdc.buf);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) { rdc.n = rdc.p = 0; return -1; }
            rdc.n = k; rdc.p = 1; return (unsigned char)rdc.buf[0];
        }
    }
    if (r->p < r->n) return (unsigned char)r->buf[r->p++];
    if (r->timeout_ms >= 0 && !rt_poll0_(r->fd)) { struct pollfd pf = { r->fd, POLLIN, 0 }; int pr; while ((pr = poll(&pf, 1, (int)r->timeout_ms)) < 0 && errno == EINTR) {} if (pr == 0) { r->timed_out = 1; return -1; } }
    for (;;) {
        ssize_t k = read(r->fd, r->buf, r->seekable ? sizeof r->buf : 1);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return -1;
        r->n = k; r->p = 1; return (unsigned char)r->buf[0];
    }
}
static void rd_done(Rd *r) { if (r->src) { r->n = 0; return; } if (r->cached) return; if (r->seekable && r->p < r->n) lseek(r->fd, -(off_t)(r->n - r->p), SEEK_CUR); r->n = r->p = 0; }
static int opt_arg(int argc, char **argv, int *i, const char **a, const char **val) {   /* -dX or -d X */
    if ((*a)[1]) { *val = *a + 1; } else if (*i + 1 < argc) { *val = argv[++*i]; } else return 0;
    *a = " "; return 1;
}
int bash_internal_read(int argc, char **argv) {
    int raw = 0, i = 1, fd = 0; const char *prompt = NULL, *aname = NULL; int delim = '\n'; long nchars = -1; int exact = 0; long tmo = -1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *a = argv[i] + 1; *a && *a != ' '; a++) {
            const char *v;
            if (*a == 'r') raw = 1; else if (*a == 's') {}
            else if (strchr("padnNtu", *a)) {
                char o = *a; if (!opt_arg(argc, argv, &i, &a, &v)) { bash_rt_warn("read: -%c: option requires an argument", o); return 2; }
                if (o == 'p') prompt = v; else if (o == 'a') aname = v; else if (o == 'd') delim = (unsigned char)v[0];
                else if (o == 'n' || o == 'N') { nchars = atol(v); exact = o == 'N'; }
                else if (o == 'u') fd = atoi(v);
                else { double t = strtod(v, NULL); tmo = (long)(t * 1000); }
                break;
            } else { bash_rt_warn("read: -%c: invalid option", *a); return 2; }
        }
    }
    if (prompt && isatty(fd)) wr(2, prompt, strlen(prompt));
    if (tmo == 0) { if (rt_poll0_(fd)) return bash_rt_min->p < bash_rt_min->n ? 0 : 1; struct pollfd pf = { fd, POLLIN, 0 }; return poll(&pf, 1, 0) > 0 ? 0 : 1; }
    Rd r; rd_init(&r, fd, tmo);
    Buf line = { 0 }; int got_delim = 0, eof = 0; long cnt = 0;
    if (nchars == 0) goto assign;
    for (;;) {
        int c = rd_get(&r);
        if (c < 0) { eof = 1; break; }
        if (!exact && c == delim) { got_delim = 1; break; }
        if (c == 0) continue;
        if (c == '\\' && !raw) { int d = rd_get(&r); if (d < 0) { eof = 1; break; } if (d == '\n' && delim == '\n') continue; c = d; }
        bputc(&line, (char)c); cnt++;
        if (nchars > 0 && cnt >= nchars) { got_delim = 1; break; }
    }
    rd_done(&r);
assign:;
    if (r.timed_out) { free(line.s); return 142; }
    const char *s = line.s ? line.s : "";
    const char *ifs = bash_rt_getvar("IFS"); if (!ifs) ifs = " \t\n";
    int nv = argc - i;
    if (exact && !aname && nv <= 1) { bash_rt_setvar(nv ? argv[i] : "REPLY", s); }
    else if (aname) {
        char *copy = strdup(s); char **f = NULL; int nf = 0; char *p = copy;
        for (;;) { while (*p && strchr(ifs, *p) && isspace((unsigned char)*p)) p++; if (!*p) break; char *st = p; while (*p && !strchr(ifs, *p)) p++; f = realloc(f, (nf + 1) * sizeof *f); f[nf++] = st; if (*p) { *p++ = 0; while (*p && strchr(ifs, *p) && isspace((unsigned char)*p)) p++; } }
        bash_rt_set_array(aname, nf, f, 0); free(f); free(copy);
    } else if (nv == 0) bash_rt_setvar("REPLY", s);
    else {
        char *copy = strdup(s); char *p = copy;
        for (int k = 0; k < nv; k++) {
            while (*p && strchr(ifs, *p) && isspace((unsigned char)*p)) p++;
            if (k == nv - 1) { char *e = p + strlen(p); while (e > p && strchr(ifs, e[-1]) && isspace((unsigned char)e[-1])) e--; *e = 0; bash_rt_setvar(argv[i + k], p); break; }
            char *st = p; while (*p && !strchr(ifs, *p)) p++;
            char save = *p; *p = 0; bash_rt_setvar(argv[i + k], st);
            if (save) { p++; if (!isspace((unsigned char)save)) {} else while (*p && strchr(ifs, *p) && isspace((unsigned char)*p)) p++;
                        if (isspace((unsigned char)save) && *p && strchr(ifs, *p) && !isspace((unsigned char)*p)) p++; }
        }
        free(copy);
    }
    free(line.s);
    return (eof && !got_delim) ? 1 : 0;
}
static int mapfile_impl(int argc, char **argv) {
    int i = 1, strip = 0, delim = '\n', fd = 0; long count = 0, skip = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *a = argv[i] + 1; *a && *a != ' '; a++) {
            const char *v;
            if (*a == 't') strip = 1;
            else if (strchr("dnsu", *a)) { char o = *a; if (!opt_arg(argc, argv, &i, &a, &v)) { bash_rt_warn("%s: -%c: option requires an argument", argv[0], o); return 2; }
                if (o == 'd') delim = (unsigned char)v[0]; else if (o == 'n') count = atol(v); else if (o == 's') skip = atol(v); else fd = atoi(v); break; }
            else { bash_rt_warn("%s: -%c: invalid option", argv[0], *a); return 2; }
        }
    }
    const char *name = i < argc ? argv[i] : "MAPFILE";
    Rd r; rd_init(&r, fd, -1);
    char **items = NULL; int n = 0; long lines = 0; Buf cur = { 0 }; int have = 0;
    for (;;) {
        int c = rd_get(&r);
        if (c >= 0) { have = 1; if (c != delim || !strip) bputc(&cur, (char)c); }
        if (c < 0 || c == delim) {
            if (have) { if (lines >= skip) { items = realloc(items, (n + 1) * sizeof *items); items[n++] = cur.s ? cur.s : strdup(""); cur.s = NULL; } else free(cur.s); cur.s = NULL; cur.n = cur.cap = 0; lines++; }
            have = 0;
            if (c < 0 || (count && n >= count)) break;
        }
    }
    rd_done(&r);
    bash_rt_set_array(name, n, items, 0);
    for (int k = 0; k < n; k++) free(items[k]);
    free(items); return 0;
}
int bash_internal_mapfile(int argc, char **argv) { return mapfile_impl(argc, argv); }
int bash_internal_readarray(int argc, char **argv) { return mapfile_impl(argc, argv); }

/* ------------------------------------------------------------------ sleep */
int bash_external_sleep(int argc, char **argv) {
    if (argc < 2) { bash_rt_warn("sleep: missing operand"); return 1; }
    double total = 0;
    for (int i = 1; i < argc; i++) {
        char *e; double v = strtod(argv[i], &e); double mul = 1;
        if (e == argv[i]) { bash_rt_warn("sleep: invalid time interval '%s'", argv[i]); return 1; }
        if (*e == 'm') mul = 60; else if (*e == 'h') mul = 3600; else if (*e == 'd') mul = 86400;
        total += v * mul;
    }
    struct timespec ts = { (time_t)total, (long)((total - (time_t)total) * 1e9) };
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
    return 0;
}

/* --------------------------------------------------------------------- ls */
static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void ls_long(const char *dir, const char *name, Buf *o) {
    char path[4096]; struct stat st; snprintf(path, sizeof path, "%s%s%s", dir ? dir : "", dir && *dir ? "/" : "", name);
    if (lstat(path, &st) != 0) return;
    char m[11] = "----------";
    if (S_ISDIR(st.st_mode)) m[0] = 'd'; else if (S_ISLNK(st.st_mode)) m[0] = 'l'; else if (S_ISCHR(st.st_mode)) m[0] = 'c'; else if (S_ISBLK(st.st_mode)) m[0] = 'b'; else if (S_ISFIFO(st.st_mode)) m[0] = 'p'; else if (S_ISSOCK(st.st_mode)) m[0] = 's';
    const char *bits = "rwxrwxrwx"; for (int i = 0; i < 9; i++) if (st.st_mode & (0400 >> i)) m[1 + i] = bits[i];
    if (st.st_mode & S_ISUID) m[3] = (m[3] == 'x') ? 's' : 'S'; if (st.st_mode & S_ISGID) m[6] = (m[6] == 'x') ? 's' : 'S'; if (st.st_mode & S_ISVTX) m[9] = (m[9] == 'x') ? 't' : 'T';
    struct passwd *pw = getpwuid(st.st_uid); struct group *gr = getgrgid(st.st_gid);
    char tm[32]; struct tm lt; localtime_r(&st.st_mtime, &lt); strftime(tm, sizeof tm, "%b %e %H:%M", &lt);
    char line[8192]; int n = snprintf(line, sizeof line, "%s %lu %s %s %lld %s %s", m, (unsigned long)st.st_nlink, pw ? pw->pw_name : "?", gr ? gr->gr_name : "?", (long long)st.st_size, tm, name);
    bput(o, line, n);
    if (S_ISLNK(st.st_mode)) { char t[4096]; ssize_t l = readlink(path, t, sizeof t - 1); if (l > 0) { t[l] = 0; bputs(o, " -> "); bputs(o, t); } }
    bputc(o, '\n');
}
int bash_external_ls(int argc, char **argv) {
    int all = 0, lng = 0, dflag = 0, i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) { if (!strcmp(argv[i], "--")) { i++; break; } for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'a' || *p == 'A') all = 1; else if (*p == 'l') lng = 1; else if (*p == 'd') dflag = 1; } }
    int nargs = argc - i; char **paths = argv + i; char *dot = "."; if (!nargs) { paths = &dot; nargs = 1; }
    for (int k = 0; k < nargs; k++) {
        const char *p = paths[k]; Buf o = { 0 }; struct stat st;
        if (stat(p, &st) != 0 && lstat(p, &st) != 0) { errs("ls", p); rc = 2; continue; }
        if (nargs > 1 && k) bputc(&o, '\n');
        if (S_ISDIR(st.st_mode) && !dflag) {
            if (nargs > 1) { bputs(&o, p); bputs(&o, ":\n"); }
            DIR *d = opendir(p); if (!d) { errs("ls", p); rc = 2; free(o.s); continue; }
            char **names = NULL; int nn = 0; struct dirent *e;
            while ((e = readdir(d))) { if (!all && e->d_name[0] == '.') continue; names = realloc(names, (nn + 1) * sizeof *names); names[nn++] = strdup(e->d_name); }
            closedir(d); qsort(names, nn, sizeof *names, cmp_str);
            for (int j = 0; j < nn; j++) { if (lng) ls_long(p, names[j], &o); else { bputs(&o, names[j]); bputc(&o, '\n'); } free(names[j]); }
            free(names);
        } else { if (lng) { ls_long("", p, &o); } else { bputs(&o, p); bputc(&o, '\n'); } }
        if (o.s) out(o.s, o.n); free(o.s);
    }
    return rc;
}

/* --------------------------------------------------------------------- wc */
static int ndigits(long long v) { int n = 1; while (v >= 10) { v /= 10; n++; } return n; }
int bash_external_wc(int argc, char **argv) {
    int fl = 0, fw = 0, fc = 0, i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) { if (!strcmp(argv[i], "--")) { i++; break; } for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'l') fl = 1; else if (*p == 'w') fw = 1; else if (*p == 'c' || *p == 'm') fc = 1; } }
    if (!fl && !fw && !fc) fl = fw = fc = 1;
    int ncols = fl + fw + fc, nf = argc - i; int nin = nf ? nf : 1;
    long long (*cnt)[3] = calloc(nin, sizeof *cnt); int *okv = calloc(nin, sizeof(int)); long long tot[3] = { 0 }; long long totbytes = 0; int allreg = nf > 0;
    for (int f = 0; f < nin; f++) {
        const char *path = nf ? argv[i + f] : NULL; Buf b = { 0 };
        if (slurp(path, &b) != 0) { errs("wc", path); rc = 1; continue; }
        okv[f] = 1; int inw = 0;
        for (size_t k = 0; k < b.n; k++) { unsigned char c = b.s[k]; if (c == '\n') cnt[f][0]++; if (isspace(c)) inw = 0; else if (!inw) { inw = 1; cnt[f][1]++; } }
        cnt[f][2] = b.n; free(b.s);
        for (int k = 0; k < 3; k++) tot[k] += cnt[f][k];
        struct stat st; if (path && stat(path, &st) == 0 && S_ISREG(st.st_mode)) totbytes += st.st_size; else allreg = 0;
    }
    int width = (ncols == 1 && nin == 1) ? 0 : allreg ? ndigits(totbytes) : 7;
    for (int f = 0; f <= nin; f++) {
        if (f == nin && nin < 2) break; if (f < nin && !okv[f]) continue;
        long long *v = f == nin ? tot : cnt[f]; char line[512]; int n = 0; int first = 1;
        int sel[3] = { fl, fw, fc };
        for (int k = 0; k < 3; k++) if (sel[k]) { n += snprintf(line + n, sizeof line - n, "%s%*lld", first ? "" : " ", width, v[k]); first = 0; }
        if (nf) n += snprintf(line + n, sizeof line - n, " %s", f == nin ? "total" : argv[i + f]);
        line[n++] = '\n'; out(line, n);
    }
    free(cnt); free(okv); return rc;
}

/* ------------------------------------------------------------ head / tail */
static int parse_count_opts(int argc, char **argv, int *i, long *n, int *plus, int *bytes, const char *name) {
    *n = 10; *plus = 0; *bytes = 0;
    for (; *i < argc && argv[*i][0] == '-' && argv[*i][1]; (*i)++) {
        const char *a = argv[*i];
        if (!strcmp(a, "--")) { (*i)++; break; }
        if (!strcmp(a, "-n") || !strcmp(a, "-c")) { *bytes = a[1] == 'c'; if (*i + 1 >= argc) { bash_rt_warn("%s: option requires an argument -- '%c'", name, a[1]); return -1; } const char *v = argv[++*i]; if (*v == '+') { *plus = 1; v++; } else if (*v == '-' && !strcmp(name, "head")) { v++; /* head -n -N unsupported -> treat as N */ } if (!str_to_int(v, n)) { bash_rt_warn("%s: invalid number of lines: '%s'", name, argv[*i]); return -1; } }
        else if (!strncmp(a, "-n", 2) || !strncmp(a, "-c", 2)) { *bytes = a[1] == 'c'; const char *v = a + 2; if (*v == '+') { *plus = 1; v++; } if (!str_to_int(v, n)) { bash_rt_warn("%s: invalid number of lines: '%s'", name, a + 2); return -1; } }
        else if (isdigit((unsigned char)a[1])) { if (!str_to_int(a + 1, n)) return -1; }
        else if (!strcmp(a, "-q")) {} else { bash_rt_warn("%s: invalid option -- '%s'", name, a + 1); return -1; }
    }
    return 0;
}
int bash_external_head(int argc, char **argv) {
    int i = 1, plus, bytes; long n; int rc = 0;
    if (parse_count_opts(argc, argv, &i, &n, &plus, &bytes, "head") < 0) return 1;
    int nf = argc - i; static char buf[65536];
    for (int f = 0; f < (nf ? nf : 1); f++) {
        const char *path = nf ? argv[i + f] : "-";
        int fd = !strcmp(path, "-") ? 0 : open(path, O_RDONLY);
        if (fd < 0) { errs("head", path); rc = 1; continue; }
        if (nf > 1) { char h[4096]; int l = snprintf(h, sizeof h, "%s==> %s <==\n", f ? "\n" : "", path); out(h, l); }
        long left = n;   /* stream: stop as soon as enough was read, give back the rest on seekable input */
        while (left > 0) {
            ssize_t k = read(fd, buf, sizeof buf);
            if (k < 0) { if (errno == EINTR) continue; errs("head", path); rc = 1; break; }
            if (k == 0) break;
            size_t use = 0;
            if (bytes) use = (size_t)k < (size_t)left ? (size_t)k : (size_t)left, left -= use;
            else { while (use < (size_t)k && left > 0) { if (buf[use++] == '\n') left--; } }
            out(buf, use);
            if (use < (size_t)k) { lseek(fd, -(off_t)(k - use), SEEK_CUR); break; }
        }
        if (fd) close(fd);
    }
    return rc;
}
int bash_external_tail(int argc, char **argv) {
    int i = 1, plus, bytes; long n; int rc = 0;
    if (parse_count_opts(argc, argv, &i, &n, &plus, &bytes, "tail") < 0) return 1;
    int nf = argc - i;
    for (int f = 0; f < (nf ? nf : 1); f++) {
        const char *path = nf ? argv[i + f] : NULL; Buf b = { 0 };
        if (slurp(path, &b) != 0) { errs("tail", path); rc = 1; continue; }
        if (nf > 1) { char h[4096]; int l = snprintf(h, sizeof h, "%s==> %s <==\n", f ? "\n" : "", path); out(h, l); }
        size_t start;
        if (bytes) start = plus ? (size_t)(n > 0 ? n - 1 : 0) : ((size_t)n < b.n ? b.n - n : 0);
        else if (plus) { long lines = 1; start = 0; while (start < b.n && lines < n) { if (b.s[start++] == '\n') lines++; } }
        else { long lines = 0; size_t k = b.n; if (k && b.s[k - 1] == '\n') k--; start = 0; while (k > 0) { if (b.s[k - 1] == '\n') { if (++lines >= n) { start = k; break; } } k--; } if (n == 0) start = b.n; }
        if (start > b.n) start = b.n; out(b.s + start, b.n - start); free(b.s);
    }
    return rc;
}

/* ------------------------------------------------------- file management */
static int mkdir_p(const char *path, mode_t mode) {
    char *p = strdup(path); size_t L = strlen(p); while (L > 1 && p[L - 1] == '/') p[--L] = 0;
    for (char *q = p + 1; *q; q++) if (*q == '/') { *q = 0; if (mkdir(p, mode) != 0 && errno != EEXIST) { free(p); return -1; } *q = '/'; }
    int r = mkdir(p, mode); if (r != 0 && errno == EEXIST && is_dir(p)) r = 0; free(p); return r;
}
int bash_external_mkdir(int argc, char **argv) {
    int pflag = 0, i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) { if (!strcmp(argv[i], "--")) { i++; break; } if (strchr(argv[i], 'p')) pflag = 1; }
    if (i >= argc) { bash_rt_warn("mkdir: missing operand"); return 1; }
    for (; i < argc; i++) { int r = pflag ? mkdir_p(argv[i], 0777) : mkdir(argv[i], 0777); if (r != 0) { bash_rt_warn("mkdir: cannot create directory '%s': %s", argv[i], strerror(errno)); rc = 1; } }
    return rc;
}
int bash_external_rmdir(int argc, char **argv) {
    int rc = 0; for (int i = 1; i < argc; i++) if (argv[i][0] != '-' && rmdir(argv[i]) != 0) { bash_rt_warn("rmdir: failed to remove '%s': %s", argv[i], strerror(errno)); rc = 1; } return rc;
}
static int rm_cb(const char *p, const struct stat *st, int t, struct FTW *f) { return remove(p); }
int bash_external_rm(int argc, char **argv) {
    int rec = 0, force = 0, i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) { if (!strcmp(argv[i], "--")) { i++; break; } for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'r' || *p == 'R') rec = 1; else if (*p == 'f') force = 1; } }
    if (i >= argc && !force) { bash_rt_warn("rm: missing operand"); return 1; }
    for (; i < argc; i++) {
        struct stat st;
        if (lstat(argv[i], &st) != 0) { if (!force) { bash_rt_warn("rm: cannot remove '%s': %s", argv[i], strerror(errno)); rc = 1; } continue; }
        if (S_ISDIR(st.st_mode)) { if (!rec) { bash_rt_warn("rm: cannot remove '%s': Is a directory", argv[i]); rc = 1; continue; } if (nftw(argv[i], rm_cb, 16, FTW_DEPTH | FTW_PHYS) != 0) { bash_rt_warn("rm: cannot remove '%s': %s", argv[i], strerror(errno)); rc = 1; } }
        else if (unlink(argv[i]) != 0) { if (!force) { bash_rt_warn("rm: cannot remove '%s': %s", argv[i], strerror(errno)); rc = 1; } }
    }
    return rc;
}
int bash_external_touch(int argc, char **argv) {
    int rc = 0; for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) continue;
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0666); if (fd < 0) { bash_rt_warn("touch: cannot touch '%s': %s", argv[i], strerror(errno)); rc = 1; continue; }
        futimens(fd, NULL); close(fd);
    } return rc;
}
int bash_external_basename(int argc, char **argv) {
    if (argc < 2) { bash_rt_warn("basename: missing operand"); return 1; }
    char *p = strdup(argv[1]); size_t L = strlen(p); while (L > 1 && p[L - 1] == '/') p[--L] = 0;
    char *b = strrchr(p, '/'); b = (b && L > 1) ? b + 1 : p;
    if (argc > 2) { size_t sl = strlen(argv[2]), bl = strlen(b); if (sl < bl && !strcmp(b + bl - sl, argv[2])) b[bl - sl] = 0; }
    outs(b); outs("\n"); free(p); return 0;
}
int bash_external_dirname(int argc, char **argv) {
    if (argc < 2) { bash_rt_warn("dirname: missing operand"); return 1; }
    for (int i = 1; i < argc; i++) {
        char *p = strdup(argv[i]); size_t L = strlen(p); while (L > 1 && p[L - 1] == '/') p[--L] = 0;
        char *s = strrchr(p, '/');
        if (!s) outs("."); else { while (s > p && s[-1] == '/') s--; if (s == p) outs("/"); else out(p, s - p); }
        outs("\n"); free(p);
    } return 0;
}
int bash_external_realpath(int argc, char **argv) {
    int rc = 0; for (int i = 1; i < argc; i++) { if (argv[i][0] == '-' && argv[i][1]) continue; char *r = realpath(argv[i], NULL); if (!r) { errs("realpath", argv[i]); rc = 1; continue; } outs(r); outs("\n"); free(r); } return rc;
}
static int copy_file(const char *src, const char *dst) {
    int in = open(src, O_RDONLY); if (in < 0) return -1; struct stat st; fstat(in, &st);
    int o = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode & 0777); if (o < 0) { close(in); return -1; }
    char buf[65536]; ssize_t n; while ((n = read(in, buf, sizeof buf)) > 0) wr(o, buf, n);
    close(in); close(o); return 0;
}
static int copy_rec(const char *src, const char *dst) {
    struct stat st; if (stat(src, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) return copy_file(src, dst);
    if (mkdir(dst, st.st_mode & 0777) != 0 && errno != EEXIST) return -1;
    DIR *d = opendir(src); if (!d) return -1; struct dirent *e; int rc = 0;
    while ((e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; char a[4096], b[4096]; snprintf(a, sizeof a, "%s/%s", src, e->d_name); snprintf(b, sizeof b, "%s/%s", dst, e->d_name); if (copy_rec(a, b) != 0) rc = -1; }
    closedir(d); return rc;
}
static const char *base_of(const char *p) { const char *b = strrchr(p, '/'); return b ? b + 1 : p; }
static int cp_mv(int argc, char **argv, int mv) {
    int rec = 0, i = 1, rc = 0; const char *name = mv ? "mv" : "cp";
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) { if (!strcmp(argv[i], "--")) { i++; break; } for (const char *p = argv[i] + 1; *p; p++) if (*p == 'r' || *p == 'R' || *p == 'a') rec = 1; }
    if (argc - i < 2) { bash_rt_warn("%s: missing file operand", name); return 1; }
    const char *dst = argv[argc - 1]; int ddir = is_dir(dst);
    if (argc - i > 2 && !ddir) { bash_rt_warn("%s: target '%s' is not a directory", name, dst); return 1; }
    for (int k = i; k < argc - 1; k++) {
        char t[4096]; const char *d = dst; if (ddir) { snprintf(t, sizeof t, "%s/%s", dst, base_of(argv[k])); d = t; }
        if (mv) {
            if (rename(argv[k], d) == 0) continue;
            if (errno == EXDEV) { char t2[4096]; snprintf(t2, sizeof t2, "%s", d); char *av[] = { "mv", "--", argv[k], t2, NULL }; if (bash_rt_exec_external(4, av)) rc = 1; continue; }
            bash_rt_warn("mv: cannot move '%s' to '%s': %s", argv[k], d, strerror(errno)); rc = 1;
        }
        else if (is_dir(argv[k]) && !rec) { bash_rt_warn("cp: -r not specified; omitting directory '%s'", argv[k]); rc = 1; }
        else if (copy_rec(argv[k], d) != 0) { bash_rt_warn("cp: cannot copy '%s' to '%s': %s", argv[k], d, strerror(errno)); rc = 1; }
    }
    return rc;
}
int bash_external_cp(int argc, char **argv) { return cp_mv(argc, argv, 0); }
int bash_external_mv(int argc, char **argv) { return cp_mv(argc, argv, 1); }
int bash_external_chmod(int argc, char **argv) {
    if (argc < 3) { bash_rt_warn("chmod: missing operand"); return 1; }
    char *e; long m = strtol(argv[1], &e, 8); if (*e) { bash_rt_warn("chmod: only octal modes are supported"); return 1; }
    int rc = 0; for (int i = 2; i < argc; i++) if (chmod(argv[i], (mode_t)m) != 0) { bash_rt_warn("chmod: cannot access '%s': %s", argv[i], strerror(errno)); rc = 1; }
    return rc;
}

/* ------------------------------------------------------------------- grep */
int bash_external_grep(int argc, char **argv) {
    int icase = 0, inv = 0, num = 0, count = 0, ere = 0, fixed = 0, quiet = 0, lonly = 0, hflag = -1, wflag = 0, xflag = 0, i = 1;
    char *pat = NULL;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i]; if (!strcmp(a, "--")) { i++; break; }
        for (const char *p = a + 1; *p; p++) {
            switch (*p) {
            case 'i': icase = 1; break; case 'v': inv = 1; break; case 'n': num = 1; break; case 'c': count = 1; break;
            case 'E': ere = 1; break; case 'F': fixed = 1; break; case 'q': quiet = 1; break; case 'l': lonly = 1; break;
            case 'H': hflag = 1; break; case 'h': hflag = 0; break; case 'w': wflag = 1; break; case 'x': xflag = 1; break; case 's': case 'r': break;
            case 'e': if (p[1]) { pat = strdup(p + 1); } else if (i + 1 < argc) pat = strdup(argv[++i]); p = " "; break;
            default: bash_rt_warn("grep: invalid option -- '%c'", *p); return 2;
            }
            if (*p == ' ') break;
        }
    }
    if (!pat) { if (i >= argc) { bash_rt_warn("grep: no pattern"); return 2; } pat = strdup(argv[i++]); }
    regex_t re; char *rp = pat;
    if (fixed) { Buf q = { 0 }; for (char *c = pat; *c; c++) { if (strchr("^$.*[]\\+?(){}|", *c)) bputc(&q, '\\'); bputc(&q, *c); } rp = q.s ? q.s : strdup(""); ere = 1; }
    if (xflag || wflag) { Buf q = { 0 }; bputs(&q, xflag ? "^(" : "(^|[^[:alnum:]_])("); bputs(&q, rp); bputs(&q, xflag ? ")$" : ")([^[:alnum:]_]|$)"); rp = q.s; ere = 1; }
    if (regcomp(&re, rp, (ere ? REG_EXTENDED : 0) | (icase ? REG_ICASE : 0) | REG_NOSUB) != 0) { bash_rt_warn("grep: invalid regular expression"); return 2; }
    int nf = argc - i, multi = hflag < 0 ? nf > 1 : hflag, any = 0, rc = 0;
    int literal = !icase && !xflag && !wflag && !strpbrk(pat, "^$.*[]\\+?(){}|") && *pat; size_t plen = strlen(pat);
    static char rb[65536];
    for (int f = 0; f < (nf ? nf : 1); f++) {
        const char *path = nf ? argv[i + f] : NULL;
        int fd = !path || !strcmp(path, "-") ? 0 : open(path, O_RDONLY);
        if (fd < 0) { errs("grep", path); rc = 2; continue; }
        long ln = 0, matches = 0; Buf o = { 0 }, carry = { 0 }; int stop = 0, eof = 0;
        while (!stop && !eof) {   /* streamed: -q / -l stop at the first match even on endless input */
            ssize_t k = read(fd, rb, sizeof rb);
            if (k < 0) { if (errno == EINTR) continue; errs("grep", path); rc = 2; break; }
            if (k == 0) { eof = 1; if (!carry.n) break; bputc(&carry, '\n'); }
            else bput(&carry, rb, k);
            size_t pos = 0, lim = carry.n;
            while (pos < lim) {
                char *nl = memchr(carry.s + pos, '\n', lim - pos); if (!nl) break;
                size_t e = nl - carry.s; char *line = carry.s + pos; carry.s[e] = 0; ln++;
                int m = literal ? (e - pos >= plen && memmem(line, e - pos, pat, plen) != NULL) : regexec(&re, line, 0, NULL, 0) == 0; if (inv) m = !m;
                carry.s[e] = '\n'; pos = e + 1;
                if (!m) continue;
                matches++; any = 1;
                if (quiet || lonly) { stop = 1; break; }
                if (!count) { if (multi) { bputs(&o, path ? path : "(standard input)"); bputc(&o, ':'); } if (num) { char t[24]; snprintf(t, sizeof t, "%ld:", ln); bputs(&o, t); } bput(&o, line, e - (line - carry.s)); bputc(&o, '\n'); }
            }
            memmove(carry.s, carry.s + pos, carry.n - pos); carry.n -= pos;
            if (o.n > 32768) { out(o.s, o.n); o.n = 0; }
        }
        if (fd) close(fd);
        free(carry.s);
        if (count && !quiet) { if (multi) { bputs(&o, path ? path : "(standard input)"); bputc(&o, ':'); } char t[24]; snprintf(t, sizeof t, "%ld\n", matches); bputs(&o, t); }
        if (lonly && matches && !quiet) { bputs(&o, path ? path : "(standard input)"); bputc(&o, '\n'); }
        if (o.n) out(o.s, o.n); free(o.s);
        if (quiet && any) { regfree(&re); return 0; }
    }
    regfree(&re); return rc ? rc : (any ? 0 : 1);
}

/* ----------------------------------------------------- sort / uniq / seq */
static int s_rev, s_num, s_fold, s_field; static char s_sep;
typedef struct { char *s; union { double k; unsigned long long u; }; } SL;
static const char *field_of(const char *l) {
    if (!s_field) return l;
    int f = 1; const char *p = l;
    while (f < s_field) { if (s_sep) { const char *q = strchr(p, s_sep); if (!q) return p + strlen(p); p = q + 1; } else { while (*p == ' ' || *p == '\t') p++; while (*p && *p != ' ' && *p != '\t') p++; } f++; }
    if (!s_sep) while (*p == ' ' || *p == '\t') p++;
    return p;
}
static int cmp_lines(const void *a, const void *b) {
    const SL *x = a, *y = b; int r;
    if (s_num) r = x->k < y->k ? -1 : x->k > y->k ? 1 : 0;
    else if (s_field || s_fold) { const char *fx = field_of(x->s), *fy = field_of(y->s); r = s_fold ? strcasecmp(fx, fy) : strcmp(fx, fy); }
    else r = strcmp(x->s, y->s);
    if (r == 0) r = strcmp(x->s, y->s);
    return s_rev ? -r : r;
}
/* stable-free merge sort with an inlined comparator (qsort's indirect calls dominate for short lines) */
static inline int cmp_fast(const SL *x, const SL *y) {
    if (s_num && !s_field) { if (x->k != y->k) { int r = x->k < y->k ? -1 : 1; return s_rev ? -r : r; } int r = strcmp(x->s, y->s); return s_rev ? -r : r; }
    if (!s_num && !s_field && !s_fold) {      /* compare an 8-byte big-endian prefix first */
        int r; if (x->u != y->u) r = x->u < y->u ? -1 : 1; else if (!(x->u & 0xff) ) r = 0; else r = strcmp(x->s + 7, y->s + 7);
        return s_rev ? -r : r;
    }
    return cmp_lines(x, y);
}
static void msort(SL *a, int n) {
    if (n < 2) return;
    SL *t = malloc(n * sizeof *t);
    for (int i = 0; i < n; i += 8) { int hi = i + 8 < n ? i + 8 : n;      /* insertion sort small runs */
        for (int j = i + 1; j < hi; j++) { SL v = a[j]; int k = j - 1; while (k >= i && cmp_fast(&a[k], &v) > 0) { a[k + 1] = a[k]; k--; } a[k + 1] = v; } }
    SL *src = a, *dst = t;
    for (int w = 8; w < n; w *= 2) {
        for (int i = 0; i < n; i += 2 * w) {
            int m = i + w < n ? i + w : n, hi = i + 2 * w < n ? i + 2 * w : n, l = i, r = m, o = i;
            while (l < m && r < hi) dst[o++] = cmp_fast(&src[r], &src[l]) < 0 ? src[r++] : src[l++];
            while (l < m) dst[o++] = src[l++]; while (r < hi) dst[o++] = src[r++];
        }
        SL *x = src; src = dst; dst = x;
    }
    if (src != a) memcpy(a, src, n * sizeof *a);
    free(t);
}
int bash_external_sort(int argc, char **argv) {
    int uniq = 0, i = 1; s_rev = s_num = s_fold = s_field = 0; s_sep = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i]; if (!strcmp(a, "--")) { i++; break; }
        for (const char *p = a + 1; *p; p++) {
            if (*p == 'r') s_rev = 1; else if (*p == 'n') s_num = 1; else if (*p == 'u') uniq = 1; else if (*p == 'f') s_fold = 1;
            else if (*p == 'k') { const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : "1"); s_field = atoi(v); if (strchr(v, 'n')) s_num = 1; break; }
            else if (*p == 't') { const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : ""); s_sep = v[0]; break; }
        }
    }
    Buf all = { 0 }; int nf = argc - i, rc = 0;
    for (int f = 0; f < (nf ? nf : 1); f++) { const char *path = nf ? argv[i + f] : NULL; if (slurp(path, &all) != 0) { errs("sort", path); rc = 2; } }
    SL *lines = NULL; int n = 0, cap = 0; size_t pos = 0;
    while (pos < all.n) {
        char *nl = memchr(all.s + pos, '\n', all.n - pos); size_t e = nl ? (size_t)(nl - all.s) : all.n;
        if (n == cap) { cap = cap ? cap * 2 : 1024; lines = realloc(lines, cap * sizeof *lines); }
        all.s[e] = 0; lines[n].s = all.s + pos; if (s_num) lines[n].k = strtod(field_of(lines[n].s), NULL);
        else { unsigned long long u = 0; const unsigned char *q = (const unsigned char *)lines[n].s; int j = 0;
               for (; j < 8 && q[j]; j++) u = (u << 8) | q[j]; u <<= 8 * (8 - j); lines[n].u = u; }
        n++; pos = e + 1;
    }
    msort(lines, n);
    Buf o = { 0 }; if (all.n) { o.cap = all.n + 16; o.s = malloc(o.cap); }
    for (int k = 0; k < n; k++) {
        if (uniq && k) { int same = s_num || s_field || s_fold ? (cmp_lines(&lines[k - 1], &lines[k]) == 0) : !strcmp(lines[k - 1].s, lines[k].s); if (same) continue; }
        bputs(&o, lines[k].s); bputc(&o, '\n');
    }
    if (o.s) out(o.s, o.n); free(o.s); free(lines); free(all.s); return rc;
}
int bash_external_uniq(int argc, char **argv) {
    int cnt = 0, dup = 0, only = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'c') cnt = 1; else if (*p == 'd') dup = 1; else if (*p == 'u') only = 1; }
    Buf all = { 0 }; if (slurp(i < argc ? argv[i] : NULL, &all) != 0) { errs("uniq", argv[i]); return 1; }
    Buf o = { 0 }; size_t pos = 0; char *prev = NULL; long c = 0;
    #define FLUSH() do { if (prev) { if ((!dup && !only) || (dup && c > 1) || (only && c == 1)) { if (cnt) { char t[32]; snprintf(t, sizeof t, "%7ld ", c); bputs(&o, t); } bputs(&o, prev); bputc(&o, '\n'); } prev = NULL; } } while (0)
    while (pos < all.n) { char *nl = memchr(all.s + pos, '\n', all.n - pos); size_t e = nl ? (size_t)(nl - all.s) : all.n; char *l = all.s + pos; all.s[e] = 0; pos = e + 1;
        if (prev && !strcmp(prev, l)) c++; else { FLUSH(); prev = l; c = 1; } }
    FLUSH();
    if (o.s) out(o.s, o.n); free(o.s); free(all.s); return 0;
}
int bash_external_seq(int argc, char **argv) {
    double a = 1, s = 1, b; int isf = 0;
    for (int k = 1; k < argc; k++) if (strchr(argv[k], '.')) isf = 1;
    if (argc == 2) b = strtod(argv[1], NULL); else if (argc == 3) { a = strtod(argv[1], NULL); b = strtod(argv[2], NULL); }
    else if (argc == 4) { a = strtod(argv[1], NULL); s = strtod(argv[2], NULL); b = strtod(argv[3], NULL); }
    else { bash_rt_warn("seq: missing operand"); return 1; }
    if (s == 0) { bash_rt_warn("seq: invalid zero increment value"); return 1; }
    Buf o = { 0 };
    for (double v = a; s > 0 ? v <= b + 1e-9 : v >= b - 1e-9; v += s) { char t[64]; int l = isf ? snprintf(t, sizeof t, "%g\n", v) : snprintf(t, sizeof t, "%lld\n", (long long)v); bput(&o, t, l); if (o.n > 60000) { out(o.s, o.n); o.n = 0; } }
    if (o.s) out(o.s, o.n); free(o.s); return 0;
}

/* --------------------------------------------------------------------- tr */
static int tr_expand(const char *s, unsigned char *set) {
    int n = 0;
    for (; *s; s++) {
        unsigned char c = *s;
        if (c == '\\' && s[1]) { s++; c = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s == 'r' ? '\r' : *s == '\\' ? '\\' : *s; }
        else if (c == '[' && s[1] == ':') {
            const char *e = strstr(s, ":]");
            if (e) { char nm[16] = { 0 }; size_t l = e - s - 2; if (l < sizeof nm) memcpy(nm, s + 2, l);
                for (int ch = 0; ch < 256; ch++) { int m = !strcmp(nm, "lower") ? islower(ch) : !strcmp(nm, "upper") ? isupper(ch) : !strcmp(nm, "digit") ? isdigit(ch) : !strcmp(nm, "space") ? isspace(ch) : !strcmp(nm, "alpha") ? isalpha(ch) : !strcmp(nm, "alnum") ? isalnum(ch) : !strcmp(nm, "punct") ? ispunct(ch) : 0; if (m) set[n++] = ch; }
                s = e + 1; continue; }
        }
        if (s[1] == '-' && s[2]) { unsigned char hi = s[2]; for (int ch = c; ch <= hi; ch++) set[n++] = ch; s += 2; }
        else set[n++] = c;
    }
    return n;
}
int bash_external_tr(int argc, char **argv) {
    int del = 0, sq = 0, comp = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) for (const char *p = argv[i] + 1; *p; p++) { if (*p == 'd') del = 1; else if (*p == 's') sq = 1; else if (*p == 'c' || *p == 'C') comp = 1; }
    if (i >= argc) { bash_rt_warn("tr: missing operand"); return 1; }
    unsigned char s1[4096], s2[4096]; int n1 = tr_expand(argv[i++], s1), n2 = 0;
    if (comp) { unsigned char t[256] = { 0 }; for (int k = 0; k < n1; k++) t[s1[k]] = 1; n1 = 0; for (int c = 0; c < 256; c++) if (!t[c]) s1[n1++] = c; }
    if (i < argc) n2 = tr_expand(argv[i], s2);
    if (!del && !sq && !n2) { bash_rt_warn("tr: missing operand after '%s'", argv[i - 1]); return 1; }
    int map[256], inset[256] = { 0 }, sqset[256] = { 0 }; for (int c = 0; c < 256; c++) map[c] = c;
    for (int k = 0; k < n1; k++) { inset[s1[k]] = 1; if (!del && n2) map[s1[k]] = s2[k < n2 ? k : n2 - 1]; }
    if (sq) { if (n2 && !del) for (int k = 0; k < n2; k++) sqset[s2[k]] = 1; else for (int k = 0; k < n1; k++) sqset[s1[k]] = 1; }
    Buf b = { 0 }; slurp(NULL, &b); Buf o = { 0 }; int last = -1;
    for (size_t k = 0; k < b.n; k++) {
        unsigned char c = b.s[k];
        if (del && inset[c]) continue;
        int r = inset[c] && !del ? map[c] : c;
        if (sq && sqset[r] && last == r) continue;
        bputc(&o, (char)r); last = r;
    }
    if (o.s) out(o.s, o.n); free(o.s); free(b.s); return 0;
}

/* -------------------------------------------------------------------- cut */
typedef struct { long lo, hi; } Rng;
static Rng rngs[64]; static int nrng; static const char *rng_src; static unsigned char rmap[1024];
static int in_list(const char *list, long idx) {
    if (list != rng_src) {      /* parse once; the list is a constant for the whole command */
        nrng = 0; rng_src = list; memset(rmap, 0, sizeof rmap); char *l = strdup(list), *save, *tok;
        for (tok = strtok_r(l, ",", &save); tok && nrng < 64; tok = strtok_r(NULL, ",", &save)) {
            char *d = strchr(tok, '-'); long lo, hi;
            if (!d) { lo = hi = atol(tok); } else { *d = 0; lo = *tok ? atol(tok) : 1; hi = d[1] ? atol(d + 1) : 1L << 40; }
            rngs[nrng].lo = lo; rngs[nrng].hi = hi; nrng++; for (long q = lo; q <= hi && q < 1024; q++) if (q >= 0) rmap[q] = 1;
        }
        free(l);
    }
    if (idx >= 0 && idx < 1024) return rmap[idx];
    for (int i = 0; i < nrng; i++) if (idx >= rngs[i].lo && idx <= rngs[i].hi) return 1;
    return 0;
}
int bash_external_cut(int argc, char **argv) {
    char delim = '\t'; const char *fl = NULL, *cl = NULL; int only = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        const char *a = argv[i]; if (!strcmp(a, "--")) { i++; break; }
        if (a[1] == 'd') delim = a[2] ? a[2] : (i + 1 < argc ? argv[++i][0] : '\t');
        else if (a[1] == 'f') fl = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);
        else if (a[1] == 'c' || a[1] == 'b') cl = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);
        else if (!strcmp(a, "-s")) only = 1;
    }
    rng_src = NULL;
    if (!fl && !cl) { bash_rt_warn("cut: you must specify a list of bytes, characters, or fields"); return 1; }
    Buf all = { 0 }; if (slurp(i < argc ? argv[i] : NULL, &all) != 0) { errs("cut", argv[i]); return 1; }
    Buf o = { 0 }; size_t pos = 0;
    while (pos < all.n) {
        size_t e = pos; while (e < all.n && all.s[e] != '\n') e++;
        char *line = all.s + pos; all.s[e] = 0; pos = e + 1;
        if (cl) { for (long k = 0; line[k]; k++) if (in_list(cl, k + 1)) bputc(&o, line[k]); bputc(&o, '\n'); }
        else if (!strchr(line, delim)) { if (!only) { bputs(&o, line); bputc(&o, '\n'); } }
        else { long f = 1; int first = 1; char *p = line; for (;;) { char *q = strchr(p, delim); size_t l = q ? (size_t)(q - p) : strlen(p); if (in_list(fl, f)) { if (!first) bputc(&o, delim); bput(&o, p, l); first = 0; } if (!q) break; p = q + 1; f++; } bputc(&o, '\n'); }
    }
    if (o.s) out(o.s, o.n); free(o.s); free(all.s); return 0;
}

/* ---------------------------------------------------------- env/date/tee */
int bash_external_env(int argc, char **argv) {
    int i = 1, mark = bash_rt_tmp_mark();
    for (; i < argc && strchr(argv[i], '=') && argv[i][0] != '='; i++) { char *n = strndup(argv[i], strchr(argv[i], '=') - argv[i]); bash_rt_tmp_set(n, strchr(argv[i], '=') + 1); free(n); }
    if (i < argc) { int rc = bash_rt_run(argc - i, argv + i); bash_rt_tmp_restore(mark); return rc; }
    int n; char **e = bash_rt_exported_env(&n); for (int k = 0; k < n; k++) { outs(e[k]); outs("\n"); free(e[k]); } free(e);
    bash_rt_tmp_restore(mark); return 0;
}
int bash_external_date(int argc, char **argv) {
    int utc = 0; const char *fmt = "%a %b %e %H:%M:%S %Z %Y"; time_t t = time(NULL);
    for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "-u")) utc = 1; else if (argv[i][0] == '+') fmt = argv[i] + 1; else if (!strcmp(argv[i], "-d") && i + 1 < argc) { const char *v = argv[++i]; if (v[0] == '@') t = atol(v + 1); } }
    const char *tz = bash_rt_getvar("TZ"); if (tz) setenv("TZ", tz, 1); else unsetenv("TZ"); tzset();
    struct tm tm; if (utc) gmtime_r(&t, &tm); else localtime_r(&t, &tm);
    char buf[1024]; size_t n = strftime(buf, sizeof buf, fmt, &tm); buf[n++] = '\n'; out(buf, n); return 0;
}
int bash_external_tee(int argc, char **argv) {
    int app = 0, i = 1; for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) if (!strcmp(argv[i], "-a")) app = 1;
    int nfd = 0, fds[64], rc = 0;
    for (; i < argc && nfd < 64; i++) { int fd = open(argv[i], O_WRONLY | O_CREAT | (app ? O_APPEND : O_TRUNC), 0666); if (fd < 0) { errs("tee", argv[i]); rc = 1; } else fds[nfd++] = fd; }
    char buf[65536]; ssize_t n;
    while ((n = read(0, buf, sizeof buf)) != 0) { if (n < 0) { if (errno == EINTR) continue; break; } wr(1, buf, n); for (int k = 0; k < nfd; k++) wr(fds[k], buf, n); }
    for (int k = 0; k < nfd; k++) close(fds[k]);
    return rc;
}

int bash_external_mktemp(int argc, char **argv) {
    int dir = 0, i = 1; const char *tmpl = NULL;
    for (; i < argc; i++) { if (!strcmp(argv[i], "-d")) dir = 1; else if (argv[i][0] == '-' && argv[i][1]) continue; else tmpl = argv[i]; }
    char path[4096];
    if (tmpl && strchr(tmpl, '/')) snprintf(path, sizeof path, "%s", tmpl);
    else { const char *td = bash_rt_getvar("TMPDIR"); snprintf(path, sizeof path, "%s/%s", td && *td ? td : "/tmp", tmpl ? tmpl : "tmp.XXXXXXXXXX"); }
    if (dir) { if (!mkdtemp(path)) { errs("mktemp", path); return 1; } }
    else { int fd = mkstemp(path); if (fd < 0) { errs("mktemp", path); return 1; } close(fd); }
    outs(path); outs("\n"); return 0;
}

/* ------------------------------------------------------------------ gate
 * The embedded tools implement the common subset of each program.  For anything else (other options, GNU
 * option permutation, locale-dependent ordering, a terminal on stdout for ls) the real program is run. */
static int simple_opts(int argc, char **argv, const char *flags, const char *argflags, int exact) {
    int ops = 0, end = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (end || a[0] != '-' || !a[1]) { ops++; continue; }
        if (!strcmp(a, "--")) { end = 1; continue; }
        if (a[1] == '-' || ops) return 0;           /* long options; options after operands (GNU permutation) */
        if (exact && a[2] && !(argflags && strchr(argflags, a[1]))) return 0;
        for (const char *p = a + 1; *p; p++) {
            if (argflags && strchr(argflags, *p)) { if (!p[1]) { if (++i >= argc) return 0; } break; }
            if (!strchr(flags, *p)) return 0;
            if (exact) break;
        }
    }
    return 1;
}
static int operands(int argc, char **argv, const char *argflags) {
    int ops = 0, end = 0;
    for (int i = 1; i < argc; i++) { const char *a = argv[i];
        if (end || a[0] != '-' || !a[1]) { ops++; continue; } if (!strcmp(a, "--")) { end = 1; continue; }
        for (const char *p = a + 1; *p; p++) if (argflags && strchr(argflags, *p)) { if (!p[1]) i++; break; } }
    return ops;
}
static int is_int_str(const char *s) { if (*s == '-' || *s == '+') s++; if (!*s) return 0; for (; *s; s++) if (!isdigit((unsigned char)*s)) return 0; return 1; }
int bash_tools_gate(const char *n, int argc, char **argv) {
    switch (n[0]) {
    case 'c':
        if (!strcmp(n, "cat")) return simple_opts(argc, argv, "nu", NULL, 1);
        if (!strcmp(n, "cp")) return simple_opts(argc, argv, "rR", NULL, 0);
        if (!strcmp(n, "chmod")) { if (argc < 3) return 0; for (const char *p = argv[1]; *p; p++) if (*p < '0' || *p > '7') return 0; for (int i = 2; i < argc; i++) if (argv[i][0] == '-') return 0; return 1; }
        if (!strcmp(n, "cut")) {
            if (!simple_opts(argc, argv, "s", "dfcb", 1) || operands(argc, argv, "dfcb") > 1) return 0;
            for (int i = 1; i < argc; i++) if (argv[i][0] == '-' && argv[i][1] == 'd') { const char *v = argv[i][2] ? argv[i] + 2 : (i + 1 < argc ? argv[i + 1] : ""); if (strlen(v) != 1) return 0; }
            return 1; }
        break;
    case 'l': if (!strcmp(n, "ls")) return !isatty(1) && bash_rt_child_c_locale() && simple_opts(argc, argv, "ad1", NULL, 0) && operands(argc, argv, NULL) <= 1; break;
    case 'w':
        if (!strcmp(n, "wc")) { for (int i = 1; i < argc; i++) if (argv[i][0] == '-' && strchr(argv[i], 'm') && !bash_rt_child_c_locale()) return 0; return simple_opts(argc, argv, "lwcm", NULL, 0); }
        if (!strcmp(n, "whoami")) return argc == 1;
        break;
    case 'h': case 't':
        if (!strcmp(n, "head") || !strcmp(n, "tail")) {
            for (int i = 1; i < argc; i++) { const char *a = argv[i];
                if (a[0] != '-' || !a[1]) continue; if (!strcmp(a, "--")) break;
                if (!strcmp(a, "-n") || !strcmp(a, "-c")) { if (++i >= argc) return 0; const char *v = argv[i]; if (*v == '+' && n[0] == 't') v++; if (!is_int_str(v) || *v == '-' || *v == '+') return 0; continue; }
                if ((a[1] == 'n' || a[1] == 'c') && a[2]) { const char *v = a + 2; if (*v == '+' && n[0] == 't') v++; if (!is_int_str(v) || *v == '-' || *v == '+') return 0; continue; }
                if (isdigit((unsigned char)a[1]) && is_int_str(a + 1)) continue;
                if (!strcmp(a, "-q")) continue;
                return 0; }
            return 1; }
        if (!strcmp(n, "touch")) return simple_opts(argc, argv, "", NULL, 0);
        if (!strcmp(n, "tee")) return simple_opts(argc, argv, "a", NULL, 1);
        if (!strcmp(n, "tr")) {
            if (!simple_opts(argc, argv, "dscC", NULL, 0)) return 0;
            for (int i = 1; i < argc; i++) { const char *a = argv[i]; if (a[0] == '-' && a[1]) continue;
                if (strstr(a, "[=") || strstr(a, "*]") || strstr(a, "[.")) return 0;
                for (const char *p = a; *p; p++) if (*p == '\\' && p[1] && !strchr("ntr\\", p[1])) return 0;
                for (const char *c = strstr(a, "[:"); c; c = strstr(c + 2, "[:")) { if (strncmp(c, "[:lower:]", 9) && strncmp(c, "[:upper:]", 9) && strncmp(c, "[:digit:]", 9) && strncmp(c, "[:space:]", 9) && strncmp(c, "[:alpha:]", 9) && strncmp(c, "[:alnum:]", 9) && strncmp(c, "[:punct:]", 9)) return 0; } }
            return 1; }
        break;
    case 'm':
        if (!strcmp(n, "mkdir")) return simple_opts(argc, argv, "p", NULL, 0);
        if (!strcmp(n, "mv")) return simple_opts(argc, argv, "f", NULL, 0);
        if (!strcmp(n, "mktemp")) return simple_opts(argc, argv, "d", NULL, 1) && operands(argc, argv, NULL) <= 1;
        break;
    case 'r':
        if (!strcmp(n, "rm")) return simple_opts(argc, argv, "rRf", NULL, 0);
        if (!strcmp(n, "rmdir") || !strcmp(n, "realpath")) return simple_opts(argc, argv, "", NULL, 0);
        break;
    case 'b': if (!strcmp(n, "basename")) return simple_opts(argc, argv, "", NULL, 0) && argc >= 2 && argc <= 3; break;
    case 'd':
        if (!strcmp(n, "dirname")) return simple_opts(argc, argv, "", NULL, 0);
        if (!strcmp(n, "date")) { for (int i = 1; i < argc; i++) { const char *a = argv[i];
                if (!strcmp(a, "-u")) continue; if (a[0] == '+') continue;
                if (!strcmp(a, "-d") && i + 1 < argc && argv[i + 1][0] == '@' && is_int_str(argv[i + 1] + 1)) { i++; continue; }
                return 0; } return 1; }
        break;
    case 'g':
        if (!strcmp(n, "grep")) {
            int ne = 0; for (int i = 1; i < argc; i++) { if (!strcmp(argv[i], "--")) break; if (argv[i][0] == '-' && argv[i][1] != '-' && strchr(argv[i], 'e')) ne++; }
            if (ne > 1 || !simple_opts(argc, argv, "ivncEFqlHhwx", "e", 0)) return 0;
            for (int i = 1; i < argc; i++) if (strchr(argv[i], '\n')) return 0;
            if (!bash_rt_child_c_locale()) for (int i = 1; i < argc; i++) for (const char *p = argv[i]; *p; p++) if ((unsigned char)*p >= 0x80) return 0;
            return 1; }
        break;
    case 's':
        if (!strcmp(n, "sort")) {
            if (!bash_rt_child_c_locale()) return 0;
            int has_t = 0, has_k = 0;
            for (int i = 1; i < argc; i++) { const char *a = argv[i]; if (a[0] != '-' || !a[1]) continue; if (!strcmp(a, "--")) break;
                for (const char *p = a + 1; *p; p++) {
                    if (*p == 't') { const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : NULL); if (!v || strlen(v) != 1) return 0; has_t = 1; break; }
                    if (*p == 'k') { const char *v = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : NULL); if (!v) return 0; const char *q = v; while (isdigit((unsigned char)*q)) q++; if (q == v || (*q && strcmp(q, "n"))) return 0; has_k = 1; break; }
                    if (!strchr("rnu", *p)) return 0; } }
            if (has_k && !has_t) return 0;
            return simple_opts(argc, argv, "rnu", "kt", 0); }
        if (!strcmp(n, "seq")) { if (argc < 2 || argc > 4) return 0; for (int i = 1; i < argc; i++) if (!is_int_str(argv[i])) return 0; return 1; }
        if (!strcmp(n, "sleep")) { for (int i = 1; i < argc; i++) { char *e; double v = strtod(argv[i], &e); (void)v; if (e == argv[i] || (*e && (strchr("smhd", *e) == NULL || e[1])) || argv[i][0] == '-') return 0; if (strpbrk(argv[i], "xXnN")) return 0; } return 1; }
        break;
    case 'u': if (!strcmp(n, "uniq")) return simple_opts(argc, argv, "cdu", NULL, 0) && operands(argc, argv, NULL) <= 1; break;
    case 'e': if (!strcmp(n, "env")) { for (int i = 1; i < argc; i++) { if (argv[i][0] == '-') return 0; if (!strchr(argv[i], '=')) break; } return 1; } break;
    }
    return 1;
}
