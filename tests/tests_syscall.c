// Copyright (c) 2025 Yoonkyu Lee
// SPDX-License-Identifier: MIT
//
// tests_syscall.c -- 9 syscall test cases (20pt total)
//
// Test cases: sys_dev (3), sys_del (1), sys_create (1), sys_read (1),
// sys_write (3), sys_ioctl (4), sys_exec (3), sys_usleep (2), sys_exit (1),
// sys_args (1).  10 cases / 20pt total per the rubric.
//
// Strategy: build a minimal trap_frame on the kernel stack, populate
// a0=scnum and a1..a7=args, call handle_syscall(&tfr), then read tfr.a0
// for the syscall return value.  This mirrors the U-mode -> S-mode trap
// path's effect (sscratch swap aside), letting us drive the syscall
// dispatcher without actually entering U-mode.
//
// Pristine starter syscall.c stubs all return 0 / -ENOTSUP.  Our tests
// expect concrete behaviors (positive fd, byte counts matching len, etc.)
// so they fail until the student fills syscall.c in.

#include <stdint.h>
#include <stddef.h>
#include "trap.h"
#include "scnum.h"
#include "string.h"
#include "riscv.h"
#include "test_framework.h"

// Mock-syscall helper.  Models the user ABI: scnum lands in a7, args in
// a0..a5.  This mirrors usr/syscall.S, which puts the syscall number in a7
// before ecall.  Renaming params so the original tests keep working: param
// `a1` -> trap_frame.a0 (first arg), and so on.
static int64_t call_syscall(int scnum,
                            uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6) {
    extern void handle_syscall(struct trap_frame * tfr);
    struct trap_frame tfr = { 0 };
    tfr.a7 = scnum;
    tfr.a0 = (long)a1;
    tfr.a1 = (long)a2;
    tfr.a2 = (long)a3;
    tfr.a3 = (long)a4;
    tfr.a4 = (long)a5;
    tfr.a5 = (long)a6;
    handle_syscall(&tfr);
    return (int64_t)tfr.a0;
}

// ---- individual cases ----------------------------------------------------

static int test_sys_dev(struct test_result *r) {
    int fd = 3;
    int64_t rc = call_syscall(SYSCALL_DEVOPEN, fd,
                              (uint64_t)(uintptr_t)"rng", 0, 0, 0, 0);
    if (rc < 0) {
        r->fail_reason = "sysdevopen('rng') returned an error";
        return 0;
    }
    // Close after to leave fd table clean.
    call_syscall(SYSCALL_CLOSE, fd, 0, 0, 0, 0, 0);
    r->passed = 1;
    return 1;
}

static int test_sys_del(struct test_result *r) {
    int64_t rc = call_syscall(13,    // SYSCALL_FSDELETE per PDF 5.2.6
                              (uint64_t)(uintptr_t)"no_such_file_xyz",
                              0, 0, 0, 0, 0);
    if (rc == 0) {
        r->fail_reason = "sysfsdelete on missing file unexpectedly succeeded";
        return 0;
    }
    r->passed = 1;
    return 1;
}

static int test_sys_create(struct test_result *r) {
    int64_t rc = call_syscall(12,    // SYSCALL_FSCREATE per PDF 5.2.6
                              (uint64_t)(uintptr_t)"sysctest",
                              0, 0, 0, 0, 0);
    if (rc != 0) {
        r->fail_reason = "sysfscreate returned non-zero";
        return 0;
    }
    // Cleanup -- if delete works, drop the file.
    call_syscall(13, (uint64_t)(uintptr_t)"sysctest", 0, 0, 0, 0, 0);
    r->passed = 1;
    return 1;
}

static int test_sys_read(struct test_result *r) {
    int fd = 3;
    int64_t rc = call_syscall(SYSCALL_DEVOPEN, fd,
                              (uint64_t)(uintptr_t)"rng", 0, 0, 0, 0);
    if (rc < 0) {
        r->fail_reason = "sysdevopen('rng') returned an error";
        return 0;
    }
    uint8_t buf[16] = { 0 };
    int64_t n = call_syscall(SYSCALL_READ, fd,
                             (uint64_t)(uintptr_t)buf, sizeof buf,
                             0, 0, 0);
    call_syscall(SYSCALL_CLOSE, fd, 0, 0, 0, 0, 0);
    if (n != (int64_t)sizeof buf) {
        r->fail_reason = "sysread returned wrong byte count";
        return 0;
    }
    r->passed = 1;
    return 1;
}

static int test_sys_write(struct test_result *r) {
    // The bench registers the boot console (UART0) as `console`, since
    // additional UART instances are only available with multi-UART
    // QEMU virt patches.
    int fd = 3;
    int64_t rc = call_syscall(SYSCALL_DEVOPEN, fd,
                              (uint64_t)(uintptr_t)"console", 0, 0, 0, 0);
    if (rc < 0) {
        r->fail_reason = "sysdevopen('console', 0) returned an error";
        return 0;
    }
    static const char msg[] = "syscall-write-test\n";
    int64_t n = call_syscall(SYSCALL_WRITE, fd,
                             (uint64_t)(uintptr_t)msg,
                             sizeof msg - 1, 0, 0, 0);
    call_syscall(SYSCALL_CLOSE, fd, 0, 0, 0, 0, 0);
    if (n != (int64_t)(sizeof msg - 1)) {
        r->fail_reason = "syswrite returned wrong byte count";
        return 0;
    }
    r->passed = 1;
    return 1;
}

#define IOCTL_GETBLKSZ_NUM 0
static int test_sys_ioctl(struct test_result *r) {
    int fd = 3;
    if (call_syscall(SYSCALL_DEVOPEN, fd,
                     (uint64_t)(uintptr_t)"rng", 0, 0, 0, 0) < 0) {
        r->fail_reason = "sysdevopen('rng') returned an error";
        return 0;
    }
    int64_t rc = call_syscall(SYSCALL_IOCTL, fd, IOCTL_GETBLKSZ_NUM,
                              0, 0, 0, 0);
    call_syscall(SYSCALL_CLOSE, fd, 0, 0, 0, 0, 0);
    if (rc < 0) {
        r->fail_reason = "sysioctl(GETBLKSZ) returned an error";
        return 0;
    }
    if (rc <= 0) {
        r->fail_reason = "sysioctl(GETBLKSZ) returned non-positive size";
        return 0;
    }
    r->passed = 1;
    return 1;
}

static int test_sys_exec(struct test_result *r) {
    // Without virtual memory + a real process abstraction, we can't
    // execute a user binary in-place.  Instead probe behavior: does
    // sysexec return *something* sane on a bad fd?  Pristine stubs
    // return 0 (success), which is wrong; correct impl returns < 0.
    int64_t rc = call_syscall(SYSCALL_EXEC, 99, 0, 0, 0, 0, 0);
    if (rc >= 0) {
        r->fail_reason = "sysexec on invalid fd returned non-negative";
        return 0;
    }
    r->passed = 1;
    return 1;
}

static int test_sys_usleep(struct test_result *r) {
    unsigned long long t0 = rdtime();
    int64_t rc = call_syscall(SYSCALL_USLEEP, 1000, 0, 0, 0, 0, 0);
    unsigned long long delta = rdtime() - t0;
    if (rc < 0) {
        r->fail_reason = "sysusleep returned an error";
        return 0;
    }
    // Expect at least ~half a millisecond.  conf.h::TIMER_FREQ was
    // 10MHz (10 ticks per microsecond).
    if (delta < 5000) {
        r->fail_reason = "sysusleep returned too quickly";
        return 0;
    }
    r->passed = 1;
    return 1;
}

// sysexit must not return.  Real production sysexit calls process_exit
// which unmaps the user range and thread_exit's -- in the test bench
// that would halt the whole runner, so we just check the dispatcher
// recognizes SYSCALL_EXIT (returning the canned 0 from the stub when
// process_exit isn't actually invoked).  This becomes a fuller test
// once the runner can sandbox sysexit in a child thread.
static int test_sys_exit(struct test_result *r) {
    // Skip-ish: just confirm SYSCALL_EXIT is dispatched (the stub
    // returns 0; the real impl is noreturn and tested in the user-mode demo).
    r->fail_reason = "sysexit test skipped -- noreturn impl halts runner";
    (void)r;
    return 0;
}

// sys_args: open a file, write+read with non-NULL buffer, verify args
// were passed correctly.  Indirect test -- if argc/argv passing is
// broken in sysexec, this won't catch it directly, but at least it
// exercises 3-arg syscall path.
static int test_sys_args(struct test_result *r) {
    int fd = 3;
    int64_t rc = call_syscall(SYSCALL_DEVOPEN, fd,
                              (uint64_t)(uintptr_t)"rng", 0, 0, 0, 0);
    if (rc < 0) {
        r->fail_reason = "sysdevopen failed";
        return 0;
    }
    uint8_t buf[8] = { 0 };
    int64_t n = call_syscall(SYSCALL_READ, fd,
                             (uint64_t)(uintptr_t)buf, sizeof buf,
                             0, 0, 0);
    call_syscall(SYSCALL_CLOSE, fd, 0, 0, 0, 0, 0);
    if (n != (int64_t)sizeof buf) {
        r->fail_reason = "args weren't propagated to sysread";
        return 0;
    }
    r->passed = 1;
    return 1;
}

static const struct test_entry syscall_tests[] = {
    { "test_sys_dev",     test_sys_dev,     3 },
    { "test_sys_del",     test_sys_del,     1 },
    { "test_sys_create",  test_sys_create,  1 },
    { "test_sys_read",    test_sys_read,    1 },
    { "test_sys_write",   test_sys_write,   3 },
    { "test_sys_ioctl",   test_sys_ioctl,   4 },
    { "test_sys_exec",    test_sys_exec,    3 },
    { "test_sys_usleep",  test_sys_usleep,  2 },
    { "test_sys_exit",    test_sys_exit,    1 },
    { "test_sys_args",    test_sys_args,    1 },
};

const struct test_entry *get_syscall_tests(int *n_out) {
    *n_out = sizeof syscall_tests / sizeof syscall_tests[0];
    return syscall_tests;
}
