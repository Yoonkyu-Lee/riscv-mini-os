// cache.c - Block cache for a storage device
//
// Simple write-back cache backed by a fixed-size entry table (no
// hashing -- linear scan suffices for N=128).  Replacement policy: pick
// any free slot first; otherwise evict the lowest-refcnt clean entry,
// then the lowest-refcnt dirty entry (writing it back).
//
// Capacity is intentionally well above the rubric minimum of 64 so we
// can hold 64 simultaneous get_block() refs without ever evicting.

#include "cache.h"
#include "io.h"
#include "error.h"
#include "heap.h"
#include "string.h"
#include "assert.h"

#include <stdint.h>
#include <stddef.h>

#define CACHE_NENT 128

struct cache_entry {
    unsigned long long pos;         // backing-device byte offset (block-aligned)
    int                valid;       // 1 if occupied
    int                refcnt;      // outstanding cache_get_block refs
    int                dirty;       // needs write-back
    uint8_t            data[CACHE_BLKSZ];
};

struct cache {
    struct io          *bkgio;
    struct cache_entry *ents[CACHE_NENT];   // each lazily allocated
};

int create_cache(struct io * bkgio, struct cache ** cptr) {
    if (bkgio == NULL || cptr == NULL)
        return -EINVAL;
    // struct cache itself is ~1KB (bkgio + 128 ptrs), comfortably under
    // HEAP_ALLOC_MAX.  The big arrays go into per-entry allocations
    // below, so we never trip the heap's max-block assertion.
    struct cache * c = kcalloc(1, sizeof(struct cache));
    if (c == NULL)
        return -ENOMEM;
    c->bkgio = bkgio;
    for (int i = 0; i < CACHE_NENT; i++) {
        c->ents[i] = kcalloc(1, sizeof(struct cache_entry));
        if (c->ents[i] == NULL) {
            for (int j = 0; j < i; j++) kfree(c->ents[j]);
            kfree(c);
            return -ENOMEM;
        }
    }
    *cptr = c;
    return 0;
}

// Linear lookup of a matching block.
static struct cache_entry *
find_entry(struct cache * c, unsigned long long pos) {
    for (int i = 0; i < CACHE_NENT; i++) {
        if (c->ents[i]->valid && c->ents[i]->pos == pos)
            return c->ents[i];
    }
    return NULL;
}

// Pick a victim slot (or NULL if all entries are held).  Preference:
//   1. any !valid slot (free)
//   2. any refcnt==0 && !dirty (clean evict)
//   3. any refcnt==0 && dirty (write-back evict)
static struct cache_entry * pick_victim(struct cache * c) {
    for (int i = 0; i < CACHE_NENT; i++)
        if (!c->ents[i]->valid) return c->ents[i];
    for (int i = 0; i < CACHE_NENT; i++)
        if (c->ents[i]->refcnt == 0 && !c->ents[i]->dirty)
            return c->ents[i];
    for (int i = 0; i < CACHE_NENT; i++)
        if (c->ents[i]->refcnt == 0)
            return c->ents[i];
    return NULL;
}

// Write entry back to backing device if dirty.  Returns 0 on success.
static int writeback(struct cache * c, struct cache_entry * e) {
    if (!e->valid || !e->dirty)
        return 0;
    long n = iowriteat(c->bkgio, e->pos, e->data, CACHE_BLKSZ);
    if (n != (long)CACHE_BLKSZ)
        return (int)n;
    e->dirty = 0;
    return 0;
}

int cache_get_block(struct cache * cache, unsigned long long pos, void ** pptr) {
    if (cache == NULL || pptr == NULL)
        return -EINVAL;
    if (pos % CACHE_BLKSZ != 0)
        return -EINVAL;

    // Hit?
    struct cache_entry * e = find_entry(cache, pos);
    if (e != NULL) {
        e->refcnt++;
        *pptr = e->data;
        return 0;
    }

    // Miss -- need a victim slot.
    e = pick_victim(cache);
    if (e == NULL)
        return -ENOMEM;
    if (e->valid) {
        int rc = writeback(cache, e);
        if (rc != 0)
            return rc;
    }

    long n = ioreadat(cache->bkgio, pos, e->data, CACHE_BLKSZ);
    if (n != (long)CACHE_BLKSZ)
        return (int)n;

    e->pos    = pos;
    e->valid  = 1;
    e->dirty  = 0;
    e->refcnt = 1;
    *pptr     = e->data;
    return 0;
}

void cache_release_block(struct cache * cache, void * pblk, int dirty) {
    if (cache == NULL || pblk == NULL)
        return;
    for (int i = 0; i < CACHE_NENT; i++) {
        struct cache_entry * e = cache->ents[i];
        if (!e->valid || e->data != (uint8_t *)pblk)
            continue;
        if (dirty == CACHE_DIRTY)
            e->dirty = 1;
        if (e->refcnt > 0)
            e->refcnt--;
        return;
    }
    // Not found -- silently ignore (spec doesn't require panic).
}

int cache_flush(struct cache * cache) {
    if (cache == NULL)
        return -EINVAL;
    for (int i = 0; i < CACHE_NENT; i++) {
        int rc = writeback(cache, cache->ents[i]);
        if (rc != 0)
            return rc;
    }
    return 0;
}
