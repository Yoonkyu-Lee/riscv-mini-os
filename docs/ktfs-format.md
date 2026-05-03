# KTFS on-disk format

Reference for the fixed-block, fixed-inode filesystem used by
riscv-mini-os. The Python implementation
([`tools/mkfs_ktfs.py`](../tools/mkfs_ktfs.py)) is the build-side
source of truth; the kernel reader/writer is in
[`kernel/ktfs.c`](../kernel/ktfs.c).

## Constants

| Symbol                        | Value | Notes                              |
| ----------------------------- | ----- | ---------------------------------- |
| `KTFS_BLKSZ`                  | 512   | bytes per block                    |
| `KTFS_INOSZ`                  | 32    | bytes per inode (16 inodes/block)  |
| `KTFS_DENSZ`                  | 16    | bytes per dentry (32 dentries/block) |
| `KTFS_MAX_FILENAME_LEN`       | 13    | ASCII chars (+ NUL terminator)     |
| `KTFS_NUM_DIRECT_DATA_BLOCKS` | 3     | direct ptrs per inode              |
| `KTFS_NUM_INDIRECT_BLOCKS`    | 1     | indirect index slot per inode      |
| `KTFS_NUM_DINDIRECT_BLOCKS`   | 2     | dindirect index slots per inode    |
| pointers per index page       | 128   | `u32` × 128 = 512 B                |

## Image layout

```
Block index      Contents
-----------      --------
0                Superblock (padded to 512 B)
1 .. B           Bitmap blocks (B = bitmap_block_count)
B+1 .. B+I       Inode blocks (I = inode_block_count, 16 inodes/block)
B+I+1 .. end     Data blocks
```

For a 16 MB image with 32 inodes:
- `block_count = 32 768`
- `bitmap_block_count = 8` (covers 8 × 4 096 = 32 768 bits)
- `inode_block_count = 2`
- `first_data_block = 11`

## Superblock (block 0)

```c
struct ktfs_superblock {
    uint32_t block_count;
    uint32_t bitmap_block_count;
    uint32_t inode_block_count;
    uint16_t root_directory_inode;
} __attribute__((packed));
```

The remaining bytes of block 0 are zero. `root_directory_inode` is
always inode 0 in this implementation.

## Bitmap

A flat bit-array indexed by absolute block number, split across
`bitmap_block_count` blocks. Bit `b` of byte `i` in bitmap block `m`
covers absolute block `m × 4096 + i × 8 + b`. **Bit set = used.**

`mkfs_ktfs` pre-marks the metadata blocks (superblock + bitmap blocks +
inode blocks) as used; the kernel allocator only walks the data area
(`alloc_data_block` skips block numbers below `first_data_block`).

## Inode (32 B, packed)

```c
struct ktfs_inode {
    uint32_t size;                                  // bytes
    uint32_t flags;                                 // unused
    uint32_t block[KTFS_NUM_DIRECT_DATA_BLOCKS];    // direct
    uint32_t indirect;                              // indirect index page
    uint32_t dindirect[KTFS_NUM_DINDIRECT_BLOCKS];  // dindirect index pages
} __attribute__((packed));
```

All `u32` block pointers are *data-relative*: absolute block =
`first_data_block + relative_index`. A pointer of `0` is a sentinel
only when the corresponding logical block is past `size`; the on-disk
layout permits relative `0` as a real pointer (the root directory itself
ordinarily starts at data-rel `0`).

### Logical block addressing

```
logical_blk in [0, 3)               -> direct[logical_blk]
logical_blk in [3, 131)             -> indirect_page[logical_blk - 3]
logical_blk in [131, 16515)         -> dindirect[0]
logical_blk in [16515, 32899)       -> dindirect[1]
```

For dindirect, the outer page indexes 128 inner index pages, each
indexing 128 data blocks (so `128 × 128 = 16 384` logical blocks per
dindirect slot).

## Directory entry (16 B, packed)

```c
struct ktfs_dir_entry {
    uint16_t inode;
    char     name[14];   // 13 chars + NUL
} __attribute__((packed));
```

Directories are inodes whose data area is a packed array of dentries.
The directory's inode `size` equals `dentry_count × 16`. A name shorter
than 13 chars is NUL-terminated; comparisons stop at the first mismatch
or NUL.

The root directory uses up to three direct blocks for dentries
(96 entries max in the current `mkfs_ktfs.py`; the kernel reader will
honour indirect/dindirect chains too if a writer extends past the
direct blocks).

## Allocation order in `mkfs_ktfs.py`

1. Mark metadata blocks (`0 .. first_data_block - 1`) as used.
2. Reserve enough data blocks for the root directory's dentries.
3. For each input file, in argv order:
   - allocate the file's data blocks linearly (logical 0, 1, 2, ...);
   - allocate any indirect / dindirect index pages on demand.
4. Write the root inode (inode 0) and per-file inodes (1..N).
5. Pack the superblock into block 0.

The functional contract — "the kernel reader can mount the image, list
the root directory, and round-trip every file" — is asserted by
`tools/tests/test_mkfs.py`.
