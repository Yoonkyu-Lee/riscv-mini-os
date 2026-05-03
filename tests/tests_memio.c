// Copyright (c) 2025 Yoonkyu Lee
// SPDX-License-Identifier: MIT
//
// tests_memio.c -- 2 memio test cases (2pt)
//
// Test cases (logical names without _gdb suffix):
//   test_memio_readat   1pt
//   test_memio_writeat  1pt
//
// Pristine starter has no create_memory_io implementation -- tests fail
// with "create_memory_io returned NULL" until io.c is filled in.
//
// Methodology: allocate a fixed buffer, hand it to create_memory_io, then
// exercise io->intf->readat/writeat through ioreadat/iowriteat.

#include <stdint.h>
#include <stddef.h>
#include "io.h"
#include "string.h"
#include "test_framework.h"

#define MEMIO_TEST_SZ 256

static uint8_t memio_buf[MEMIO_TEST_SZ];

static int test_memio_readat(struct test_result *r) {
    // Fill backing buffer with a known pattern.
    for (int i = 0; i < MEMIO_TEST_SZ; i++)
        memio_buf[i] = (uint8_t)(i ^ 0xA5);

    struct io *io = create_memory_io(memio_buf, MEMIO_TEST_SZ);
    if (io == NULL) {
        r->fail_reason = "create_memory_io returned NULL";
        return 0;
    }

    uint8_t out[64];
    long n = ioreadat(io, /*pos=*/16, out, sizeof out);
    ioclose(io);

    if (n != (long)sizeof out) {
        r->fail_reason = "memio_readat returned wrong byte count";
        return 0;
    }
    for (int i = 0; i < (int)sizeof out; i++) {
        if (out[i] != (uint8_t)((i + 16) ^ 0xA5)) {
            r->fail_reason = "memio_readat copied wrong bytes";
            return 0;
        }
    }

    r->passed = 1;
    return 1;
}

static int test_memio_writeat(struct test_result *r) {
    memset(memio_buf, 0, MEMIO_TEST_SZ);

    struct io *io = create_memory_io(memio_buf, MEMIO_TEST_SZ);
    if (io == NULL) {
        r->fail_reason = "create_memory_io returned NULL";
        return 0;
    }

    uint8_t pattern[32];
    for (int i = 0; i < 32; i++) pattern[i] = (uint8_t)(0x80 | i);

    long n = iowriteat(io, /*pos=*/64, pattern, sizeof pattern);
    ioclose(io);

    if (n != (long)sizeof pattern) {
        r->fail_reason = "memio_writeat returned wrong byte count";
        return 0;
    }
    for (int i = 0; i < 32; i++) {
        if (memio_buf[64 + i] != (uint8_t)(0x80 | i)) {
            r->fail_reason = "memio_writeat did not copy bytes correctly";
            return 0;
        }
    }
    // Outside the written range must remain zero.
    for (int i = 0; i < 64; i++) {
        if (memio_buf[i] != 0 || memio_buf[64 + 32 + i] != 0) {
            r->fail_reason = "memio_writeat leaked beyond requested range";
            return 0;
        }
    }

    r->passed = 1;
    return 1;
}

static const struct test_entry memio_tests[] = {
    { "test_memio_readat",  test_memio_readat,  1 },
    { "test_memio_writeat", test_memio_writeat, 1 },
};

const struct test_entry *get_memio_tests(int *n_out) {
    *n_out = sizeof memio_tests / sizeof memio_tests[0];
    return memio_tests;
}
