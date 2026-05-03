# Benchmarks

Microbenchmark for three primitive kernel paths. Source:
[`user/bench.c`](../user/bench.c). Driver: [`scripts/bench.sh`](../scripts/bench.sh).

## How it's measured

- The user binary reads the RISC-V `time` CSR directly (the kernel
  enables `mcounteren` and `scounteren` in its M-mode shim, so U-mode
  access is permitted).
- The QEMU `virt` timer ticks at 10 MHz, so one tick = 100 ns.
- For each path we time `N` iterations end-to-end and report the per-
  iteration cost.

## Numbers from a single QEMU run

QEMU is host-CPU bound and includes virtualization overhead. Treat
these as relative cost ratios, not bare-metal silicon numbers.

```
$ scripts/bench.sh
riscv-mini-os bench
-------------------
syscall  (_print empty)        iters=1000  per=     4 511 ns   (~4.5 µs)
fs       (_fsopen + _close)    iters= 200  per=    18 755 ns   (~19  µs)
process  (_fork + _wait)       iters= 100  per= 1 291 909 ns   (~1.3 ms)
```

## What the numbers say

- An empty `_print` round-trip costs ~4.5 µs. That's the lower bound on
  any syscall: trap save/restore + the dispatcher switch cost.
- `_fsopen + _close` is ~4× a bare syscall. The extra cost is one
  KTFS dentry walk through the root directory's first direct block,
  plus inode read.
- `_fork + _wait` dominates by three orders of magnitude. Each fork
  eagerly copies every PTE_U user page (no copy-on-write); for the
  bench process that's just `start.s` and the linked runtime, but it
  still touches the entire user mapping. `_wait` then blocks until the
  child schedules and exits.

The fork/exec gap is the obvious place to spend more cycles if the
kernel needed to scale past a handful of user processes -- the
trade-off was simplicity over working-set efficiency, called out in
[`DESIGN.md`](../DESIGN.md#process-model-fork--exec--wait).
