// tests_vm.c -- 6 virtual-memory test cases (10pt total)
//
// Mirrors last year's CP2 grader rubric (G13 grade_report_cp2.md):
//   test_vm_kernel_mapped       1pt
//   test_vm_user_initial_page   1pt
//   test_vm_user_range_map      2pt
//   test_vm_user_single_unmap   1pt
//   test_vm_user_range_unmap    2pt
//   test_vm_lazy_alloc          3pt
//
// We don't have access to QEMU's `info mem` console (the grader's tool),
// so we walk the active page table directly via csrr satp + Sv39 3-level
// traversal.  This catches the G13 failure mode where map_page picks up
// the wrong vaddr (e.g. 0xffffffffc0000000 instead of 0x00000000fffff000).

#include <stdint.h>
#include <stddef.h>
#include "conf.h"
#include "memory.h"
#include "riscv.h"
#include "string.h"
#include "test_framework.h"

// ---- Sv39 page-table walker ---------------------------------------------
//
// Returns the leaf PTE bits (whole 64-bit word) for /vaddr/, or 0 if any
// level is unmapped.  We only need raw bits (PTE_V/R/W/X/U/G/PPN) so we
// keep this independent of memory.c's struct pte definition.

#define PTE_V_BIT   (1UL << 0)
#define PTE_R_BIT   (1UL << 1)
#define PTE_W_BIT   (1UL << 2)
#define PTE_X_BIT   (1UL << 3)
#define PTE_U_BIT   (1UL << 4)
#define PTE_G_BIT   (1UL << 5)

#define PTE_LEAF_BITS (PTE_R_BIT | PTE_W_BIT | PTE_X_BIT)

#define PTE_PPN_MASK 0x003FFFFFFFFFFC00UL    // bits 53..10
#define PTE_PPN_SHIFT 10

static uint64_t pte_walk(uintptr_t vaddr, int *level_out) {
    unsigned long satp = csrr_satp();
    if ((satp >> 60) == 0) {
        if (level_out) *level_out = -1;
        return 0;     // paging disabled
    }
    uint64_t *root = (uint64_t *)((satp & ((1UL << 44) - 1)) << 12);

    int idx2 = (vaddr >> 30) & 0x1FF;
    int idx1 = (vaddr >> 21) & 0x1FF;
    int idx0 = (vaddr >> 12) & 0x1FF;

    uint64_t pte = root[idx2];
    if (!(pte & PTE_V_BIT)) { if (level_out) *level_out = -1; return 0; }
    if (pte & PTE_LEAF_BITS) {                 // 1 GiB leaf
        if (level_out) *level_out = 2;
        return pte;
    }

    uint64_t *l1 = (uint64_t *)(((pte & PTE_PPN_MASK) >> PTE_PPN_SHIFT) << 12);
    pte = l1[idx1];
    if (!(pte & PTE_V_BIT)) { if (level_out) *level_out = -1; return 0; }
    if (pte & PTE_LEAF_BITS) {                 // 2 MiB megaleaf
        if (level_out) *level_out = 1;
        return pte;
    }

    uint64_t *l0 = (uint64_t *)(((pte & PTE_PPN_MASK) >> PTE_PPN_SHIFT) << 12);
    pte = l0[idx0];
    if (level_out) *level_out = 0;
    return pte;
}

static uintptr_t pte_paddr(uint64_t pte, int level, uintptr_t vaddr) {
    uintptr_t base = ((pte & PTE_PPN_MASK) >> PTE_PPN_SHIFT) << 12;
    if (level == 0) return base | (vaddr & 0xFFF);
    if (level == 1) return base | (vaddr & 0x1FFFFF);
    return base | (vaddr & 0x3FFFFFFF);
}

// ---- 1. test_vm_kernel_mapped ------------------------------------------
//
// Verifies memory_init's boot mappings are sane.  Pick three known
// kernel-image addresses (text, rodata, data) and confirm:
//   - every walk returns a valid PTE (any leaf level)
//   - text region has X bit set, no W
//   - rodata has R bit, no W, no X
//   - data has R+W, no X
// All should also have G (global) since they're kernel mappings.

extern char _kimg_text_start[], _kimg_text_end[];
extern char _kimg_rodata_start[];
extern char _kimg_data_start[];

static int test_vm_kernel_mapped(struct test_result *r) {
    int lvl;
    uint64_t pte;

    pte = pte_walk((uintptr_t)_kimg_text_start, &lvl);
    if (lvl < 0 || !(pte & PTE_V_BIT)) {
        r->fail_reason = "kernel text page not mapped";
        return 0;
    }
    if (!(pte & PTE_X_BIT) || (pte & PTE_W_BIT)) {
        r->fail_reason = "kernel text page lacks X or has W";
        return 0;
    }

    pte = pte_walk((uintptr_t)_kimg_rodata_start, &lvl);
    if (lvl < 0 || !(pte & PTE_V_BIT)) {
        r->fail_reason = "kernel rodata page not mapped";
        return 0;
    }
    if ((pte & PTE_W_BIT) || (pte & PTE_X_BIT)) {
        r->fail_reason = "kernel rodata page has W or X";
        return 0;
    }

    pte = pte_walk((uintptr_t)_kimg_data_start, &lvl);
    if (lvl < 0 || !(pte & PTE_V_BIT)) {
        r->fail_reason = "kernel data page not mapped";
        return 0;
    }
    if (!(pte & PTE_W_BIT) || (pte & PTE_X_BIT)) {
        r->fail_reason = "kernel data page lacks W or has X";
        return 0;
    }

    r->passed = 1;
    return 1;
}

// ---- helpers for user-VMA tests -----------------------------------------
//
// We pick a vaddr inside [USER_START_VMA, USER_END_VMA) (defined in conf.h
// as 0x80100000..0x81000000 for CP1; CP2 PDF 5.2.6 says these become
// 0x0C0000000..0x100000000 -- whichever conf.h has, the test uses).

#ifndef USER_TEST_VMA
#define USER_TEST_VMA UMEM_START_VMA
#endif

static int test_vm_user_initial_page(struct test_result *r) {
    void *pp = alloc_phys_page();
    if (pp == NULL) {
        r->fail_reason = "alloc_phys_page returned NULL (memory_init free chunk list?)";
        return 0;
    }
    void *res = map_page(USER_TEST_VMA, pp, PTE_R | PTE_W | PTE_U);
    if (res == NULL) {
        r->fail_reason = "map_page returned NULL";
        return 0;
    }

    int lvl;
    uint64_t pte = pte_walk(USER_TEST_VMA, &lvl);
    if (lvl < 0 || !(pte & PTE_V_BIT)) {
        r->fail_reason = "page-table walk reports unmapped after map_page";
        return 0;
    }
    if (!(pte & PTE_U_BIT)) {
        r->fail_reason = "user page lacks U bit";
        return 0;
    }
    if (!(pte & PTE_R_BIT) || !(pte & PTE_W_BIT)) {
        r->fail_reason = "user page lacks R+W flags";
        return 0;
    }

    // Verify physical address roundtrips through PTE.
    uintptr_t got_pa = pte_paddr(pte, lvl, USER_TEST_VMA);
    if ((got_pa & ~0xFFFUL) != ((uintptr_t)pp & ~0xFFFUL)) {
        r->fail_reason = "PTE PPN does not match alloc_phys_page result";
        return 0;
    }

    unmap_and_free_range((void *)USER_TEST_VMA, PAGE_SIZE);
    r->passed = 1;
    return 1;
}

static int test_vm_user_range_map(struct test_result *r) {
    const size_t n_pages = 4;
    const size_t sz = n_pages * PAGE_SIZE;
    uintptr_t base = USER_TEST_VMA;

    void *res = alloc_and_map_range(base, sz, PTE_R | PTE_W | PTE_U);
    if (res == NULL) {
        r->fail_reason = "alloc_and_map_range returned NULL";
        return 0;
    }

    for (size_t i = 0; i < n_pages; i++) {
        int lvl;
        uint64_t pte = pte_walk(base + i * PAGE_SIZE, &lvl);
        if (lvl < 0 || !(pte & PTE_V_BIT)) {
            r->fail_reason = "range page not mapped";
            unmap_and_free_range((void *)base, sz);
            return 0;
        }
        if (!(pte & PTE_U_BIT) || !(pte & PTE_R_BIT) || !(pte & PTE_W_BIT)) {
            r->fail_reason = "range page has wrong flags";
            unmap_and_free_range((void *)base, sz);
            return 0;
        }
    }

    unmap_and_free_range((void *)base, sz);
    r->passed = 1;
    return 1;
}

static int test_vm_user_single_unmap(struct test_result *r) {
    void *pp = alloc_phys_page();
    if (pp == NULL) {
        r->fail_reason = "alloc_phys_page returned NULL";
        return 0;
    }
    if (map_page(USER_TEST_VMA, pp, PTE_R | PTE_W | PTE_U) == NULL) {
        r->fail_reason = "map_page returned NULL";
        return 0;
    }

    unmap_and_free_range((void *)USER_TEST_VMA, PAGE_SIZE);

    int lvl;
    uint64_t pte = pte_walk(USER_TEST_VMA, &lvl);
    if (lvl >= 0 && (pte & PTE_V_BIT)) {
        r->fail_reason = "PTE still valid after unmap";
        return 0;
    }
    r->passed = 1;
    return 1;
}

static int test_vm_user_range_unmap(struct test_result *r) {
    const size_t n_pages = 4;
    const size_t sz = n_pages * PAGE_SIZE;
    uintptr_t base = USER_TEST_VMA;

    if (alloc_and_map_range(base, sz, PTE_R | PTE_W | PTE_U) == NULL) {
        r->fail_reason = "alloc_and_map_range returned NULL";
        return 0;
    }
    unmap_and_free_range((void *)base, sz);

    for (size_t i = 0; i < n_pages; i++) {
        int lvl;
        uint64_t pte = pte_walk(base + i * PAGE_SIZE, &lvl);
        if (lvl >= 0 && (pte & PTE_V_BIT)) {
            r->fail_reason = "range PTE still valid after unmap";
            return 0;
        }
    }
    r->passed = 1;
    return 1;
}

// ---- lazy alloc ---------------------------------------------------------
//
// Touch an unmapped USER_VMA address with recovery armed.  The S-mode
// trap dispatcher should route the page fault into handle_umode_page_fault
// (or handle_smode_exception if we're in S-mode -- depends on which
// handler the kernel routes faults to in supervisor context).
//
// In CP2, the rubric (PDF 5.2.2) says lazy allocation should kick in for
// faults inside USER_START_VMA..USER_END_VMA.  We test that path here:
// after a fault, repeating the access should succeed without panic.
//
// Pristine starter handle_umode_page_fault returns 0 (not handled) and
// alloc_phys_page returns NULL, so this test FAILs with the recovery
// trapping out.  Once the student fills both, the second access lands on
// a freshly mapped+zeroed page.

static int test_vm_lazy_alloc(struct test_result *r) {
    volatile int * vp = (volatile int *)USER_TEST_VMA;

    // Skip the trap-recovery dance entirely if alloc_phys_page is still a
    // stub.  Without working physical alloc, lazy alloc cannot work, and
    // we don't want to leave a half-set page-table entry around for the
    // next test to step on.
    void *probe = alloc_phys_page();
    if (probe == NULL) {
        r->fail_reason = "alloc_phys_page stub -- lazy alloc cannot work";
        return 0;
    }
    free_phys_page(probe);

    int got;
    if (test_setjmp(&test_recover_buf) == 0) {
        test_recover_armed = 1;
        got = *vp;
        test_recover_armed = 0;
        if (got != 0) {
            unmap_and_free_range((void *)USER_TEST_VMA, PAGE_SIZE);
            r->fail_reason = "lazy-alloc page wasn't zero-initialized";
            return 0;
        }
        *vp = 0xDEADBEEF;
        if (*vp != (int)0xDEADBEEF) {
            unmap_and_free_range((void *)USER_TEST_VMA, PAGE_SIZE);
            r->fail_reason = "lazy-alloc page not writeable";
            return 0;
        }
        unmap_and_free_range((void *)USER_TEST_VMA, PAGE_SIZE);
        r->passed = 1;
        return 1;
    }
    // longjmp landed here -- handler didn't resolve.
    r->fail_reason = "lazy-alloc: page fault not handled";
    return 0;
}

static const struct test_entry vm_tests[] = {
    { "test_vm_kernel_mapped",      test_vm_kernel_mapped,      1 },
    { "test_vm_user_initial_page",  test_vm_user_initial_page,  1 },
    { "test_vm_user_range_map",     test_vm_user_range_map,     2 },
    { "test_vm_user_single_unmap",  test_vm_user_single_unmap,  1 },
    { "test_vm_user_range_unmap",   test_vm_user_range_unmap,   2 },
    { "test_vm_lazy_alloc",         test_vm_lazy_alloc,         3 },
};

const struct test_entry *get_vm_tests(int *n_out) {
    *n_out = sizeof vm_tests / sizeof vm_tests[0];
    return vm_tests;
}
