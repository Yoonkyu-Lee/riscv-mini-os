#!/usr/bin/env bash
# Copyright (c) 2025 Yoonkyu Lee
# SPDX-License-Identifier: NCSA
#
# Boot the kernel under QEMU, run the user-mode benchmark binary, and
# print only the bench report lines. Use to refresh docs/benchmarks.md.

set -euo pipefail

DURATION="${DURATION:-30}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/kernel"

make -s INIT=bench kernel.elf >/dev/null
make -s ktfs.raw   >/dev/null

LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT
timeout "$DURATION" make -s run INIT=bench > "$LOG" 2>&1 || true

# Strip everything except the bench's own report lines.
grep -E '^(riscv-mini-os bench|---|syscall|fs|process)' "$LOG" || {
    echo "no bench output; full log:" >&2
    cat "$LOG" >&2
    exit 1
}
