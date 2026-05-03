#!/usr/bin/env python3
# Copyright (c) 2025 Yoonkyu Lee
# SPDX-License-Identifier: NCSA
"""mkfs_ktfs - build a KTFS filesystem image.

Usage: mkfs_ktfs <out.raw> <size> <num_inodes> <files...>

Pure-Python replacement for the course-provided x86_64 vendor binary.
The on-disk layout matches what kernel/ktfs.c expects:

    block 0           : superblock {block_count, bitmap_block_count,
                                    inode_block_count, root_directory_inode}
    blocks 1..1+B-1   : bitmap (B = bitmap_block_count) -- bit set = used
    next I blocks     : inodes  (I = inode_block_count, 16 inodes per block)
    rest              : data blocks (block numbers in inode pointers are
                        data-block-relative -- offset by 1 + B + I)

Inode (32 bytes, packed, little-endian):
    size:u32, flags:u32, block[3]:u32, indirect:u32, dindirect[2]:u32

Directory entry (16 bytes, packed):
    inode:u16, name:char[14]   (max 13 ASCII chars + NUL)

Root directory inode index is 0; user files start at inode 1.

The image's allocator is a simple bump pointer over the data area: root
dentry blocks first, then per-file (data then any indirect / dindirect
index pages). Functional parity with the vendor binary is the goal --
byte parity is not.
"""
from __future__ import annotations

import argparse
import struct
import sys

BLKSZ = 512
INOSZ = 32
DENSZ = 16
MAX_FILENAME_LEN = 13                         # KTFS_MAX_FILENAME_LEN
NUM_DIRECT = 3
PTRS_PER_BLOCK = BLKSZ // 4                   # 128
INODES_PER_BLOCK = BLKSZ // INOSZ             # 16
DENTRIES_PER_BLOCK = BLKSZ // DENSZ           # 32
ROOT_INODE = 0


def parse_size(s: str) -> int:
    s = s.strip().upper()
    mul = 1
    if s.endswith("K"):
        mul, s = 1 << 10, s[:-1]
    elif s.endswith("M"):
        mul, s = 1 << 20, s[:-1]
    elif s.endswith("G"):
        mul, s = 1 << 30, s[:-1]
    return int(s) * mul


class Image:
    def __init__(self, size_bytes: int, num_inodes: int):
        if size_bytes <= 0 or size_bytes % BLKSZ != 0:
            raise ValueError(f"size must be a positive multiple of {BLKSZ}")
        if num_inodes < 1:
            raise ValueError("need at least 1 inode (the root)")

        self.block_count = size_bytes // BLKSZ
        self.num_inodes = num_inodes

        bits_per_bmp_blk = BLKSZ * 8
        self.bitmap_block_count = (
            self.block_count + bits_per_bmp_blk - 1
        ) // bits_per_bmp_blk
        self.inode_block_count = (
            num_inodes + INODES_PER_BLOCK - 1
        ) // INODES_PER_BLOCK

        self.first_inode_block = 1 + self.bitmap_block_count
        self.first_data_block = self.first_inode_block + self.inode_block_count
        if self.first_data_block >= self.block_count:
            raise ValueError("image too small for the requested layout")

        self.image = bytearray(size_bytes)
        self.inodes = [bytearray(INOSZ) for _ in range(num_inodes)]
        self.next_block = self.first_data_block
        self.dentries: list[tuple[int, str]] = []   # (inode_idx, name)
        self.root_blocks: list[int] = []            # absolute block indices

        # Mark metadata (superblock + bitmap + inode blocks) used.
        for blk in range(self.first_data_block):
            self._mark_used(blk)

    # ---- block-level primitives ---------------------------------------

    def _mark_used(self, blk: int) -> None:
        byte_idx = blk // 8
        bit = blk % 8
        bmp_blk = byte_idx // BLKSZ
        bmp_off = byte_idx % BLKSZ
        if bmp_blk >= self.bitmap_block_count:
            raise RuntimeError(f"block {blk} outside bitmap range")
        pos = (1 + bmp_blk) * BLKSZ + bmp_off
        self.image[pos] |= 1 << bit

    def _alloc_block(self) -> int:
        if self.next_block >= self.block_count:
            raise RuntimeError("out of data blocks")
        blk = self.next_block
        self.next_block += 1
        self._mark_used(blk)
        return blk

    def _data_rel(self, blk: int) -> int:
        return blk - self.first_data_block

    def _write_block(self, blk: int, data: bytes) -> None:
        if len(data) > BLKSZ:
            raise ValueError("write_block oversize")
        pos = blk * BLKSZ
        self.image[pos : pos + len(data)] = data
        # remainder of bytearray is already zero

    # ---- root directory ----------------------------------------------

    def reserve_root(self, names: list[str]) -> None:
        """Pre-allocate root dentry blocks before any file data."""
        if any(len(n) > MAX_FILENAME_LEN for n in names):
            bad = [n for n in names if len(n) > MAX_FILENAME_LEN]
            raise ValueError(f"filename(s) too long (>13 chars): {bad}")
        ndents = len(names)
        nblks = (ndents * DENSZ + BLKSZ - 1) // BLKSZ
        if nblks > NUM_DIRECT:
            # Could extend to indirect, but no production image hits this.
            raise RuntimeError(
                f"root needs {nblks} dentry blocks; only {NUM_DIRECT} direct slots"
            )
        for _ in range(nblks):
            self.root_blocks.append(self._alloc_block())
        # dentries are filled later via add_file().

    # ---- file inode ---------------------------------------------------

    def add_file(self, name: str, path: str, inode_idx: int) -> None:
        with open(path, "rb") as f:
            data = f.read()
        size = len(data)
        nblks = (size + BLKSZ - 1) // BLKSZ

        direct = [0, 0, 0]
        indirect_ptrs: list[int] = []           # data-rel ptrs for indirect[]
        # dindirect_outer_ptrs[outer_slot][inner_idx] = list of data-rel ptrs
        dindirect_outer_ptrs: list[dict[int, list[int]]] = [{}, {}]

        for logical in range(nblks):
            blk = self._alloc_block()
            chunk = data[logical * BLKSZ : (logical + 1) * BLKSZ]
            self._write_block(blk, chunk)
            rel = self._data_rel(blk)

            if logical < NUM_DIRECT:
                direct[logical] = rel
            elif logical < NUM_DIRECT + PTRS_PER_BLOCK:
                indirect_ptrs.append(rel)
            else:
                dind_off = logical - NUM_DIRECT - PTRS_PER_BLOCK
                outer = dind_off // (PTRS_PER_BLOCK * PTRS_PER_BLOCK)
                inner_idx = (dind_off // PTRS_PER_BLOCK) % PTRS_PER_BLOCK
                if outer >= 2:
                    raise RuntimeError(f"file '{name}' exceeds dindirect capacity")
                dindirect_outer_ptrs[outer].setdefault(inner_idx, []).append(rel)

        indirect_rel = 0
        if indirect_ptrs:
            ind_blk = self._alloc_block()
            buf = bytearray(BLKSZ)
            for i, p in enumerate(indirect_ptrs):
                struct.pack_into("<I", buf, i * 4, p)
            self._write_block(ind_blk, bytes(buf))
            indirect_rel = self._data_rel(ind_blk)

        dindirect_rel = [0, 0]
        for outer in range(2):
            inner_map = dindirect_outer_ptrs[outer]
            if not inner_map:
                continue
            outer_blk = self._alloc_block()
            outer_buf = bytearray(BLKSZ)
            for inner_idx in sorted(inner_map):
                ptrs = inner_map[inner_idx]
                inner_blk = self._alloc_block()
                inner_buf = bytearray(BLKSZ)
                for j, p in enumerate(ptrs):
                    struct.pack_into("<I", inner_buf, j * 4, p)
                self._write_block(inner_blk, bytes(inner_buf))
                struct.pack_into("<I", outer_buf, inner_idx * 4,
                                 self._data_rel(inner_blk))
            self._write_block(outer_blk, bytes(outer_buf))
            dindirect_rel[outer] = self._data_rel(outer_blk)

        ino = struct.pack(
            "<" + "I" * 8,
            size, 0,
            direct[0], direct[1], direct[2],
            indirect_rel,
            dindirect_rel[0], dindirect_rel[1],
        )
        assert len(ino) == INOSZ, f"inode pack {len(ino)} != {INOSZ}"
        self.inodes[inode_idx] = bytearray(ino)
        self.dentries.append((inode_idx, name))

        print(f"Added file {name} to inode {inode_idx}")
        print(f"File size: {size} bytes")
        if direct[0] or nblks == 0:
            print(f"File start data block index: {direct[0]}")
            print(f"File start address: {(self.first_data_block + direct[0]) * BLKSZ}")

    # ---- finalization -------------------------------------------------

    def finalize(self) -> None:
        # Root inode + dentry blocks
        ndents = len(self.dentries)
        if ndents and not self.root_blocks:
            raise RuntimeError("must call reserve_root() before finalize()")
        if ndents > len(self.root_blocks) * DENTRIES_PER_BLOCK:
            raise RuntimeError("dentry count exceeds reserved root capacity")

        dentry_buf = bytearray(len(self.root_blocks) * BLKSZ)
        for i, (ino_idx, name) in enumerate(self.dentries):
            off = i * DENSZ
            name_bytes = name.encode("ascii") + b"\x00"
            struct.pack_into("<H", dentry_buf, off, ino_idx)
            dentry_buf[off + 2 : off + 2 + len(name_bytes)] = name_bytes

        direct = [0, 0, 0]
        for i, blk in enumerate(self.root_blocks):
            self._write_block(blk, bytes(dentry_buf[i * BLKSZ : (i + 1) * BLKSZ]))
            direct[i] = self._data_rel(blk)
        root_size = ndents * DENSZ
        root_ino = struct.pack(
            "<" + "I" * 8,
            root_size, 0,
            direct[0], direct[1], direct[2],
            0, 0, 0,
        )
        self.inodes[ROOT_INODE] = bytearray(root_ino)

        # Inode blocks
        for ino_idx in range(self.num_inodes):
            blk = self.first_inode_block + ino_idx // INODES_PER_BLOCK
            off = (ino_idx % INODES_PER_BLOCK) * INOSZ
            pos = blk * BLKSZ + off
            self.image[pos : pos + INOSZ] = bytes(self.inodes[ino_idx])

        # Superblock (block 0)
        sb = struct.pack(
            "<IIIH",
            self.block_count,
            self.bitmap_block_count,
            self.inode_block_count,
            ROOT_INODE,
        )
        self._write_block(0, sb)

    def to_bytes(self) -> bytes:
        return bytes(self.image)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(
        description="Build a KTFS filesystem image.",
        usage="%(prog)s <out.raw> <size> <num_inodes> <files...>",
    )
    p.add_argument("output", help="output image path (e.g. ktfs.raw)")
    p.add_argument("size", help="image size with optional suffix K/M/G (e.g. 16M)")
    p.add_argument("num_inodes", type=int, help="total number of inodes (>=1)")
    p.add_argument("files", nargs="+", help="files to add (looked up in cwd)")
    args = p.parse_args(argv)

    if len(args.files) > args.num_inodes - 1:
        sys.exit(
            f"too many files ({len(args.files)}) for {args.num_inodes} "
            f"inodes (root takes 1)"
        )

    img = Image(parse_size(args.size), args.num_inodes)
    img.reserve_root(args.files)
    for i, fname in enumerate(args.files, start=1):
        img.add_file(fname, fname, i)
    img.finalize()

    with open(args.output, "wb") as f:
        f.write(img.to_bytes())

    print(f"Filesystem image created successfully: {args.output}")
    print(f"Disk size: {len(img.to_bytes())} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
