/*
 * ui.c — the installer's prompts, expressed through the full-screen TUI.
 *
 * These wrappers keep the small numbered/line-oriented interface the wizard
 * steps were written against, so a step that just needs "pick one" or "type a
 * value" stays a couple of lines while the screen it draws is the real TUI
 * (header, step counter, scrolling body, reverse-video selection).
 */

#include "cact_install.h"
#include "tui.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static struct tui *g_tui;

void ci_term_init(void) {
    g_tui = tui_open();
}

void ci_term_done(void) {
    tui_close(g_tui);
    g_tui = NULL;
}

struct tui *ci_tui(void) {
    return g_tui;
}

int ci_menu(const char *title, const char **items, int n) {
    if (!g_tui || n <= 0)
        return -1;
    int sel = 0;
    if (tui_menu(g_tui, title, NULL, NULL,
                 "Up/Down move   Enter select   0/Left back",
                 items, n, &sel) != 0)
        return 0;                    /* backed out */
    return sel + 1;
}

int ci_confirm(const char *q, int def) {
    if (!g_tui)
        return def;
    return tui_confirm(g_tui, "Confirm", NULL, q, def);
}

int ci_prompt(const char *q, char *buf, int size) {
    if (!g_tui)
        return 0;
    buf[0] = '\0';
    if (tui_input(g_tui, "Input", NULL, q, buf, size) != 0)
        return 0;
    return 1;
}

long ci_prompt_num(const char *q, long def) {
    char label[128];
    char buf[32];
    snprintf(label, sizeof(label), "%s [%ld]", q, def);
    if (!ci_prompt(label, buf, sizeof(buf)))
        return def;
    if (!buf[0])
        return def;
    char *end;
    long v = strtol(buf, &end, 10);
    if (end == buf)
        return def;
    return v;
}

void ci_press(void) {
    static const char *lines[] = { "Press Enter to continue." };
    if (!g_tui)
        return;
    tui_notice(g_tui, "Continue", NULL, NULL, "Enter continue", lines, 1);
}
