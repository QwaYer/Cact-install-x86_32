#ifndef CACT_INSTALL_H
#define CACT_INSTALL_H

#include <stdint.h>
#include <stddef.h>

#define CI_MAX_DISKS  8
#define CI_MAX_PARTS  16
#define CI_MAX_PLAN   6

#define SCHEME_MBR_BIOS  1
#define SCHEME_GPT_BIOS  2
#define SCHEME_GPT_EFI   3
#define SCHEME_MANUAL    4

#define ROLE_ROOT  1
#define ROLE_ESP   2

#define PT_EXT4  "ext4"
#define PT_FAT32 "fat32"

#define MNT_ROOT "/tmp/cactroot"
#define MNT_SRC  "/tmp/cactsrc"

struct ci_disk {
    char      name[16];
    char      base[32];
    char      data[40];
    char      status[40];
    uint64_t  sectors;
};

struct ci_partdev {
    char      name[16];
    uint64_t  start;
    uint64_t  size;
    int       table;
};

struct ci_lpart {
    char      dev[20];
    char      fstype[12];
    char      mnt[40];
    int       number;
    int       role;
    int       to_format;
};

struct ci_plan {
    int       scheme;
    int       nparts;
    int       root_idx;
    int       esp_idx;
    struct ci_lpart parts[CI_MAX_PLAN];
};

struct ci_fdisk_cmd {
    int       n;
    char      tok[40][24];
};

/* ui.c */
void ci_term_init(void);
void ci_term_done(void);
int  ci_menu(const char *title, const char **items, int n);
int  ci_confirm(const char *q, int def);
int  ci_prompt(const char *q, char *buf, int size);
long ci_prompt_num(const char *q, long def);
void ci_press(void);

/* run.c */
int ci_exec(const char *path, char *const argv[]);

/* fsops.c */
int  ci_mkdir_p(const char *path);
int  ci_file_exists(const char *path);
int  ci_write_file(const char *path, const char *data, uint32_t len);
int  ci_copy_file(const char *src, const char *dst);
int  ci_mount(const char *dev, const char *mnt, const char *fstype);
int  ci_umount(const char *dev_or_mnt);

/* disk.c */
int      ci_list_disks(struct ci_disk *out, int max);
int      ci_disk_parts(const struct ci_disk *d, struct ci_partdev *out, int max);
uint64_t ci_probe_capacity(const char *data_path);
int      ci_read_status(const char *path, char *buf, int max);

/* layout.c */
int ci_layout_guided(const struct ci_disk *d, int scheme, long esp_mib,
                     struct ci_plan *plan, struct ci_fdisk_cmd *cmd);
int ci_layout_manual(const struct ci_disk *d, struct ci_plan *plan,
                     const struct ci_partdev *parts, int nparts);
int ci_run_fdisk(const struct ci_disk *d, const struct ci_fdisk_cmd *cmd);
void ci_plan_print(const struct ci_plan *plan);
void ci_fill_part_devices(const struct ci_disk *d, struct ci_plan *plan);

/* grub.c */
int ci_grub_install(const struct ci_disk *d, const struct ci_plan *plan,
                    const char *payload_grub);
int ci_deploy_grub_files(const struct ci_plan *plan, const char *root_mnt,
                         const char *payload_grub);
void ci_grub_cfg_write(const char *root_mnt);

#endif
