#include "cact_install.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

static const char *scheme_name(int s) {
    switch (s) {
    case SCHEME_MBR_BIOS: return "MBR / BIOS";
    case SCHEME_GPT_BIOS: return "GPT / BIOS";
    case SCHEME_GPT_EFI:  return "GPT / EFI (ESP + root)";
    case SCHEME_MANUAL:   return "manual";
    default:              return "?";
    }
}

static int cmd_add(struct ci_fdisk_cmd *c, const char *s) {
    if (c->n >= 40)
        return -1;
    strncpy(c->tok[c->n], s, sizeof(c->tok[0]) - 1);
    c->tok[c->n][sizeof(c->tok[0]) - 1] = '\0';
    c->n++;
    return 0;
}

static void part_set(struct ci_lpart *p, int number, int role,
                     const char *fstype, const char *mnt) {
    memset(p, 0, sizeof(*p));
    p->number = number;
    p->role = role;
    p->to_format = 1;
    snprintf(p->fstype, sizeof(p->fstype), "%s", fstype);
    snprintf(p->mnt, sizeof(p->mnt), "%s", mnt);
}

int ci_layout_guided(const struct ci_disk *d, int scheme, long esp_mib,
                     struct ci_plan *plan, struct ci_fdisk_cmd *cmd) {
    memset(plan, 0, sizeof(*plan));
    memset(cmd, 0, sizeof(*cmd));
    plan->scheme = scheme;
    plan->esp_idx = -1;

    uint64_t cap = d->sectors;
    if (cap < 4096) {
        printf("disk %s too small for a CactOS layout (%u sectors)\n",
               d->name, (unsigned)cap);
        return -1;
    }

    char end_root[24];
    snprintf(end_root, sizeof(end_root), "%u",
             (unsigned)(cap > 1 ? cap - 1 : cap));

    if (scheme == SCHEME_MBR_BIOS) {
        plan->nparts = 1;
        plan->root_idx = 0;
        part_set(&plan->parts[0], 1, ROLE_ROOT, PT_EXT4, MNT_ROOT);
        cmd_add(cmd, "o");
        cmd_add(cmd, "n");
        cmd_add(cmd, "default");
        cmd_add(cmd, end_root);
        cmd_add(cmd, "t");
        cmd_add(cmd, "1");
        cmd_add(cmd, "83");
        cmd_add(cmd, "b");
        cmd_add(cmd, "1");
        cmd_add(cmd, "w");
    } else if (scheme == SCHEME_GPT_BIOS) {
        plan->nparts = 1;
        plan->root_idx = 0;
        part_set(&plan->parts[0], 1, ROLE_ROOT, PT_EXT4, MNT_ROOT);
        cmd_add(cmd, "g");
        cmd_add(cmd, "n");
        cmd_add(cmd, "default");
        cmd_add(cmd, end_root);
        cmd_add(cmd, "t");
        cmd_add(cmd, "1");
        cmd_add(cmd, "linux");
        cmd_add(cmd, "w");
    } else if (scheme == SCHEME_GPT_EFI) {
        if (esp_mib < 8)
            esp_mib = 512;
        uint64_t esp_sectors = (uint64_t)esp_mib * 2048u;
        if (2048u + esp_sectors + 2048u >= cap) {
            printf("disk %s too small for a %ld MiB ESP\n", d->name, esp_mib);
            return -1;
        }
        plan->nparts = 2;
        plan->root_idx = 1;
        plan->esp_idx = 0;
        part_set(&plan->parts[0], 1, ROLE_ESP, PT_FAT32, MNT_ROOT "/boot");
        part_set(&plan->parts[1], 2, ROLE_ROOT, PT_EXT4, MNT_ROOT);
        char esp_arg[24];
        snprintf(esp_arg, sizeof(esp_arg), "+%ldM", esp_mib);
        cmd_add(cmd, "g");
        cmd_add(cmd, "n");
        cmd_add(cmd, "default");
        cmd_add(cmd, esp_arg);
        cmd_add(cmd, "t");
        cmd_add(cmd, "1");
        cmd_add(cmd, "efi");
        cmd_add(cmd, "n");
        cmd_add(cmd, "default");
        cmd_add(cmd, end_root);
        cmd_add(cmd, "t");
        cmd_add(cmd, "2");
        cmd_add(cmd, "linux");
        cmd_add(cmd, "w");
    } else {
        return -1;
    }
    return 0;
}

static void dev_of(const struct ci_disk *d, int number, char *out, int outsize) {
    const char *base = d->name;
    int digit = base[0] && base[strlen(base) - 1] >= '0' &&
                base[strlen(base) - 1] <= '9';
    if (digit)
        snprintf(out, outsize, "%sp%d", base, number);
    else
        snprintf(out, outsize, "%s%d", base, number);
}

void ci_fill_part_devices(const struct ci_disk *d, struct ci_plan *plan) {
    for (int i = 0; i < plan->nparts; i++) {
        dev_of(d, plan->parts[i].number, plan->parts[i].dev,
               (int)sizeof(plan->parts[i].dev));
    }
}

static const char *role_name(int r) {
    switch (r) {
    case ROLE_ROOT: return "root ext4";
    case ROLE_ESP:  return "ESP fat32";
    default:        return "skip";
    }
}

void ci_plan_print(const struct ci_plan *plan) {
    printf("Layout: %s\n", scheme_name(plan->scheme));
    for (int i = 0; i < plan->nparts; i++) {
        const struct ci_lpart *p = &plan->parts[i];
        printf("  %s  %s", p->dev, p->role ? role_name(p->role) : "---");
        if (p->to_format)
            printf("  (will be formatted %s)", p->fstype);
        printf("\n");
    }
}

int ci_run_fdisk(const struct ci_disk *d, const struct ci_fdisk_cmd *cmd) {
    char *argv[48];
    int n = 0;
    argv[n++] = (char *)"/sbin/fdisk";
    argv[n++] = (char *)d->base;
    if (cmd) {
        for (int i = 0; i < cmd->n && n < 46; i++)
            argv[n++] = (char *)cmd->tok[i];
    }
    argv[n] = NULL;
    printf("fdisk %s", d->base);
    if (cmd) {
        for (int i = 2; i < n; i++)
            printf(" %s", argv[i]);
    }
    printf("\n");
    fflush(stdout);
    return ci_exec("/sbin/fdisk", argv);
}

static int prompt_role(const char *name) {
    static const char *items[] = {
        "root — CactOS root (ext4), will be formatted",
        "esp — EFI system partition (fat32), will be formatted",
        "skip — leave untouched",
    };
    printf("Assign partition %s:\n", name);
    int r = ci_menu("", items, 3);
    if (r == 1)
        return ROLE_ROOT;
    if (r == 2)
        return ROLE_ESP;
    return -1;
}

int ci_layout_manual(const struct ci_disk *d, struct ci_plan *plan,
                     const struct ci_partdev *parts, int nparts) {
    (void)d;
    memset(plan, 0, sizeof(*plan));
    plan->scheme = SCHEME_MANUAL;
    plan->esp_idx = -1;
    plan->root_idx = -1;
    plan->nparts = 0;

    for (int i = 0; i < nparts && plan->nparts < CI_MAX_PLAN; i++) {
        int role = prompt_role(parts[i].name);
        if (role == ROLE_ROOT && plan->root_idx >= 0) {
            printf("only one root partition is supported; skipping %s\n",
                   parts[i].name);
            continue;
        }
        if (role == ROLE_ESP && plan->esp_idx >= 0) {
            printf("only one ESP is supported; skipping %s\n", parts[i].name);
            continue;
        }
        if (role < 0)
            continue;

        struct ci_lpart *p = &plan->parts[plan->nparts];
        memset(p, 0, sizeof(*p));
        snprintf(p->dev, sizeof(p->dev), "%s", parts[i].name);
        p->number = i + 1;
        p->role = role;
        p->to_format = 1;
        if (role == ROLE_ROOT) {
            plan->root_idx = plan->nparts;
            snprintf(p->fstype, sizeof(p->fstype), "%s", PT_EXT4);
            snprintf(p->mnt, sizeof(p->mnt), "%s", MNT_ROOT);
        } else {
            plan->esp_idx = plan->nparts;
            snprintf(p->fstype, sizeof(p->fstype), "%s", PT_FAT32);
            snprintf(p->mnt, sizeof(p->mnt), "%s" "/boot", MNT_ROOT);
        }
        plan->nparts++;
    }

    if (plan->root_idx < 0) {
        printf("no root partition selected; cannot install\n");
        return -1;
    }

    int gpt = 0;
    for (int i = 0; i < nparts; i++)
        if (parts[i].table == 2)
            gpt = 1;
    plan->scheme = plan->esp_idx >= 0 ? SCHEME_GPT_EFI
                 : gpt              ? SCHEME_GPT_BIOS
                                    : SCHEME_MBR_BIOS;
    return 0;
}
