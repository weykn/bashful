/* tools.h - helpers shared by the embedded commands (tool_*.c).  bash_internal_* are bash builtins;
 * bash_external_* behave like external programs but run inside the host process: bash_external_cat() reads
 * and writes fds directly, bash_internal_kill() calls kill(2), etc.  Each embedded program implements the
 * common subset of its options; tool_gate.c decides per call whether the real program must run instead. */
#ifndef TOOLS_H
#define TOOLS_H
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
static inline void bput(Buf *b, const char *s, size_t n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2; b->s = realloc(b->s, b->cap); }
    memcpy(b->s + b->n, s, n); b->n += n; b->s[b->n] = 0;
}
static inline void bputs(Buf *b, const char *s) { bput(b, s, strlen(s)); }
static inline void bputc(Buf *b, char c) { bput(b, &c, 1); }
static inline void wr(int fd, const char *s, size_t n) { if (fd == 1) { bash_rt_out(s, n); return; } if (fd == 2) bash_rt_flush(); while (n) { ssize_t w = write(fd, s, n); if (w < 0) { if (errno == EINTR) continue; return; } s += w; n -= w; } }
static inline void out(const char *s, size_t n) { wr(1, s, n); }
static inline void outs(const char *s) { out(s, strlen(s)); }
static inline void errs(const char *name, const char *what) { bash_rt_warn("%s: %s: %s", name, what, strerror(errno)); }
static inline int slurp(const char *path, Buf *b) {     /* "-" or NULL = stdin */
    int fd = (!path || !strcmp(path, "-")) ? 0 : open(path, O_RDONLY);
    if (fd < 0) return -1;
    char buf[65536]; ssize_t n;
    for (;;) { n = read(fd, buf, sizeof buf); if (n < 0 && errno == EINTR) continue; if (n <= 0) break; bput(b, buf, n); }
    int e = n < 0 ? errno : 0; if (fd) close(fd); if (e) { errno = e; return -1; }
    if (!b->s) b->s = calloc(1, 1);
    return 0;
}
static inline int is_dir(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISDIR(st.st_mode); }
static inline int str_to_int(const char *s, long *v) { char *e; errno = 0; *v = strtol(s, &e, 10); return *s && !*e && !errno; }

#endif
