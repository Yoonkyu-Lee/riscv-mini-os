// Copyright (c) 2025 Yoonkyu Lee
// SPDX-License-Identifier: NCSA
//
// test_framework.h -- shared declarations for the test bench
//
// Carries the earlier framework forward (S-mode + U-mode trap recovery,
// clobber detector, test runner) and adds kernel-specific suite
// registries.

#ifndef _TEST_FRAMEWORK_H_
#define _TEST_FRAMEWORK_H_

#include <stdint.h>

// ---- setjmp / longjmp (defined in setjmp.S, RV64 ABI) -------------------

struct test_jmp_buf {
    uint64_t ra;
    uint64_t sp;
    uint64_t s[12];
};

int  test_setjmp (struct test_jmp_buf *buf);
void test_longjmp(struct test_jmp_buf *buf, int val) __attribute__((noreturn));

// ---- recovery state (defined in excp_replace.c) -------------------------

extern volatile int      test_recover_armed;
extern volatile int      test_recover_cause;
extern volatile uint64_t test_recover_sepc;
extern volatile uint64_t test_recover_stval;
extern struct test_jmp_buf test_recover_buf;

// ---- clobber detection (defined in clobber.S) ---------------------------
//
// Wraps a single call to /fn/ with up to 6 args.  Sets clobber_mask: bit
// N set => sN was clobbered by /fn/ (12 bits, s0-s11).

uint64_t clobber_call(void (*fn)(),
                      uint64_t a0, uint64_t a1, uint64_t a2,
                      uint64_t a3, uint64_t a4, uint64_t a5);
extern volatile uint32_t clobber_mask;

// ---- test runner (defined in test_runner.c) -----------------------------

struct test_result {
    const char *name;
    int         max_score;
    int         passed;
    int         score;
    const char *fail_reason;
};

typedef int (*test_fn_t)(struct test_result *r);

struct test_entry {
    const char *name;
    test_fn_t   fn;
    int         max_score;
};

void run_test(const struct test_entry *entry,
              int *score_acc, int *max_acc);

void run_test_group(const char *header,
                    const struct test_entry *entries, int n,
                    int *score_acc, int *max_acc);

// ---- per-suite test registries (defined in tests_*.c) -------------------

extern const struct test_entry *get_locks_tests(int *n_out);     // Phase A
extern const struct test_entry *get_memio_tests(int *n_out);     // Phase B
extern const struct test_entry *get_elf_tests(int *n_out);       // Phase B
extern const struct test_entry *get_cache_tests(int *n_out);     // Phase B
extern const struct test_entry *get_vioblk_tests(int *n_out);    // Phase C
extern const struct test_entry *get_ktfs_tests(int *n_out);      // Phase D
extern const struct test_entry *get_vm_tests(int *n_out);        // Phase E
extern const struct test_entry *get_syscall_tests(int *n_out);   // Phase F
extern const struct test_entry *get_ktfsrw_tests(int *n_out);    // Phase F

#endif // _TEST_FRAMEWORK_H_
