/*
 * archive.c — read, list and re-pack localrepo.tar.zst.
 *
 * The archive the boot medium hands over is the whole firmware mirror (~500
 * MB packed).  The live session uses it as-is, but the target should not carry
 * every firmware blob in existence, so this file can stream a *selection* of
 * it into a fresh archive: it decompresses the source, copies the tar members
 * a keep-predicate accepts, and compresses the result with the vendored zstd.
 *
 * Both halves stream, so nothing bigger than the I/O buffers is held in memory,
 * and ci_archive_walk() can hand every header to a caller without unpacking a
 * single body (that is how the installer lists the firmware groups).
 */

#include "cact_install.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stat.h>

#define ZSTD_STATIC_LINKING_ONLY
#include <compat/zstd_lib.h>

#define AR_IOBUF  (128 * 1024)     /* >= ZSTD_DStreamOutSize() */
/* The payload is already-compressed *.zst blobs and raw ucode/GPU blobs, so a
 * high level buys almost nothing and costs minutes of CPU — the media build
 * settled on the same trade-off. */
#define AR_LEVEL  3

/* ── tar header (ustar, see common/ex_archive.c) ─────────────────────────── */

struct ar_tarhdr {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};

#define AR_BLOCK 512

/* ── zstd reader ─────────────────────────────────────────────────────────── */

struct ar_rd {
    int            fd;
    ZSTD_DStream  *ds;
    void          *ws;
    unsigned char *in;   size_t in_cap, in_len, in_pos;
    unsigned char *out;  size_t out_cap, out_len, out_pos;
    int            ended, failed;
};

static int ar_rd_open(struct ar_rd *r, int fd) {
    memset(r, 0, sizeof(*r));
    r->fd = fd;
    r->in_cap = AR_IOBUF;
    r->out_cap = AR_IOBUF;
    r->in = malloc(r->in_cap);
    r->out = malloc(r->out_cap);
    if (!r->in || !r->out) return -1;

    ZSTD_FrameHeader fh;
    for (;;) {
        if (r->in_len == r->in_cap) return -1;
        ssize_t n = read(r->fd, r->in + r->in_len, r->in_cap - r->in_len);
        if (n <= 0) return -1;
        r->in_len += (size_t)n;
        size_t hr = ZSTD_getFrameHeader(&fh, r->in, r->in_len);
        if (ZSTD_isError(hr)) return -1;
        if (hr == 0) break;
    }
    size_t ws = ZSTD_estimateDStreamSize(fh.windowSize);
    r->ws = malloc(ws);
    if (!r->ws) return -1;
    r->ds = ZSTD_initStaticDStream(r->ws, ws);
    return r->ds ? 0 : -1;
}

static void ar_rd_close(struct ar_rd *r) {
    free(r->ws);
    free(r->in);
    free(r->out);
    r->ws = NULL; r->in = NULL; r->out = NULL;
}

static int ar_rd_refill(struct ar_rd *r) {
    if (r->failed) return -1;
    for (;;) {
        if (r->in_pos == r->in_len) {
            if (r->ended) return 0;
            ssize_t n = read(r->fd, r->in, r->in_cap);
            if (n < 0) { r->failed = 1; return -1; }
            r->in_len = (size_t)n;
            r->in_pos = 0;
            if (n == 0) { r->ended = 1; return 0; }
        }
        ZSTD_inBuffer  in  = { r->in, r->in_len, r->in_pos };
        ZSTD_outBuffer out = { r->out, r->out_cap, 0 };
        size_t ret = ZSTD_decompressStream(r->ds, &out, &in);
        if (ZSTD_isError(ret)) { r->failed = 1; return -1; }
        r->in_pos = in.pos;                /* absolute within the buffer */
        r->out_len = out.pos;
        r->out_pos = 0;
        if (r->out_len > 0) return 1;
        if (ret == 0) { r->ended = 1; return 0; }
    }
}

static size_t ar_rd_read(struct ar_rd *r, void *buf, size_t n) {
    unsigned char *p = buf;
    size_t got = 0;
    while (got < n) {
        if (r->out_pos == r->out_len && ar_rd_refill(r) <= 0) break;
        size_t avail = r->out_len - r->out_pos;
        size_t want = n - got;
        if (want > avail) want = avail;
        memcpy(p + got, r->out + r->out_pos, want);
        r->out_pos += want;
        got += want;
    }
    return got;
}

static int ar_rd_skip(struct ar_rd *r, unsigned long long n) {
    unsigned char buf[4096];
    while (n) {
        size_t want = n > sizeof(buf) ? sizeof(buf) : (size_t)n;
        size_t got = ar_rd_read(r, buf, want);
        if (!got) return -1;
        n -= got;
    }
    return 0;
}

/* ── zstd writer ─────────────────────────────────────────────────────────── */

struct ar_wr {
    int            fd;
    ZSTD_CCtx     *cs;
    void          *ws;
    unsigned char *out;  size_t out_cap;
};

static int ar_wr_flush_out(struct ar_wr *w, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(w->fd, w->out + off, len - off);
        if (n <= 0) return -1;
        off += (size_t)n;
    }
    return 0;
}

static int ar_wr_open(struct ar_wr *w, int fd) {
    memset(w, 0, sizeof(*w));
    w->fd = fd;
    size_t ws = ZSTD_estimateCStreamSize(AR_LEVEL);
    w->ws = malloc(ws);
    w->out_cap = AR_IOBUF;
    w->out = malloc(w->out_cap);
    if (!w->ws || !w->out) return -1;
    w->cs = ZSTD_initStaticCCtx(w->ws, ws);
    if (!w->cs) return -1;
    if (ZSTD_isError(ZSTD_CCtx_setParameter(w->cs, ZSTD_c_compressionLevel, AR_LEVEL)))
        return -1;
    return 0;
}

static void ar_wr_close(struct ar_wr *w) {
    free(w->ws);
    free(w->out);
    w->ws = NULL; w->out = NULL;
}

static int ar_wr_write(struct ar_wr *w, const void *buf, size_t n) {
    ZSTD_inBuffer in = { buf, n, 0 };
    while (in.pos < in.size) {
        ZSTD_outBuffer out = { w->out, w->out_cap, 0 };
        size_t ret = ZSTD_compressStream(w->cs, &out, &in);
        if (ZSTD_isError(ret)) return -1;
        if (out.pos && ar_wr_flush_out(w, out.pos) != 0) return -1;
    }
    return 0;
}

static int ar_wr_finish(struct ar_wr *w) {
    for (;;) {
        ZSTD_outBuffer out = { w->out, w->out_cap, 0 };
        size_t ret = ZSTD_endStream(w->cs, &out);
        if (ZSTD_isError(ret)) return -1;
        if (out.pos && ar_wr_flush_out(w, out.pos) != 0) return -1;
        if (ret == 0) return 0;
    }
}

/* ── tar plumbing ────────────────────────────────────────────────────────── */

static unsigned long long ar_tar_size(const char *f) {
    if ((unsigned char)f[0] & 0x80) {          /* base-256 (GNU) */
        unsigned long long v = 0;
        for (int i = 1; i < 12; i++) v = (v << 8) | (unsigned char)f[i];
        return v;
    }
    unsigned long long v = 0;
    for (int i = 0; i < 12; i++) {
        char c = f[i];
        if (c == ' ' || c == '\0') break;
        if (c < '0' || c > '7') break;
        v = v * 8 + (unsigned long long)(c - '0');
    }
    return v;
}

static int ar_is_zero(const unsigned char *b) {
    for (int i = 0; i < AR_BLOCK; i++) if (b[i]) return 0;
    return 1;
}

static void ar_tar_name(const struct ar_tarhdr *h, char *out, size_t n) {
    if (h->prefix[0]) snprintf(out, n, "%s/%s", h->prefix, h->name);
    else              snprintf(out, n, "%s", h->name);
}

/* Copy `n` bytes from the reader to the writer (streamed). */
static int ar_copy(struct ar_rd *r, struct ar_wr *w, unsigned long long n) {
    unsigned char buf[8192];
    while (n) {
        size_t want = n > sizeof(buf) ? sizeof(buf) : (size_t)n;
        size_t got = ar_rd_read(r, buf, want);
        if (!got) return -1;
        if (ar_wr_write(w, buf, got) != 0) return -1;
        n -= got;
    }
    return 0;
}

/* Walk the members of the archive without unpacking anything: every ustar
 * header is handed to `cb`, and only the bodies are skipped over.  A non-zero
 * return from `cb` stops the walk early (used to keep the group scan short). */
int ci_archive_walk(const char *src, ci_arc_cb cb, void *ud) {
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;

    struct ar_rd r;
    if (ar_rd_open(&r, in) != 0) { ar_rd_close(&r); close(in); return -1; }

    unsigned char blk[AR_BLOCK];
    int rc = 0;
    for (;;) {
        if (ar_rd_read(&r, blk, AR_BLOCK) != AR_BLOCK) { rc = -1; break; }
        if (ar_is_zero(blk)) break;

        const struct ar_tarhdr *h = (const struct ar_tarhdr *)blk;
        char member[512];
        ar_tar_name(h, member, sizeof(member));
        unsigned long long size = ar_tar_size(h->size);
        unsigned long long pad = (AR_BLOCK - (size % AR_BLOCK)) % AR_BLOCK;

        if (cb && cb(member, h->typeflag, size, ud) != 0) {
            rc = 0;
            break;
        }
        if (ar_rd_skip(&r, size + pad) != 0) { rc = -1; break; }
    }

    ar_rd_close(&r);
    close(in);
    return rc;
}

/* Re-pack `src` into `dst`, keeping only the members `keep` accepts (a NULL
 * keep keeps everything).  Headers are copied verbatim — the source is already
 * valid ustar — and both halves stream, so nothing bigger than the I/O buffers
 * is ever held in memory. */
int ci_archive_repack(const char *src, const char *dst,
                      ci_arc_keep keep, void *ud) {
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); return -1; }

    struct ar_rd r;
    struct ar_wr w;
    if (ar_rd_open(&r, in) != 0) { ar_rd_close(&r); close(in); close(out); return -1; }
    if (ar_wr_open(&w, out) != 0) { ar_wr_close(&w); ar_rd_close(&r); close(in); close(out); return -1; }

    unsigned char blk[AR_BLOCK];
    int rc = 0;
    for (;;) {
        if (ar_rd_read(&r, blk, AR_BLOCK) != AR_BLOCK) { rc = -1; break; }
        if (ar_is_zero(blk)) {                       /* end marker: two blocks */
            static const unsigned char zero[AR_BLOCK] = { 0 };
            if (ar_wr_write(&w, blk, AR_BLOCK) != 0 ||
                ar_wr_write(&w, zero, AR_BLOCK) != 0) rc = -1;
            break;
        }
        const struct ar_tarhdr *h = (const struct ar_tarhdr *)blk;
        char member[512];
        ar_tar_name(h, member, sizeof(member));
        unsigned long long size = ar_tar_size(h->size);
        unsigned long long pad = (AR_BLOCK - (size % AR_BLOCK)) % AR_BLOCK;

        if (keep && !keep(member, h->typeflag, ud)) {
            if (ar_rd_skip(&r, size + pad) != 0) { rc = -1; break; }
            continue;
        }
        if (ar_wr_write(&w, blk, AR_BLOCK) != 0) { rc = -1; break; }
        if (ar_copy(&r, &w, size + pad) != 0) { rc = -1; break; }
    }

    if (rc == 0 && ar_wr_finish(&w) != 0) rc = -1;

    ar_wr_close(&w);
    ar_rd_close(&r);
    close(in);
    close(out);
    return rc;
}

static int ar_skip_prefix_keep(const char *name, char type, void *ud) {
    (void)type;
    const char *prefix = (const char *)ud;
    size_t l = prefix ? strlen(prefix) : 0;
    return !(l && strncmp(name, prefix, l) == 0);
}

/* Re-pack `src` into `dst`, dropping every member under `skip_prefix`
 * (a name like "usr/lib/cact-install/").  Returns 0 on success. */
int ci_archive_rebuild(const char *src, const char *dst, const char *skip_prefix) {
    return ci_archive_repack(src, dst, ar_skip_prefix_keep, (void *)skip_prefix);
}
