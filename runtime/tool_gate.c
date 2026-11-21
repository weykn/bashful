#include "tools.h"


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
