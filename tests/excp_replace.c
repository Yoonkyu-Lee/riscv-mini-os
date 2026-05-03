// Copyright (c) 2025 Yoonkyu Lee
// SPDX-License-Identifier: NCSA
//
// excp_replace.c -- drop-in replacement for excp.c with fault recovery
//
// This module replaces excp.o for test.elf.  It exports the same
// `handle_smode_exception` symbol so trap.s dispatches into our version,
// adding a recoverable path so individual tests can cause synchronous
// faults (NULL deref, misalignment, etc.) without bringing down the
// whole bench.
//
// Recovery protocol (mirrors mp1 test bench):
//   1. Test runner calls test_setjmp(&test_recover_buf) -- saves frame.
//   2. Runner sets test_recover_armed = 1 just before invoking the
//      function under test.
//   3. If the function faults, trap.s pushes a trap frame and calls
//      handle_smode_exception.  We see armed != 0, capture cause + sepc,
//      and longjmp back to the runner.
//   4. Runner clears test_recover_armed after the call.

#include <stdint.h>
#include <stddef.h>
#include "trap.h"
#include "riscv.h"
#include "console.h"
#include "string.h"
#include "assert.h"
#include "test_framework.h"

// ---- recovery state ------------------------------------------------------

volatile int      test_recover_armed = 0;
volatile int      test_recover_cause = 0;
volatile uint64_t test_recover_sepc  = 0;
volatile uint64_t test_recover_stval = 0;
struct test_jmp_buf test_recover_buf;

// ---- exception names (for non-recovered fall-through) -------------------

static const char * const excp_names[] = {
    [RISCV_SCAUSE_INSTR_ADDR_MISALIGNED]  = "Misaligned instruction address",
    [RISCV_SCAUSE_INSTR_ACCESS_FAULT]     = "Instruction access fault",
    [RISCV_SCAUSE_ILLEGAL_INSTR]          = "Illegal instruction",
    [RISCV_SCAUSE_BREAKPOINT]             = "Breakpoint",
    [RISCV_SCAUSE_LOAD_ADDR_MISALIGNED]   = "Misaligned load address",
    [RISCV_SCAUSE_LOAD_ACCESS_FAULT]      = "Load access fault",
    [RISCV_SCAUSE_STORE_ADDR_MISALIGNED]  = "Misaligned store address",
    [RISCV_SCAUSE_STORE_ACCESS_FAULT]     = "Store access fault",
    [RISCV_SCAUSE_ECALL_FROM_UMODE]       = "Environment call from U mode",
    [RISCV_SCAUSE_ECALL_FROM_SMODE]       = "Environment call from S mode",
    [RISCV_SCAUSE_INSTR_PAGE_FAULT]       = "Instruction page fault",
    [RISCV_SCAUSE_LOAD_PAGE_FAULT]        = "Load page fault",
    [RISCV_SCAUSE_STORE_PAGE_FAULT]       = "Store page fault",
};

// ---- handle_smode_exception ---------------------------------------------
//
// Called from trap.s on any synchronous S-mode exception.  If recovery is
// armed, capture cause/sepc/stval into globals and longjmp back to the
// runner.  Otherwise fall through to the original panic-with-message
// behavior.

// Forward decl from memory.c -- our excp_replace tries lazy alloc on
// page faults inside the user VMA range (PDF 5.2.2) before falling
// through to the recovery / panic path.  Mirrors how the production
// excp.c is expected to be wired.
extern int handle_umode_page_fault(struct trap_frame * tfr, uintptr_t vma);

void handle_smode_exception(unsigned int cause, struct trap_frame * tfr) {
    if (cause == RISCV_SCAUSE_LOAD_PAGE_FAULT  ||
        cause == RISCV_SCAUSE_STORE_PAGE_FAULT ||
        cause == RISCV_SCAUSE_INSTR_PAGE_FAULT)
    {
        uintptr_t vma = csrr_stval();
        if (handle_umode_page_fault(tfr, vma))
            return;   // resolved -- sret will retry the faulting insn
    }
    if (test_recover_armed) {
        test_recover_cause  = (int)cause;
        test_recover_sepc   = (uint64_t)tfr->sepc;
        test_recover_stval  = csrr_stval();
        test_recover_armed  = 0;            // disarm before unwinding
        // Trap entry hardware-cleared SIE.  Normally sret would copy SPIE
        // back to SIE, but we longjmp out instead -- so re-enable globally
        // here, otherwise the runner returns to test code with interrupts
        // permanently masked (breaks any cv-based driver wait).
        asm volatile ("csrsi sstatus, %0" :: "I" (RISCV_SSTATUS_SIE));
        test_longjmp(&test_recover_buf, 1);
        // unreachable
    }

    // Not armed -- replicate the original informative panic.
    const char * name = NULL;
    char msgbuf[80];

    if (cause < sizeof(excp_names)/sizeof(excp_names[0]))
        name = excp_names[cause];

    if (name != NULL) {
        switch (cause) {
        case RISCV_SCAUSE_LOAD_PAGE_FAULT:
        case RISCV_SCAUSE_STORE_PAGE_FAULT:
        case RISCV_SCAUSE_INSTR_PAGE_FAULT:
        case RISCV_SCAUSE_LOAD_ADDR_MISALIGNED:
        case RISCV_SCAUSE_STORE_ADDR_MISALIGNED:
        case RISCV_SCAUSE_INSTR_ADDR_MISALIGNED:
        case RISCV_SCAUSE_LOAD_ACCESS_FAULT:
        case RISCV_SCAUSE_STORE_ACCESS_FAULT:
        case RISCV_SCAUSE_INSTR_ACCESS_FAULT:
            snprintf(msgbuf, sizeof(msgbuf),
                "%s at %p for %p in S mode",
                name, (void *)tfr->sepc, (void *)csrr_stval());
            break;
        default:
            snprintf(msgbuf, sizeof(msgbuf),
                "%s at %p in S mode",
                name, (void *)tfr->sepc);
        }
    } else {
        snprintf(msgbuf, sizeof(msgbuf),
            "Exception %u at %p in S mode",
            cause, (void *)tfr->sepc);
    }

    panic(msgbuf);
}

// U-mode exception path.  Same recovery pattern as the S-mode
// handler -- if test_recover_armed, capture state and longjmp.
void handle_umode_exception(unsigned int cause, struct trap_frame * tfr) {
    if (test_recover_armed) {
        test_recover_cause  = (int)cause;
        test_recover_sepc   = (uint64_t)tfr->sepc;
        test_recover_stval  = csrr_stval();
        test_recover_armed  = 0;
        asm volatile ("csrsi sstatus, %0" :: "I" (RISCV_SSTATUS_SIE));
        test_longjmp(&test_recover_buf, 1);
    }
    char msgbuf[80];
    snprintf(msgbuf, sizeof msgbuf,
             "U-mode exception cause=%u at %p (stval=%p)",
             cause, (void *)tfr->sepc, (void *)csrr_stval());
    panic(msgbuf);
}
