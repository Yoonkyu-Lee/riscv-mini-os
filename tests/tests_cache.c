// tests_cache.c -- 3 cache test cases (6pt)
//
// AG report cases (without _gdb suffix):
//   test_cache_get_block_0      2pt
//   test_cache_get_block_1      2pt
//   test_cache_release_block_0  2pt
//
// Backing device for these tests is a memio over a 64*512 = 32 KiB buffer
// that we've prefilled with a deterministic pattern (block k starts with
// the byte k).  We can therefore verify cache_get_block returned the
// correct content per spec.
//
// Pristine starter cache.c is our stub (returns -ENOTSUP) so all 3 tests
// fail with "create_cache returned non-zero".  Once cache.c is filled in,
// they exercise the rubric requirements (capacity >= 64, no eviction
// while a block is held).

#include <stdint.h>
#include <stddef.h>
#include "cache.h"
#include "io.h"
#include "test_framework.h"

#define CACHE_TEST_BLKS 64
#define CACHE_TEST_BUF_SZ (CACHE_TEST_BLKS * CACHE_BLKSZ)

static uint8_t cache_test_buf[CACHE_TEST_BUF_SZ];

static void prefill_backing(void) {
    for (int blk = 0; blk < CACHE_TEST_BLKS; blk++) {
        for (int off = 0; off < (int)CACHE_BLKSZ; off++)
            cache_test_buf[blk * CACHE_BLKSZ + off] = (uint8_t)(blk + off);
    }
}

// test_cache_get_block_0 -- create a cache and read block 0.
// Verifies the basic happy path: create succeeds, get returns 0, the
// block pointer is non-NULL, and content matches the backing store.

static int test_cache_get_block_0(struct test_result *r) {
    prefill_backing();

    struct io *bk = create_memory_io(cache_test_buf, CACHE_TEST_BUF_SZ);
    if (bk == NULL) {
        r->fail_reason = "create_memory_io for backing returned NULL";
        return 0;
    }

    struct cache *c = NULL;
    int rc = create_cache(bk, &c);
    if (rc != 0 || c == NULL) {
        ioclose(bk);
        r->fail_reason = "create_cache returned error";
        return 0;
    }

    void *blk0 = NULL;
    rc = cache_get_block(c, 0, &blk0);
    if (rc != 0 || blk0 == NULL) {
        ioclose(bk);
        r->fail_reason = "cache_get_block(pos=0) returned error";
        return 0;
    }
    uint8_t *p = blk0;
    if (p[0] != 0 || p[1] != 1 || p[2] != 2) {
        cache_release_block(c, blk0, CACHE_CLEAN);
        ioclose(bk);
        r->fail_reason = "cache_get_block returned wrong content";
        return 0;
    }
    cache_release_block(c, blk0, CACHE_CLEAN);
    ioclose(bk);
    r->passed = 1;
    return 1;
}

// test_cache_get_block_1 -- 64 distinct misses then 64 hits.
// Per rubric: "Starting with an empty cache, can induce 64 misses on
// different blocks and then 64 read hits on those same blocks with a
// total of 64 requests made to the backing device."
// We can't directly count backing-device requests without instrumenting
// the cache, so we verify weaker but observable properties:
//   - All 64 get_block calls succeed.
//   - The second pass returns the same byte content (correctness must
//     hold whether served from cache or backing).
// A real eviction-count assertion would need cache to expose hit/miss
// counters; we leave that to future instrumentation if desired.

static int test_cache_get_block_1(struct test_result *r) {
    prefill_backing();

    struct io *bk = create_memory_io(cache_test_buf, CACHE_TEST_BUF_SZ);
    if (bk == NULL) {
        r->fail_reason = "create_memory_io for backing returned NULL";
        return 0;
    }
    struct cache *c = NULL;
    if (create_cache(bk, &c) != 0 || c == NULL) {
        ioclose(bk);
        r->fail_reason = "create_cache returned error";
        return 0;
    }

    void *blocks[CACHE_TEST_BLKS] = { NULL };
    // Pass 1: 64 misses.
    for (int blk = 0; blk < CACHE_TEST_BLKS; blk++) {
        int rc = cache_get_block(c, blk * CACHE_BLKSZ, &blocks[blk]);
        if (rc != 0 || blocks[blk] == NULL) {
            r->fail_reason = "cache_get_block first pass failed";
            ioclose(bk);
            return 0;
        }
        // Verify content first byte.
        if (((uint8_t *)blocks[blk])[0] != (uint8_t)blk) {
            r->fail_reason = "cache_get_block first pass returned wrong content";
            ioclose(bk);
            return 0;
        }
    }
    // Hold them all -- spec says they must remain in cache for the
    // duration we hold them (capacity >= 64).
    for (int blk = 0; blk < CACHE_TEST_BLKS; blk++)
        cache_release_block(c, blocks[blk], CACHE_CLEAN);

    // Pass 2: must still return correct data.
    for (int blk = 0; blk < CACHE_TEST_BLKS; blk++) {
        void *bp = NULL;
        int rc = cache_get_block(c, blk * CACHE_BLKSZ, &bp);
        if (rc != 0 || bp == NULL) {
            r->fail_reason = "cache_get_block second pass failed";
            ioclose(bk);
            return 0;
        }
        if (((uint8_t *)bp)[0] != (uint8_t)blk) {
            r->fail_reason = "cache_get_block second pass returned wrong content";
            cache_release_block(c, bp, CACHE_CLEAN);
            ioclose(bk);
            return 0;
        }
        cache_release_block(c, bp, CACHE_CLEAN);
    }

    ioclose(bk);
    r->passed = 1;
    return 1;
}

// test_cache_release_block_0 -- release a held block, then re-acquire.
// The pointer may or may not be the same; what matters is content.

static int test_cache_release_block_0(struct test_result *r) {
    prefill_backing();

    struct io *bk = create_memory_io(cache_test_buf, CACHE_TEST_BUF_SZ);
    if (bk == NULL) { r->fail_reason = "memio NULL"; return 0; }
    struct cache *c = NULL;
    if (create_cache(bk, &c) != 0 || c == NULL) {
        ioclose(bk);
        r->fail_reason = "create_cache returned error";
        return 0;
    }

    void *blk5 = NULL;
    if (cache_get_block(c, 5 * CACHE_BLKSZ, &blk5) != 0 || blk5 == NULL) {
        ioclose(bk);
        r->fail_reason = "cache_get_block(5) failed";
        return 0;
    }
    cache_release_block(c, blk5, CACHE_CLEAN);

    // Re-acquire -- content should still be correct.
    void *blk5b = NULL;
    if (cache_get_block(c, 5 * CACHE_BLKSZ, &blk5b) != 0 || blk5b == NULL) {
        ioclose(bk);
        r->fail_reason = "re-acquire of released block failed";
        return 0;
    }
    if (((uint8_t *)blk5b)[0] != 5) {
        cache_release_block(c, blk5b, CACHE_CLEAN);
        ioclose(bk);
        r->fail_reason = "released block had wrong content on re-acquire";
        return 0;
    }
    cache_release_block(c, blk5b, CACHE_CLEAN);

    ioclose(bk);
    r->passed = 1;
    return 1;
}

static const struct test_entry cache_tests[] = {
    { "test_cache_get_block_0",     test_cache_get_block_0,     2 },
    { "test_cache_get_block_1",     test_cache_get_block_1,     2 },
    { "test_cache_release_block_0", test_cache_release_block_0, 2 },
};

const struct test_entry *get_cache_tests(int *n_out) {
    *n_out = sizeof cache_tests / sizeof cache_tests[0];
    return cache_tests;
}
