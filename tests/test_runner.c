// test_runner.c -- per-test driver and AG-style report formatter
//
// Each test is invoked through run_test(), which wraps the call in:
//   - clobber_mask = 0          (wrapper records this run only)
//   - test_setjmp(...)          (recover from synchronous faults)
//   - test_recover_armed = 1    (handle_smode_exception longjmps on fault)
//
// Output mirrors the original Su25 autograder report format so we can
// diff against tmp/autograder_report_*.md (MP2) or sp25_mp1_final_ag_report.txt
// (MP1).  Adapted from mp1/test/test_runner.c for MP2's S-mode trap.

#include <stdint.h>
#include <stddef.h>
#include "console.h"
#include "string.h"
#include "test_framework.h"

// ---- preserved-across-longjmp state (use globals) -----------------------

static const struct test_entry * volatile g_running_entry;
static struct test_result        volatile g_running_result;

// Penalty bookkeeping (set once, applied at summary time).
volatile int  test_clobber_penalty_applied = 0;
const char   *test_first_clobber_test      = NULL;

// ---- runtime address comparison -----------------------------------------
//
// Routes pointer compares through a function call so GCC can't fold
// `&extern_a == &extern_b` to a compile-time false.  Used by
// REQUIRE_IMPL to detect that mp1.S's weak stub is in play.

int addr_eq(const void *a, const void *b) { return a == b; }

// ---- AG-style line printers ---------------------------------------------
//
// Header: ">---------<NAME>" + dashes + "PASSED"|"FAILED"
// Total line width ~56 chars (matches Su25 format).

static void print_test_header(const char *name, int passed) {
    kprintf(">---------<%s>", name);
    int dashes = 38 - (int)strlen(name);
    if (dashes < 0) dashes = 0;
    while (dashes-- > 0) kprintf("-");
    kprintf("%s\n", passed ? "PASSED" : "FAILED");
}

static void print_test_body(const struct test_result *r) {
    if (r->passed) {
        if (clobber_mask)
            kprintf(" PASS, register clobber\n");
        else
            kprintf(" PASS\n");
    } else {
        kprintf("ERROR DETECTED:\n%s\n",
                r->fail_reason ? r->fail_reason : "(no reason given)");
    }
    kprintf("\n");
    kprintf("Score: %d/%d\n\n", r->score, r->max_score);
}

// ---- driver --------------------------------------------------------------

void run_test(const struct test_entry *entry,
              int *score_acc, int *max_acc) {

    g_running_entry = entry;

    // Initialize result struct freshly each run.
    g_running_result.name        = entry->name;
    g_running_result.max_score   = entry->max_score;
    g_running_result.passed      = 0;
    g_running_result.score       = 0;
    g_running_result.fail_reason = NULL;

    // Reset shared state before the test runs.  Per-suite state (e.g.
    // thread ready_list, alarm sleep_list) is reset by suite-specific
    // helpers in test_helpers.c -- the runner doesn't know which globals
    // a particular test depends on.
    clobber_mask = 0;

    if (test_setjmp(&test_recover_buf) == 0) {
        // Direct path -- arm recovery, run the test.
        test_recover_armed = 1;
        entry->fn((struct test_result *)&g_running_result);
        test_recover_armed = 0;

        // If the test function returned without setting passed, treat
        // as a failure with a generic reason.
        if (g_running_result.passed) {
            g_running_result.score = g_running_result.max_score;
        } else {
            if (g_running_result.fail_reason == NULL)
                g_running_result.fail_reason = "test reported FAIL";
            g_running_result.score = 0;
        }
    } else {
        // longjmp landed here -- a fault happened during the test.
        test_recover_armed = 0;
        g_running_result.passed = 0;
        g_running_result.score  = 0;
        // Format a fail reason describing the fault.
        static char fault_buf[80];
        snprintf(fault_buf, sizeof fault_buf,
                 "fault scause=%d at 0x%lx (stval=0x%lx)",
                 test_recover_cause,
                 (long)test_recover_sepc,
                 (long)test_recover_stval);
        g_running_result.fail_reason = fault_buf;
    }

    // Record clobber penalty (first occurrence wins).
    if (clobber_mask && !test_clobber_penalty_applied) {
        test_clobber_penalty_applied = 1;
        test_first_clobber_test      = entry->name;
    }

    // Emit the AG-style block.
    print_test_header(g_running_result.name, g_running_result.passed);
    print_test_body((const struct test_result *)&g_running_result);

    // Tally.
    *score_acc += g_running_result.score;
    *max_acc   += g_running_result.max_score;
}

void run_test_group(const char *header,
                    const struct test_entry *entries, int n,
                    int *score_acc, int *max_acc) {
    if (header && *header) {
        kprintf("------ %s ------\n\n", header);
    }
    for (int i = 0; i < n; i++) {
        run_test(&entries[i], score_acc, max_acc);
    }
}
