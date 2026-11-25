/* tool_builtins.c - bash builtins: echo, printf, kill, cd, declare/local/export, set, read, mapfile, ... */
#include "tools.h"

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
        if (*p == '(') {   /* %(strftime format)T: the argument is seconds since the epoch, -1 now, -2 the shell's start */
            const char *e = strchr(p, ')');
            if (e && e[1] == 'T') {
                const char *arg = *ai < argc ? argv[(*ai)++] : NULL; if (arg) used = 1;
                long long t = arg && *arg ? arg_int(arg) : -1;
                time_t tt = t == -1 ? time(NULL) : t == -2 ? bash_rt_start_time() : (time_t)t;
                char f[256], out[1024]; snprintf(f, sizeof f, "%.*s", (int)(e - p - 1), p + 1);
                struct tm *tm = localtime(&tt);
                size_t L = tm ? strftime(out, sizeof out, *f ? f : "%X", tm) : 0; out[L] = 0;
                spec[k++] = 's'; spec[k] = 0;
                int need = snprintf(NULL, 0, spec, out); char *buf = malloc(need + 1); snprintf(buf, need + 1, spec, out); bput(o, buf, need); free(buf);
                p = e + 1;
                continue;
            }
        }
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
            if (conv == 'q') { char *q = bash_rt_printf_q(s); t.s = q; t.n = strlen(q); s = q; }
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
    long v = 0; for (int i = 1; i < argc; i++) { int e; v = bash_rt_arith_eval_nf(argv[i], &e); if (e) return 1; }
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
/* bytes already buffered (read-ahead cache, in-memory stage input, here-string, local buffer) for bulk scanning */
static size_t rd_avail(Rd *r, const char **b) {
    if (r->mem) { RtMin *m = bash_rt_min; if (!m) return 0; *b = m->s + m->p; return m->n - m->p; }
    if (r->src) { *b = r->src; return strlen(r->src); }
    if (r->cached) { *b = rdc.buf + rdc.p; return rdc.n - rdc.p; }
    *b = r->buf + r->p; return r->n - r->p;
}
static void rd_take(Rd *r, size_t k) {
    if (r->mem) bash_rt_min->p += k; else if (r->src) r->src += k; else if (r->cached) rdc.p += k; else r->p += k;
}
/* append the run of ordinary bytes at the read position (stops before delim, NUL and, unless raw, '\\') */
static void rd_bulk(Rd *r, Buf *line, int delim, int raw, long *cnt) {
    const char *b; size_t n = rd_avail(r, &b), k = 0;
    while (k < n) { unsigned char ch = (unsigned char)b[k]; if (ch == delim || ch == 0 || (ch == '\\' && !raw)) break; k++; }
    if (k) { bput(line, b, k); rd_take(r, k); *cnt += (long)k; }
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
    Buf line = { 0 }, em = { 0 }; int got_delim = 0, eof = 0; long cnt = 0;   /* em: 1 per byte that was backslash-escaped (created lazily) */
    if (nchars == 0) goto assign;
    for (;;) {
        if (nchars < 0 && !exact) rd_bulk(&r, &line, delim, raw, &cnt);
        int c = rd_get(&r);
        if (c < 0) { eof = 1; break; }
        if (!exact && c == delim) { got_delim = 1; break; }
        if (c == 0) continue;
        int escd = 0;
        if (c == '\\' && !raw) { int d = rd_get(&r); if (d < 0) { eof = 1; break; } if (d == '\n' && delim == '\n') continue; c = d; escd = 1; }
        if (escd || em.s) { while (em.n < line.n) bputc(&em, 0); bputc(&em, (char)escd); }
        bputc(&line, (char)c); cnt++;
        if (nchars > 0 && cnt >= nchars) { got_delim = 1; break; }
    }
    rd_done(&r);
assign:;
    if (r.timed_out) { free(line.s); return 142; }
    const char *s = line.s ? line.s : "";
    /* field splitting: IFS membership through a table (strchr per character dominated `while read a b c`) */
    const char *ifs = bash_rt_ifs(); unsigned char cls[256] = { 0 };   /* 1 = IFS, 3 = IFS whitespace */
    for (const unsigned char *q = (const unsigned char *)ifs; *q; q++) cls[*q] = isspace(*q) ? 3 : 1;
    while (em.s && em.n < line.n) bputc(&em, 0);
    char *copy_base = NULL;   /* set to each working copy so the escape mask can be indexed */
#define CLS(p) ((em.s && em.s[(p) - copy_base]) ? 0 : cls[(unsigned char)*(p)])
#define IFS_WS_AT(p) (CLS(p) == 3)
#define IFS_ANY_AT(p) (CLS(p) != 0)
    int nv = argc - i;
    /* bash's get_word_from_string: a word runs to the next IFS character; the delimiter after it is
     * IFS whitespace, at most one other IFS character, then IFS whitespace again */
#define SKIP_WS(p) do { while (*(p) && IFS_WS_AT(p)) (p)++; } while (0)
#define NEXT_WORD(p, st) do { (st) = (p); while (*(p) && !IFS_ANY_AT(p)) (p)++; } while (0)
#define EAT_DELIM(p) do { SKIP_WS(p); if (*(p) && IFS_ANY_AT(p) && !IFS_WS_AT(p)) (p)++; SKIP_WS(p); } while (0)
    if (exact && !aname && nv <= 1) { bash_rt_setvar(nv ? argv[i] : "REPLY", s); }
    else if (aname) {
        char *copy = strdup(s); copy_base = copy; char **f = NULL; int nf = 0, cap = 0; char *p = copy, *st;
        SKIP_WS(p);
        while (*p) {
            NEXT_WORD(p, st); char *e = p; EAT_DELIM(p); *e = 0;
            if (nf == cap) { cap = cap ? cap * 2 : 8; f = realloc(f, cap * sizeof *f); } f[nf++] = st;
        }
        bash_rt_set_array(aname, nf, f, 0); free(f); free(copy);
    } else if (nv == 0) bash_rt_setvar("REPLY", s);
    else {
        char *copy = strdup(s); copy_base = copy; char *p = copy, *st;
        SKIP_WS(p);
        for (int k = 0; k < nv - 1; k++) { NEXT_WORD(p, st); char *e = p; EAT_DELIM(p); *e = 0; bash_rt_setvar(argv[i + k], st); }
        /* last variable: one remaining word without its delimiter, otherwise the rest minus trailing IFS whitespace */
        char *rest = p; NEXT_WORD(p, st); char *e = p; EAT_DELIM(p);
        if (!*p) *e = 0;
        else { e = rest + strlen(rest); while (e > rest && IFS_WS_AT(e - 1)) e--; *e = 0; }
        bash_rt_setvar(argv[i + nv - 1], rest);
        free(copy);
    }
#undef SKIP_WS
#undef NEXT_WORD
#undef EAT_DELIM
#undef CLS
#undef IFS_WS_AT
#undef IFS_ANY_AT
    free(em.s);
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
        { long dummy = 0; size_t before = cur.n; rd_bulk(&r, &cur, delim, 1, &dummy); if (cur.n > before) have = 1; }
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

/* umask [-p] [-S] [mode]: the mode is octal, or symbolic like chmod's (u=rwx,g=rx,o= / go-w / a+r), which
 * describes the permissions that are *allowed*: the mask is its complement */
static void umask_symbolic(Buf *o, mode_t m) {
    static const char *who = "ugo";
    for (int i = 0; i < 3; i++) {
        int allow = ~m >> (6 - 3 * i) & 7;
        if (i) bputc(o, ',');
        bputc(o, who[i]); bputc(o, '=');
        if (allow & 4) bputc(o, 'r');
        if (allow & 2) bputc(o, 'w');
        if (allow & 1) bputc(o, 'x');
    }
}
static int umask_parse_symbolic(const char *s, mode_t old, mode_t *out) {
    int perm = ~old & 0777;
    while (*s) {
        int who = 0;
        for (; *s && strchr("ugoa", *s); s++) who |= *s == 'u' ? 0700 : *s == 'g' ? 0070 : *s == 'o' ? 0007 : 0777;
        if (!who) who = 0777;
        if (!*s || !strchr("+-=", *s)) { bash_rt_warn("umask: `%c': invalid symbolic mode operator", *s ? *s : ' '); return 1; }
        while (*s && strchr("+-=", *s)) {
            char op = *s++; int bits = 0;
            for (; *s && *s != ',' && !strchr("+-=", *s); s++) {
                if (*s == 'r') bits |= 0444; else if (*s == 'w') bits |= 0222; else if (*s == 'x') bits |= 0111;
                else { bash_rt_warn("umask: `%c': invalid symbolic mode character", *s); return 1; }
            }
            bits &= who;
            if (op == '+') perm |= bits; else if (op == '-') perm &= ~bits; else perm = (perm & ~who) | bits;
        }
        if (*s == ',') s++;
    }
    *out = (mode_t)(~perm & 0777);
    return 0;
}
int bash_internal_umask(int argc, char **argv) {
    int i = 1, sym = 0, reuse = 0;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'S') sym = 1; else if (*p == 'p') reuse = 1;
            else { bash_rt_warn("umask: -%c: invalid option", *p); bash_rt_flush(); { static const char u[] = "umask: usage: umask [-p] [-S] [mode]\n"; if (write(2, u, sizeof u - 1) < 0) {} } return 2; }
        }
    }
    mode_t old = umask(0); umask(old);
    if (i >= argc) {   /* print it */
        Buf o = { 0 };
        if (reuse) bputs(&o, sym ? "umask -S " : "umask ");
        if (sym) umask_symbolic(&o, old);
        else { char t[16]; snprintf(t, sizeof t, "%04o", (unsigned)old); bputs(&o, t); }
        bputc(&o, '\n'); outs(o.s); free(o.s);
        return 0;
    }
    const char *m = argv[i]; mode_t nm;
    if (isdigit((unsigned char)*m)) {
        long v = 0; const char *p = m;
        for (; *p >= '0' && *p <= '7'; p++) { v = v * 8 + (*p - '0'); if (v > 0777) break; }
        if (*p || v > 0777) { bash_rt_warn("umask: %s: octal number out of range", m); return 1; }
        nm = (mode_t)v;
    } else if (umask_parse_symbolic(m, old, &nm)) return 1;
    bash_rt_umask_changing();
    umask(nm);
    if (sym) { Buf o = { 0 }; umask_symbolic(&o, nm); bputc(&o, '\n'); outs(o.s); free(o.s); }
    return 0;
}
