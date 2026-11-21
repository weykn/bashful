/* rt_frames.c - function frames and locals, in-process subshells, $(...) capture, in-process pipelines */
#include "rt.h"

/* ------------------------------------------- locals, frames, subshells */
typedef struct { int id; Var saved; } Loc;
static Loc *locs; static int nloc, caploc;
Frame *frames; int nframes, capframes;
Sub *cur_sub; unsigned gen_counter;
pid_t g_last_bg, g_shell_pid;

void pos_release_block(void) { if (!(cur_sub && pos_block == cur_sub->sblock)) free(pos_block); pos_block = NULL; }
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
int g_is_child;
bash_body_t trap_fn[65]; int trap_mode[65];   /* 0 default, 1 ignore, 2 run */
void bash_rt_exit(int code) {
    if (cur_sub) { cur_sub->exit_code = code & 255; _longjmp(cur_sub->jb, 1); }
    if (!g_is_child && trap_mode[0] == 2) { bash_body_t f = trap_fn[0]; trap_mode[0] = 0; g_status = code; f(); }
    bash_rt_flush();
    bash_rt_fd_sync();          /* leave the shared input offset where bash would */
    if (g_is_child) _exit(code & 255);
    exit(code & 255);
}

/* top-level source line of a script: a fatal expansion error outside any subshell abandons the rest of the
 * line with status 1 and the script continues with the next line (what non-interactive bash does) */
static jmp_buf *line_jb;
int bash_rt_line(bash_body_t body) {
    jmp_buf jb, *prev = line_jb; volatile int st;
    line_jb = &jb;
    if (_setjmp(jb) == 0) st = body();
    else {
        while (nframes > 0) frame_pop();
        while (nrs > 0) bash_rt_redir_pop();
        bash_rt_tmp_restore(0);
        st = 1; g_status = 1;
    }
    line_jb = prev; return st;
}
void bash_rt_expansion_error(void) {
    if (!cur_sub && line_jb && !g_is_child) _longjmp(*line_jb, 1);
    bash_rt_exit(1);
}
void bash_rt_cap_materialize(void) {   /* the capture needs a real fd 1 from here on */
    Cap *c = g_cap; if (!c || !c->sinking) return;
    c->sinking = 0; bash_rt_sinking = 0;
    bash_rt_flush();                     /* output still pending from before the capture began */
    c->memfd = mkmem(); c->saved = fd_dup_high(1); dup2(c->memfd, 1);
    if (c->n) raw_write(1, c->s, c->n);
    free(c->s); c->s = NULL; c->n = c->cap = 0;
}
char *capture(bash_body_t body, int *status) {
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
