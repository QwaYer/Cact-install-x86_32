#include "cact_install.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>

static struct termios saved_term;
static int term_saved;
static int canonical_ok;

void ci_term_init(void) {
    canonical_ok = 0;
    if (tcgetattr(STDIN_FILENO, &saved_term) == 0) {
        struct termios t = saved_term;
        t.c_lflag |= (ICANON | ECHO);
        t.c_lflag &= ~(unsigned int)ISIG;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0) {
            term_saved = 1;
            canonical_ok = 1;
        }
    }
}

void ci_term_done(void) {
    if (term_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_term);
        term_saved = 0;
    }
}

static void out(const char *s) {
    write(STDOUT_FILENO, s, strlen(s));
}

static void trim_cr(char *s) {
    int n = (int)strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r'))
        s[--n] = '\0';
}

static int read_line_canonical(const char *prompt, char *buf, int size) {
    if (prompt)
        out(prompt);
    if (!fgets(buf, size, stdin))
        return -1;
    trim_cr(buf);
    return (int)strlen(buf);
}

static int read_line_raw(const char *prompt, char *buf, int size) {
    int n = 0;
    for (;;) {
        char c;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r <= 0) {
            if (n == 0)
                return -1;
            break;
        }
        if (c == '\n' || c == '\r') {
            out("\n");
            break;
        }
        if (c == '\b' || c == 0x7f) {
            if (n > 0 && prompt) {
                n--;
                out("\r");
                out(prompt);
                if (n)
                    write(STDOUT_FILENO, buf, (size_t)n);
                out("\x1b[K");
            }
            continue;
        }
        if ((unsigned char)c < 0x20)
            continue;
        if (n < size - 1) {
            buf[n++] = c;
            write(STDOUT_FILENO, &c, 1);
        }
    }
    buf[n] = '\0';
    return n;
}

static int read_line(const char *prompt, char *buf, int size) {
    if (canonical_ok)
        return read_line_canonical(prompt, buf, size);
    return read_line_raw(prompt, buf, size);
}

int ci_prompt(const char *q, char *buf, int size) {
    if (read_line(q, buf, size) < 0)
        return 0;
    return 1;
}

long ci_prompt_num(const char *q, long def) {
    char buf[32];
    char label[96];
    snprintf(label, sizeof(label), "%s [%ld] ", q, def);
    if (read_line(label, buf, sizeof(buf)) < 0)
        return def;
    if (!buf[0])
        return def;
    char *end;
    long v = strtol(buf, &end, 10);
    if (end == buf)
        return def;
    return v;
}

int ci_menu(const char *title, const char **items, int n) {
    if (title && title[0])
        out(title);
    for (int i = 0; i < n; i++) {
        char line[128];
        snprintf(line, sizeof(line), "  %d) %s\n", i + 1, items[i]);
        out(line);
    }
    for (;;) {
        char buf[32];
        char q[48];
        snprintf(q, sizeof(q), "Choice [1-%d, 0 = back]: ", n);
        if (read_line(q, buf, sizeof(buf)) < 0)
            return 0;
        if (!buf[0])
            continue;
        char *end;
        long v = strtol(buf, &end, 10);
        if (end == buf)
            continue;
        if (v == 0)
            return 0;
        if (v >= 1 && v <= n)
            return (int)v;
    }
}

int ci_confirm(const char *q, int def) {
    for (;;) {
        char label[256];
        snprintf(label, sizeof(label), "%s %s", q, def ? "[Y/n] " : "[y/N] ");
        char buf[16];
        if (read_line(label, buf, sizeof(buf)) < 0)
            return def;
        if (!buf[0])
            return def;
        if (buf[0] == 'y' || buf[0] == 'Y')
            return 1;
        if (buf[0] == 'n' || buf[0] == 'N')
            return 0;
    }
}

void ci_press(void) {
    char buf[8];
    read_line("Press Enter to continue...", buf, sizeof(buf));
}
