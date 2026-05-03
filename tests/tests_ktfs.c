// tests_ktfs.c -- 10 read-only KTFS test cases (10pt total)
//
// AG report cases (without _gdb suffix), all 1pt:
//   test_ktfs_close
//   test_ktfs_cntl
//   test_ktfs_readat_small               (file < 1 block)
//   test_ktfs_readat_block               (= 1 block)
//   test_ktfs_readat_block_2             (= 2 blocks)
//   test_ktfs_readat_indirect            (uses indirect block)
//   test_ktfs_readat_dindirect           (uses doubly-indirect block)
//   test_ktfs_readat_direct_to_indirect  (spans direct->indirect boundary)
//   test_ktfs_readat_non_block           (size not block-aligned)
//   test_ktfs_readat_two_files           (open two files, content distinct)
//
// Backing: the fixture filesystem image built by Makefile from
// test/fixtures/*.txt.  The image is spliced into the kernel ELF as
// .rodata.blob; at boot we wrap it in a memio and call ktfs_mount.
//
// Pristine starter ktfs.c stubs all return 0 -- ktfs_mount returns 0
// (lying about success), ktfs_open returns 0 with *ioptr untouched
// (NULL), ktfs_readat returns 0.  Every test fails on either
// "ktfs_open returned NULL ioptr" or "ktfs_readat returned 0".
//
// Last year's G13 group's failure mode was different: ktfs_mount
// crashed on sparse inode tables, taking down all subsequent tests.
// Our test bench would catch that as a fault recovery (cause=12 page
// fault) reported per test; the regression signature would be 10/10
// PANIC entries instead of "ktfs_open returned 0".

#include <stdint.h>
#include <stddef.h>
#include "io.h"
#include "fs.h"
#include "ktfs.h"
#include "string.h"
#include "test_framework.h"

extern char _kimg_blob_start[];
extern char _kimg_blob_end[];

// Patterns mkfs_ktfs put into each fixture file.  Each file's byte i is
// (i + seed) & 0xFF.  Match the seeds in the fixture-build python.
struct fixture_info {
    const char * name;
    long size;
    int seed;
};

static const struct fixture_info FIX_SMALL  = { "small.txt", 100,    0 };
static const struct fixture_info FIX_BLOCK  = { "block.txt", 512,    1 };
static const struct fixture_info FIX_BLOCK2 = { "block2.txt", 1024,  2 };
static const struct fixture_info FIX_NONBLK = { "non_block.txt", 700, 3 };
static const struct fixture_info FIX_DIRIND = { "dir2ind.txt", 2048, 4 };
static const struct fixture_info FIX_INDIR  = { "indirect.txt", 16*1024, 5 };
static const struct fixture_info FIX_DIND   = { "dindirect.txt", 64*1024 + 512, 6 };
static const struct fixture_info FIX_A      = { "file_a.txt", 200,   7 };
static const struct fixture_info FIX_B      = { "file_b.txt", 300,   8 };

// Mount the blob via memio.  Idempotent: ktfs_flush undoes prior state.
// Returns 0 on success, sets r->fail_reason on failure.
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
        r->fail_reason = "ktfs_mount returned an error";
        return -1;
    }
    return 0;
}

// Verify file content matches the seed pattern.  Reads the whole file
// in 256-byte chunks via ktfs_readat.  Returns 0 on success or sets
// r->fail_reason.
static int verify_pattern(struct test_result *r,
                          const struct fixture_info *fi)
{
    struct io *io = NULL;
    int rc = fsopen(fi->name, &io);
    if (rc != 0 || io == NULL) {
        r->fail_reason = "ktfs_open returned NULL ioptr";
        return -1;
    }

    static uint8_t buf[256];
    long got = 0;
    while (got < fi->size) {
        long want = fi->size - got;
        if (want > (long)sizeof buf) want = sizeof buf;
        long n = ioreadat(io, got, buf, want);
        if (n != want) {
            ioclose(io);
            r->fail_reason = "ktfs_readat returned wrong byte count";
            return -1;
        }
        for (long i = 0; i < want; i++) {
            if (buf[i] != (uint8_t)((got + i + fi->seed) & 0xFF)) {
                ioclose(io);
                r->fail_reason = "ktfs_readat returned wrong content";
                return -1;
            }
        }
        got += want;
    }
    ioclose(io);
    return 0;
}

// ---- individual tests ----------------------------------------------------

static int test_ktfs_close(struct test_result *r) {
    if (mount_blob(r) < 0) return 0;

    struct io *io = NULL;
    if (fsopen(FIX_SMALL.name, &io) != 0 || io == NULL) {
        r->fail_reason = "ktfs_open returned NULL ioptr";
        return 0;
    }
    // Close should drop refcnt to 0 and release any state.
    ioclose(io);

    // Re-open should still work.
    struct io *io2 = NULL;
    if (fsopen(FIX_SMALL.name, &io2) != 0 || io2 == NULL) {
        r->fail_reason = "ktfs_open after close returned NULL ioptr";
        return 0;
    }
    ioclose(io2);
    r->passed = 1;
    return 1;
}

static int test_ktfs_cntl(struct test_result *r) {
    if (mount_blob(r) < 0) return 0;

    struct io *io = NULL;
    if (fsopen(FIX_BLOCK.name, &io) != 0 || io == NULL) {
        r->fail_reason = "ktfs_open returned NULL ioptr";
        return 0;
    }
    unsigned long end = 0;
    int rc = ioctl(io, IOCTL_GETEND, &end);
    ioclose(io);

    if (rc != 0) {
        r->fail_reason = "IOCTL_GETEND returned an error";
        return 0;
    }
    if ((long)end != FIX_BLOCK.size) {
        r->fail_reason = "IOCTL_GETEND reported wrong size";
        return 0;
    }
    r->passed = 1;
    return 1;
}

#define MAKE_READAT_TEST(NAME, FIX)                                \
    static int test_ktfs_readat_##NAME(struct test_result *r) {    \
        if (mount_blob(r) < 0) return 0;                           \
        if (verify_pattern(r, &FIX) < 0) return 0;                 \
        r->passed = 1;                                             \
        return 1;                                                  \
    }

MAKE_READAT_TEST(small,              FIX_SMALL)
MAKE_READAT_TEST(block,              FIX_BLOCK)
MAKE_READAT_TEST(block_2,            FIX_BLOCK2)
MAKE_READAT_TEST(non_block,          FIX_NONBLK)
MAKE_READAT_TEST(direct_to_indirect, FIX_DIRIND)
MAKE_READAT_TEST(indirect,           FIX_INDIR)
MAKE_READAT_TEST(dindirect,          FIX_DIND)

static int test_ktfs_readat_two_files(struct test_result *r) {
    if (mount_blob(r) < 0) return 0;
    if (verify_pattern(r, &FIX_A) < 0) return 0;
    if (verify_pattern(r, &FIX_B) < 0) return 0;
    r->passed = 1;
    return 1;
}

static const struct test_entry ktfs_tests[] = {
    { "test_ktfs_close",                    test_ktfs_close,                    1 },
    { "test_ktfs_cntl",                     test_ktfs_cntl,                     1 },
    { "test_ktfs_readat_small",             test_ktfs_readat_small,             1 },
    { "test_ktfs_readat_block",             test_ktfs_readat_block,             1 },
    { "test_ktfs_readat_block_2",           test_ktfs_readat_block_2,           1 },
    { "test_ktfs_readat_indirect",          test_ktfs_readat_indirect,          1 },
    { "test_ktfs_readat_dindirect",         test_ktfs_readat_dindirect,         1 },
    { "test_ktfs_readat_direct_to_indirect",test_ktfs_readat_direct_to_indirect,1 },
    { "test_ktfs_readat_non_block",         test_ktfs_readat_non_block,         1 },
    { "test_ktfs_readat_two_files",         test_ktfs_readat_two_files,         1 },
};

const struct test_entry *get_ktfs_tests(int *n_out) {
    *n_out = sizeof ktfs_tests / sizeof ktfs_tests[0];
    return ktfs_tests;
}
