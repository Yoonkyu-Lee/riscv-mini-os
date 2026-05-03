// Copyright (c) 2025 Yoonkyu Lee
// SPDX-License-Identifier: MIT
//
// bench - microbenchmark for fork+wait, syscall, and KTFS open cost.
//
// Reads the riscv `time` CSR directly (the kernel sets mcounteren and
// scounteren so U-mode access is permitted). The QEMU virt timer ticks
// at 10 MHz, so one tick ~= 100 ns; the report below is shown in
// nanoseconds so the numbers are intuitive.
//
// Build & run:
//     make -C kernel run INIT=bench
//
// The numbers are not directly comparable to bare-metal silicon; QEMU
// timing is host-CPU dependent and includes virtualization overhead.
// The point is to expose the relative cost of each path.

#include <stddef.h>
#include "syscall.h"
#include "string.h"

#define ITERS_FORK     100
#define ITERS_PRINT   1000
#define ITERS_FSOPEN   200

static inline unsigned long rdtime(void) {
    unsigned long t;
    __asm__ __volatile__("csrr %0, time" : "=r"(t));
    return t;
}

static void report(const char * label, unsigned long ticks, int iters) {
    // Tick @ 10 MHz -> 100 ns; report ns/iter.
    unsigned long total_ns = ticks * 100;
    unsigned long per_ns   = iters ? (total_ns / iters) : 0;
    char buf[80];
    snprintf(buf, sizeof buf, "%s  iters=%d  total=%lu ns  per=%lu ns\n",
             label, iters, total_ns, per_ns);
    _print(buf);
}

static void bench_print(void) {
    unsigned long t0 = rdtime();
    for (int i = 0; i < ITERS_PRINT; i++)
        _print("");                           // empty syscall round-trip
    unsigned long t1 = rdtime();
    report("syscall  (_print empty)", t1 - t0, ITERS_PRINT);
}

static void bench_fork_wait(void) {
    unsigned long t0 = rdtime();
    for (int i = 0; i < ITERS_FORK; i++) {
        int tid = _fork();
        if (tid == 0) {
            _exit();
        }
        _wait(tid);
    }
    unsigned long t1 = rdtime();
    report("process  (_fork + _wait)", t1 - t0, ITERS_FORK);
}

static void bench_fsopen(void) {
    unsigned long t0 = rdtime();
    for (int i = 0; i < ITERS_FSOPEN; i++) {
        int fd = _fsopen(-1, "fib");
        if (fd < 0) {
            _print("bench: _fsopen failed\n");
            return;
        }
        _close(fd);
    }
    unsigned long t1 = rdtime();
    report("fs       (_fsopen + _close)", t1 - t0, ITERS_FSOPEN);
}

void main(void) {
    _print("riscv-mini-os bench\n");
    _print("-------------------\n");
    bench_print();
    bench_fsopen();
    bench_fork_wait();
    _exit();
}
