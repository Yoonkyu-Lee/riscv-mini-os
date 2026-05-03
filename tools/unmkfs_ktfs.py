#!/usr/bin/env python3
# Copyright (c) 2025 Yoonkyu Lee
# SPDX-License-Identifier: MIT
"""unmkfs_ktfs - extract files from a KTFS filesystem image.

Usage: unmkfs_ktfs <image.raw> [--list | --out <dir>]

Pure-Python KTFS reader that mirrors what kernel/ktfs.c does on mount.
Used as a round-trip oracle for tools/mkfs_ktfs.py.
"""
from __future__ import annotations

import argparse
import os
import struct
import sys

BLKSZ = 512
INOSZ = 32
DENSZ = 16
MAX_FILENAME_LEN = 13
NUM_DIRECT = 3
PTRS_PER_BLOCK = BLKSZ // 4
INODES_PER_BLOCK = BLKSZ // INOSZ


class KTFS:
    def __init__(self, image: bytes):
        self.image = image
        bc, bbc, ibc = struct.unpack_from("<III", image, 0)
        root = struct.unpack_from("<H", image, 12)[0]
        self.block_count = bc
        self.bitmap_block_count = bbc
        self.inode_block_count = ibc
        self.root_inode_idx = root
        self.first_inode_block = 1 + bbc
        self.first_data_block = self.first_inode_block + ibc
        if len(image) != bc * BLKSZ:
            raise ValueError(
                f"image size {len(image)} != block_count*BLKSZ {bc * BLKSZ}"
            )

    def read_inode(self, idx: int) -> dict:
        blk = self.first_inode_block + idx // INODES_PER_BLOCK
        off = (idx % INODES_PER_BLOCK) * INOSZ
        pos = blk * BLKSZ + off
        size, flags, b0, b1, b2, ind, d0, d1 = struct.unpack_from(
            "<" + "I" * 8, self.image, pos
        )
        return dict(
            size=size, flags=flags,
            direct=(b0, b1, b2), indirect=ind, dindirect=(d0, d1),
        )

    def _data_block(self, rel: int) -> bytes:
        pos = (self.first_data_block + rel) * BLKSZ
        return self.image[pos : pos + BLKSZ]

    def _ptrs(self, rel: int) -> list[int]:
        blk = self._data_block(rel)
        return list(struct.unpack_from("<" + "I" * PTRS_PER_BLOCK, blk, 0))

    def resolve_block(self, ino: dict, logical: int) -> int:
        if logical < NUM_DIRECT:
            return ino["direct"][logical]
        logical -= NUM_DIRECT
        if logical < PTRS_PER_BLOCK:
            return self._ptrs(ino["indirect"])[logical]
        logical -= PTRS_PER_BLOCK
        outer = logical // (PTRS_PER_BLOCK * PTRS_PER_BLOCK)
        rest = logical % (PTRS_PER_BLOCK * PTRS_PER_BLOCK)
        inner_idx = rest // PTRS_PER_BLOCK
        leaf_idx = rest % PTRS_PER_BLOCK
        if outer >= 2:
            raise IndexError("logical block beyond dindirect capacity")
        outer_ptrs = self._ptrs(ino["dindirect"][outer])
        inner_ptrs = self._ptrs(outer_ptrs[inner_idx])
        return inner_ptrs[leaf_idx]

    def read_file(self, ino: dict) -> bytes:
        size = ino["size"]
        nblks = (size + BLKSZ - 1) // BLKSZ
        out = bytearray()
        for i in range(nblks):
            blk = self._data_block(self.resolve_block(ino, i))
            out += blk
        return bytes(out[:size])

    def list_root(self) -> list[tuple[int, str]]:
        root = self.read_inode(self.root_inode_idx)
        ndents = root["size"] // DENSZ
        entries = []
        raw = self.read_file(root)   # root contents = packed dentries
        for i in range(ndents):
            off = i * DENSZ
            inode = struct.unpack_from("<H", raw, off)[0]
            name = raw[off + 2 : off + 2 + MAX_FILENAME_LEN + 1].split(b"\x00")[0]
            entries.append((inode, name.decode("ascii", "replace")))
        return entries


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="Extract files from a KTFS image.")
    p.add_argument("image")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--list", action="store_true", help="list files only")
    g.add_argument("--out", default=None, help="extract files into this dir")
    args = p.parse_args(argv)

    with open(args.image, "rb") as f:
        fs = KTFS(f.read())

    entries = fs.list_root()
    for ino_idx, name in entries:
        ino = fs.read_inode(ino_idx)
        print(f"inode {ino_idx:3d}  {ino['size']:>10d}  {name}")
        if args.out:
            os.makedirs(args.out, exist_ok=True)
            with open(os.path.join(args.out, name), "wb") as f:
                f.write(fs.read_file(ino))
    return 0


if __name__ == "__main__":
    sys.exit(main())
