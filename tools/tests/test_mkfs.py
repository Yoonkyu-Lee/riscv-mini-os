# Copyright (c) 2025 Yoonkyu Lee
# SPDX-License-Identifier: NCSA
"""Round-trip tests for mkfs_ktfs.py / unmkfs_ktfs.py.

Run with: python3 -m unittest discover tools/tests

Each test packs files into a KTFS image and reads them back via the
in-process KTFS reader (which mirrors kernel/ktfs.c's mount logic). If
byte-for-byte equality holds across mkfs -> unmkfs -> compare, the image
is functionally valid -- the same property the kernel relies on.
"""
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))   # tools/

import mkfs_ktfs                              # noqa: E402
from unmkfs_ktfs import KTFS                  # noqa: E402


def _mkfs(out_path: str, size: str, n_inodes: int, files: list[tuple[str, bytes]],
          tmp: str) -> None:
    """Materialize files into tmp/ and run mkfs_ktfs.main()."""
    paths = []
    cwd = os.getcwd()
    os.chdir(tmp)
    try:
        for name, content in files:
            with open(name, "wb") as f:
                f.write(content)
            paths.append(name)
        rc = mkfs_ktfs.main([out_path, size, str(n_inodes), *paths])
        if rc != 0:
            raise RuntimeError(f"mkfs returned {rc}")
    finally:
        os.chdir(cwd)


def _roundtrip(self, files, size="1M", n_inodes=16):
    with tempfile.TemporaryDirectory() as tmp:
        img_path = os.path.join(tmp, "out.raw")
        _mkfs(img_path, size, n_inodes, files, tmp)
        with open(img_path, "rb") as f:
            fs = KTFS(f.read())
        listing = fs.list_root()
        self.assertEqual(
            sorted(name for _, name in listing),
            sorted(name for name, _ in files),
        )
        by_name = {name: idx for idx, name in listing}
        for name, expected in files:
            ino = fs.read_inode(by_name[name])
            self.assertEqual(ino["size"], len(expected), f"{name}: size mismatch")
            got = fs.read_file(ino)
            self.assertEqual(got, expected, f"{name}: content mismatch")


class TestMkfsRoundtrip(unittest.TestCase):
    def test_single_small_file(self):
        _roundtrip(self, [("hi.txt", b"hello world\n")])

    def test_multiple_small_files(self):
        _roundtrip(self, [
            ("a.txt", b"A" * 100),
            ("b.txt", b"B" * 200),
            ("c.txt", b"C" * 300),
        ])

    def test_block_aligned(self):
        _roundtrip(self, [("aligned.bin", b"X" * 512)])

    def test_two_block_file(self):
        _roundtrip(self, [("two.bin", b"Y" * 1024)])

    def test_direct_to_indirect_boundary(self):
        # 4 blocks: pushes one block into indirect[]
        _roundtrip(self, [("dir2ind.bin", bytes(range(256)) * 8)])

    def test_indirect_full(self):
        # 3 direct + 128 indirect = 131 blocks worth of data (just over 64KB)
        body = b"".join(bytes([i & 0xff]) * 512 for i in range(131))
        _roundtrip(self, [("ind.bin", body)])

    def test_dindirect_modest(self):
        # Push a few blocks into dindirect (logical 131..)
        # 140 blocks ~ 70KB
        body = b"".join(bytes([i & 0xff]) * 512 for i in range(140))
        _roundtrip(self, [("dind.bin", body)], size="2M")

    def test_realistic_user_binary_layout(self):
        # Mirror the production ktfs.raw shape: ~78KB user binaries.
        files = [(f"bin{i}", os.urandom(78864)) for i in range(4)]
        _roundtrip(self, files, size="16M", n_inodes=32)


if __name__ == "__main__":
    unittest.main()
