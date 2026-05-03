#!/usr/bin/env bash
# Copyright (c) 2025 Yoonkyu Lee
# SPDX-License-Identifier: MIT
#
# Install the toolchain riscv-mini-os needs on Ubuntu 22.04 / 24.04.
# Idempotent. Requires sudo for apt.

set -euo pipefail

PKGS=(
    build-essential
    gcc-riscv64-unknown-elf
    binutils-riscv64-unknown-elf
    qemu-system-misc
    gdb-multiarch
    python3
    make
)

if ! command -v apt-get >/dev/null 2>&1; then
    echo "This script targets Ubuntu/Debian (apt). On other distros, install" >&2
    echo "equivalents of: ${PKGS[*]}" >&2
    exit 1
fi

echo ">> apt-get update"
sudo apt-get update -y

echo ">> apt-get install: ${PKGS[*]}"
sudo apt-get install -y --no-install-recommends "${PKGS[@]}"

echo ">> verify"
riscv64-unknown-elf-gcc --version | head -1
qemu-system-riscv64 --version | head -1
python3 --version
echo "OK"
