/* rt_fd.h - runtime-private: the read builtin keeps a read-ahead buffer on one seekable fd across calls
 * (a `while read` loop costs one read(2) per 64 KiB instead of read+lseek per line).  The kernel offset is
 * then ahead of the logical one, so anything that could observe that fd's offset first calls
 * bash_rt_fd_sync(), which seeks back.  These wrappers catch every such point in the runtime. */
#ifndef RT_FD_H
#define RT_FD_H
#include <unistd.h>
#include <spawn.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
extern int bash_rt_rdc_fd;          /* fd with buffered read-ahead, -1 when none */
void bash_rt_fd_sync(void);
/* $(...) output is first collected in memory; anything that needs a real fd 1 turns it into a memfd first */
extern int bash_rt_sinking;
void bash_rt_cap_materialize(void);
#define RT_NEED_FD1() do { if (bash_rt_sinking) bash_rt_cap_materialize(); } while (0)
/* in-process pipeline stages read the previous stage's output from memory; same idea for fd 0 */
typedef struct { const char *s; size_t n, p; } RtMin;
extern RtMin *bash_rt_min;
void bash_rt_min_materialize(void);
#define RT_NEED_FD0() do { if (bash_rt_min) bash_rt_min_materialize(); } while (0)
#define RT_NEED_FDS() do { RT_NEED_FD1(); RT_NEED_FD0(); } while (0)
static inline int rt_dup2_(int a, int b) { if (a == 1 || b == 1) RT_NEED_FD1(); if (a == 0 || b == 0) RT_NEED_FD0(); if (b == bash_rt_rdc_fd || a == bash_rt_rdc_fd) bash_rt_fd_sync(); return dup2(a, b); }
static inline int rt_close_(int a) { if (a == 1) RT_NEED_FD1(); if (a == 0) RT_NEED_FD0(); if (a == bash_rt_rdc_fd) bash_rt_fd_sync(); return close(a); }
static inline pid_t rt_fork_(void) { RT_NEED_FDS(); if (bash_rt_rdc_fd >= 0) bash_rt_fd_sync(); return fork(); }
static inline int rt_spawn_(pid_t *p, const char *path, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at, char *const av[], char *const ev[]) {
    RT_NEED_FDS(); if (bash_rt_rdc_fd >= 0) bash_rt_fd_sync(); return posix_spawn(p, path, fa, at, av, ev); }
static inline int rt_execve_(const char *path, char *const av[], char *const ev[]) { RT_NEED_FDS(); if (bash_rt_rdc_fd >= 0) bash_rt_fd_sync(); return execve(path, av, ev); }
static inline int rt_isatty_(int fd) { if ((fd == 1 && bash_rt_sinking) || (fd == 0 && bash_rt_min)) return 0; return isatty(fd); }
static inline ssize_t rt_read_(int fd, void *b, size_t n) {
    if (fd == 0 && bash_rt_min) { RtMin *m = bash_rt_min; size_t k = m->n - m->p; if (k > n) k = n; memcpy(b, m->s + m->p, k); m->p += k; return (ssize_t)k; }
    return read(fd, b, n); }
static inline off_t rt_lseek_(int fd, off_t o, int wh) {   /* stage input behaves like the pipe it stands for */
    if (fd == 0 && bash_rt_min) { errno = ESPIPE; return -1; }
    return lseek(fd, o, wh); }
static inline int rt_poll0_(int fd) { return fd == 0 && bash_rt_min; }   /* data is always ready */
#define read rt_read_
#define lseek rt_lseek_
#define isatty rt_isatty_
#define dup2 rt_dup2_
#define close rt_close_
#define fork rt_fork_
#define posix_spawn rt_spawn_
#define execve rt_execve_
#endif
