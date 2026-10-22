#ifndef CACT_TUI_WIRE_H
#define CACT_TUI_WIRE_H

/*
 * tui_wire — the pure half of the installer's full-screen TUI.
 *
 * A frame is composed in a character grid plus a per-cell attribute byte and
 * then serialised into the escape stream the CactOS console understands:
 * absolute cursor addressing (ESC[r;cH), erase-to-end-of-line (ESC[K) and SGR
 * (reverse video only, the one attribute the console renders).  There is no
 * diffing: every row is emitted with its own ESC[K, which clears whatever a
 * previous, wider frame left behind.
 *
 * Nothing here touches the terminal, libc or a global, so the layout maths and
 * the key decoder are exercised by a host test (tools/test-tui-wire.sh) instead
 * of by booting the installer — the same split the FAT32 module uses.
 *
 * The console renders bytes, not UTF-8, so frames are ASCII only.
 */

#include <stdint.h>

#define TW_MAX_ROWS 80
#define TW_MAX_COLS 200

/* Per-cell attributes.  Only reverse video is drawn by the console; the colour
 * bits are carried so a widget can mark intent even when it is not rendered. */
#define TW_A_REV   0x01
#define TW_A_BOLD  0x02

struct tw_grid {
    int      rows;
    int      cols;
    char     cell[TW_MAX_ROWS * TW_MAX_COLS];
    uint8_t  attr[TW_MAX_ROWS * TW_MAX_COLS];
};

/* Clamp rows/cols into range and blank the grid. */
void tw_init(struct tw_grid *g, int rows, int cols);
void tw_clear(struct tw_grid *g);

void tw_put(struct tw_grid *g, int r, int c, char ch, uint8_t attr);
void tw_puts(struct tw_grid *g, int r, int c, const char *s, uint8_t attr);
/* Like tw_puts(), but never writes past column c+maxw-1. */
void tw_puts_clip(struct tw_grid *g, int r, int c, const char *s, int maxw,
                  uint8_t attr);
void tw_fill(struct tw_grid *g, int r, int c, int w, char ch, uint8_t attr);
/* Draw a one-cell ASCII box frame spanning rows r..r+h-1, cols c..c+w-1. */
void tw_box(struct tw_grid *g, int r, int c, int h, int w);
/* Horizontal run of `ch` with '+' ends, to underline a header. */
void tw_rule(struct tw_grid *g, int r, int c, int w, char ch);

/* Serialise the whole grid.  Returns the number of bytes written, or -1 when
 * `cap` is too small.  The stream starts with ESC[2J so a frame never inherits
 * the previous screen. */
int tw_emit(const struct tw_grid *g, char *out, int cap);

/*
 * Length of the longest prefix of `s` (at most `max` bytes) that ends on an
 * escape-sequence boundary.
 *
 * The CactOS tty renders output in fixed 255-byte chunks and its escape parser
 * state does not survive a chunk boundary — a sequence split across two writes
 * loses its head and prints its tail as literal text.  Callers therefore cap
 * every write with this, and never emit one sequence in several writes.
 * Always returns at least 1 (the stream is never left stuck).
 */
int tw_chunk(const char *s, int len, int max);

/* Event codes.  TWK_CHAR carries ev->ch. */
enum tw_key {
    TWK_NONE = 0,
    TWK_CHAR,
    TWK_UP,
    TWK_DOWN,
    TWK_LEFT,
    TWK_RIGHT,
    TWK_HOME,
    TWK_END,
    TWK_PGUP,
    TWK_PGDN,
    TWK_ENTER,
    TWK_ESC,
    TWK_BACKSPACE,
    TWK_TAB,
    TWK_DELETE,
};

struct tw_event {
    enum tw_key key;
    char        ch;
};

/*
 * Decode one event from `len` buffered input bytes.
 *
 * Returns the number of bytes consumed, or 0 when more input is needed (a
 * lone ESC, or a CSI still arriving).  On a consumed event `ev` is filled; an
 * unrecognised byte is consumed and reported as TWK_NONE, so the caller can
 * always make progress.  An ESC followed by anything but '[' consumes just the
 * ESC and leaves the next byte in the buffer — that is what makes a bare Esc
 * usable as "back" while CSI sequences stay intact.
 */
int tw_decode(const unsigned char *buf, int len, struct tw_event *ev);

#endif
