#!/usr/bin/env bash
# Copyright (c) 2025 Yoonkyu Lee
# SPDX-License-Identifier: NCSA
#
# Boot the kernel under QEMU and exit cleanly after a fixed wall-clock
# window. Wrapped by the asciinema recorder when capturing the README
# demo GIF.
#
# Usage:
#   scripts/demo.sh [INIT=trekfib] [DURATION=20]
#
# Recording a GIF (one-time, requires sudo + asciinema + agg):
#   sudo apt-get install -y asciinema && cargo install --locked agg
#   asciinema rec docs/img/demo.cast -c "scripts/demo.sh" --overwrite
#   agg --speed 1.5 docs/img/demo.cast docs/img/demo.gif

set -euo pipefail

INIT="${INIT:-trekfib}"
DURATION="${DURATION:-20}"

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT/kernel"

echo ">> building kernel + ktfs.raw (one-shot)"
make -s kernel.elf >/dev/null
make -s ktfs.raw   >/dev/null

echo ">> booting QEMU virt with INIT=$INIT (timeout ${DURATION}s)"
echo "----------------------------------------------------------"
timeout "$DURATION" make -s run INIT="$INIT" || true
echo "----------------------------------------------------------"
echo ">> done"
