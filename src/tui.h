#ifndef CACT_TUI_H
#define CACT_TUI_H

/*
 * tui — the installer's full-screen interface.
 *
 * A single outer frame with a header (title + wizard step), a body the caller
 * fills, and a hint line, all drawn through the CactOS console's escape
 * support: absolute cursor addressing, erase-to-EOL and reverse video.  Every
 * widget runs its own input loop and repaints the frame, so callers read like
 * the wizard they describe.
 *
 * Frames are ASCII only (the console renders bytes, not UTF-8), and the grid
 * maths / key decoding live in tui_wire.c so they can be host-tested.
 */

#include "tui_wire.h"

struct tui;

/* Open /dev/tty, switch it to raw mode and read its size.  NULL when the
 * installer has no terminal to talk to (piped output, no tty). */
struct tui *tui_open(void);
void        tui_close(struct tui *t);

int  tui_rows(const struct tui *t);
int  tui_cols(const struct tui *t);
/* Body drawing area: rows r .. r+h-1 are free of the frame. */
int  tui_body_top(const struct tui *t);
int  tui_body_height(const struct tui *t);
struct tw_grid *tui_grid(struct tui *t);

/* Send the current grid to the terminal and park the cursor.  Pass -1 for
 * either coordinate to leave the cursor on the hint line. */
void tui_flush(struct tui *t, int cur_r, int cur_c);

/* Blocking key read.  Returns 0 with *ev filled, -1 when input is lost. */
int  tui_key(struct tui *t, struct tw_event *ev);

/*
 * Widgets.  Each draws a full frame, runs its own loop and returns 0 when the
 * user accepts, -1 when the user backs out.  `step` is the wizard position
 * ("disk  [2/9]"), `note` an optional line under the header, `hint` the key
 * legend on the bottom line.
 */
int tui_menu(struct tui *t, const char *title, const char *step,
             const char *note, const char *hint,
             const char **items, int n, int *sel);

/* Checkbox list: Space toggles, 'a' all, 'n' none, 'r' restores the initial
 * selection, Enter accepts.  `on` is a caller-owned n-byte array.
 *
 * When `sep` is non-NULL, rows with sep[i] != 0 are section headers: drawn as a
 * bar, skipped by navigation, and left out of the toggle/count commands.  The
 * selection always lands on a real row. */
int tui_checklist(struct tui *t, const char *title, const char *step,
                  const char *note, const char *hint,
                  const char **items, const unsigned char *sep,
                  unsigned char *on, int n);

int tui_input(struct tui *t, const char *title, const char *step,
              const char *label, char *buf, int size);

int tui_confirm(struct tui *t, const char *title, const char *step,
                const char *question, int def);

/* Show `lines` and wait for any key.  Esc backs out. */
int tui_notice(struct tui *t, const char *title, const char *step,
               const char *note, const char *hint,
               const char **lines, int n);

/* A step list that fills in as the install proceeds; no input loop. */
#define TUI_TASK_PENDING 0
#define TUI_TASK_RUN     1
#define TUI_TASK_OK      2
#define TUI_TASK_FAIL    3

void tui_tasks_begin(struct tui *t, const char *title, const char *step,
                     const char *hint, const char **names, int n);
int  tui_tasks_state(const struct tui *t, int i);
void tui_tasks_set(struct tui *t, int i, int state);
void tui_tasks_set_text(struct tui *t, int i, const char *text);
void tui_tasks_note(struct tui *t, int line, const char *text);
void tui_tasks_end(struct tui *t);

#endif
