// Copyright (c) 2025 Yoonkyu Lee
// SPDX-License-Identifier: NCSA
//
// tests_vioblk.c -- 5 VirtIO block test cases (7pt)
//
// Test cases (logical names without _gdb suffix):
//   test_vioblk_readat_simple_1   1pt
//   test_vioblk_readat_simple_2   2pt
//   test_vioblk_writeat_simple_0  1pt  
//   test_vioblk_writeat_simple_1  1pt  
//   test_vioblk_writeat_simple_2  2pt
//
// Pristine starter dev/vioblk.c is broken-compile (we patched it to a
// stub) -- vioblk_attach's FIXME is not filled in, so the device never
// registers and open_device("vioblk", 0, ...) returns -ENODEV.  All
// five tests fail at "open_device(vioblk, 0) failed".
//
// Once vioblk.c is implemented, the tests:
//   - readat_simple_1: read 1 block (512 B) from offset 0
//   - readat_simple_2: read 4 blocks (2048 B) from offset 1024
//   - writeat_simple_0: write a 1-block pattern at offset 0, read back, verify
//   - writeat_simple_1: write at offset 0, then write at offset 4096, read both
//   - writeat_simple_2: write 4 blocks across, verify each block content

#include <stdint.h>
#include <stddef.h>
#include "device.h"
#include "io.h"
#include "string.h"
#include "test_framework.h"

#define VIOBLK_BLKSZ 512

static int open_vioblk(struct test_result *r, struct io **out) {
    int rc = open_device("vioblk", 1, out);   // blk1 scratch image
    if (rc != 0 || *out == NULL) {
        r->fail_reason = "open_device(vioblk, 0) failed";
        return 0;
    }
    return 1;
}

static int test_vioblk_readat_simple_1(struct test_result *r) {
    struct io *io;
    if (!open_vioblk(r, &io)) return 0;

    uint8_t buf[VIOBLK_BLKSZ];
    memset(buf, 0xCD, sizeof buf);
    long n = ioreadat(io, /*pos=*/0, buf, sizeof buf);
    ioclose(io);

    if (n != (long)sizeof buf) {
        r->fail_reason = "vioblk_readat returned wrong byte count";
        return 0;
    }
    // Sanity: at least one byte must differ from the pre-fill (0xCD) since
    // the disk image wasn't all 0xCD.  Weak guard but catches no-op reads.
    int any_diff = 0;
    for (size_t i = 0; i < sizeof buf; i++)
        if (buf[i] != 0xCD) { any_diff = 1; break; }
    if (!any_diff) {
        r->fail_reason = "vioblk_readat did not modify the buffer";
        return 0;
    }
    r->passed = 1;
    return 1;
}

static int test_vioblk_readat_simple_2(struct test_result *r) {
    struct io *io;
    if (!open_vioblk(r, &io)) return 0;

    static uint8_t buf[VIOBLK_BLKSZ * 4];
    memset(buf, 0xAA, sizeof buf);
    long n = ioreadat(io, /*pos=*/VIOBLK_BLKSZ * 2, buf, sizeof buf);
    ioclose(io);

    if (n != (long)sizeof buf) {
        r->fail_reason = "vioblk_readat (multi-block) returned wrong count";
        return 0;
    }
    int any_diff = 0;
    for (size_t i = 0; i < sizeof buf; i++)
        if (buf[i] != 0xAA) { any_diff = 1; break; }
    if (!any_diff) {
        r->fail_reason = "vioblk_readat (multi-block) did not modify buffer";
        return 0;
    }
    r->passed = 1;
    return 1;
}

static int test_vioblk_writeat_simple_0(struct test_result *r) {
    struct io *io;
    if (!open_vioblk(r, &io)) return 0;

    uint8_t pattern[VIOBLK_BLKSZ];
    for (size_t i = 0; i < sizeof pattern; i++)
        pattern[i] = (uint8_t)(i ^ 0x55);

    // Write 1 block at offset 0 (start of disk).  an earlier implementation failed here on
    // `queue_reset == 1` assertion; mutation test target.
    long w = iowriteat(io, /*pos=*/0, pattern, sizeof pattern);
    if (w != (long)sizeof pattern) {
        ioclose(io);
        r->fail_reason = "vioblk_writeat returned wrong byte count";
        return 0;
    }
    uint8_t back[VIOBLK_BLKSZ];
    long rd = ioreadat(io, /*pos=*/0, back, sizeof back);
    ioclose(io);
    if (rd != (long)sizeof back) {
        r->fail_reason = "readback after writeat returned wrong count";
        return 0;
    }
    if (memcmp(back, pattern, sizeof pattern) != 0) {
        r->fail_reason = "writeat at offset 0 did not persist";
        return 0;
    }
    r->passed = 1;
    return 1;
}

static int test_vioblk_writeat_simple_1(struct test_result *r) {
    struct io *io;
    if (!open_vioblk(r, &io)) return 0;

    uint8_t pat_a[VIOBLK_BLKSZ], pat_b[VIOBLK_BLKSZ];
    for (size_t i = 0; i < VIOBLK_BLKSZ; i++) {
        pat_a[i] = (uint8_t)(0x10 + i);
        pat_b[i] = (uint8_t)(0xE0 - i);
    }

    if (iowriteat(io, /*pos=*/VIOBLK_BLKSZ * 8, pat_a, VIOBLK_BLKSZ) != VIOBLK_BLKSZ) {
        ioclose(io);
        r->fail_reason = "writeat #1 returned wrong byte count";
        return 0;
    }
    if (iowriteat(io, /*pos=*/VIOBLK_BLKSZ * 16, pat_b, VIOBLK_BLKSZ) != VIOBLK_BLKSZ) {
        ioclose(io);
        r->fail_reason = "writeat #2 returned wrong byte count";
        return 0;
    }
    uint8_t back_a[VIOBLK_BLKSZ], back_b[VIOBLK_BLKSZ];
    long ra = ioreadat(io, VIOBLK_BLKSZ * 8, back_a, VIOBLK_BLKSZ);
    long rb = ioreadat(io, VIOBLK_BLKSZ * 16, back_b, VIOBLK_BLKSZ);
    ioclose(io);

    if (ra != VIOBLK_BLKSZ || rb != VIOBLK_BLKSZ) {
        r->fail_reason = "readback after dual writeat returned wrong count";
        return 0;
    }
    if (memcmp(back_a, pat_a, VIOBLK_BLKSZ) != 0 ||
        memcmp(back_b, pat_b, VIOBLK_BLKSZ) != 0) {
        r->fail_reason = "dual writeat content mismatch";
        return 0;
    }
    r->passed = 1;
    return 1;
}

static int test_vioblk_writeat_simple_2(struct test_result *r) {
    struct io *io;
    if (!open_vioblk(r, &io)) return 0;

    static uint8_t pattern[VIOBLK_BLKSZ * 4];
    for (size_t i = 0; i < sizeof pattern; i++)
        pattern[i] = (uint8_t)(i & 0xFF);

    if (iowriteat(io, /*pos=*/VIOBLK_BLKSZ * 32, pattern, sizeof pattern)
            != (long)sizeof pattern) {
        ioclose(io);
        r->fail_reason = "multi-block writeat returned wrong count";
        return 0;
    }
    static uint8_t back[VIOBLK_BLKSZ * 4];
    long rd = ioreadat(io, VIOBLK_BLKSZ * 32, back, sizeof back);
    ioclose(io);
    if (rd != (long)sizeof back) {
        r->fail_reason = "multi-block readback returned wrong count";
        return 0;
    }
    if (memcmp(back, pattern, sizeof pattern) != 0) {
        r->fail_reason = "multi-block writeat content mismatch";
        return 0;
    }
    r->passed = 1;
    return 1;
}

static const struct test_entry vioblk_tests[] = {
    { "test_vioblk_readat_simple_1",  test_vioblk_readat_simple_1,  1 },
    { "test_vioblk_readat_simple_2",  test_vioblk_readat_simple_2,  2 },
    { "test_vioblk_writeat_simple_0", test_vioblk_writeat_simple_0, 1 },
    { "test_vioblk_writeat_simple_1", test_vioblk_writeat_simple_1, 1 },
    { "test_vioblk_writeat_simple_2", test_vioblk_writeat_simple_2, 2 },
};

const struct test_entry *get_vioblk_tests(int *n_out) {
    *n_out = sizeof vioblk_tests / sizeof vioblk_tests[0];
    return vioblk_tests;
}
