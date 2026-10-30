/*
 * Read-only Xbox disc built from a folder of game files
 *
 * The plan file written by the game library holds the volume descriptor and
 * directory tables of an XDVDFS disc (already laid out) and, for every file,
 * where it sits in the image and which host file supplies its bytes. Reads
 * are answered from those, so a game folder plays without being copied into
 * a disc image first. Opened as "xplan:<plan file>".
 *
 * Plan file (little endian):
 *   "XPLAN001"
 *   u64 image size
 *   u32 blob count, then per blob:  u64 offset, u32 length, bytes
 *   u32 file count, then per file:  u64 offset, u64 size, u16 path length,
 *                                   path (UTF-8), files sorted by offset
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/option.h"
#include "qemu/thread.h"
#include "qemu/cutils.h"
#include "qobject/qdict.h"
#include <glib/gstdio.h>
#include "block/block_int.h"

#ifdef _WIN32
#include <io.h>
#endif

#define XPLAN_FD_CACHE 8

typedef struct XPlanBlob {
    uint64_t off;
    uint32_t len;
    uint8_t *data;
} XPlanBlob;

typedef struct XPlanFile {
    uint64_t off;
    uint64_t size;
    char *path;
} XPlanFile;

typedef struct BDRVXPlanState {
    uint64_t image_size;
    XPlanBlob *blobs;
    uint32_t nblobs;
    XPlanFile *files;
    uint32_t nfiles;

    QemuMutex lock;
    int fd[XPLAN_FD_CACHE];       /* open host files */
    uint32_t fd_file[XPLAN_FD_CACHE];
    unsigned fd_next;
} BDRVXPlanState;

static const char xplan_magic[8] = "XPLAN001";

static uint64_t rd64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void xplan_parse_filename(const char *filename, QDict *options,
                                 Error **errp)
{
    if (!strstart(filename, "xplan:", &filename)) {
        error_setg(errp, "File name string must start with 'xplan:'");
        return;
    }
    qdict_put_str(options, "plan", filename);
}

static QemuOptsList xplan_runtime_opts = {
    .name = "xplan",
    .head = QTAILQ_HEAD_INITIALIZER(xplan_runtime_opts.head),
    .desc = {
        {
            .name = "plan",
            .type = QEMU_OPT_STRING,
            .help = "Plan file describing the disc",
        },
        { /* end of list */ }
    },
};

static void xplan_free(BDRVXPlanState *s)
{
    for (uint32_t i = 0; i < s->nblobs; i++) {
        g_free(s->blobs[i].data);
    }
    g_free(s->blobs);
    for (uint32_t i = 0; i < s->nfiles; i++) {
        g_free(s->files[i].path);
    }
    g_free(s->files);
    s->blobs = NULL;
    s->files = NULL;
    s->nblobs = s->nfiles = 0;
}

static int xplan_load(BDRVXPlanState *s, const char *plan, Error **errp)
{
    gchar *buf = NULL;
    gsize len = 0;
    GError *gerr = NULL;
    const uint8_t *p, *end;

    if (!g_file_get_contents(plan, &buf, &len, &gerr)) {
        error_setg(errp, "Can't read disc plan '%s': %s", plan,
                   gerr ? gerr->message : "unknown error");
        g_clear_error(&gerr);
        return -EIO;
    }
    p = (const uint8_t *)buf;
    end = p + len;
    if (len < 8 + 8 + 4 || memcmp(p, xplan_magic, 8) != 0) {
        error_setg(errp, "'%s' is not a disc plan", plan);
        goto fail;
    }
    p += 8;
    s->image_size = rd64(p);
    p += 8;
    if (s->image_size == 0 || s->image_size > (1ULL << 40)) {
        error_setg(errp, "Bad disc size in plan");
        goto fail;
    }

    s->nblobs = rd32(p);
    p += 4;
    if (s->nblobs > 1000000) {
        error_setg(errp, "Bad disc plan");
        s->nblobs = 0;
        goto fail;
    }
    s->blobs = g_new0(XPlanBlob, s->nblobs);
    for (uint32_t i = 0; i < s->nblobs; i++) {
        if (end - p < 12) {
            error_setg(errp, "Disc plan is cut short");
            s->nblobs = i;
            goto fail;
        }
        s->blobs[i].off = rd64(p);
        s->blobs[i].len = rd32(p + 8);
        p += 12;
        if ((uint64_t)(end - p) < s->blobs[i].len ||
            s->blobs[i].off + s->blobs[i].len > s->image_size) {
            error_setg(errp, "Disc plan is cut short");
            s->nblobs = i;
            goto fail;
        }
        s->blobs[i].data = g_memdup2(p, s->blobs[i].len ? s->blobs[i].len : 1);
        p += s->blobs[i].len;
    }

    if (end - p < 4) {
        error_setg(errp, "Disc plan is cut short");
        goto fail;
    }
    s->nfiles = rd32(p);
    p += 4;
    if (s->nfiles > 10000000) {
        error_setg(errp, "Bad disc plan");
        s->nfiles = 0;
        goto fail;
    }
    s->files = g_new0(XPlanFile, s->nfiles);
    for (uint32_t i = 0; i < s->nfiles; i++) {
        uint32_t plen;
        if (end - p < 18) {
            error_setg(errp, "Disc plan is cut short");
            s->nfiles = i;
            goto fail;
        }
        s->files[i].off = rd64(p);
        s->files[i].size = rd64(p + 8);
        plen = p[16] | (p[17] << 8);
        p += 18;
        if ((uint64_t)(end - p) < plen ||
            s->files[i].off + s->files[i].size > s->image_size ||
            (i > 0 && s->files[i].off < s->files[i - 1].off)) {
            error_setg(errp, "Disc plan is cut short or out of order");
            s->nfiles = i;
            goto fail;
        }
        s->files[i].path = g_strndup((const char *)p, plen);
        p += plen;
    }
    g_free(buf);
    return 0;

fail:
    g_free(buf);
    xplan_free(s);
    return -EINVAL;
}

static int xplan_open(BlockDriverState *bs, QDict *options, int flags,
                      Error **errp)
{
    BDRVXPlanState *s = bs->opaque;
    QemuOpts *opts;
    const char *plan;
    int ret;

    GRAPH_RDLOCK_GUARD_MAINLOOP();

    opts = qemu_opts_create(&xplan_runtime_opts, NULL, 0, &error_abort);
    if (!qemu_opts_absorb_qdict(opts, options, errp)) {
        qemu_opts_del(opts);
        return -EINVAL;
    }
    plan = qemu_opt_get(opts, "plan");
    if (!plan) {
        error_setg(errp, "xplan block driver requires a 'plan' option");
        qemu_opts_del(opts);
        return -EINVAL;
    }

    qemu_mutex_init(&s->lock);
    for (int i = 0; i < XPLAN_FD_CACHE; i++) {
        s->fd[i] = -1;
    }
    ret = xplan_load(s, plan, errp);
    qemu_opts_del(opts);
    if (ret < 0) {
        qemu_mutex_destroy(&s->lock);
        return ret;
    }
    bs->total_sectors = DIV_ROUND_UP(s->image_size, BDRV_SECTOR_SIZE);
    return 0;
}

static void xplan_close(BlockDriverState *bs)
{
    BDRVXPlanState *s = bs->opaque;

    for (int i = 0; i < XPLAN_FD_CACHE; i++) {
        if (s->fd[i] >= 0) {
            close(s->fd[i]);
            s->fd[i] = -1;
        }
    }
    xplan_free(s);
    qemu_mutex_destroy(&s->lock);
}

/* Read from host file `idx` at `foff`; whatever can't be read stays zero.
 * Called with the lock held. */
static void xplan_read_file(BDRVXPlanState *s, uint32_t idx, uint64_t foff,
                            uint8_t *dst, uint64_t len)
{
    int slot = -1;
    int fd;

    for (int i = 0; i < XPLAN_FD_CACHE; i++) {
        if (s->fd[i] >= 0 && s->fd_file[i] == idx) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        fd = g_open(s->files[idx].path, O_RDONLY | O_BINARY, 0);
        if (fd < 0) {
            return;
        }
        slot = s->fd_next++ % XPLAN_FD_CACHE;
        if (s->fd[slot] >= 0) {
            close(s->fd[slot]);
        }
        s->fd[slot] = fd;
        s->fd_file[slot] = idx;
    }
    fd = s->fd[slot];

#ifdef _WIN32
    if (_lseeki64(fd, (__int64)foff, SEEK_SET) < 0) {
        return;
    }
#else
    if (lseek(fd, (off_t)foff, SEEK_SET) < 0) {
        return;
    }
#endif
    while (len) {
        unsigned chunk = len > (1u << 20) ? (1u << 20) : (unsigned)len;
        int n = read(fd, dst, chunk);
        if (n <= 0) {
            break;
        }
        dst += n;
        len -= n;
    }
}

static int coroutine_fn GRAPH_RDLOCK
xplan_co_preadv(BlockDriverState *bs, int64_t offset, int64_t bytes,
                QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    BDRVXPlanState *s = bs->opaque;
    uint8_t *buf;
    uint64_t end = (uint64_t)offset + bytes;
    uint32_t lo, hi;

    buf = g_try_malloc0(bytes ? bytes : 1);
    if (!buf) {
        return -ENOMEM;
    }

    qemu_mutex_lock(&s->lock);
    for (uint32_t i = 0; i < s->nblobs; i++) {
        XPlanBlob *b = &s->blobs[i];
        uint64_t a = MAX((uint64_t)offset, b->off);
        uint64_t z = MIN(end, b->off + b->len);
        if (a < z) {
            memcpy(buf + (a - offset), b->data + (a - b->off), z - a);
        }
    }
    /* first file that ends after `offset` (files are sorted by start and
     * do not overlap) */
    lo = 0;
    hi = s->nfiles;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (s->files[mid].off + s->files[mid].size <= (uint64_t)offset) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    for (uint32_t i = lo; i < s->nfiles && s->files[i].off < end; i++) {
        XPlanFile *f = &s->files[i];
        uint64_t a = MAX((uint64_t)offset, f->off);
        uint64_t z = MIN(end, f->off + f->size);
        if (a < z) {
            xplan_read_file(s, i, a - f->off, buf + (a - offset), z - a);
        }
    }
    qemu_mutex_unlock(&s->lock);

    qemu_iovec_from_buf(qiov, 0, buf, bytes);
    g_free(buf);
    return 0;
}

static int coroutine_fn GRAPH_RDLOCK
xplan_co_pwritev(BlockDriverState *bs, int64_t offset, int64_t bytes,
                 QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    return -EACCES; /* a disc */
}

static const char *const xplan_strong_runtime_opts[] = {
    "plan",
    NULL
};

static BlockDriver bdrv_xplan = {
    .format_name            = "xplan",
    .protocol_name          = "xplan",
    .instance_size          = sizeof(BDRVXPlanState),

    .bdrv_parse_filename    = xplan_parse_filename,
    .bdrv_open              = xplan_open,
    .bdrv_close             = xplan_close,

    .bdrv_co_preadv         = xplan_co_preadv,
    .bdrv_co_pwritev        = xplan_co_pwritev,

    .strong_runtime_opts    = xplan_strong_runtime_opts,
};

static void bdrv_xplan_init(void)
{
    bdrv_register(&bdrv_xplan);
}

block_init(bdrv_xplan_init);
