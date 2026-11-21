/* tool_text.c - embedded text programs: cat, wc, head, tail, grep, sort, uniq, seq, tr, cut */
#include "tools.h"


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
/* numeric key like GNU sort -n in the C locale: blanks, optional '-', digits, optional '.' and digits.
 * No exponents, hex, "inf" or '+' (strtod would accept those). */
static double num_key(const char *p) {
    while (*p == ' ' || *p == '\t') p++;
    int neg = *p == '-'; if (neg) p++;
    double v = 0; while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
    if (*p == '.') { double sc = 0.1; for (p++; *p >= '0' && *p <= '9'; p++, sc /= 10) v += (*p - '0') * sc; }
    return neg ? -v : v;
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
    if (s_num) { if (x->k != y->k) { int r = x->k < y->k ? -1 : 1; return s_rev ? -r : r; } int r = strcmp(x->s, y->s); return s_rev ? -r : r; }
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
        all.s[e] = 0; lines[n].s = all.s + pos; if (s_num) lines[n].k = num_key(field_of(lines[n].s));
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
    Buf b = { 0 }; slurp(NULL, &b);
    unsigned char m8[256]; for (int c = 0; c < 256; c++) m8[c] = (unsigned char)map[c];
    if (!del && !sq) { for (size_t k = 0; k < b.n; k++) b.s[k] = (char)m8[(unsigned char)b.s[k]]; if (b.n) out(b.s, b.n); free(b.s); return 0; }
    size_t w = 0; int last = -1;   /* output never grows: filter in place */
    for (size_t k = 0; k < b.n; k++) {
        unsigned char c = b.s[k];
        if (del && inset[c]) continue;
        int r = inset[c] && !del ? m8[c] : c;
        if (sq && sqset[r] && last == r) continue;
        b.s[w++] = (char)r; last = r;
    }
    if (w) out(b.s, w); free(b.s); return 0;
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
