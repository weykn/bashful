/* rt_io.c - buffered stdout, diagnostics, redirections */
#include "rt.h"

/* ------------------------------------------------- buffered stdout */
char obuf[1 << 16]; size_t on; int o_line;
void raw_write(int fd, const char *s, size_t n) {
    while (n) { ssize_t w = write(fd, s, n); if (w < 0) { if (errno == EINTR) continue; return; } s += w; n -= w; }
}
Cap *g_cap; int bash_rt_sinking;
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

const char *g_argv0 = "bash";
/* the line of the command running now (the compiler stores it before each command): $LINENO, and bash's
 * "script: line N: " in messages, which an interactive shell leaves out */
int g_lineno;
void bash_rt_lineno(int line) { g_lineno = line; }
void bash_rt_warn(const char *fmt, ...) {
    char buf[1024]; int n;
    if (g_lineno > 0 && !g_interactive) n = snprintf(buf, sizeof buf, "%s: line %d: ", g_argv0, g_lineno);
    else n = snprintf(buf, sizeof buf, "%s: ", g_argv0);
    va_list ap; va_start(ap, fmt); n += vsnprintf(buf + n, sizeof buf - n - 1, fmt, ap); va_end(ap);
    if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
    buf[n++] = '\n'; bash_rt_flush(); raw_write(2, buf, n);
}

/* ------------------------------------------------------- fds & capture */
int fd_dup_high(int fd) { return fcntl(fd, F_DUPFD_CLOEXEC, 100); }
int mkmem(void) {
    int fd = memfd_create("bashrt", MFD_CLOEXEC);
    if (fd >= 0) return fd;
    char t[] = "/tmp/bashrtXXXXXX"; fd = mkstemp(t); if (fd >= 0) unlink(t); return fd;
}
void write_all(int fd, const char *s, size_t n) { raw_write(fd, s, n); }

typedef struct { int fd, saved; } RS;
static RS *rstack; static int caprs; int nrs, redir_err;
static void rs_push(int fd, int saved) {
    if (nrs == caprs) { caprs = caprs ? caprs * 2 : 32; rstack = xrealloc(rstack, caprs * sizeof *rstack); }
    rstack[nrs].fd = fd; rstack[nrs].saved = saved; nrs++;
}
void bash_rt_redir_push(void) { rs_push(-1, -1); redir_err = 0; }
void raw_lseek_set(int fd) { lseek(fd, 0, SEEK_SET); }
void redir_save(int fd) { if (fd == 1) RT_NEED_FD1(); if (fd == 0) RT_NEED_FD0(); if (fd == 1 || fd == 2) bash_rt_flush(); rs_push(fd, fd_dup_high(fd)); }
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
char *slurp_fd(int fd) {
    SB b = { 0 }; char buf[4096]; lseek(fd, 0, SEEK_SET);
    for (;;) { ssize_t n = read(fd, buf, sizeof buf); if (n < 0 && errno == EINTR) continue; if (n <= 0) break; sb_putn(&b, buf, n); }
    return sb_take(&b);
}
