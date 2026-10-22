/*
 * Host test for the pure half of the installer TUI (tui_wire.c).
 *
 * The installer itself is a freestanding PIE for CactLibc and cannot run here,
 * so — exactly as FAT32/test_fat32.c does for the fs module — the layout maths
 * and the escape-stream/key-decoder logic are compiled against the host libc
 * and checked directly.
 *
 *   tools/test-tui-wire.sh
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "tui_wire.h"

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  ok: %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

static int has(const char *hay, const char *needle) {
    return hay && strstr(hay, needle) != NULL;
}

int main(void) {
    printf("== grid ==\n");

    struct tw_grid *g = malloc(sizeof(*g));
    tw_init(g, 4, 20);
    CHECK(g->rows == 4 && g->cols == 20, "tw_init keeps the requested size");
    CHECK(g->cell[0] == ' ' && g->attr[0] == 0, "grid starts blank");

    tw_init(g, 999, 999);
    CHECK(g->rows == TW_MAX_ROWS && g->cols == TW_MAX_COLS,
          "tw_init clamps to the buffer");

    tw_init(g, 6, 20);
    tw_box(g, 0, 0, 4, 10);
    CHECK(g->cell[0 * 20 + 0] == '+', "box top-left corner");
    CHECK(g->cell[0 * 20 + 9] == '+', "box top-right corner");
    CHECK(g->cell[3 * 20 + 0] == '+', "box bottom-left corner");
    CHECK(g->cell[1 * 20 + 0] == '|', "box left edge");
    CHECK(g->cell[0 * 20 + 5] == '-', "box top edge");

    tw_clear(g);
    tw_puts(g, 1, 2, "hello", 0);
    CHECK(g->cell[1 * 20 + 2] == 'h' && g->cell[1 * 20 + 6] == 'o',
          "tw_puts writes left to right");
    tw_puts_clip(g, 2, 2, "abcdefgh", 3, 0);
    CHECK(g->cell[2 * 20 + 2] == 'a' && g->cell[2 * 20 + 5] == ' ',
          "tw_puts_clip stops at maxw");

    /* Writes off the right edge must not wrap onto the next row. */
    tw_clear(g);
    tw_puts(g, 0, 18, "abcd", 0);
    CHECK(g->cell[0 * 20 + 18] == 'a' && g->cell[0 * 20 + 19] == 'b' &&
          g->cell[1 * 20 + 0] == ' ',
          "tw_puts clips at the row end");
    tw_put(g, -1, 0, 'x', 0);
    tw_put(g, 0, 20, 'x', 0);
    CHECK(1, "out-of-range writes are ignored, not crashes");

    printf("== emit ==\n");

    tw_init(g, 2, 4);
    tw_puts(g, 0, 0, "AB", 0);
    tw_put(g, 0, 2, 'C', TW_A_REV);
    char *out = malloc(4096);
    int n = tw_emit(g, out, 4096);
    CHECK(n > 0, "tw_emit succeeds for a small frame");
    out[n] = '\0';
    CHECK(has(out, "\033[2J"), "frame starts by clearing the screen");
    CHECK(has(out, "\033[1;1H"), "row 1 is addressed absolutely");
    CHECK(has(out, "\033[2;1H"), "row 2 is addressed absolutely");
    CHECK(has(out, "AB\033[7mC\033[27m"), "reverse run is entered and left");
    CHECK(out[n - 3] == '\033' && out[n - 1] == 'K',
          "every row ends with ESC[K");
    CHECK(tw_emit(g, out, 8) == -1, "a short buffer is reported, not overrun");

    printf("== write chunking ==\n");

    /* Plain text may be cut anywhere. */
    {
        char plain[600];
        memset(plain, 'a', sizeof(plain));
        CHECK(tw_chunk(plain, 600, 240) == 240, "a plain run fills the chunk");
    }

    /* A sequence straddling the limit must not be split: 235 'a's then
     * ESC[5;5H, so a naive cut at 240 would stop inside the sequence. */
    {
        char s[300];
        memset(s, 'a', 235);
        memcpy(s + 235, "\033[5;5H", 6);
        s[241] = 'b';
        s[242] = 'b';
        s[243] = 'b';
        int c = tw_chunk(s, 244, 240);
        CHECK(c == 235, "the chunk stops before a sequence that would not fit");
        int c2 = tw_chunk(s + c, 244 - c, 240);
        CHECK(c2 == 9, "the sequence comes through whole in the next chunk");
    }

    /* Every produced chunk must leave the parser in a clean state. */
    {
        struct tw_grid *g2 = malloc(sizeof(*g2));
        tw_init(g2, 6, 40);
        tw_puts(g2, 3, 2, "chunk me", TW_A_REV);
        char *stream = malloc(8192);
        int n = tw_emit(g2, stream, 8192);
        CHECK(n > 0, "emit produced a stream");
        char *joined = malloc((size_t)n + 1);
        int off = 0, jn = 0;
        while (off < n) {
            int c = tw_chunk(stream + off, n - off, 17);
            CHECK(c > 0 && c <= 17, "chunk is within the cap");
            memcpy(joined + jn, stream + off, (size_t)c);
            jn += c;
            off += c;
        }
        CHECK(jn == n && memcmp(joined, stream, (size_t)n) == 0,
              "chunks reassemble the stream exactly");
        free(joined);
        free(stream);
        free(g2);
    }

    printf("== key decode ==\n");

    struct tw_event e;
    unsigned char k[8];

    memcpy(k, "\033[A", 3);
    CHECK(tw_decode(k, 3, &e) == 3 && e.key == TWK_UP, "ESC[A -> up");
    memcpy(k, "\033[B", 3);
    CHECK(tw_decode(k, 3, &e) == 3 && e.key == TWK_DOWN, "ESC[B -> down");
    memcpy(k, "\033[C", 3);
    CHECK(tw_decode(k, 3, &e) == 3 && e.key == TWK_RIGHT, "ESC[C -> right");
    memcpy(k, "\033[D", 3);
    CHECK(tw_decode(k, 3, &e) == 3 && e.key == TWK_LEFT, "ESC[D -> left");
    memcpy(k, "\033[H", 3);
    CHECK(tw_decode(k, 3, &e) == 3 && e.key == TWK_HOME, "ESC[H -> home");
    memcpy(k, "\033[F", 3);
    CHECK(tw_decode(k, 3, &e) == 3 && e.key == TWK_END, "ESC[F -> end");
    memcpy(k, "\033[5~", 4);
    CHECK(tw_decode(k, 4, &e) == 4 && e.key == TWK_PGUP, "ESC[5~ -> page up");
    memcpy(k, "\033[6~", 4);
    CHECK(tw_decode(k, 4, &e) == 4 && e.key == TWK_PGDN, "ESC[6~ -> page down");

    k[0] = '\033';
    CHECK(tw_decode(k, 1, &e) == 0, "a lone ESC waits for more input");
    memcpy(k, "\033[", 2);
    CHECK(tw_decode(k, 2, &e) == 0, "an unfinished CSI waits for more input");
    memcpy(k, "\033[12", 4);
    CHECK(tw_decode(k, 4, &e) == 0, "CSI parameter bytes still arriving");

    memcpy(k, "\033x", 2);
    int used = tw_decode(k, 2, &e);
    CHECK(used == 1 && e.key == TWK_ESC, "ESC then a plain key -> ESC, 1 byte");

    memcpy(k, "\033[Z", 3);
    CHECK(tw_decode(k, 3, &e) == 3 && e.key == TWK_NONE,
          "an unknown CSI is consumed, not returned");

    k[0] = '\r';
    CHECK(tw_decode(k, 1, &e) == 1 && e.key == TWK_ENTER, "CR -> enter");
    k[0] = '\n';
    CHECK(tw_decode(k, 1, &e) == 1 && e.key == TWK_ENTER, "LF -> enter");
    k[0] = 0x7F;
    CHECK(tw_decode(k, 1, &e) == 1 && e.key == TWK_BACKSPACE, "DEL -> backspace");
    k[0] = '\t';
    CHECK(tw_decode(k, 1, &e) == 1 && e.key == TWK_TAB, "TAB -> tab");
    k[0] = 'q';
    CHECK(tw_decode(k, 1, &e) == 1 && e.key == TWK_CHAR && e.ch == 'q',
          "a printable byte -> char");
    k[0] = 0x01;
    CHECK(tw_decode(k, 1, &e) == 1 && e.key == TWK_NONE,
          "a control byte is consumed as none");

    free(out);
    free(g);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
