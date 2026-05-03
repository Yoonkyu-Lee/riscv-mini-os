// Copyright (c) 2025 Yoonkyu Lee
// SPDX-License-Identifier: NCSA
//
// tests_locks.c -- 1 lock test case (1pt total)
//
// Test case (logical name without _gdb suffix): test_locks (1pt).
//
// Verifies the recursive-ownership semantics required by the doxygen spec:
//   - lock_acquire on an unheld lock takes ownership and sets count=1
//   - re-acquiring with the same owner increments count (recursive)
//   - lock_release decrements; only at count=0 does the lock become free
//   - thread_exit must drain held locks (we don't exercise that here --
//     covered indirectly by other tests once threading is filled in)
//
// With the patched thread.c -- this test PASSES out
// of the gate because the lock implementation is part of the framework
// import.  The test exists so that any future regression (e.g. someone
// rewriting thread.c without lock_*) is caught immediately.

#include <stdint.h>
#include <stddef.h>
#include "thread.h"
#include "test_framework.h"

static int test_locks(struct test_result *r) {
    struct lock l;
    lock_init(&l);

    // After init: unheld.
    if (l.count != 0 || l.owner != NULL) {
        r->fail_reason = "lock_init did not zero owner/count";
        return 0;
    }

    // First acquire: take ownership.
    lock_acquire(&l);
    if (l.count != 1) {
        r->fail_reason = "lock_acquire on free lock did not set count=1";
        return 0;
    }

    // Recursive acquire: same thread, count must increment.
    lock_acquire(&l);
    if (l.count != 2) {
        r->fail_reason = "recursive lock_acquire did not increment count";
        return 0;
    }

    // First release: count drops, but lock still held (owner intact).
    lock_release(&l);
    if (l.count != 1 || l.owner == NULL) {
        r->fail_reason = "first lock_release should keep owner+count=1";
        return 0;
    }

    // Final release: lock fully free.
    lock_release(&l);
    if (l.count != 0 || l.owner != NULL) {
        r->fail_reason = "final lock_release did not clear owner/count";
        return 0;
    }

    // Re-acquire should work after full release.
    lock_acquire(&l);
    if (l.count != 1) {
        r->fail_reason = "lock_acquire after full release failed";
        return 0;
    }
    lock_release(&l);

    r->passed = 1;
    return 1;
}

static const struct test_entry locks_tests[] = {
    { "test_locks", test_locks, 1 },
};

const struct test_entry *get_locks_tests(int *n_out) {
    *n_out = sizeof locks_tests / sizeof locks_tests[0];
    return locks_tests;
}
