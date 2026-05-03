// process.c - user process
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//



#ifdef PROCESS_TRACE
#define TRACE
#endif

#ifdef PROCESS_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "assert.h"
#include "process.h"
#include "elf.h"
#include "fs.h"
#include "io.h"
#include "string.h"
#include "thread.h"
#include "riscv.h"
#include "trap.h"
#include "memory.h"
#include "heap.h"
#include "error.h"
#include "console.h"
#include "intr.h"

// COMPILE-TIME PARAMETERS
//


#ifndef NPROC
#define NPROC 16
#endif

// INTERNAL FUNCTION DECLARATIONS
//


static int build_stack(void * stack, int argc, char ** argv);


static void fork_func(struct condition * forked, struct trap_frame * tfr);

// INTERNAL GLOBAL VARIABLES
//


static struct process main_proc;


static struct process * proctab[NPROC] = {
    &main_proc
};

// EXPORTED GLOBAL VARIABLES
//

char procmgr_initialized = 0;

// EXPORTED FUNCTION DEFINITIONS
//

void procmgr_init(void) {
    assert (memory_initialized && heap_initialized);
    assert (!procmgr_initialized);

    main_proc.idx = 0;
    main_proc.tid = running_thread();
    main_proc.mtag = active_mspace();
    thread_set_process(main_proc.tid, &main_proc);
    procmgr_initialized = 1;
}

int process_exec(struct io * exeio, int argc, char ** argv) {
    struct process * p = current_process();
    if (p == NULL) return -EINVAL;
    if (exeio == NULL) return -EINVAL;

    // Lazy allocation handles text/data/heap pages via
    // handle_umode_page_fault.  We pre-map only the top stack page so
    // build_stack can write argv/argc safely from S mode.
    void * stack_top = (char *)UMEM_END_VMA - PAGE_SIZE;
    void * spp = alloc_phys_page();
    if (spp == NULL) return -ENOMEM;
    memset(spp, 0, PAGE_SIZE);
    if (map_page((uintptr_t)stack_top, spp,
                 PTE_R | PTE_W | PTE_U) == NULL) {
        free_phys_page(spp);
        return -ENOMEM;
    }

    void (*entry)(void) = NULL;
    int rc = elf_load(exeio, &entry);
    if (rc != 0) {
        unmap_and_free_range(stack_top, PAGE_SIZE);
        return rc;
    }

    int stksz = build_stack(stack_top, argc, argv);
    if (stksz < 0) {
        unmap_and_free_range(stack_top, PAGE_SIZE);
        return stksz;
    }

    // Set up the user trap frame and jump to user mode.  trap_frame_jump
    // does the sret with SPP=0 (return to U-mode) and SPIE=1.
    struct trap_frame tfr = { 0 };
    tfr.sepc    = entry;
    tfr.sp      = (void *)((uintptr_t)UMEM_END_VMA - stksz);
    tfr.a0      = argc;
    tfr.a1      = (long)((uintptr_t)tfr.sp);
    // SPP=0 (user), SPIE=1 (interrupts on after sret).
    tfr.sstatus = (csrr_sstatus() & ~RISCV_SSTATUS_SPP) | RISCV_SSTATUS_SPIE;

    // Re-entry trap frame lives at the top of THIS thread's stack -- not
    // necessarily the main thread (e.g. forked children).
    void * anchor = running_thread_stack_anchor();
    void * sscratch_for_trap =
        (char *)anchor - sizeof(struct trap_frame);

    trap_frame_jump(&tfr, sscratch_for_trap);
    // unreachable
    return 0;
}

int process_fork(const struct trap_frame * tfr) {
    struct process * parent = current_process();
    if (parent == NULL || tfr == NULL) return -EINVAL;

    // Find a free slot in proctab.
    int idx = -1;
    for (int i = 1; i < NPROC; i++) {
        if (proctab[i] == NULL) { idx = i; break; }
    }
    if (idx < 0) return -EMPROC;

    struct process * child = kcalloc(1, sizeof(struct process));
    if (child == NULL) return -ENOMEM;
    child->idx = idx;

    // Clone iotab (each io gets a refcount bump).
    for (int i = 0; i < PROCESS_IOMAX; i++) {
        if (parent->iotab[i] != NULL)
            child->iotab[i] = ioaddref(parent->iotab[i]);
    }

    // Clone the active memory space.  This produces a fresh page table
    // with deep copies of all user pages.
    mtag_t child_mtag = clone_active_mspace();
    if (child_mtag == 0) {
        for (int i = 0; i < PROCESS_IOMAX; i++)
            if (child->iotab[i] != NULL) ioclose(child->iotab[i]);
        kfree(child);
        return -ENOMEM;
    }
    child->mtag = child_mtag;

    proctab[idx] = child;

    // Spawn the child thread.  fork_func receives the parent's tfr (so it
    // can copy it locally and set a0=0), plus a condition var the parent
    // waits on so we don't free the tfr stack slot before the child is done.
    struct condition forked;
    condition_init(&forked, "fork");

    int tid = thread_spawn("forked", (void (*)())fork_func,
                           (uint64_t)(uintptr_t)&forked,
                           (uint64_t)(uintptr_t)tfr);
    if (tid <= 0) {
        proctab[idx] = NULL;
        for (int i = 0; i < PROCESS_IOMAX; i++)
            if (child->iotab[i] != NULL) ioclose(child->iotab[i]);
        // Best-effort: discard the cloned mspace (need to switch first).
        mtag_t saved = switch_mspace(child_mtag);
        reset_active_mspace();
        switch_mspace(saved);
        kfree(child);
        return -EMTHR;
    }
    child->tid = tid;
    thread_set_process(tid, child);

    // Wait until the child has consumed the tfr (signaled "forked").
    int pie = disable_interrupts();
    condition_wait(&forked);
    restore_interrupts(pie);

    return tid;
}

void process_exit(void) {
    struct process * p = current_process();
    if (p != NULL) {
        for (int i = 0; i < PROCESS_IOMAX; i++) {
            if (p->iotab[i] != NULL) {
                ioclose(p->iotab[i]);
                p->iotab[i] = NULL;
            }
        }
        if (p->idx > 0) {
            // Forked child: discard the cloned mspace and drop the
            // dynamically allocated process struct.
            discard_active_mspace();
            proctab[p->idx] = NULL;
            kfree(p);
        } else {
            // Main process: keep the kernel mspace, just free user pages.
            reset_active_mspace();
        }
    }
    thread_exit();   // noreturn
}

// INTERNAL FUNCTION DEFINITIONS
//

int build_stack(void * stack, int argc, char ** argv) {
    size_t stksz, argsz;
    uintptr_t * newargv;
    char * p;
    int i;

    // We need to be able to fit argv[] on the initial stack page, so _argc_
    // cannot be too large. Note that argv[] contains argc+1 elements (last one
    // is a NULL pointer).

    if (PAGE_SIZE / sizeof(char*) - 1 < argc)
        return -ENOMEM;
    
    stksz = (argc+1) * sizeof(char*);

    // Add the sizes of the null-terminated strings that argv[] points to.

    for (i = 0; i < argc; i++) {
        argsz = strlen(argv[i])+1;
        if (PAGE_SIZE - stksz < argsz)
            return -ENOMEM;
        stksz += argsz;
    }

    // Round up stksz to a multiple of 16 (RISC-V ABI requirement).

    stksz = ROUND_UP(stksz, 16);
    assert (stksz <= PAGE_SIZE);

    // Set _newargv_ to point to the location of the argument vector on the new
    // stack and set _p_ to point to the stack space after it to which we will
    // copy the strings. Note that the string pointers we write to the new
    // argument vector must point to where the user process will see the stack.
    // The user stack will be at the highest page in user memory, the address of
    // which is `(UMEM_END_VMA - PAGE_SIZE)`. The offset of the _p_ within the
    // stack is given by `p - newargv'.

    newargv = stack + PAGE_SIZE - stksz;
    p = (char*)(newargv+argc+1);

    for (i = 0; i < argc; i++) {
        newargv[i] = (UMEM_END_VMA - PAGE_SIZE) + ((void*)p - (void*)stack);
        argsz = strlen(argv[i])+1;
        memcpy(p, argv[i], argsz);
        p += argsz;
    }

    newargv[argc] = 0;
    return stksz;
}

void fork_func(struct condition * done, struct trap_frame * tfr) {
    // Snapshot the parent's tfr (it lives on the parent's kernel stack),
    // signal so the parent can return, then sret to user mode in the
    // child's cloned mspace.
    struct trap_frame local_tfr = *tfr;
    condition_broadcast(done);

    struct process * me = current_process();
    if (me != NULL && me->mtag != 0) switch_mspace(me->mtag);

    local_tfr.a0 = 0;       // child's _fork() return value
    void * anchor = running_thread_stack_anchor();
    void * sscratch_for_trap = (char *)anchor - sizeof(struct trap_frame);
    trap_frame_jump(&local_tfr, sscratch_for_trap);
}