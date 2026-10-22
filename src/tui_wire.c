#include "tui_wire.h"

/* ── small buffer writer (libc-free: the wire layer must stay host-testable) ── */

struct sink {
    char *buf;
    int   cap;
    int   n;
    int   over;
};

static void sk_ch(struct sink *s, char c) {
    if (s->n >= s->cap) { s->over = 1; return; }
    s->buf[s->n++] = c;
}

static void sk_str(struct sink *s, const char *p) {
    while (*p) sk_ch(s, *p++);
}

static void sk_uint(struct sink *s, unsigned v) {
    char tmp[12];
    int  n = 0;
    if (v == 0) { sk_ch(s, '0'); return; }
    while (v && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) sk_ch(s, tmp[--n]);
}

/* ── grid ───────────────────────────────────────────────────────────────── */

void tw_init(struct tw_grid *g, int rows, int cols) {
    if (!g) return;
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    if (rows > TW_MAX_ROWS) rows = TW_MAX_ROWS;
    if (cols > TW_MAX_COLS) cols = TW_MAX_COLS;
    g->rows = rows;
    g->cols = cols;
    tw_clear(g);
}

void tw_clear(struct tw_grid *g) {
    if (!g) return;
    for (int i = 0; i < g->rows * g->cols; i++) {
        g->cell[i] = ' ';
        g->attr[i] = 0;
    }
}

static int in_grid(const struct tw_grid *g, int r, int c) {
    return g && r >= 0 && r < g->rows && c >= 0 && c < g->cols;
}

void tw_put(struct tw_grid *g, int r, int c, char ch, uint8_t attr) {
    if (!in_grid(g, r, c)) return;
    g->cell[r * g->cols + c] = ch ? ch : ' ';
    g->attr[r * g->cols + c] = attr;
}

void tw_puts(struct tw_grid *g, int r, int c, const char *s, uint8_t attr) {
    if (!g || !s) return;
    for (int i = 0; s[i] && c + i < g->cols; i++)
        tw_put(g, r, c + i, s[i], attr);
}

void tw_puts_clip(struct tw_grid *g, int r, int c, const char *s, int maxw,
                  uint8_t attr) {
    if (!g || !s || maxw <= 0) return;
    for (int i = 0; s[i] && i < maxw && c + i < g->cols; i++)
        tw_put(g, r, c + i, s[i], attr);
}

void tw_fill(struct tw_grid *g, int r, int c, int w, char ch, uint8_t attr) {
    for (int i = 0; i < w; i++)
        tw_put(g, r, c + i, ch, attr);
}

void tw_rule(struct tw_grid *g, int r, int c, int w, char ch) {
    if (w <= 0) return;
    tw_put(g, r, c, '+', 0);
    tw_put(g, r, c + w - 1, '+', 0);
    for (int i = 1; i < w - 1; i++)
        tw_put(g, r, c + i, ch, 0);
}

void tw_box(struct tw_grid *g, int r, int c, int h, int w) {
    if (h < 2 || w < 2) return;
    tw_put(g, r, c, '+', 0);
    tw_put(g, r, c + w - 1, '+', 0);
    tw_put(g, r + h - 1, c, '+', 0);
    tw_put(g, r + h - 1, c + w - 1, '+', 0);
    for (int i = 1; i < w - 1; i++) {
        tw_put(g, r, c + i, '-', 0);
        tw_put(g, r + h - 1, c + i, '-', 0);
    }
    for (int i = 1; i < h - 1; i++) {
        tw_put(g, r + i, c, '|', 0);
        tw_put(g, r + i, c + w - 1, '|', 0);
    }
}

/* ── serialiser ─────────────────────────────────────────────────────────── */

int tw_emit(const struct tw_grid *g, char *out, int cap) {
    if (!g || !out || cap <= 0) return -1;
    struct sink s = { out, cap, 0, 0 };

    /* Start from a blank screen: frames of different widths never mix. */
    sk_str(&s, "\033[2J");

    for (int r = 0; r < g->rows; r++) {
        sk_str(&s, "\033[");
        sk_uint(&s, (unsigned)(r + 1));
        sk_str(&s, ";1H");
        sk_str(&s, "\033[0m");

        int prev_rev = 0;
        for (int c = 0; c < g->cols; c++) {
            int idx = r * g->cols + c;
            int rev = (g->attr[idx] & TW_A_REV) ? 1 : 0;
            if (rev != prev_rev) {
                sk_str(&s, rev ? "\033[7m" : "\033[27m");
                prev_rev = rev;
            }
            char ch = g->cell[idx];
            sk_ch(&s, ch ? ch : ' ');
        }
        sk_str(&s, "\033[0m\033[K");
    }

    if (s.over) return -1;
    return s.n;
}

/* ── write chunking ─────────────────────────────────────────────────────── */

int tw_chunk(const char *s, int len, int max) {
    if (!s || len <= 0)
        return 0;
    if (max <= 0)
        return 1;

    int i = 0;
    while (i < len && i < max) {
        if (s[i] != 0x1B) {
            i++;
            continue;
        }
        /* A CSI runs to its final byte; anything shorter is just an ESC. */
        int j = i + 1;
        if (j < len && s[j] == '[') {
            j++;
            while (j < len && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';' ||
                               s[j] == '?'))
                j++;
            if (j >= len)
                break;               /* incomplete: cut before the ESC */
            j++;                     /* the final byte itself */
        }
        if (j > max)
            break;                   /* would not fit: cut before the ESC */
        i = j;
    }
    return i > 0 ? i : 1;
}

/* ── key decoder ────────────────────────────────────────────────────────── */

static struct tw_event ev(enum tw_key k, char ch) {
    struct tw_event e;
    e.key = k;
    e.ch  = ch;
    return e;
}

int tw_decode(const unsigned char *buf, int len, struct tw_event *out) {
    if (!buf || len <= 0 || !out) return 0;

    unsigned char c = buf[0];

    if (c == 0x1B) {
        if (len < 2) return 0;                 /* CSI may still be arriving */
        if (buf[1] != '[') {
            *out = ev(TWK_ESC, 0);
            return 1;                          /* leave the next byte alone */
        }
        /* CSI: parameters, then a final byte. */
        int i = 2;
        while (i < len && ((buf[i] >= '0' && buf[i] <= '9') || buf[i] == ';'))
            i++;
        if (i >= len) return 0;                /* parameter bytes still coming */
        switch (buf[i]) {
        case 'A': *out = ev(TWK_UP, 0);    return i + 1;
        case 'B': *out = ev(TWK_DOWN, 0);  return i + 1;
        case 'C': *out = ev(TWK_RIGHT, 0); return i + 1;
        case 'D': *out = ev(TWK_LEFT, 0);  return i + 1;
        case 'H': *out = ev(TWK_HOME, 0);  return i + 1;
        case 'F': *out = ev(TWK_END, 0);   return i + 1;
        default:
            /* Parameterised forms: ESC[5~ PageUp, ESC[6~ PageDown,
             * ESC[3~ Delete.  Anything else is swallowed. */
            if (buf[i] == '~') {
                int p = 0;
                for (int j = 2; j < i; j++)
                    if (buf[j] >= '0' && buf[j] <= '9')
                        p = p * 10 + (buf[j] - '0');
                if (p == 5) *out = ev(TWK_PGUP, 0);
                else if (p == 6) *out = ev(TWK_PGDN, 0);
                else if (p == 3) *out = ev(TWK_DELETE, 0);
                else *out = ev(TWK_NONE, 0);
                return i + 1;
            }
            *out = ev(TWK_NONE, 0);
            return i + 1;
        }
    }

    if (c == '\r' || c == '\n') {
        *out = ev(TWK_ENTER, 0);
        return 1;
    }
    if (c == 0x7F || c == 0x08) {
        *out = ev(TWK_BACKSPACE, 0);
        return 1;
    }
    if (c == '\t') {
        *out = ev(TWK_TAB, 0);
        return 1;
    }
    if (c >= 0x20 && c < 0x7F) {
        *out = ev(TWK_CHAR, (char)c);
        return 1;
    }
    *out = ev(TWK_NONE, 0);
    return 1;
}
