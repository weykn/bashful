/* tool_files.c - embedded system programs: sleep, ls, mkdir, rm, cp, mv, ..., env, date, tee, mktemp */
#include "tools.h"

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
