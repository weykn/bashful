/* rt_time.c - the time keyword: bash_rt_time_start notes the wall clock and the CPU times (this process and its
 * children, like bash), the pipeline runs as usual, bash_rt_time_end prints the report to stderr in bash's
 * format: TIMEFORMAT when it is set (empty: no report), else "\nreal\t%3lR\nuser\t%3lU\nsys\t%3lS", or the POSIX
 * "real %2R\nuser %2U\nsys %2S" for time -p. */
#include "rt.h"
#include <sys/time.h>
#include <sys/resource.h>
#include <stdarg.h>

typedef struct { struct timeval real, user, sys; } TimeMark;
static TimeMark *marks; static int nmarks, capmarks;

static struct timeval tv_add(struct timeval a, struct timeval b) {
    a.tv_sec += b.tv_sec; a.tv_usec += b.tv_usec;
    if (a.tv_usec >= 1000000) { a.tv_sec++; a.tv_usec -= 1000000; }
    return a;
}
/* the report, built in a fixed buffer (a longer TIMEFORMAT is cut off) */
typedef struct { char s[4096]; size_t n; } Out;
static void oput(Out *o, const char *fmt, ...) {
    if (o->n >= sizeof o->s - 1) return;
    va_list ap; va_start(ap, fmt);
    int k = vsnprintf(o->s + o->n, sizeof o->s - o->n, fmt, ap);
    va_end(ap);
    if (k > 0) o->n += (size_t)k < sizeof o->s - o->n ? (size_t)k : sizeof o->s - 1 - o->n;
}
static long long tv_us(struct timeval t) { return (long long)t.tv_sec * 1000000 + t.tv_usec; }
static void mark_now(TimeMark *m) {
    struct rusage self, kids;
    gettimeofday(&m->real, NULL);
    getrusage(RUSAGE_SELF, &self);
    getrusage(RUSAGE_CHILDREN, &kids);
    m->user = tv_add(self.ru_utime, kids.ru_utime);
    m->sys = tv_add(self.ru_stime, kids.ru_stime);
}
int bash_rt_time_start(void) {
    if (nmarks == capmarks) { capmarks = capmarks ? capmarks * 2 : 8; marks = xrealloc(marks, capmarks * sizeof *marks); }
    mark_now(&marks[nmarks]);
    return nmarks++;
}
/* bash's mkfmt: seconds, `prec` fraction digits (0-6, truncated, not rounded), "MmS.FFFs" with l */
static void put_time(Out *o, long long us, int prec, int lng) {
    if (us < 0) us = 0;
    long long sec = us / 1000000; long frac = (long)(us % 1000000);
    if (lng) { oput(o, "%lldm", sec / 60); sec %= 60; }
    oput(o, "%lld", sec);
    if (prec > 0) {
        static const int div[] = { 0, 100000, 10000, 1000, 100, 10, 1 };
        oput(o, ".");
        for (int i = 1; i <= prec; i++) { oput(o, "%c", (char)('0' + frac / div[i])); frac %= div[i]; }
    }
    if (lng) oput(o, "s");
}
void bash_rt_time_end(int slot, int posix) {
    if (slot < 0 || slot >= nmarks) return;
    int st = bash_rt_get_status();
    TimeMark now, start = marks[slot];
    nmarks = slot;   /* also drops marks left behind by a jump out of an inner time */
    mark_now(&now);
    long long real = tv_us(now.real) - tv_us(start.real);
    long long user = tv_us(now.user) - tv_us(start.user);
    long long sys = tv_us(now.sys) - tv_us(start.sys);
    const char *fmt = posix ? "real %2R\nuser %2U\nsys %2S" : bash_rt_getvar("TIMEFORMAT");
    if (!fmt) fmt = "\nreal\t%3lR\nuser\t%3lU\nsys\t%3lS";
    if (!*fmt) return;
    static Out o; o.n = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { oput(&o, "%c", *p); continue; }
        if (!p[1]) { oput(&o, "%%"); break; }
        p++;
        if (*p == '%') { oput(&o, "%%"); continue; }
        if (*p == 'P') {   /* CPU percentage, two decimals, at most 100 */
            long long cpu = real > 0 ? (user + sys) * 10000 / real : 0;
            if (cpu > 10000) cpu = 10000;
            oput(&o, "%lld.%02lld", cpu / 100, cpu % 100);
            continue;
        }
        const char *q = p; int prec = 3, lng = 0;
        if (*q >= '0' && *q <= '9') { prec = *q - '0'; if (prec > 6) prec = 6; q++; }
        if (*q == 'l') { lng = 1; q++; }
        if (*q == 'R' || *q == 'U' || *q == 'S') {
            put_time(&o, *q == 'R' ? real : *q == 'U' ? user : sys, prec, lng);
            p = q;
        } else {
            bash_rt_warn("TIMEFORMAT: `%c': invalid format character", *q ? *q : '%');
            bash_rt_set_status(st);
            return;   /* like bash: no report at all */
        }
    }
    oput(&o, "\n");
    bash_rt_flush();
    raw_write(2, o.s, o.n);
    bash_rt_set_status(st);
}
