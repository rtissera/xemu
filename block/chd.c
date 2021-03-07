/*
 * QEMU Block driver for CHD images
 *
 * Copyright (c) 2021 Romain Tisserand
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "block/block_int.h"
#include "qemu/module.h"
#include "qemu/bswap.h"
#include <libchdr/chd.h>

/* Maximum compressed block size */
#define MAX_BLOCK_SIZE (64 * 1024 * 1024)

typedef struct BDRVCHDState {
    CoMutex lock;
    chd_file *chd;
    chd_header* header;
    uint8_t* hunkmem;
    int32_t oldhunk;
} BDRVCHDState;

static int chd_probe(const uint8_t *buf, int buf_size, const char *filename)
{
    chd_file* file;
    chd_error err = chd_open(filename,CHD_OPEN_READ, NULL, &file);
    if (err != CHDERR_NONE)
        return 0;
    return 1;
}

static int chd_openfile(BlockDriverState *bs, QDict *options, int flags,
                      Error **errp)
{
    BDRVCHDState *s = bs->opaque;

    int ret;

    chd_file* file;
    chd_error err = chd_open(filename,CHD_OPEN_READ, NULL, &file);
    if (err != CHDERR_NONE)
        goto fail;
    s->file = file;
    s->header = chd_get_header(file);
    if (s->header == NULL)
	goto fail;

    /* allocate storage for sector reads */
    s->hunkmem = (uint8_t*)malloc(chd_header->hunkbytes);
    s->oldhunk = -1;
       
    /* FIXME very approximate calculation, does not take multiple tracks and gaps into account */
    bs->total_sectors = chd_header->hunkbytes * chd_header->totalhunks;
    qemu_co_mutex_init(&s->lock);
    return 0;

fail:
    chd_close(file);
    return ret;
}

static void chd_refresh_limits(BlockDriverState *bs, Error **errp)
{
    bs->bl.request_alignment = BDRV_SECTOR_SIZE; /* No sub-sector I/O */
}

static inline int chd_read_block(BlockDriverState *bs, int block_num)
{
    int hunknum, hunkofs;
    chd_error err;
    BDRVCHDState *s = bs->opaque;

    hunknum = (block_num * BDRV_SECTOR_SIZE) / s->hunkbytes;
    hunkofs = (block_num * BDRV_SECTOR_SIZE) % s->hunkbytes;
    if (hunknum != s->oldhunk)
    {
        err = chd_read(s->file, hunknum, s->hunkmem);
        if (err != CHDERR_NONE)
            goto fail;
        else
            oldhunk = hunknum;
    }

    memcpy(buf, hunkmem + hunkofs * (2352 + 96), 2352);

#if 0
    if (s->current_block != block_num) {
        int ret;
        uint32_t bytes = s->offsets[block_num + 1] - s->offsets[block_num];

        ret = bdrv_pread(bs->file, s->offsets[block_num],
                         s->compressed_block, bytes);
        if (ret != bytes) {
            return -1;
        }

        s->zstream.next_in = s->compressed_block;
        s->zstream.avail_in = bytes;
        s->zstream.next_out = s->uncompressed_block;
        s->zstream.avail_out = s->block_size;
        ret = inflateReset(&s->zstream);
        if (ret != Z_OK) {
            return -1;
        }
        ret = inflate(&s->zstream, Z_FINISH);
        if (ret != Z_STREAM_END || s->zstream.total_out != s->block_size) {
            return -1;
        }

        s->current_block = block_num;
    }
#endif
    return 0;
}

static int coroutine_fn
chd_co_preadv(BlockDriverState *bs, uint64_t offset, uint64_t bytes,
                QEMUIOVector *qiov, int flags)
{
    BDRVCHDState *s = bs->opaque;
    uint64_t sector_num = offset >> BDRV_SECTOR_BITS;
    int nb_sectors = bytes >> BDRV_SECTOR_BITS;
    int ret, i;

    assert(QEMU_IS_ALIGNED(offset, BDRV_SECTOR_SIZE));
    assert(QEMU_IS_ALIGNED(bytes, BDRV_SECTOR_SIZE));

    qemu_co_mutex_lock(&s->lock);

#if 0
    for (i = 0; i < nb_sectors; i++) {
        void *data;
        uint32_t sector_offset_in_block =
            ((sector_num + i) % s->sectors_per_block),
            block_num = (sector_num + i) / s->sectors_per_block;
        if (chd_read_block(bs, block_num) != 0) {
            ret = -EIO;
            goto fail;
        }

        data = s->uncompressed_block + sector_offset_in_block * 512;
        qemu_iovec_from_buf(qiov, i * 512, data, 512);
    }
#endif

    ret = -EIO;
fail:
    qemu_co_mutex_unlock(&s->lock);

    return ret;
}

static void chd_closefile(BlockDriverState *bs)
{
    BDRVCHDState *s = bs->opaque;
    chd_close(s->chd);
}

static BlockDriver bdrv_chd = {
    .format_name    = "chd",
    .instance_size  = sizeof(BDRVCHDState),
    .bdrv_probe     = chd_probe,
    .bdrv_open      = chd_openfile,
    .bdrv_child_perm     = bdrv_default_perms,
    .bdrv_refresh_limits = chd_refresh_limits,
    .bdrv_co_preadv = chd_co_preadv,
    .bdrv_close     = chd_closefile,
    .is_format      = true,
};

static void bdrv_chd_init(void)
{
    bdrv_register(&bdrv_chd);
}

block_init(bdrv_chd_init);
