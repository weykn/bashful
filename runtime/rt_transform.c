/* rt_transform.c - the string side of ${x@op}: bash's quoting (@Q, @A, @K) and backslash-escape decoding (@E) */
#include "rt.h"

/* length of the valid UTF-8 character at s (2-4 bytes), 0 if it is not one */
static int u8_char(const unsigned char *s) {
    int n = s[0] >= 0xf0 && s[0] < 0xf5 ? 4 : s[0] >= 0xe0 ? 3 : s[0] >= 0xc2 && s[0] < 0xe0 ? 2 : 0;
    if (s[0] >= 0xf8) return 0;
    for (int i = 1; i < n; i++) if ((s[i] & 0xc0) != 0x80) return 0;
    return n;
}
/* bash's ansic_shouldquote: a byte that is not printable (a valid character in a UTF-8 locale is) */
static int needs_ansic(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p >= 0x80 && g_utf8) { int n = u8_char(p); if (n) { p += n - 1; continue; } return 1; }
        if (*p < 0x20 || *p >= 0x7f) return 1;
    }
    return 0;
}
static void ansic(SB *o, const char *s) {   /* $'...' */
    sb_puts(o, "$'");
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        int c = *p; char t[8];
        switch (c) {
        case '\a': sb_puts(o, "\\a"); continue;
        case '\b': sb_puts(o, "\\b"); continue;
        case '\f': sb_puts(o, "\\f"); continue;
        case '\n': sb_puts(o, "\\n"); continue;
        case '\r': sb_puts(o, "\\r"); continue;
        case '\t': sb_puts(o, "\\t"); continue;
        case '\v': sb_puts(o, "\\v"); continue;
        case 27: sb_puts(o, "\\E"); continue;
        case '\\': sb_puts(o, "\\\\"); continue;
        case '\'': sb_puts(o, "\\'"); continue;
        }
        if (c >= 0x80 && g_utf8) { int n = u8_char(p); if (n) { sb_putn(o, (const char *)p, n); p += n - 1; continue; } }
        if (c < 0x20 || c >= 0x7f) { snprintf(t, sizeof t, "\\%03o", c); sb_puts(o, t); }
        else sb_putc(o, (char)c);
    }
    sb_putc(o, '\'');
}
/* @Q: 'single quoted' (' as '\''), or $'...' when the string has unprintable characters */
void rt_quote_q(SB *o, const char *s) {
    if (needs_ansic(s)) { ansic(o, s); return; }
    sb_putc(o, '\'');
    for (; *s; s++) if (*s == '\'') sb_puts(o, "'\\''"); else sb_putc(o, *s);
    sb_putc(o, '\'');
}
/* printf %q: backslashes before the characters the shell treats specially (~ and # only at the start), ''
 * for the empty string, $'...' when there are unprintable characters; a malloc'd string */
char *bash_rt_printf_q(const char *s) {
    SB o = { 0 };
    if (!*s) sb_puts(&o, "''");
    else if (needs_ansic(s)) ansic(&o, s);
    else for (const char *p = s; *p; p++) {
        if (strchr(" !\"$&'()*,;<>?[\\]^`{|}", *p) || (p == s && (*p == '~' || *p == '#'))) sb_putc(&o, '\\');
        sb_putc(&o, *p);
    }
    return sb_take(&o);
}
/* the values in @A / @K of arrays: "double quoted" (\ " $ ` escaped), or $'...' */
void rt_quote_dq(SB *o, const char *s) {
    if (needs_ansic(s)) { ansic(o, s); return; }
    sb_putc(o, '"');
    for (; *s; s++) { if (strchr("\\\"$`", *s)) sb_putc(o, '\\'); sb_putc(o, *s); }
    sb_putc(o, '"');
}
/* an associative array's key in @A / @K: as it is, double quoted when it has shell metacharacters */
void rt_quote_key(SB *o, const char *s) {
    if (!*s || strpbrk(s, " \t\n'\"\\|&;()<>!{}*[?]^$`~#=")) rt_quote_dq(o, s); else sb_puts(o, s);
}
static void put_utf8(SB *o, unsigned long c) {
    char t[4]; int n;
    if (c < 0x80) { t[0] = (char)c; n = 1; }
    else if (c < 0x800) { t[0] = (char)(0xc0 | c >> 6); t[1] = (char)(0x80 | (c & 0x3f)); n = 2; }
    else if (c < 0x10000) { t[0] = (char)(0xe0 | c >> 12); t[1] = (char)(0x80 | (c >> 6 & 0x3f)); t[2] = (char)(0x80 | (c & 0x3f)); n = 3; }
    else { t[0] = (char)(0xf0 | c >> 18); t[1] = (char)(0x80 | (c >> 12 & 0x3f)); t[2] = (char)(0x80 | (c >> 6 & 0x3f)); t[3] = (char)(0x80 | (c & 0x3f)); n = 4; }
    sb_putn(o, t, n);
}
static int hexval(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
/* @E: the escapes of $'...' */
char *rt_ansic_decode(const char *s) {
    SB o = { 0 };
    for (; *s; s++) {
        if (*s != '\\' || !s[1]) { sb_putc(&o, *s); continue; }
        char c = *++s; unsigned long v; int k, h;
        switch (c) {
        case 'a': sb_putc(&o, '\a'); break;
        case 'b': sb_putc(&o, '\b'); break;
        case 'e': case 'E': sb_putc(&o, 27); break;
        case 'f': sb_putc(&o, '\f'); break;
        case 'n': sb_putc(&o, '\n'); break;
        case 'r': sb_putc(&o, '\r'); break;
        case 't': sb_putc(&o, '\t'); break;
        case 'v': sb_putc(&o, '\v'); break;
        case '\\': case '\'': case '"': case '?': sb_putc(&o, c); break;
        case 'c':
            if (s[1]) { s++; sb_putc(&o, (char)(*s == '?' ? 127 : toupper((unsigned char)*s) & 0x1f)); }
            else sb_puts(&o, "\\c");
            break;
        case 'x': case 'u': case 'U':
            k = c == 'x' ? 2 : c == 'u' ? 4 : 8; v = 0;
            int got = 0;
            while (got < k && (h = hexval((unsigned char)s[1])) >= 0) { v = v * 16 + h; s++; got++; }
            if (!got) { sb_putc(&o, '\\'); sb_putc(&o, c); break; }
            if (c == 'x') sb_putc(&o, (char)v); else put_utf8(&o, v);
            break;
        default:
            if (c >= '0' && c <= '7') {
                v = c - '0';
                for (k = 1; k < 3 && s[1] >= '0' && s[1] <= '7'; k++) v = v * 8 + (*++s - '0');
                sb_putc(&o, (char)v);
            } else { sb_putc(&o, '\\'); sb_putc(&o, c); }
        }
    }
    return sb_take(&o);
}
