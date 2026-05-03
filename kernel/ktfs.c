// ktfs.c - KTFS read-only filesystem driver (CP1)
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef KTFS_TRACE
#define TRACE
#endif

#ifdef KTFS_DEBUG
#define DEBUG
#endif

#include "heap.h"
#include "fs.h"
#include "ioimpl.h"
#include "ktfs.h"
#include "error.h"
#include "thread.h"
#include "string.h"
#include "console.h"
#include "cache.h"

#include <stdint.h>
#include <stddef.h>

// Block-pointer indirection counts derived from the on-disk schema.
#define KTFS_PTRS_PER_BLOCK  (KTFS_BLKSZ / sizeof(uint32_t))   // 128

// Per-open file state.  We keep the inode index + size + cached inode
// blocks so we don't re-read them on every readat.
struct ktfs_file {
    struct io           io;            // first member -- ioclose math relies on this
    uint16_t            inode_idx;
    uint32_t            size;          // cached inode.size
    int                 in_use;
    char                name[KTFS_MAX_FILENAME_LEN + 2];
};

// Internal globals (filesystem is single-mount).
static struct cache *      g_cache;
static struct io *         g_bkgio;
static uint32_t            g_block_count;
static uint32_t            g_bitmap_block_count;
static uint32_t            g_inode_block_count;
static uint16_t            g_root_inode_idx;
static uint32_t            g_first_inode_block;   // 1 + bitmap_block_count
static uint32_t            g_first_data_block;    // first_inode_block + inode_block_count

#ifndef KTFS_MAX_OPEN
#define KTFS_MAX_OPEN 16
#endif

static struct ktfs_file g_open_files[KTFS_MAX_OPEN];

// FUNCTION ALIASES (driver -> generic fs.h API)
int fsmount(struct io * io)
    __attribute__ ((alias("ktfs_mount")));
int fsopen(const char * name, struct io ** ioptr)
    __attribute__ ((alias("ktfs_open")));
int fsflush(void)
    __attribute__ ((alias("ktfs_flush")));

// INTERNAL FUNCTION DECLARATIONS

static void ktfs_close(struct io * io);
static long ktfs_readat(struct io * io, unsigned long long pos,
                        void * buf, long len);
static int  ktfs_cntl(struct io * io, int cmd, void * arg);

static int  read_inode(uint16_t inode_idx, struct ktfs_inode * out);
static int  resolve_or_alloc(struct ktfs_inode * ino, uint16_t inode_idx,
                             uint32_t logical_blk, int alloc,
                             uint32_t * out);
static int  free_data_block(uint32_t data_blk_idx);
static int  write_inode(uint16_t inode_idx, const struct ktfs_inode * in);
static int  data_block_for_offset(const struct ktfs_inode * ino,
                                  unsigned long long byte_off,
                                  uint32_t * data_blk_out);
static int  find_dentry(const char * name, uint16_t * inode_out);
static unsigned long long data_blk_to_pos(uint32_t data_blk_idx);

// EXPORTED FUNCTION DEFINITIONS

int ktfs_mount(struct io * io) {
    if (io == NULL)
        return -EINVAL;

    if (create_cache(io, &g_cache) != 0)
        return -EIO;
    g_bkgio = io;

    // Read the superblock (block 0).
    void * sb_blk = NULL;
    if (cache_get_block(g_cache, 0, &sb_blk) != 0)
        return -EIO;
    struct ktfs_superblock * sb = (struct ktfs_superblock *)sb_blk;

    g_block_count        = sb->block_count;
    g_bitmap_block_count = sb->bitmap_block_count;
    g_inode_block_count  = sb->inode_block_count;
    g_root_inode_idx     = sb->root_directory_inode;
    g_first_inode_block  = 1 + g_bitmap_block_count;
    g_first_data_block   = g_first_inode_block + g_inode_block_count;

    cache_release_block(g_cache, sb_blk, CACHE_CLEAN);

    // Reset open-file table.
    for (int i = 0; i < KTFS_MAX_OPEN; i++)
        g_open_files[i].in_use = 0;

    return 0;
}

static const struct iointf ktfs_iointf = {
    .close   = &ktfs_close,
    .readat  = &ktfs_readat,
    .cntl    = &ktfs_cntl,
};

int ktfs_open(const char * name, struct io ** ioptr) {
    if (name == NULL || ioptr == NULL)
        return -EINVAL;
    if (g_cache == NULL)
        return -EIO;

    uint16_t inode_idx;
    int rc = find_dentry(name, &inode_idx);
    if (rc != 0)
        return rc;

    // Find a free open-file slot.
    int slot = -1;
    for (int i = 0; i < KTFS_MAX_OPEN; i++) {
        if (!g_open_files[i].in_use) { slot = i; break; }
    }
    if (slot < 0)
        return -ENOMEM;

    struct ktfs_inode ino;
    rc = read_inode(inode_idx, &ino);
    if (rc != 0)
        return rc;

    struct ktfs_file * f = &g_open_files[slot];
    f->in_use    = 1;
    f->inode_idx = inode_idx;
    f->size      = ino.size;
    // Stash the name (lower-case copy of caller's, capped).
    int i;
    for (i = 0; i < (int)KTFS_MAX_FILENAME_LEN && name[i]; i++)
        f->name[i] = name[i];
    f->name[i] = '\0';

    ioinit1(&f->io, &ktfs_iointf);
    *ioptr = &f->io;
    return 0;
}

void ktfs_close(struct io * io) {
    struct ktfs_file * f = (struct ktfs_file *)io;
    f->in_use = 0;
}

long ktfs_readat(struct io * io, unsigned long long pos,
                 void * buf, long len) {
    struct ktfs_file * f = (struct ktfs_file *)io;
    if (len <= 0)
        return 0;
    if (pos >= f->size)
        return 0;
    unsigned long long avail = f->size - pos;
    if ((unsigned long long)len > avail)
        len = (long)avail;

    struct ktfs_inode ino;
    if (read_inode(f->inode_idx, &ino) != 0)
        return -EIO;

    long copied = 0;
    char * cbuf = buf;
    while (copied < len) {
        unsigned long long byte_off = pos + copied;
        uint32_t data_blk;
        int rc = data_block_for_offset(&ino, byte_off, &data_blk);
        if (rc != 0)
            return rc;

        unsigned long long blk_pos = data_blk_to_pos(data_blk);
        unsigned long long blk_off = byte_off % KTFS_BLKSZ;
        long chunk = KTFS_BLKSZ - blk_off;
        if (chunk > len - copied)
            chunk = len - copied;

        void * blk = NULL;
        if (cache_get_block(g_cache, blk_pos, &blk) != 0)
            return -EIO;
        memcpy(cbuf + copied, (char *)blk + blk_off, chunk);
        cache_release_block(g_cache, blk, CACHE_CLEAN);
        copied += chunk;
    }
    return copied;
}

int ktfs_cntl(struct io * io, int cmd, void * arg) {
    struct ktfs_file * f = (struct ktfs_file *)io;
    switch (cmd) {
    case IOCTL_GETBLKSZ:
        return 1;     // file API is byte-granular per PDF 5.1.3
    case IOCTL_GETEND:
        if (arg == NULL) return -EINVAL;
        *(unsigned long long *)arg = f->size;
        return 0;
    case IOCTL_SETEND: {
        // Truncate or extend the file to *(unsigned long long*)arg.
        // On extend, new bytes are zero-fill (the on-disk inode tracks
        // size; subsequent reads stop at f->size, and resolve_or_alloc
        // will lazy-allocate freshly-zeroed blocks on later writes).
        // On truncate, free any data blocks that fall fully beyond the
        // new end and clear partial-block tail bytes.
        if (arg == NULL) return -EINVAL;
        unsigned long long new_size = *(const unsigned long long *)arg;
        if (new_size > 0xFFFFFFFFULL) return -EINVAL;

        struct ktfs_inode ino;
        if (read_inode(f->inode_idx, &ino) != 0) return -EIO;

        if (new_size < ino.size) {
            // Shrink: zero out the trailing partial-block region of the
            // new last block (so future reads after re-extension don't
            // surface stale bytes).  Free fully-out-of-range blocks.
            unsigned long long tail_byte = new_size;
            unsigned long long old_byte  = ino.size;
            uint32_t old_last_blk = (uint32_t)((old_byte + KTFS_BLKSZ - 1)
                                               / KTFS_BLKSZ);
            uint32_t new_last_blk = (uint32_t)((new_size + KTFS_BLKSZ - 1)
                                               / KTFS_BLKSZ);

            // Zero the partial tail of the last surviving block.
            if (new_size % KTFS_BLKSZ != 0 && new_last_blk > 0) {
                uint32_t logical = new_last_blk - 1;
                uint32_t blk;
                if (resolve_or_alloc(&ino, f->inode_idx, logical,
                                     /*alloc=*/0, &blk) == 0) {
                    void * b = NULL;
                    if (cache_get_block(g_cache, data_blk_to_pos(blk),
                                        &b) == 0) {
                        memset((char *)b + (tail_byte % KTFS_BLKSZ), 0,
                               KTFS_BLKSZ - (tail_byte % KTFS_BLKSZ));
                        cache_release_block(g_cache, b, CACHE_DIRTY);
                    }
                }
            }
            // Free blocks [new_last_blk, old_last_blk).
            for (uint32_t lb = new_last_blk; lb < old_last_blk; lb++) {
                uint32_t blk;
                if (resolve_or_alloc(&ino, f->inode_idx, lb,
                                     /*alloc=*/0, &blk) == 0)
                    free_data_block(blk);
            }
        }

        ino.size = (uint32_t)new_size;
        if (write_inode(f->inode_idx, &ino) != 0) return -EIO;
        f->size = (uint32_t)new_size;
        cache_flush(g_cache);
        return 0;
    }
    default:
        return -ENOTSUP;
    }
}

int ktfs_flush(void) {
    if (g_cache == NULL)
        return -EIO;
    return cache_flush(g_cache);
}

// CP2 write side: writeat / create / delete.
//
// Layout reminders:
//   block 0           : superblock
//   blocks 1..1+B-1   : bitmap (B = bitmap_block_count) -- bit set = used
//   next I blocks     : inodes (I = inode_block_count, 16 inodes per block)
//   rest              : data blocks (block numbers in inode are
//                       data-block-relative -- offset by g_first_data_block).

// Find a free data block (data-block-relative index), mark it in the
// bitmap, and zero its contents.  Returns the relative index, or
// negative error.  (We don't need to track which inode owns the block
// at this layer.)
static int alloc_data_block(uint32_t * out_idx) {
    // Search bitmap blocks for the first 0 bit covering a data-block
    // position.  Bitmap is byte-array; bit i within byte b covers
    // block (b*8 + i) of the entire image (not data-relative).
    uint32_t total_blocks = g_block_count;
    for (uint32_t bmp_blk = 0; bmp_blk < g_bitmap_block_count; bmp_blk++) {
        unsigned long long bmp_pos =
            (unsigned long long)(1 + bmp_blk) * KTFS_BLKSZ;
        void * bmp = NULL;
        if (cache_get_block(g_cache, bmp_pos, &bmp) != 0) return -EIO;
        uint8_t * bytes = bmp;
        for (uint32_t byte = 0; byte < KTFS_BLKSZ; byte++) {
            if (bytes[byte] == 0xFF) continue;
            for (int bit = 0; bit < 8; bit++) {
                if (bytes[byte] & (1 << bit)) continue;
                uint32_t blk_num = bmp_blk * KTFS_BLKSZ * 8 + byte * 8 + bit;
                if (blk_num < g_first_data_block) continue;   // metadata
                if (blk_num >= total_blocks) {
                    cache_release_block(g_cache, bmp, CACHE_CLEAN);
                    return -ENOMEM;
                }
                bytes[byte] |= (1 << bit);
                cache_release_block(g_cache, bmp, CACHE_DIRTY);

                // Zero out the new data block.
                void * dblk = NULL;
                if (cache_get_block(g_cache,
                        (unsigned long long)blk_num * KTFS_BLKSZ, &dblk) != 0)
                    return -EIO;
                memset(dblk, 0, KTFS_BLKSZ);
                cache_release_block(g_cache, dblk, CACHE_DIRTY);

                *out_idx = blk_num - g_first_data_block;
                return 0;
            }
        }
        cache_release_block(g_cache, bmp, CACHE_CLEAN);
    }
    return -ENOMEM;
}

// Free a data block (clear bitmap bit).  data_blk_idx is data-relative.
static int free_data_block(uint32_t data_blk_idx) {
    uint32_t blk_num = g_first_data_block + data_blk_idx;
    uint32_t byte = (blk_num / 8) % KTFS_BLKSZ;
    uint32_t bmp_blk = (blk_num / 8) / KTFS_BLKSZ;
    int bit = blk_num % 8;
    if (bmp_blk >= g_bitmap_block_count) return -EINVAL;

    void * bmp = NULL;
    if (cache_get_block(g_cache,
            (unsigned long long)(1 + bmp_blk) * KTFS_BLKSZ, &bmp) != 0)
        return -EIO;
    ((uint8_t *)bmp)[byte] &= ~(1 << bit);
    cache_release_block(g_cache, bmp, CACHE_DIRTY);
    return 0;
}

// Persist a (possibly modified) inode struct back to disk.
static int write_inode(uint16_t inode_idx, const struct ktfs_inode * in) {
    uint32_t per_block = KTFS_BLKSZ / KTFS_INOSZ;
    uint32_t blk_index = g_first_inode_block + inode_idx / per_block;
    uint32_t blk_off   = (inode_idx % per_block) * KTFS_INOSZ;
    void * blk = NULL;
    if (cache_get_block(g_cache,
            (unsigned long long)blk_index * KTFS_BLKSZ, &blk) != 0)
        return -EIO;
    memcpy((char *)blk + blk_off, in, sizeof(struct ktfs_inode));
    cache_release_block(g_cache, blk, CACHE_DIRTY);
    return 0;
}

// Look up (and optionally allocate) the data block backing the
// /logical_blk/-th block of /ino/.  When alloc==1 and the block isn't
// allocated yet, alloc_data_block + persist into the inode.  Returns
// data-relative index in *out, or negative error.
static int resolve_or_alloc(struct ktfs_inode * ino, uint16_t inode_idx,
                            uint32_t logical_blk, int alloc,
                            uint32_t * out)
{
    (void)inode_idx;
    // Direct: data block 0 is legitimately used by root dir, so we can't
    // use 0 as "unallocated" sentinel.  Use file size instead.
    uint32_t allocated_blocks = (ino->size + KTFS_BLKSZ - 1) / KTFS_BLKSZ;

    if (logical_blk < KTFS_NUM_DIRECT_DATA_BLOCKS) {
        if (logical_blk < allocated_blocks) {
            *out = ino->block[logical_blk];
            return 0;
        }
        if (!alloc) { *out = 0; return -EINVAL; }
        uint32_t new_blk;
        int rc = alloc_data_block(&new_blk);
        if (rc != 0) return rc;
        ino->block[logical_blk] = new_blk;
        // Bump size so subsequent calls in the same writeat loop see this
        // block as allocated.  Caller may further extend size on writeat
        // completion.
        if ((logical_blk + 1) * KTFS_BLKSZ > ino->size)
            ino->size = (logical_blk + 1) * KTFS_BLKSZ;
        *out = new_blk;
        return 0;
    }
    logical_blk -= KTFS_NUM_DIRECT_DATA_BLOCKS;

    if (logical_blk < KTFS_PTRS_PER_BLOCK) {
        // Indirect block: 0 is a reliable sentinel (root holds block 0).
        if (ino->indirect == 0) {
            if (!alloc) { *out = 0; return -EINVAL; }
            uint32_t new_blk;
            int rc = alloc_data_block(&new_blk);
            if (rc != 0) return rc;
            ino->indirect = new_blk;
        }
        void * iblk = NULL;
        if (cache_get_block(g_cache, data_blk_to_pos(ino->indirect),
                            &iblk) != 0)
            return -EIO;
        uint32_t * arr = iblk;
        if (arr[logical_blk] != 0) {
            *out = arr[logical_blk];
            cache_release_block(g_cache, iblk, CACHE_CLEAN);
            return 0;
        }
        if (!alloc) {
            cache_release_block(g_cache, iblk, CACHE_CLEAN);
            *out = 0;
            return -EINVAL;
        }
        uint32_t new_blk;
        int rc = alloc_data_block(&new_blk);
        if (rc != 0) {
            cache_release_block(g_cache, iblk, CACHE_CLEAN);
            return rc;
        }
        arr[logical_blk] = new_blk;
        cache_release_block(g_cache, iblk, CACHE_DIRTY);
        *out = new_blk;
        return 0;
    }
    // Doubly-indirect: logical_blk indexes into one of two top-level
    // dindirect[] entries, each pointing to a block of 128 indirect
    // pointers, each pointing to a data block.
    logical_blk -= KTFS_PTRS_PER_BLOCK;
    uint32_t per_dind = KTFS_PTRS_PER_BLOCK * KTFS_PTRS_PER_BLOCK;
    if (logical_blk >= per_dind * KTFS_NUM_DINDIRECT_BLOCKS) {
        *out = 0;
        return -EINVAL;
    }
    uint32_t which_dind = logical_blk / per_dind;
    uint32_t into_dind  = logical_blk % per_dind;
    uint32_t which_ind  = into_dind / KTFS_PTRS_PER_BLOCK;
    uint32_t into_ind   = into_dind % KTFS_PTRS_PER_BLOCK;

    // Top-level dindirect block: 0 sentinel works (root never uses it).
    if (ino->dindirect[which_dind] == 0) {
        if (!alloc) { *out = 0; return -EINVAL; }
        uint32_t new_blk;
        int rc = alloc_data_block(&new_blk);
        if (rc != 0) return rc;
        ino->dindirect[which_dind] = new_blk;
    }
    void * dblk = NULL;
    if (cache_get_block(g_cache, data_blk_to_pos(ino->dindirect[which_dind]),
                        &dblk) != 0)
        return -EIO;
    uint32_t * darr = dblk;

    // Mid-level indirect block.
    if (darr[which_ind] == 0) {
        if (!alloc) {
            cache_release_block(g_cache, dblk, CACHE_CLEAN);
            *out = 0; return -EINVAL;
        }
        uint32_t new_blk;
        int rc = alloc_data_block(&new_blk);
        if (rc != 0) {
            cache_release_block(g_cache, dblk, CACHE_CLEAN);
            return rc;
        }
        darr[which_ind] = new_blk;
        cache_release_block(g_cache, dblk, CACHE_DIRTY);
    } else {
        cache_release_block(g_cache, dblk, CACHE_CLEAN);
    }

    // Re-fetch dblk to read back the (possibly just-allocated) ind ptr.
    if (cache_get_block(g_cache, data_blk_to_pos(ino->dindirect[which_dind]),
                        &dblk) != 0)
        return -EIO;
    uint32_t ind_blk = ((uint32_t *)dblk)[which_ind];
    cache_release_block(g_cache, dblk, CACHE_CLEAN);

    // Leaf indirect entry.
    void * iblk = NULL;
    if (cache_get_block(g_cache, data_blk_to_pos(ind_blk), &iblk) != 0)
        return -EIO;
    uint32_t * iarr = iblk;
    if (iarr[into_ind] != 0) {
        *out = iarr[into_ind];
        cache_release_block(g_cache, iblk, CACHE_CLEAN);
        return 0;
    }
    if (!alloc) {
        cache_release_block(g_cache, iblk, CACHE_CLEAN);
        *out = 0; return -EINVAL;
    }
    uint32_t new_blk;
    int rc = alloc_data_block(&new_blk);
    if (rc != 0) {
        cache_release_block(g_cache, iblk, CACHE_CLEAN);
        return rc;
    }
    iarr[into_ind] = new_blk;
    cache_release_block(g_cache, iblk, CACHE_DIRTY);
    *out = new_blk;
    return 0;
}

long ktfs_writeat(struct io * io, unsigned long long pos,
                  const void * buf, long len) {
    struct ktfs_file * f = (struct ktfs_file *)io;
    if (len <= 0) return 0;
    if (g_cache == NULL) return -EIO;

    struct ktfs_inode ino;
    if (read_inode(f->inode_idx, &ino) != 0) return -EIO;

    long copied = 0;
    const char * cbuf = buf;
    while (copied < len) {
        unsigned long long byte_off = pos + copied;
        uint32_t logical = (uint32_t)(byte_off / KTFS_BLKSZ);
        uint32_t data_blk;
        int rc = resolve_or_alloc(&ino, f->inode_idx, logical,
                                  /*alloc=*/1, &data_blk);
        if (rc != 0) return rc;

        unsigned long long blk_pos = data_blk_to_pos(data_blk);
        unsigned long long blk_off = byte_off % KTFS_BLKSZ;
        long chunk = KTFS_BLKSZ - blk_off;
        if (chunk > len - copied) chunk = len - copied;

        void * blk = NULL;
        if (cache_get_block(g_cache, blk_pos, &blk) != 0) return -EIO;
        memcpy((char *)blk + blk_off, cbuf + copied, chunk);
        cache_release_block(g_cache, blk, CACHE_DIRTY);
        copied += chunk;
    }

    // Update file size if we extended.
    unsigned long long new_size = pos + copied;
    if (new_size > ino.size) {
        ino.size = (uint32_t)new_size;
        f->size  = ino.size;
    }
    if (write_inode(f->inode_idx, &ino) != 0) return -EIO;
    cache_flush(g_cache);
    return copied;
}

// Find a free inode (16 per inode block).  Returns inode index, or -1.
static int find_free_inode(uint16_t * out) {
    uint32_t per_block = KTFS_BLKSZ / KTFS_INOSZ;
    for (uint32_t blk = 0; blk < g_inode_block_count; blk++) {
        void * b = NULL;
        if (cache_get_block(g_cache,
                (unsigned long long)(g_first_inode_block + blk) * KTFS_BLKSZ,
                &b) != 0)
            return -EIO;
        struct ktfs_inode * arr = b;
        for (uint32_t i = 0; i < per_block; i++) {
            uint16_t inode_idx = (uint16_t)(blk * per_block + i);
            if (inode_idx == g_root_inode_idx) continue;
            // Free inode: all-zero bytes (mkfs convention -- size==0
            // and no block pointers means slot is unused).
            uint8_t * raw = (uint8_t *)&arr[i];
            int all_zero = 1;
            for (uint32_t k = 0; k < KTFS_INOSZ; k++) {
                if (raw[k] != 0) { all_zero = 0; break; }
            }
            if (all_zero) {
                *out = inode_idx;
                cache_release_block(g_cache, b, CACHE_CLEAN);
                return 0;
            }
        }
        cache_release_block(g_cache, b, CACHE_CLEAN);
    }
    return -ENOMEM;
}

int ktfs_create(const char * name) {
    if (g_cache == NULL || name == NULL) return -EINVAL;

    int nlen = 0;
    while (name[nlen] && nlen <= (int)KTFS_MAX_FILENAME_LEN) nlen++;
    if (nlen == 0 || nlen > (int)KTFS_MAX_FILENAME_LEN) return -EINVAL;

    // Reject duplicate names.
    {
        uint16_t dup;
        if (find_dentry(name, &dup) == 0) return -EINVAL;
    }

    // Find a free inode slot and zero it (besides we'll set size=0).
    uint16_t new_inode;
    int rc = find_free_inode(&new_inode);
    if (rc != 0) return rc;
    struct ktfs_inode fresh = { 0 };
    if (write_inode(new_inode, &fresh) != 0) return -EIO;

    // Append a dentry to the root inode's data area.
    struct ktfs_inode root;
    if (read_inode(g_root_inode_idx, &root) != 0) return -EIO;

    uint32_t dent_no = root.size / KTFS_DENSZ;
    uint32_t logical = (dent_no * KTFS_DENSZ) / KTFS_BLKSZ;
    uint32_t blk_off = (dent_no * KTFS_DENSZ) % KTFS_BLKSZ;
    uint32_t data_blk;
    rc = resolve_or_alloc(&root, g_root_inode_idx, logical, 1, &data_blk);
    if (rc != 0) return rc;

    void * dblk = NULL;
    if (cache_get_block(g_cache, data_blk_to_pos(data_blk), &dblk) != 0)
        return -EIO;
    struct ktfs_dir_entry * de =
        (struct ktfs_dir_entry *)((char *)dblk + blk_off);
    de->inode = new_inode;
    memset(de->name, 0, sizeof de->name);
    for (int i = 0; i < nlen && i < (int)KTFS_MAX_FILENAME_LEN; i++)
        de->name[i] = name[i];
    cache_release_block(g_cache, dblk, CACHE_DIRTY);

    root.size += KTFS_DENSZ;
    if (write_inode(g_root_inode_idx, &root) != 0) return -EIO;
    cache_flush(g_cache);
    return 0;
}

int ktfs_delete(const char * name) {
    if (g_cache == NULL || name == NULL) return -EINVAL;

    uint16_t target_inode;
    int rc = find_dentry(name, &target_inode);
    if (rc != 0) return rc;

    // Free the target inode's data blocks.
    struct ktfs_inode ino;
    if (read_inode(target_inode, &ino) != 0) return -EIO;
    for (int i = 0; i < KTFS_NUM_DIRECT_DATA_BLOCKS; i++)
        if (ino.block[i] != 0) free_data_block(ino.block[i]);
    if (ino.indirect != 0) {
        // Free the indirect-pointed blocks first.
        void * iblk = NULL;
        if (cache_get_block(g_cache, data_blk_to_pos(ino.indirect),
                            &iblk) == 0) {
            uint32_t * arr = iblk;
            for (uint32_t k = 0; k < KTFS_PTRS_PER_BLOCK; k++)
                if (arr[k] != 0) free_data_block(arr[k]);
            cache_release_block(g_cache, iblk, CACHE_CLEAN);
        }
        free_data_block(ino.indirect);
    }
    // Zero the inode itself.
    struct ktfs_inode zero = { 0 };
    if (write_inode(target_inode, &zero) != 0) return -EIO;

    // Remove the dentry.  Walk root, find matching entry, swap with last
    // entry, decrement size.  (Spec says dentries must be contiguous.)
    struct ktfs_inode root;
    if (read_inode(g_root_inode_idx, &root) != 0) return -EIO;
    uint32_t total = root.size / KTFS_DENSZ;
    uint32_t found_idx = total;     // sentinel

    for (uint32_t i = 0; i < total; i++) {
        unsigned long long off = (unsigned long long)i * KTFS_DENSZ;
        uint32_t blk;
        if (resolve_or_alloc(&root, g_root_inode_idx,
                             (uint32_t)(off / KTFS_BLKSZ), 0, &blk) != 0)
            continue;
        void * d = NULL;
        if (cache_get_block(g_cache, data_blk_to_pos(blk), &d) != 0)
            continue;
        struct ktfs_dir_entry * de =
            (struct ktfs_dir_entry *)((char *)d + (off % KTFS_BLKSZ));
        if (de->inode == target_inode) {
            found_idx = i;
            cache_release_block(g_cache, d, CACHE_CLEAN);
            break;
        }
        cache_release_block(g_cache, d, CACHE_CLEAN);
    }

    if (found_idx == total) return -ENOENT;
    // Move last dentry into found slot (if not the same).
    if (found_idx != total - 1) {
        struct ktfs_dir_entry last;
        unsigned long long lastoff =
            (unsigned long long)(total - 1) * KTFS_DENSZ;
        uint32_t lblk;
        if (resolve_or_alloc(&root, g_root_inode_idx,
                             (uint32_t)(lastoff / KTFS_BLKSZ), 0, &lblk) != 0)
            return -EIO;
        void * d = NULL;
        if (cache_get_block(g_cache, data_blk_to_pos(lblk), &d) != 0)
            return -EIO;
        memcpy(&last, (char *)d + (lastoff % KTFS_BLKSZ), KTFS_DENSZ);
        cache_release_block(g_cache, d, CACHE_CLEAN);

        unsigned long long fndoff =
            (unsigned long long)found_idx * KTFS_DENSZ;
        uint32_t fblk;
        if (resolve_or_alloc(&root, g_root_inode_idx,
                             (uint32_t)(fndoff / KTFS_BLKSZ), 0, &fblk) != 0)
            return -EIO;
        void * fd = NULL;
        if (cache_get_block(g_cache, data_blk_to_pos(fblk), &fd) != 0)
            return -EIO;
        memcpy((char *)fd + (fndoff % KTFS_BLKSZ), &last, KTFS_DENSZ);
        cache_release_block(g_cache, fd, CACHE_DIRTY);
    }
    root.size -= KTFS_DENSZ;
    if (write_inode(g_root_inode_idx, &root) != 0) return -EIO;
    cache_flush(g_cache);
    return 0;
}

// INTERNAL FUNCTION DEFINITIONS

// Read inode /idx/ from the inode-block array.  Each inode is
// KTFS_INOSZ (32) bytes; KTFS_BLKSZ/KTFS_INOSZ = 16 inodes per block.
static int read_inode(uint16_t inode_idx, struct ktfs_inode * out) {
    uint32_t per_block = KTFS_BLKSZ / KTFS_INOSZ;
    uint32_t blk_index = g_first_inode_block + inode_idx / per_block;
    uint32_t blk_off   = (inode_idx % per_block) * KTFS_INOSZ;

    void * blk = NULL;
    if (cache_get_block(g_cache, (unsigned long long)blk_index * KTFS_BLKSZ,
                        &blk) != 0)
        return -EIO;
    memcpy(out, (char *)blk + blk_off, sizeof(struct ktfs_inode));
    cache_release_block(g_cache, blk, CACHE_CLEAN);
    return 0;
}

// KTFS stores data-block numbers as data-block-relative indices, not
// absolute disk block numbers.  Translate to a byte offset that
// cache_get_block can use (block must be aligned to CACHE_BLKSZ).
static unsigned long long data_blk_to_pos(uint32_t data_blk_idx) {
    return (unsigned long long)(g_first_data_block + data_blk_idx) * KTFS_BLKSZ;
}

// Resolve a logical block index (within a file) to its data-block-
// relative number, walking direct -> indirect -> doubly-indirect.
static int resolve_block(const struct ktfs_inode * ino,
                         uint32_t logical_blk,
                         uint32_t * out)
{
    if (logical_blk < KTFS_NUM_DIRECT_DATA_BLOCKS) {
        *out = ino->block[logical_blk];
        return 0;
    }
    logical_blk -= KTFS_NUM_DIRECT_DATA_BLOCKS;

    if (logical_blk < KTFS_PTRS_PER_BLOCK) {
        // Single-indirect.
        void * iblk = NULL;
        if (cache_get_block(g_cache, data_blk_to_pos(ino->indirect),
                            &iblk) != 0)
            return -EIO;
        uint32_t v = ((uint32_t *)iblk)[logical_blk];
        cache_release_block(g_cache, iblk, CACHE_CLEAN);
        *out = v;
        return 0;
    }
    logical_blk -= KTFS_PTRS_PER_BLOCK;

    // Doubly-indirect.
    uint32_t per_indirect = KTFS_PTRS_PER_BLOCK;
    uint32_t per_dind     = per_indirect * KTFS_PTRS_PER_BLOCK;
    if (logical_blk >= per_dind * KTFS_NUM_DINDIRECT_BLOCKS)
        return -EINVAL;

    uint32_t which_dind = logical_blk / per_dind;
    uint32_t into_dind  = logical_blk % per_dind;
    uint32_t which_ind  = into_dind / per_indirect;
    uint32_t into_ind   = into_dind % per_indirect;

    void * dblk = NULL;
    if (cache_get_block(g_cache, data_blk_to_pos(ino->dindirect[which_dind]),
                        &dblk) != 0)
        return -EIO;
    uint32_t indirect_blk = ((uint32_t *)dblk)[which_ind];
    cache_release_block(g_cache, dblk, CACHE_CLEAN);

    void * iblk = NULL;
    if (cache_get_block(g_cache, data_blk_to_pos(indirect_blk),
                        &iblk) != 0)
        return -EIO;
    uint32_t v = ((uint32_t *)iblk)[into_ind];
    cache_release_block(g_cache, iblk, CACHE_CLEAN);

    *out = v;
    return 0;
}

static int data_block_for_offset(const struct ktfs_inode * ino,
                                 unsigned long long byte_off,
                                 uint32_t * data_blk_out) {
    return resolve_block(ino, (uint32_t)(byte_off / KTFS_BLKSZ),
                         data_blk_out);
}

// Walk the root directory's data blocks, looking for a dentry with the
// given name.  Returns 0 + sets *out on success.
static int find_dentry(const char * name, uint16_t * out) {
    struct ktfs_inode root;
    int rc = read_inode(g_root_inode_idx, &root);
    if (rc != 0)
        return rc;
    // Each root dentry occupies KTFS_DENSZ (=16) bytes.  The directory
    // size is dentry_count * 16.
    uint32_t total_dents = root.size / KTFS_DENSZ;

    for (uint32_t d = 0; d < total_dents; d++) {
        unsigned long long byte_off = (unsigned long long)d * KTFS_DENSZ;
        uint32_t blk;
        if (resolve_block(&root, (uint32_t)(byte_off / KTFS_BLKSZ), &blk) != 0)
            return -EIO;
        unsigned long long blk_byte_pos = data_blk_to_pos(blk);
        void * dblk = NULL;
        if (cache_get_block(g_cache, blk_byte_pos, &dblk) != 0)
            return -EIO;
        const struct ktfs_dir_entry * de =
            (const struct ktfs_dir_entry *)((char *)dblk +
                                            (byte_off % KTFS_BLKSZ));

        // Match name (null-terminated within the dentry).
        int match = 1;
        for (int i = 0; i < (int)(KTFS_MAX_FILENAME_LEN + 1); i++) {
            char a = de->name[i];
            char b = name[i];
            if (a != b) { match = 0; break; }
            if (a == '\0') break;
        }
        if (match) {
            *out = de->inode;
            cache_release_block(g_cache, dblk, CACHE_CLEAN);
            return 0;
        }
        cache_release_block(g_cache, dblk, CACHE_CLEAN);
    }
    return -ENOENT;
}
