/* rt_exec.c - command dispatch, external programs, forked pipelines, jobs, traps, bash fallback */
#include "rt.h"

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
/* bash_rt_embed is defined by the compiled script (0 with --no-embed): external tools then run the system program */
static inline int tool_usable(int i) { return !tool_table[i].ext || bash_rt_embed; }
static int tool_is_external(int i) { return tool_table[i].ext; }
static int run_impl(int argc, char **argv, int fns) {
    if (argc == 0) return g_cs_status;
    if (fns) for (int i = 0; i < nfuncs; i++) if (!strcmp(funcs[i].name, argv[0])) return funcs[i].fn(argc, argv);
    for (int i = 0; tool_table[i].name; i++) if (!strcmp(tool_table[i].name, argv[0]) && tool_usable(i)) {
        if (tool_is_external(i)) return bash_rt_tool_call(i, tool_table[i].fn, argc, argv);
        return tool_table[i].fn(argc, argv);
    }
    return bash_rt_exec_external(argc, argv);
}
int bash_rt_run(int argc, char **argv) { return run_impl(argc, argv, 1); }
int bash_rt_run_nofunc(int argc, char **argv) { return run_impl(argc, argv, 0); }
int bash_rt_kind(const char *name) {
    for (int i = 0; i < nfuncs; i++) if (!strcmp(funcs[i].name, name)) return 1;
    for (int i = 0; tool_table[i].name; i++) if (!strcmp(tool_table[i].name, name) && tool_usable(i)) return tool_table[i].ext ? 3 : 2;
    return 0;
}
int bash_rt_is_known(const char *name) {
    for (int i = 0; i < nfuncs; i++) if (!strcmp(funcs[i].name, name)) return 1;
    for (int i = 0; tool_table[i].name; i++) if (!strcmp(tool_table[i].name, name) && tool_usable(i)) return 1;
    return 0;
}
int bash_rt_return_status(int argc, char **argv) {
    if (argc > 1) g_status = atoi(argv[1]) & 255;
    return g_status;
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
static int g_exec_noenv;   /* exec -c: the program gets an empty environment */
static char **build_envp(void) {
    if (g_exec_noenv) { char **e = xmalloc(sizeof *e); e[0] = NULL; return e; }
    int n; char **e = bash_rt_exported_env(&n); e[n] = NULL; return e;
}
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
    if (!bash_rt_embed || !bash_rt_tool_gate(idx, argc, argv)) return bash_rt_exec_external(argc, argv);
    int r = fn(argc, argv);
    return r == BASH_TOOL_FALLBACK ? bash_rt_exec_external(argc, argv) : r;
}
/* exec builtin: replace the shell (or, inside an in-process subshell, the subshell).
 * Options like bash: -a NAME (argv[0] for the program), -l (a dash in front of argv[0]), -c (empty environment). */
int bash_rt_exec_builtin(int argc, char **argv) {
    int i = 1, login = 0, noenv = 0; const char *argv0 = NULL;
    while (i < argc && argv[i][0] == '-' && argv[i][1]) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *o = argv[i] + 1; *o; o++) {
            if (*o == 'c') noenv = 1;
            else if (*o == 'l') login = 1;
            else if (*o == 'a') {
                if (o[1]) argv0 = o + 1;
                else if (i + 1 < argc) argv0 = argv[++i];
                else { bash_rt_warn("exec: -a: option requires an argument"); return 2; }
                break;
            } else {
                bash_rt_warn("exec: -%c: invalid option", *o);
                bash_rt_warn("exec: usage: exec [-cl] [-a name] [command [argument ...]] [redirection ...]");
                return 2;
            }
        }
        i++;
    }
    if (i >= argc) return 0;
    argc -= i; argv += i;
    /* functions and builtins run in-process; so do embedded programs, unless argv[0] or the environment
     * must be what the real program sees */
    int known = bash_rt_is_known(argv[0]);
    if (known && (argv0 || login || noenv) && bash_rt_kind(argv[0]) == 3) known = 0;
    if (known) { int st = bash_rt_run(argc, argv); bash_rt_exit(st); return st; }
    char *path = bash_rt_path_search(argv[0]);
    if (!path) { bash_rt_warn("%s: not found", argv[0]); bash_rt_exit(127); return 127; }
    char **av = xmalloc((argc + 1) * sizeof *av);
    for (int k = 0; k < argc; k++) av[k] = argv[k];
    av[argc] = NULL;
    if (argv0) av[0] = (char *)argv0;
    if (login) { char *d = xmalloc(strlen(av[0]) + 2); d[0] = '-'; strcpy(d + 1, av[0]); av[0] = d; }
    bash_rt_flush();
    g_exec_noenv = noenv;
    if (cur_sub) { int st = spawn_wait(path, av); g_exec_noenv = 0; free(path); bash_rt_exit(st); return st; }
    char **envp = build_envp(); execve(path, av, envp);
    int e = errno;
    if (e == ENOEXEC) { char bash[4096]; if (find_bash(bash, sizeof bash)) { char **bv = xmalloc((argc + 2) * sizeof *bv); bv[0] = "bash"; bv[1] = path; for (int k = 1; k <= argc; k++) bv[k + 1] = av[k]; execve(bash, bv, envp); } }
    g_exec_noenv = 0;
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
void subshell_traps_end(Sub *s) {
    TrapSave *t = s->trapsave; s->trapsave = NULL;
    if (trap_mode[0] == 2 && trap_fn[0] != t->fn[0]) { bash_body_t f = trap_fn[0]; trap_mode[0] = 0; if (_setjmp(s->jb) == 0) f(); }
    memcpy(trap_mode, t->mode, sizeof trap_mode); memcpy(trap_fn, t->fn, sizeof trap_fn);
    for (int k = 1; k < 65; k++) install_sig(k);
    free(t);
}
__attribute__((noinline, cold)) void run_pending_traps(void) {
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
