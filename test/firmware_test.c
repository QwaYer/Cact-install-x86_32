/*
 * Host test for the firmware grouping in src/firmware.c.
 *
 * The prefix table and the section arrangement are pure logic, so they are
 * checked here against the real top-level names of the linux-firmware mirror
 * (Projects/Firmware-for-Cact-x86/lib/firmware) rather than by booting the
 * installer — the same split FAT32/test and tui_wire_test use.
 *
 *   tools/test-firmware.sh
 */

#include <stdio.h>
#include <string.h>

#include "cact_install.h"

/* Link stubs: the real implementations live in archive.c and fsops.c, which
 * are built for CactLibc and cannot be compiled for the host. */
int  ci_archive_walk(const char *src, ci_arc_cb cb, void *ud) {
    (void)src; (void)cb; (void)ud;
    return -1;
}
void ci_fmt_bytes(uint64_t bytes, char *out, int n) {
    snprintf(out, (size_t)n, "%llu", (unsigned long long)bytes);
}

static int failures = 0;

#define CHECK_TRUE(cond, msg) do { \
    if (cond) { printf("  ok:   %s\n", msg); } \
    else { printf("  FAIL: %s\n", msg); failures++; } \
} while (0)

static void expect_cat(const char *name, int want) {
    int got = ci_fw_category(name);
    if (got == want) {
        printf("  ok:   %-16s -> %s\n", name, ci_fw_category_name(got));
    } else {
        printf("  FAIL: %-16s -> %s (want %s)\n", name,
               ci_fw_category_name(got), ci_fw_category_name(want));
        failures++;
    }
}

/* The mirror's real layout: every top-level directory plus the loose board
 * blobs at the root (which form the empty-named group). */
static const char *mirror[] = {
    "3com", "adaptec", "advansys", "aeonsemi", "airoha", "amd", "amdgpu",
    "amdnpu", "amdtee", "amd-ucode", "amlogic", "amphion", "ar3k", "arm",
    "asihpi", "ath10k", "ath11k", "ath12k", "ath6k", "ath9k_htc", "atmel",
    "atusb", "av7110", "bnx2", "bnx2x", "brcm", "cadence", "cavium", "cirrus",
    "cis", "cnm", "cpia2", "cs46xx", "cxgb3", "cxgb4", "cypress", "dabusb",
    "dell", "dpaa2", "dsp56k", "e100", "ea", "edgeport", "emi26", "emu",
    "ene-ub6250", "go7007", "HP", "i915", "imx", "inside-secure", "intel",
    "intel-ucode", "isci", "ixp4xx", "kaweth", "keyspan", "keyspan_pda",
    "LENOVO", "Lontium", "matrox", "mediatek", "meson", "microchip", "mixart",
    "morsemicro", "moxa", "nvidia", "nxp", "ositech", "pcxhr", "powervr",
    "qca", "r128", "radeon", "realtek", "rockchip", "rsi", "rtl_bt",
    "rtl_nic", "rtlwifi", "rtw88", "rtw89", "sb16", "sdca", "slicoss", "sxg",
    "tehuti", "ti", "ti-connectivity", "tigon", "ti-keystone", "ueagle-atm",
    "vx", "vxge", "wfx", "xe", "yamaha",
    "",
};

int main(void) {
    printf("== the prefix table's ordering traps ==\n");
    /* Specific prefixes must win over the general ones that contain them. */
    expect_cat("rtl_bt",         CI_FW_CAT_BT);
    expect_cat("rtl_nic",        CI_FW_CAT_NET);
    expect_cat("rtlwifi",        CI_FW_CAT_NET);
    expect_cat("rtw88",          CI_FW_CAT_NET);
    expect_cat("ti-connectivity",CI_FW_CAT_NET);
    expect_cat("ti-keystone",    CI_FW_CAT_OTHER);
    expect_cat("ti",             CI_FW_CAT_AUDIO);
    expect_cat("amd-ucode",      CI_FW_CAT_CPU);
    expect_cat("amdgpu",         CI_FW_CAT_GPU);
    expect_cat("amdnpu",         CI_FW_CAT_OTHER);
    expect_cat("amdtee",         CI_FW_CAT_OTHER);
    expect_cat("amd",            CI_FW_CAT_GPU);
    expect_cat("intel-ucode",    CI_FW_CAT_CPU);
    expect_cat("LENOVO",         CI_FW_CAT_OTHER);
    expect_cat("HP",             CI_FW_CAT_OTHER);
    expect_cat("",               CI_FW_CAT_OTHER);

    printf("== every real mirror name gets a section ==\n");
    int nmirror = (int)(sizeof(mirror) / sizeof(mirror[0]));
    int counts[CI_FW_CAT_COUNT];
    memset(counts, 0, sizeof(counts));
    int bad = 0;
    for (int i = 0; i < nmirror; i++) {
        int c = ci_fw_category(mirror[i]);
        if (c < 0 || c >= CI_FW_CAT_COUNT) {
            printf("  FAIL: %s -> out of range\n", mirror[i]);
            bad++;
            failures++;
            continue;
        }
        counts[c]++;
    }
    for (int c = 0; c < CI_FW_CAT_COUNT; c++)
        printf("  %-14s %3d group(s)\n", ci_fw_category_name(c), counts[c]);
    CHECK_TRUE(bad == 0, "no name falls outside the categories");

    printf("== section arrangement ==\n");
    static struct ci_fw_group groups[CI_FW_MAX_GROUPS];
    int n = 0;
    for (int i = 0; i < nmirror && n < CI_FW_MAX_GROUPS; i++) {
        memset(&groups[n], 0, sizeof(groups[n]));
        snprintf(groups[n].name, sizeof(groups[n].name), "%s", mirror[i]);
        groups[n].files = (uint32_t)(i + 1);
        groups[n].bytes = (uint64_t)(i + 1) * 1024;
        /* pretend the machine asked for Realtek's wifi and Intel's display */
        if (!strcmp(mirror[i], "rtw88") || !strcmp(mirror[i], "i915"))
            groups[n].wanted = 1;
        n++;
    }

    static struct ci_fw_row rows[CI_FW_MAX_ROWS];
    int nr = ci_fw_arrange(groups, n, rows, CI_FW_MAX_ROWS);
    printf("  %d group(s) -> %d row(s)\n", n, nr);

    int seen[CI_FW_MAX_GROUPS];
    memset(seen, 0, sizeof(seen));
    int last_cat = -1, headers = 0, items = 0, order_ok = 1, headers_ok = 1;
    for (int i = 0; i < nr; i++) {
        if (rows[i].header) {
            headers++;
            if (rows[i].group != -1)
                headers_ok = 0;
            int c;
            for (c = 0; c < CI_FW_CAT_COUNT; c++)
                if (!strcmp(rows[i].label, ci_fw_category_name(c))) break;
            if (c <= last_cat)
                order_ok = 0;                 /* sections repeat or go backwards */
            last_cat = c;
            printf("  -- %s\n", rows[i].label);
        } else {
            items++;
            if (rows[i].group < 0 || rows[i].group >= n) {
                printf("  FAIL: bad group index %d\n", rows[i].group);
                failures++;
            } else if (seen[rows[i].group]++) {
                printf("  FAIL: %s appears twice\n", groups[rows[i].group].name);
                failures++;
            }
            printf("     %s\n", rows[i].label);
            int c = ci_fw_category(groups[rows[i].group].name);
            if (c != last_cat) {
                printf("  FAIL: %s sits under the wrong section\n",
                       groups[rows[i].group].name);
                failures++;
            }
        }
    }
    CHECK_TRUE(headers >= 2, "several sections are emitted");
    CHECK_TRUE(items == n, "every group appears exactly once");
    CHECK_TRUE(order_ok, "sections ascend and never repeat");
    CHECK_TRUE(headers_ok, "headers are not selectable rows");

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
