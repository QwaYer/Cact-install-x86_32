/*
 * firmware.c — firmware groups, hardware-driven preselection, and the keep
 * predicate that produces the target's small archive.
 *
 * The boot archive is the whole Linux firmware mirror; almost none of it is
 * for the machine in front of you.  So the installer offers the archive's
 * top-level firmware directories as groups, with the ones this machine needs
 * already ticked, and re-packs only that selection onto the target.
 *
 * The detection needs no hardware tables: it reads the kernel log.  On the live
 * medium /usr/lib/firmware/ is empty, so every driver that comes up records
 * "firmware: <name> not found"; a medium that does carry a blob records
 * "firmware: <name> loaded".  Either way the log names the firmware this
 * machine asked for, and the group is the first path component of that name.
 */

#include "cact_install.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#define FW_PREFIX     "usr/lib/firmware/"
#define FW_PREFIX_LEN (sizeof(FW_PREFIX) - 1)
#define KMSG_MAX      (1u << 20)

/* The group a firmware *name* belongs to: the first path component, so
 * "rtw88/rtw8822c_fw.bin" -> "rtw88" and "rt2870.bin" -> "" (the root group). */
static void fw_group_of_name(const char *name, char *out, int n) {
    const char *slash = strchr(name, '/');
    size_t len = slash ? (size_t)(slash - name) : strlen(name);
    if (len >= (size_t)n)
        len = (size_t)n - 1;
    memcpy(out, name, len);
    out[len] = '\0';
}

/* Same, for an archive member path ("usr/lib/firmware/rtw88/x").  Returns -1
 * for anything that is not firmware at all. */
static int fw_group_of_member(const char *member, char *out, int n) {
    if (strncmp(member, FW_PREFIX, FW_PREFIX_LEN) != 0)
        return -1;
    const char *rest = member + FW_PREFIX_LEN;
    if (!rest[0])
        return -1;
    fw_group_of_name(rest, out, n);
    return 0;
}

/* ── group scan ─────────────────────────────────────────────────────────── */

struct fw_scan {
    struct ci_fw_group *out;
    int                 n;
    int                 max;
};

static int fw_scan_cb(const char *name, char type, unsigned long long size,
                      void *ud) {
    struct fw_scan *s = (struct fw_scan *)ud;
    if (type == '5' || type == '2')
        return 0;                  /* directories and symlinks carry no data */

    char grp[CI_FW_NAME_MAX];
    if (fw_group_of_member(name, grp, sizeof(grp)) != 0)
        return 0;

    for (int i = 0; i < s->n; i++) {
        if (strcmp(s->out[i].name, grp) == 0) {
            s->out[i].files++;
            s->out[i].bytes += size;
            return 0;
        }
    }
    if (s->n >= s->max)
        return 0;                  /* full: the rest of the archive is ignored */
    struct ci_fw_group *g = &s->out[s->n++];
    memset(g, 0, sizeof(*g));
    snprintf(g->name, sizeof(g->name), "%s", grp);
    g->files = 1;
    g->bytes = size;
    return 0;
}

int ci_fw_scan(const char *arc, struct ci_fw_group *out, int max) {
    if (!arc || !out || max <= 0)
        return -1;
    struct fw_scan s;
    s.out = out;
    s.n = 0;
    s.max = max;
    if (ci_archive_walk(arc, fw_scan_cb, &s) != 0)
        return -1;
    return s.n;
}

/* ── detection from the kernel log ──────────────────────────────────────── */

static char *read_all(const char *path, size_t *out_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    size_t cap = 64 * 1024, n = 0;
    char *b = (char *)malloc(cap);
    if (!b) {
        close(fd);
        return NULL;
    }
    for (;;) {
        if (n + 1 >= cap) {
            if (cap >= KMSG_MAX)
                break;             /* enough of the log; newer lines first */
            char *nb = (char *)realloc(b, cap * 2);
            if (!nb)
                break;
            b = nb;
            cap *= 2;
        }
        ssize_t r = read(fd, b + n, cap - n - 1);
        if (r <= 0)
            break;
        n += (size_t)r;
    }
    close(fd);
    b[n] = '\0';
    *out_len = n;
    return b;
}

static int fw_prefix_ci(const char *name, const char *prefix);   /* below */

/* Loaded driver modules are the second signal: a group named like a module
 * that is actually loaded belongs to hardware this machine has, even when the
 * driver never asked for firmware (or got it from cctkfs).  Names under three
 * characters are skipped — too easy to match by accident. */
static int fw_modules_mark(struct ci_fw_group *groups, int n) {
    size_t len = 0;
    char *mods = read_all("/proc/modules", &len);
    if (!mods)
        return 0;

    int marked = 0;
    char *p = mods;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        if (nl)
            *nl = '\0';

        char name[64];
        int i = 0;
        while (p[i] && p[i] != ' ' && p[i] != '\t' && i < (int)sizeof(name) - 1) {
            name[i] = p[i];
            i++;
        }
        name[i] = '\0';

        if (i >= 3) {
            for (int g = 0; g < n; g++) {
                if (groups[g].wanted || !groups[g].name[0])
                    continue;
                if (strlen(groups[g].name) < 3)
                    continue;
                if (fw_prefix_ci(name, groups[g].name) ||
                    fw_prefix_ci(groups[g].name, name)) {
                    groups[g].wanted = 1;
                    marked++;
                }
            }
        }
        p = nl ? nl + 1 : NULL;
    }
    free(mods);
    return marked;
}

int ci_fw_detect(struct ci_fw_group *groups, int n) {
    if (!groups || n <= 0)
        return 0;

    size_t len = 0;
    char *log = read_all("/dev/kmsg", &len);
    if (!log)
        return 0;

    int marked = 0;
    char *p = log;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        if (nl)
            *nl = '\0';

        const char *hit = strstr(p, "firmware: ");
        if (hit) {
            const char *name = hit + 10;   /* strlen("firmware: ") */
            char nm[128];
            int i = 0;
            while (name[i] && name[i] != ' ' && name[i] != '(' &&
                   i < (int)sizeof(nm) - 1) {
                nm[i] = name[i];
                i++;
            }
            nm[i] = '\0';

            if (nm[0]) {
                char grp[CI_FW_NAME_MAX];
                fw_group_of_name(nm, grp, sizeof(grp));
                for (int g = 0; g < n; g++) {
                    if (strcmp(groups[g].name, grp) == 0) {
                        if (!groups[g].wanted) {
                            groups[g].wanted = 1;
                            marked++;
                        }
                        break;
                    }
                }
            }
        }
        p = nl ? nl + 1 : NULL;
    }

    marked += fw_modules_mark(groups, n);

    free(log);
    return marked;
}

/* ── categories ─────────────────────────────────────────────────────────── */

/*
 * The mirror is ~100 vendor directories plus several hundred board blobs at the
 * top level, which is far too flat to tick through.  Sections come from a
 * prefix table over the real directory names, so the list reads as
 * CPU / Graphics / Network / Bluetooth / Audio / Storage / Other.
 *
 * First match wins, so specific entries must precede general ones ("rtl_bt"
 * before "rtl", "ti-connectivity" before "ti", "amdgpu" before "amd").
 */
struct fw_cat_rule {
    const char *prefix;
    int         cat;
};

static const struct fw_cat_rule fw_cat_rules[] = {
    /* microcode: node-independent, wanted on nearly every machine */
    { "intel-ucode",     CI_FW_CAT_CPU },
    { "amd-ucode",       CI_FW_CAT_CPU },
    { "microcode",       CI_FW_CAT_CPU },

    /* bluetooth */
    { "rtl_bt",          CI_FW_CAT_BT },
    { "btintel",         CI_FW_CAT_BT },
    { "btrtl",           CI_FW_CAT_BT },
    { "btusb",           CI_FW_CAT_BT },
    { "ar3k",            CI_FW_CAT_BT },

    /* network: wifi, ethernet and the PHY/DSP blobs that hang off them */
    { "ti-connectivity", CI_FW_CAT_NET },
    { "ath",             CI_FW_CAT_NET },
    { "rtw",             CI_FW_CAT_NET },
    { "rtlwifi",         CI_FW_CAT_NET },
    { "rtl_nic",         CI_FW_CAT_NET },
    { "rtl",             CI_FW_CAT_NET },
    { "iwlwifi",         CI_FW_CAT_NET },
    { "iwldvm",          CI_FW_CAT_NET },
    { "brcm",            CI_FW_CAT_NET },
    { "mrvl",            CI_FW_CAT_NET },
    { "rsi",             CI_FW_CAT_NET },
    { "wfx",             CI_FW_CAT_NET },
    { "qca",             CI_FW_CAT_NET },
    { "realtek",         CI_FW_CAT_NET },
    { "mediatek",        CI_FW_CAT_NET },
    { "cypress",         CI_FW_CAT_NET },
    { "bnx",             CI_FW_CAT_NET },
    { "cxgb",            CI_FW_CAT_NET },
    { "e100",            CI_FW_CAT_NET },
    { "dpaa2",           CI_FW_CAT_NET },
    { "cavium",          CI_FW_CAT_NET },
    { "slicoss",         CI_FW_CAT_NET },
    { "sxg",             CI_FW_CAT_NET },
    { "tehuti",          CI_FW_CAT_NET },
    { "tigon",           CI_FW_CAT_NET },
    { "vxge",            CI_FW_CAT_NET },
    { "kaweth",          CI_FW_CAT_NET },
    { "atusb",           CI_FW_CAT_NET },
    { "ueagle",          CI_FW_CAT_NET },
    { "ixp4xx",          CI_FW_CAT_NET },
    { "3com",            CI_FW_CAT_NET },
    { "ositech",         CI_FW_CAT_NET },
    { "aeonsemi",        CI_FW_CAT_NET },
    { "airoha",          CI_FW_CAT_NET },

    /* graphics.  The NPU/TEE blobs share AMD's name but are not display. */
    { "amdnpu",          CI_FW_CAT_OTHER },
    { "amdtee",          CI_FW_CAT_OTHER },
    { "i915",            CI_FW_CAT_GPU },
    { "amdgpu",          CI_FW_CAT_GPU },
    { "radeon",          CI_FW_CAT_GPU },
    { "nvidia",          CI_FW_CAT_GPU },
    { "matrox",          CI_FW_CAT_GPU },
    { "powervr",         CI_FW_CAT_GPU },
    { "r128",            CI_FW_CAT_GPU },
    { "mali",            CI_FW_CAT_GPU },
    { "adreno",          CI_FW_CAT_GPU },
    { "xe",              CI_FW_CAT_GPU },
    { "amd",             CI_FW_CAT_GPU },

    /* audio: codecs, DSPs and the SoundWire blobs */
    { "sof",             CI_FW_CAT_AUDIO },
    { "sdca",            CI_FW_CAT_AUDIO },
    { "cirrus",          CI_FW_CAT_AUDIO },
    { "cs46xx",          CI_FW_CAT_AUDIO },
    { "asihpi",          CI_FW_CAT_AUDIO },
    { "dsp56k",          CI_FW_CAT_AUDIO },
    { "mixart",          CI_FW_CAT_AUDIO },
    { "pcxhr",           CI_FW_CAT_AUDIO },
    { "sb16",            CI_FW_CAT_AUDIO },
    { "yamaha",          CI_FW_CAT_AUDIO },
    { "emu",             CI_FW_CAT_AUDIO },
    { "ti-keystone",     CI_FW_CAT_OTHER },
    { "ti",              CI_FW_CAT_AUDIO },

    /* storage controllers and readers */
    { "adaptec",         CI_FW_CAT_STORAGE },
    { "advansys",        CI_FW_CAT_STORAGE },
    { "isci",            CI_FW_CAT_STORAGE },
    { "ene-ub6250",      CI_FW_CAT_STORAGE },
};

static int fw_prefix_ci(const char *name, const char *prefix) {
    for (int i = 0; prefix[i]; i++) {
        char c = name[i];
        if (!c)
            return 0;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + 32);
        if (c != prefix[i])
            return 0;
    }
    return 1;
}

int ci_fw_category(const char *group) {
    if (!group || !group[0])
        return CI_FW_CAT_OTHER;          /* the loose board blobs */
    for (unsigned i = 0; i < sizeof(fw_cat_rules) / sizeof(fw_cat_rules[0]); i++)
        if (fw_prefix_ci(group, fw_cat_rules[i].prefix))
            return fw_cat_rules[i].cat;
    return CI_FW_CAT_OTHER;
}

const char *ci_fw_category_name(int cat) {
    switch (cat) {
    case CI_FW_CAT_CPU:     return "CPU microcode";
    case CI_FW_CAT_GPU:     return "Graphics";
    case CI_FW_CAT_NET:     return "Network";
    case CI_FW_CAT_BT:      return "Bluetooth";
    case CI_FW_CAT_AUDIO:   return "Audio";
    case CI_FW_CAT_STORAGE: return "Storage";
    default:                return "Other";
    }
}

/* Name order with the root group pushed to the end of its section, so the
 * vendor directories read first. */
static int fw_name_cmp(const char *a, const char *b) {
    if (!a[0] || !b[0]) {
        if (!a[0] && !b[0]) return 0;
        return a[0] ? -1 : 1;
    }
    return strcmp(a, b);
}

int ci_fw_arrange(const struct ci_fw_group *groups, int n,
                  struct ci_fw_row *out, int max) {
    if (!groups || !out || n <= 0 || max <= 0)
        return 0;
    if (n > CI_FW_MAX_GROUPS)
        n = CI_FW_MAX_GROUPS;

    int idx[CI_FW_MAX_GROUPS];
    for (int i = 0; i < n; i++)
        idx[i] = i;

    /* Insertion sort: n is a few dozen, and this keeps the order stable and
     * obvious (category, then name). */
    for (int i = 1; i < n; i++) {
        int g = idx[i];
        int cat = ci_fw_category(groups[g].name);
        int j = i - 1;
        while (j >= 0) {
            int catj = ci_fw_category(groups[idx[j]].name);
            int after;
            if (catj != cat)
                after = catj > cat;
            else
                after = fw_name_cmp(groups[idx[j]].name, groups[g].name) > 0;
            if (!after)
                break;
            idx[j + 1] = idx[j];
            j--;
        }
        idx[j + 1] = g;
    }

    int rows = 0, last_cat = -1;
    for (int i = 0; i < n && rows < max; i++) {
        int g = idx[i];
        int cat = ci_fw_category(groups[g].name);
        if (cat != last_cat) {
            struct ci_fw_row *r = &out[rows++];
            memset(r, 0, sizeof(*r));
            snprintf(r->label, sizeof(r->label), "%s", ci_fw_category_name(cat));
            r->header = 1;
            r->group = -1;
            last_cat = cat;
            if (rows >= max)
                break;
        }
        struct ci_fw_row *r = &out[rows++];
        char sz[24];
        ci_fmt_bytes(groups[g].bytes, sz, sizeof(sz));
        memset(r, 0, sizeof(*r));
        snprintf(r->label, sizeof(r->label), "%-22.22s %4u files  %8.8s%s",
                 groups[g].name[0] ? groups[g].name : "(board blobs)",
                 (unsigned)groups[g].files, sz,
                 groups[g].wanted ? "   <- this machine" : "");
        r->header = 0;
        r->group = g;
    }
    return rows;
}

/* ── mini-archive keep predicate ────────────────────────────────────────── */

int ci_fw_keep(const char *name, char type, void *ud) {
    const struct ci_fw_sel *sel = (const struct ci_fw_sel *)ud;
    if (!sel)
        return 1;
    if (type == '5')
        return 1;                  /* keep the tree the blobs hang off */

    char grp[CI_FW_NAME_MAX];
    if (fw_group_of_member(name, grp, sizeof(grp)) != 0)
        return 1;                  /* not firmware: never silently dropped */

    for (int i = 0; i < sel->n; i++)
        if (sel->groups[i].selected && strcmp(sel->groups[i].name, grp) == 0)
            return 1;
    return 0;
}
