#include "tui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>          /* TIOCGWINSZ, struct winsize */

/* Frame rows.  The body is what a widget gets to draw in. */
#define R_HEADER   1
#define R_SEP1     2
#define R_NOTE     3
#define R_BODY     4
#define R_HINT(t)  ((t)->rows - 1)

#define TASK_TEXT 64
#define TASK_NOTE 96

struct tui {
    int rows, cols;
    int in, out;
    int in_owned, out_owned;      /* /dev/tty opened by us (vs. inherited fd) */

    struct termios saved;
    int saved_ok;

    struct tw_grid g;
    char          *emit;
    int            emit_cap;

    unsigned char  pend[16];
    int            pend_n;

    /* tui_tasks_* state. */
    const char   **task_names;
    unsigned char *task_state;
    char          *task_text;     /* task_n * TASK_TEXT */
    int            task_n;
    char           task_note[3][TASK_NOTE];
    const char    *task_title, *task_step, *task_hint;
};

/* ── terminal plumbing ──────────────────────────────────────────────────── */

static int open_tty(struct tui *t) {
    t->in = open("/dev/tty", O_RDWR);
    if (t->in >= 0) {
        t->out = t->in;
        t->in_owned = t->out_owned = 1;
        return 0;
    }
    t->in = STDIN_FILENO;
    t->out = STDOUT_FILENO;
    t->in_owned = t->out_owned = 0;
    return 0;
}

struct tui *tui_open(void) {
    struct tui *t = (struct tui *)malloc(sizeof(*t));
    if (!t) return NULL;
    memset(t, 0, sizeof(*t));

    open_tty(t);

    if (tcgetattr(t->in, &t->saved) == 0) {
        struct termios raw = t->saved;
        /* Byte-at-a-time input with no echo, and Ctrl-C reaches us as 0x03
         * instead of killing the installer mid-write. */
        raw.c_lflag &= ~(unsigned)(ICANON | ECHO | ISIG);
        raw.c_iflag = 0;
        raw.c_oflag = 0;
        if (tcsetattr(t->in, TCSANOW, &raw) == 0)
            t->saved_ok = 1;
    }

    struct winsize ws;
    memset(&ws, 0, sizeof(ws));
    t->rows = 25;
    t->cols = 80;
    if (ioctl(t->in, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col) {
        t->rows = ws.ws_row;
        t->cols = ws.ws_col;
    }
    if (t->rows > TW_MAX_ROWS) t->rows = TW_MAX_ROWS;
    if (t->cols > TW_MAX_COLS) t->cols = TW_MAX_COLS;
    /* Two separators, a header, a note and a hint line must all fit. */
    if (t->rows < 8) t->rows = 8;
    if (t->cols < 30) t->cols = 30;
    /* The console wraps the moment a glyph lands in the final column
     * (max_x = w - cellw, then cursor_x > max_x), and on the last line that
     * wrap calls scroll() — so a frame that used the full width slid the whole
     * screen up on every repaint.  Keep the last column empty. */
    if (t->cols > 1) t->cols--;

    t->emit_cap = t->rows * (t->cols * 2 + 40) + 64;
    t->emit = (char *)malloc((size_t)t->emit_cap);
    if (!t->emit) { free(t); return NULL; }

    tw_init(&t->g, t->rows, t->cols);
    return t;
}

void tui_close(struct tui *t) {
    if (!t) return;
    /* Leave the terminal blank so the shell prompt starts on a clean screen. */
    const char *bye = "\033[2J\033[1;1H";
    write(t->out, bye, strlen(bye));
    if (t->saved_ok)
        tcsetattr(t->in, TCSANOW, &t->saved);
    if (t->in_owned && t->in >= 0) close(t->in);
    else if (t->out_owned && t->out >= 0) close(t->out);
    free(t->task_state);
    free(t->task_text);
    free(t->emit);
    free(t);
}

int tui_rows(const struct tui *t) { return t->rows; }
int tui_cols(const struct tui *t) { return t->cols; }
int tui_body_top(const struct tui *t) { (void)t; return R_BODY; }
int tui_body_height(const struct tui *t) { return t->rows - 6; }
struct tw_grid *tui_grid(struct tui *t) { return &t->g; }

static void wr(struct tui *t, const char *p, int n) {
    int off = 0;
    while (off < n) {
        ssize_t w = write(t->out, p + off, (size_t)(n - off));
        if (w <= 0) return;
        off += (int)w;
    }
}

/* The CactOS tty renders output in fixed 255-byte chunks and its escape parser
 * state does not survive a chunk boundary, so a sequence must fit inside one
 * write() and a longer stream may only be cut between sequences. */
#define WRITE_MAX 240

static int u2a(char *b, unsigned v) {
    char tmp[12];
    int n = 0, k = 0;
    if (v == 0) { b[0] = '0'; return 1; }
    while (v && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) b[k++] = tmp[--n];
    return k;
}

void tui_flush(struct tui *t, int cur_r, int cur_c) {
    int n = tw_emit(&t->g, t->emit, t->emit_cap);
    for (int off = 0; off < n; ) {
        int c = tw_chunk(t->emit + off, n - off, WRITE_MAX);
        wr(t, t->emit + off, c);
        off += c;
    }

    if (cur_r < 0 || cur_c < 0) {
        cur_r = t->rows - 1;
        cur_c = t->cols - 2;
    }
    /* One write, one complete sequence: emitting the pieces separately is what
     * printed "5;79H" on the screen on every repaint. */
    char cup[24];
    int k = 0;
    cup[k++] = '\033';
    cup[k++] = '[';
    k += u2a(cup + k, (unsigned)(cur_r + 1));
    cup[k++] = ';';
    k += u2a(cup + k, (unsigned)(cur_c + 1));
    cup[k++] = 'H';
    wr(t, cup, k);
}

int tui_key(struct tui *t, struct tw_event *ev) {
    for (;;) {
        struct tw_event e;
        int used = tw_decode(t->pend, t->pend_n, &e);
        if (used > 0) {
            int rest = t->pend_n - used;
            if (rest > 0)
                memmove(t->pend, t->pend + used, (size_t)rest);
            t->pend_n = rest;
            if (e.key == TWK_NONE)
                continue;                 /* an unassigned byte: keep reading */
            *ev = e;
            return 0;
        }
        if (t->pend_n >= (int)sizeof(t->pend)) {
            t->pend_n = 0;                /* never wedge on an unknown CSI */
            continue;
        }
        unsigned char c;
        ssize_t r = read(t->in, &c, 1);
        if (r != 1)
            return -1;                    /* input lost */
        t->pend[t->pend_n++] = c;
    }
}

/* ── frame chrome ───────────────────────────────────────────────────────── */

static void frame(struct tui *t, const char *title, const char *step,
                  const char *note, const char *hint) {
    struct tw_grid *g = &t->g;
    int iw = t->cols - 4;                 /* inner width, text starts at col 2 */

    tw_clear(g);
    tw_box(g, 0, 0, t->rows, t->cols);

    tw_put(g, R_SEP1, 0, '+', 0);
    tw_put(g, R_SEP1, t->cols - 1, '+', 0);
    tw_fill(g, R_SEP1, 1, t->cols - 2, '-', 0);
    tw_put(g, t->rows - 2, 0, '+', 0);
    tw_put(g, t->rows - 2, t->cols - 1, '+', 0);
    tw_fill(g, t->rows - 2, 1, t->cols - 2, '-', 0);

    if (title)
        tw_puts_clip(g, R_HEADER, 2, title, iw, TW_A_REV);
    if (step) {
        int sl = (int)strlen(step);
        int c = t->cols - 2 - sl;
        if (c > 2) tw_puts(g, R_HEADER, c, step, 0);
    }
    if (note)
        tw_puts_clip(g, R_NOTE, 2, note, iw, 0);
    if (hint)
        tw_puts_clip(g, R_HINT(t), 2, hint, iw, 0);
}

/* Shared scrolling bookkeeping: keep `sel` inside a body of `h` rows. */
static void scroll_to(int *first, int sel, int n, int h) {
    if (sel < *first) *first = sel;
    if (sel >= *first + h) *first = sel - h + 1;
    if (*first > n - h) *first = n - h;
    if (*first < 0) *first = 0;
}

static void scroll_marks(struct tui *t, int first, int h, int n) {
    if (first > 0)
        tw_put(&t->g, R_BODY, t->cols - 2, '^', 0);
    if (first + h < n)
        tw_put(&t->g, R_BODY + h - 1, t->cols - 2, 'v', 0);
}

/* ── menu ───────────────────────────────────────────────────────────────── */

int tui_menu(struct tui *t, const char *title, const char *step,
             const char *note, const char *hint,
             const char **items, int n, int *sel) {
    int h = tui_body_height(t);
    int first = 0;
    if (n <= 0) return -1;
    if (!hint) hint = "Up/Down move   Enter select   Left/q back";
    if (*sel < 0 || *sel >= n) *sel = 0;

    for (;;) {
        scroll_to(&first, *sel, n, h);
        frame(t, title, step, note, hint);
        for (int i = 0; i < h && first + i < n; i++) {
            int idx = first + i;
            char line[TW_MAX_COLS];
            int  sel_row = (idx == *sel);
            if (sel_row)
                tw_fill(&t->g, R_BODY + i, 2, t->cols - 4, ' ', TW_A_REV);
            snprintf(line, sizeof(line), "%s %s",
                     sel_row ? ">" : " ", items[idx]);
            tw_puts_clip(&t->g, R_BODY + i, 2, line, t->cols - 4,
                         sel_row ? TW_A_REV : 0);
        }
        scroll_marks(t, first, h, n);
        tui_flush(t, -1, -1);

        struct tw_event ev;
        if (tui_key(t, &ev) < 0) return -1;
        switch (ev.key) {
        case TWK_UP:    *sel = (*sel + n - 1) % n; break;
        case TWK_DOWN:  *sel = (*sel + 1) % n; break;
        case TWK_PGUP:  *sel -= h; if (*sel < 0) *sel = 0; break;
        case TWK_PGDN:  *sel += h; if (*sel >= n) *sel = n - 1; break;
        case TWK_HOME:  *sel = 0; break;
        case TWK_END:   *sel = n - 1; break;
        case TWK_ENTER: return 0;
        case TWK_ESC:
        case TWK_LEFT:  return -1;
        case TWK_CHAR:
            if (ev.ch == 'q') return -1;
            break;
        default: break;
        }
    }
}

/* ── checkbox list ──────────────────────────────────────────────────────── */

/* Step `i` by `dir` until a selectable row is reached (wrapping), so a section
 * header can never hold the selection. */
static int cl_scan(const unsigned char *sep, int i, int dir, int n) {
    for (int k = 1; k <= n; k++) {
        int j = ((i + dir * k) % n + n) % n;
        if (!sep || !sep[j])
            return j;
    }
    return i;
}

static int cl_first(const unsigned char *sep, int n) {
    return cl_scan(sep, 0, 1, n);
}

static int cl_last(const unsigned char *sep, int n) {
    return cl_scan(sep, n - 1, -1, n);
}

int tui_checklist(struct tui *t, const char *title, const char *step,
                  const char *note, const char *hint,
                  const char **items, const unsigned char *sep,
                  unsigned char *on, int n) {
    int h = tui_body_height(t);
    int first = 0, sel = 0;
    if (n <= 0) return -1;
    if (!hint) hint = "Up/Down move  Space toggle  a all  n none  r reset  Enter ok";

    /* 'r' restores exactly the selection the caller started from (the
     * auto-detected set), so a user who clears everything can get it back. */
    unsigned char *base = (unsigned char *)malloc((size_t)n);
    if (!base) return -1;
    memcpy(base, on, (size_t)n);
    sel = cl_first(sep, n);

    for (;;) {
        scroll_to(&first, sel, n, h);
        frame(t, title, step, note, hint);

        int count = 0, real = 0;
        for (int i = 0; i < n; i++) {
            if (sep && sep[i])
                continue;
            real++;
            if (on[i]) count++;
        }

        char status[40];
        snprintf(status, sizeof(status), "%d/%d selected", count, real);
        {
            int sl = (int)strlen(status);
            int c = t->cols - 2 - sl;
            if (c > 2) tw_puts(&t->g, R_NOTE, c, status, 0);
        }

        for (int i = 0; i < h && first + i < n; i++) {
            int idx = first + i;
            char line[TW_MAX_COLS];
            int  sel_row = (idx == sel);

            if (sep && sep[idx]) {
                tw_fill(&t->g, R_BODY + i, 2, t->cols - 4, ' ', TW_A_REV);
                snprintf(line, sizeof(line), " %s", items[idx]);
                tw_puts_clip(&t->g, R_BODY + i, 2, line, t->cols - 4, TW_A_REV);
                continue;
            }

            if (sel_row)
                tw_fill(&t->g, R_BODY + i, 2, t->cols - 4, ' ', TW_A_REV);
            snprintf(line, sizeof(line), "%s [%c] %s",
                     sel_row ? ">" : " ", on[idx] ? 'x' : ' ', items[idx]);
            tw_puts_clip(&t->g, R_BODY + i, 2, line, t->cols - 4,
                         sel_row ? TW_A_REV : 0);
        }
        scroll_marks(t, first, h, n);
        tui_flush(t, -1, -1);

        struct tw_event ev;
        if (tui_key(t, &ev) < 0) { free(base); return -1; }
        switch (ev.key) {
        case TWK_UP:    sel = cl_scan(sep, sel, -1, n); break;
        case TWK_DOWN:  sel = cl_scan(sep, sel, 1, n); break;
        case TWK_PGUP:  sel -= h; if (sel < 0) sel = 0;
                        sel = cl_scan(sep, sel, 1, n); break;
        case TWK_PGDN:  sel += h; if (sel >= n) sel = n - 1;
                        sel = cl_scan(sep, sel, -1, n); break;
        case TWK_HOME:  sel = cl_first(sep, n); break;
        case TWK_END:   sel = cl_last(sep, n); break;
        case TWK_CHAR:
            if (ev.ch == ' ')      on[sel] = (unsigned char)!on[sel];
            else if (ev.ch == 'a') for (int i = 0; i < n; i++) { if (!sep || !sep[i]) on[i] = 1; }
            else if (ev.ch == 'n') for (int i = 0; i < n; i++) { if (!sep || !sep[i]) on[i] = 0; }
            else if (ev.ch == 'r') memcpy(on, base, (size_t)n);
            else if (ev.ch == 'q') { free(base); return -1; }
            break;
        case TWK_ENTER: free(base); return 0;
        case TWK_ESC:
        case TWK_LEFT:  free(base); return -1;
        default: break;
        }
    }
}

/* ── single-line input ──────────────────────────────────────────────────── */

int tui_input(struct tui *t, const char *title, const char *step,
              const char *label, char *buf, int size) {
    int len = (int)strlen(buf);
    int pos = len;
    if (len > size - 1) { len = size - 1; pos = len; buf[len] = '\0'; }

    for (;;) {
        frame(t, title, step, label, "Enter accept   Esc cancel   Backspace edit");
        tw_puts_clip(&t->g, R_BODY, 2, ">", 1, TW_A_REV);
        tw_puts_clip(&t->g, R_BODY, 4, buf, t->cols - 6, 0);
        tw_fill(&t->g, R_BODY, 4 + len, t->cols - 6 - len, ' ', 0);
        tui_flush(t, R_BODY, 4 + pos);

        struct tw_event ev;
        if (tui_key(t, &ev) < 0) return -1;
        if (ev.key == TWK_ENTER) return 0;
        if (ev.key == TWK_ESC)   return -1;
        if (ev.key == TWK_LEFT)  { if (pos > 0) pos--; continue; }
        if (ev.key == TWK_RIGHT) { if (pos < len) pos++; continue; }
        if (ev.key == TWK_HOME)  { pos = 0; continue; }
        if (ev.key == TWK_END)   { pos = len; continue; }
        if (ev.key == TWK_BACKSPACE) {
            if (pos > 0) {
                memmove(buf + pos - 1, buf + pos, (size_t)(len - pos + 1));
                pos--; len--;
            }
            continue;
        }
        if (ev.key == TWK_DELETE) {
            if (pos < len) {
                memmove(buf + pos, buf + pos + 1, (size_t)(len - pos));
                len--;
            }
            continue;
        }
        if (ev.key == TWK_CHAR) {
            if (len < size - 1) {
                memmove(buf + pos + 1, buf + pos, (size_t)(len - pos + 1));
                buf[pos] = ev.ch;
                pos++; len++;
            }
        }
    }
}

/* ── confirm / notice ───────────────────────────────────────────────────── */

int tui_confirm(struct tui *t, const char *title, const char *step,
                const char *question, int def) {
    for (;;) {
        frame(t, title, step, question, "y confirm   n cancel   Esc back");
        tui_flush(t, -1, -1);
        struct tw_event ev;
        if (tui_key(t, &ev) < 0) return -1;
        if (ev.key == TWK_ENTER) return def;
        if (ev.key == TWK_ESC || ev.key == TWK_LEFT) return -1;
        if (ev.key == TWK_CHAR) {
            if (ev.ch == 'y' || ev.ch == 'Y') return 1;
            if (ev.ch == 'n' || ev.ch == 'N') return 0;
        }
    }
}

int tui_notice(struct tui *t, const char *title, const char *step,
               const char *note, const char *hint,
               const char **lines, int n) {
    int h = tui_body_height(t);
    if (!hint) hint = "Enter continue   Left/q back";
    for (;;) {
        frame(t, title, step, note, hint);
        for (int i = 0; i < h && i < n; i++)
            tw_puts_clip(&t->g, R_BODY + i, 2, lines[i], t->cols - 4, 0);
        tui_flush(t, -1, -1);

        struct tw_event ev;
        if (tui_key(t, &ev) < 0) return -1;
        if (ev.key == TWK_ESC || ev.key == TWK_LEFT) return -1;
        if (ev.key == TWK_CHAR && ev.ch == 'q') return -1;
        if (ev.key == TWK_ENTER || ev.key == TWK_CHAR) return 0;
    }
}

/* ── task list ──────────────────────────────────────────────────────────── */

void tui_tasks_begin(struct tui *t, const char *title, const char *step,
                     const char *hint, const char **names, int n) {
    free(t->task_state);
    free(t->task_text);
    t->task_names = names;
    t->task_n = n;
    t->task_title = title;
    t->task_step = step;
    t->task_hint = hint ? hint : "installing...";
    t->task_state = (unsigned char *)malloc((size_t)(n > 0 ? n : 1));
    t->task_text  = (char *)malloc((size_t)(n > 0 ? n : 1) * TASK_TEXT);
    if (t->task_state) memset(t->task_state, TUI_TASK_PENDING, (size_t)(n > 0 ? n : 1));
    if (t->task_text)  memset(t->task_text, 0, (size_t)(n > 0 ? n : 1) * TASK_TEXT);
    for (int i = 0; i < 3; i++) t->task_note[i][0] = '\0';
}

static void tasks_draw(struct tui *t) {
    int h = tui_body_height(t);
    frame(t, t->task_title, t->task_step, NULL, t->task_hint);
    int row = R_BODY;
    for (int i = 0; i < t->task_n && row < R_BODY + h - 3; i++, row++) {
        const char *mark = " ";
        if (t->task_state[i] == TUI_TASK_RUN)  mark = ">";
        if (t->task_state[i] == TUI_TASK_OK)   mark = "x";
        if (t->task_state[i] == TUI_TASK_FAIL) mark = "!";
        char line[TW_MAX_COLS];
        const char *txt = (t->task_text && t->task_text[i * TASK_TEXT])
                              ? &t->task_text[i * TASK_TEXT] : "";
        if (txt[0])
            snprintf(line, sizeof(line), "[%s] %s - %s", mark, t->task_names[i], txt);
        else
            snprintf(line, sizeof(line), "[%s] %s", mark, t->task_names[i]);
        tw_puts_clip(&t->g, row, 2, line, t->cols - 4,
                     t->task_state[i] == TUI_TASK_FAIL ? TW_A_REV : 0);
    }
    for (int i = 0; i < 3 && i < h; i++)
        if (t->task_note[i][0])
            tw_puts_clip(&t->g, t->rows - 2 - (3 - i), 2, t->task_note[i],
                         t->cols - 4, 0);
    tui_flush(t, -1, -1);
}

int tui_tasks_state(const struct tui *t, int i) {
    if (!t->task_state || i < 0 || i >= t->task_n)
        return TUI_TASK_PENDING;
    return t->task_state[i];
}

void tui_tasks_set(struct tui *t, int i, int state) {
    if (!t->task_state || i < 0 || i >= t->task_n) return;
    t->task_state[i] = (unsigned char)state;
    tasks_draw(t);
}

void tui_tasks_set_text(struct tui *t, int i, const char *text) {
    if (!t->task_text || i < 0 || i >= t->task_n) return;
    snprintf(&t->task_text[i * TASK_TEXT], TASK_TEXT, "%s", text ? text : "");
}

void tui_tasks_note(struct tui *t, int line, const char *text) {
    if (line < 0 || line >= 3) return;
    snprintf(t->task_note[line], TASK_NOTE, "%s", text ? text : "");
    if (t->task_state) tasks_draw(t);
}

void tui_tasks_end(struct tui *t) {
    free(t->task_state);
    free(t->task_text);
    t->task_state = NULL;
    t->task_text  = NULL;
    t->task_n = 0;
}
