// Copyright (c) 2025 Yoonkyu Lee
// SPDX-License-Identifier: NCSA
//
// test_main.c -- entry point for the kernel test bench
//
// Boots the kernel infrastructure (intr, plic, devmgr, heap, thread,
// timer, rtc, uart, viorng), then runs the registered test suites
// (memio / elf / cache / vioblk / ktfs / vm / syscall / ktfsrw,
// ~70 pt total) and prints a summary.

#include <stdint.h>
#include "conf.h"
#include "console.h"
#include "see.h"
#include "intr.h"
#include "device.h"
#include "heap.h"
#include "thread.h"
#include "timer.h"
#include "memory.h"
#include "process.h"
#include "dev/rtc.h"
#include "dev/virtio.h"
#include "dev/uart.h"
#include "test_framework.h"

extern char _kimg_end[];
extern void virtio_attach(void *mmio_base, int irqno);
extern void uart_attach(void *mmio_base, int irqno);

// ---- recovery smoke test -------------------------------------------------
//
// Arm test_recover_buf, deref NULL, expect to land in the longjmp
// branch with cause/sepc/stval captured.

static void recovery_smoke_test(void) {
    kprintf("[recovery smoke test]\n");

    volatile int * null_ptr = (volatile int *)0;

    if (test_setjmp(&test_recover_buf) == 0) {
        test_recover_armed = 1;
        int unused = *null_ptr;            // expected to fault
        (void)unused;
        test_recover_armed = 0;
        kprintf("  FAIL: NULL deref did not fault\n\n");
        return;
    }
    kprintf("  recovered: cause=%d sepc=0x%lx stval=0x%lx\n",
            test_recover_cause,
            (long)test_recover_sepc,
            (long)test_recover_stval);
    kprintf("  PASS\n\n");
}

// ---- clobber wrapper smoke test -----------------------------------------

extern void clobber_test_good(void);
extern void clobber_test_bad_s0(void);

static void clobber_smoke_test(void) {
    kprintf("[clobber smoke test]\n");

    clobber_mask = 0;
    clobber_call((void (*)())clobber_test_good, 0, 0, 0, 0, 0, 0);
    if (clobber_mask == 0)
        kprintf("  good fn:  mask=0x%x  PASS\n", (unsigned)clobber_mask);
    else
        kprintf("  good fn:  mask=0x%x  FAIL (false positive)\n",
                (unsigned)clobber_mask);

    clobber_mask = 0;
    clobber_call((void (*)())clobber_test_bad_s0, 0, 0, 0, 0, 0, 0);
    if (clobber_mask & 0x1)
        kprintf("  bad  fn:  mask=0x%x  PASS (s0 clobber detected)\n",
                (unsigned)clobber_mask);
    else
        kprintf("  bad  fn:  mask=0x%x  FAIL (missed s0 clobber)\n",
                (unsigned)clobber_mask);
    kprintf("\n");
}

void main(void) {
    console_init();

    kprintf("\n");
    kprintf("==========================================\n");
    kprintf("[riscv-mini-os test bench]\n");
    kprintf("==========================================\n");
    kprintf("\n");

    // Kernel init -- bring up enough infrastructure to register the
    // device tree and probe drivers before the test suites run.
    intrmgr_init();
    devmgr_init();
    // memory_init does heap_init internally and turns on paging via satp.
    // (PDF 5.2.6: heap_init removed from main.c once memory.c is wired.)
    memory_init();
    thrmgr_init();
    timer_init();
    procmgr_init();

    rtc_attach((void *)RTC_MMIO_BASE);
    uart_attach((void *)UART1_MMIO_BASE, UART0_INTR_SRCNO + 1);

    // Attach 8 VirtIO MMIO slots.  vioblk_attach is a weak no-op until
    // Phase C links the real one in; viorng works out of the box.
    for (int i = 0; i < 8; i++)
        virtio_attach((void *)VIRTIO_MMIO_BASE(i), VIRTIO0_INTR_SRCNO + i);

    enable_interrupts();

    recovery_smoke_test();
    clobber_smoke_test();

    // ---- run registered tests ------------------------------------------
    int score = 0, max = 0;
    int n;
    const struct test_entry *e;
    extern volatile int  test_clobber_penalty_applied;
    extern const char   *test_first_clobber_test;

    e = get_locks_tests(&n);
    run_test_group("Locks tests", e, n, &score, &max);

    e = get_memio_tests(&n);
    run_test_group("Memio tests", e, n, &score, &max);

    e = get_elf_tests(&n);
    run_test_group("ELF tests", e, n, &score, &max);

    e = get_cache_tests(&n);
    run_test_group("Cache tests", e, n, &score, &max);

    e = get_vioblk_tests(&n);
    run_test_group("VirtIO Block tests", e, n, &score, &max);

    e = get_ktfs_tests(&n);
    run_test_group("KTFS tests", e, n, &score, &max);

    e = get_vm_tests(&n);
    run_test_group("VM tests", e, n, &score, &max);

    e = get_syscall_tests(&n);
    run_test_group("Syscall tests", e, n, &score, &max);

    e = get_ktfsrw_tests(&n);
    run_test_group("KTFS RW tests", e, n, &score, &max);

    int penalty = test_clobber_penalty_applied ? 5 : 0;
    kprintf("==========================================\n");
    kprintf("Functionality: %d/%d\n", score, max);
    if (penalty)
        kprintf("Penalties: -%d (CLOBBER, first triggered by %s)\n",
                penalty, test_first_clobber_test ? test_first_clobber_test : "?");
    else
        kprintf("Penalties: none\n");
    kprintf("Total: %d/%d\n", score - penalty, max);
    kprintf("==========================================\n");

    halt_success();
}
