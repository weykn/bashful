/* tool_term.c - the terminal primitives of the interactive shell's line editor.  The editor itself (keys,
 * history, completion, prompts) is bash, in compiler/readline.sh; these are the few things bash cannot do:
 *
 *   __bashful_term raw          raw mode (no echo, no line editing, keys like ^C arrive as bytes); status 1
 *                               when stdin or stderr is not a terminal
 *   __bashful_term restore      back to the mode `raw` found
 *   __bashful_term key [MS]     read one key press: KEY = the character (a whole UTF-8 character), KEYN = the
 *                               value of its first byte; status 1 when nothing came within MS milliseconds,
 *                               2 at end of input
 *   __bashful_term cols         REPLY = the terminal's width
 *   __bashful_term width S [C]  REPLY = the columns S takes on screen (wide characters take 2); a control
 *                               character counts C (default 2, as the editor shows it as ^X)
 */
#include "tools.h"
#include <termios.h>
#include <sys/ioctl.h>

static struct termios saved;
static int have_saved;

static int rdbyte(int ms) {   /* a byte; -1 timeout, -2 end of input */
    if (ms >= 0) { struct pollfd p = { 0, POLLIN, 0 }; if (poll(&p, 1, ms) <= 0) return -1; }
    unsigned char c;
    for (;;) {
        ssize_t r = read(0, &c, 1);
        if (r == 1) return c;
        if (r < 0 && errno == EINTR) continue;
        return -2;
    }
}
static int term_key(int ms) {
    int c = rdbyte(ms);
    if (c == -1) return 1;
    if (c == -2) { bash_rt_setvar("KEY", ""); bash_rt_setvar("KEYN", "-1"); return 2; }
    char k[8]; int n = 0; k[n++] = (char)c;
    if (bash_rt_utf8() && c >= 0xC0) {   /* the rest of a UTF-8 character */
        int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
        while (more-- > 0) { int d = rdbyte(50); if (d < 0) break; k[n++] = (char)d; }
    }
    k[n] = 0;
    char num[8]; snprintf(num, sizeof num, "%d", c);
    bash_rt_setvar("KEY", c ? k : "");
    bash_rt_setvar("KEYN", num);
    return 0;
}
static int str_width(const char *s, int ctl) {
    size_t n = strlen(s); int w = 0;
    mbstate_t st; memset(&st, 0, sizeof st);
    for (size_t i = 0; i < n;) {
        unsigned char c = s[i];
        if (c < 32 || c == 127) { w += ctl; i++; continue; }
        if (c < 128 || !bash_rt_utf8()) { w++; i++; continue; }
        wchar_t wc; size_t k = mbrtowc(&wc, s + i, n - i, &st);
        if (k == (size_t)-1 || k == (size_t)-2 || k == 0) { memset(&st, 0, sizeof st); w++; i++; continue; }
        int cw = wcwidth(wc); w += cw < 0 ? 1 : cw; i += k;
    }
    return w;
}
int bash_internal_bashful_term(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *op = argv[1];
    char t[32];
    if (!strcmp(op, "key")) { long ms = -1; if (argc > 2) str_to_int(argv[2], &ms); bash_rt_flush(); return term_key((int)ms); }
    if (!strcmp(op, "width")) {
        long ctl = 2; if (argc > 3) str_to_int(argv[3], &ctl);
        snprintf(t, sizeof t, "%d", str_width(argc > 2 ? argv[2] : "", (int)ctl)); bash_rt_setvar("REPLY", t); return 0;
    }
    if (!strcmp(op, "cols")) {
        struct winsize ws; int c = ioctl(2, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 ? ws.ws_col : 80;
        snprintf(t, sizeof t, "%d", c); bash_rt_setvar("REPLY", t); return 0;
    }
    if (!strcmp(op, "raw")) {
        bash_rt_flush();
        if (!isatty(0) || !isatty(2) || tcgetattr(0, &saved) != 0) return 1;
        have_saved = 1;
        struct termios raw = saved;
        raw.c_iflag &= ~(unsigned)(IXON | ICRNL | INLCR | ISTRIP);
        raw.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG | IEXTEN);
        raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0;
        tcsetattr(0, TCSADRAIN, &raw);
        return 0;
    }
    if (!strcmp(op, "restore")) { bash_rt_flush(); if (have_saved) tcsetattr(0, TCSADRAIN, &saved); return 0; }
    return 2;
}
