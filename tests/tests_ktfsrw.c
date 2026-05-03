// Copyright (c) 2025 Yoonkyu Lee
// SPDX-License-Identifier: NCSA
//
// tests_ktfsrw.c -- 3 read-write KTFS test cases (10pt total)
//
// Test cases (logical names without _gdb suffix):
//   test_ktfs_writeat_persist  5pt
//   test_ktfs_create_persist   3pt
//   test_ktfs_delete           2pt
//
// Backing: same fixture image used by tests_ktfs.c (mounted via memio
// over the kernel blob).  The image is rebuilt fresh on every test.elf
// build so writes here don't leak between runs.
//
// Pristine starter ktfs_writeat/_create/_delete return 0/no-op stubs,
// so all three tests fail with "writeat returned 0", "create returned
// non-zero", or "file still openable after delete" depending on the
// exact stub.  Once the student fills in the write side, they pass.

#include <stdint.h>
#include <stddef.h>
#include "io.h"
#include "fs.h"
#include "ktfs.h"
#include "string.h"
#include "test_framework.h"

extern char _kimg_blob_start[];
extern char _kimg_blob_end[];

// Direct calls to the driver functions (fs.h alias may not be in place
// for create/delete until the student adds it).
extern int  ktfs_create(const char * name);
extern int  ktfs_delete(const char * name);
extern long ktfs_writeat(struct io * io, unsigned long long pos,
                         const void * buf, long len);

// Helper: mount the blob into the global ktfs state.
static int mount_blob(struct test_result *r) {
    long size = (long)(_kimg_blob_end - _kimg_blob_start);
    if (size <= 0) {
        r->fail_reason = "fixture blob is empty";
        return -1;
    }
    struct io *io = create_memory_io(_kimg_blob_start, size);
    if (io == NULL) {
        r->fail_reason = "create_memory_io for blob returned NULL";
        return -1;
    }
    int rc = fsmount(io);
    if (rc < 0) {
        ioclose(io);
        r->fail_reason = "fsmount returned an error";
        return -1;
    }
    return 0;
}

// 1. test_ktfs_writeat_persist: open small.txt, write a known pattern at
//    offset 0, read back via a fresh fsopen (proxy for "persists across
//    open/close").  Skips at fsmount/open if anything stubs out.

static int test_ktfs_writeat_persist(struct test_result *r) {
    if (mount_blob(r) < 0) return 0;

    // Write pattern to a known file.
    struct io *io = NULL;
    if (fsopen("small.txt", &io) != 0 || io == NULL) {
        r->fail_reason = "fsopen('small.txt') returned NULL";
        return 0;
    }

    static const char pattern[16] = "KTFS-WRITEAT-OK";   // 15 chars + NUL
    long w = ktfs_writeat(io, 0, pattern, sizeof pattern);
    ioclose(io);
    if (w != (long)sizeof pattern) {
        r->fail_reason = "ktfs_writeat returned wrong byte count";
        return 0;
    }

    // Re-open, read back, compare.
    struct io *io2 = NULL;
    if (fsopen("small.txt", &io2) != 0 || io2 == NULL) {
        r->fail_reason = "fsopen after writeat returned NULL";
        return 0;
    }
    char buf[16] = { 0 };
    long n = ioreadat(io2, 0, buf, sizeof buf);
    ioclose(io2);
    if (n != (long)sizeof buf) {
        r->fail_reason = "ioreadat after writeat returned wrong count";
        return 0;
    }
    if (memcmp(buf, pattern, sizeof pattern) != 0) {
        r->fail_reason = "writeat pattern did not persist on readback";
        return 0;
    }
    r->passed = 1;
    return 1;
}

// 2. test_ktfs_create_persist: ktfs_create + fsopen + writeat + readback.

static int test_ktfs_create_persist(struct test_result *r) {
    if (mount_blob(r) < 0) return 0;

    int rc = ktfs_create("newfile1");
    if (rc != 0) {
        r->fail_reason = "ktfs_create returned non-zero";
        return 0;
    }

    struct io *io = NULL;
    if (fsopen("newfile1", &io) != 0 || io == NULL) {
        r->fail_reason = "fsopen on freshly-created file failed";
        return 0;
    }

    static const char pattern[8] = "create!";
    long w = ktfs_writeat(io, 0, pattern, sizeof pattern);
    ioclose(io);
    if (w != (long)sizeof pattern) {
        r->fail_reason = "writeat to created file returned wrong count";
        return 0;
    }

    struct io *io2 = NULL;
    if (fsopen("newfile1", &io2) != 0 || io2 == NULL) {
        r->fail_reason = "fsopen after writeat to created file failed";
        return 0;
    }
    char buf[8] = { 0 };
    long n = ioreadat(io2, 0, buf, sizeof buf);
    ioclose(io2);
    if (n != (long)sizeof buf || memcmp(buf, pattern, sizeof pattern) != 0) {
        r->fail_reason = "created file readback mismatch";
        return 0;
    }
    r->passed = 1;
    return 1;
}

// 3. test_ktfs_delete: create -> delete -> fsopen should fail.

static int test_ktfs_delete(struct test_result *r) {
    if (mount_blob(r) < 0) return 0;

    if (ktfs_create("doomed") != 0) {
        r->fail_reason = "ktfs_create returned non-zero";
        return 0;
    }
    // Confirm it actually exists first.
    struct io *io = NULL;
    if (fsopen("doomed", &io) != 0 || io == NULL) {
        r->fail_reason = "fsopen of just-created file failed";
        return 0;
    }
    ioclose(io);

    if (ktfs_delete("doomed") != 0) {
        r->fail_reason = "ktfs_delete returned non-zero";
        return 0;
    }
    // After delete, fsopen should fail.
    struct io *io2 = NULL;
    int rc = fsopen("doomed", &io2);
    if (rc == 0) {
        r->fail_reason = "fsopen succeeded on deleted file";
        return 0;
    }
    r->passed = 1;
    return 1;
}

static const struct test_entry ktfsrw_tests[] = {
    { "test_ktfs_writeat_persist", test_ktfs_writeat_persist, 5 },
    { "test_ktfs_create_persist",  test_ktfs_create_persist,  3 },
    { "test_ktfs_delete",          test_ktfs_delete,          2 },
};

const struct test_entry *get_ktfsrw_tests(int *n_out) {
    *n_out = sizeof ktfsrw_tests / sizeof ktfsrw_tests[0];
    return ktfsrw_tests;
}
