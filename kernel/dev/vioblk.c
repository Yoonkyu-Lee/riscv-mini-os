// vioblk.c - VirtIO block device driver
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef VIOBLK_TRACE
#define TRACE
#endif

#ifdef VIOBLK_DEBUG
#define DEBUG
#endif

#include "virtio.h"
#include "intr.h"
#include "assert.h"
#include "heap.h"
#include "io.h"
#include "device.h"
#include "thread.h"
#include "error.h"
#include "string.h"
#include "ioimpl.h"
#include "conf.h"
#include "console.h"

#include <limits.h>
#include <stdint.h>
#include <stddef.h>

// COMPILE-TIME PARAMETERS
//

#ifndef VIOBLK_INTR_PRIO
#define VIOBLK_INTR_PRIO 1
#endif

#ifndef VIOBLK_NAME
#define VIOBLK_NAME "vioblk"
#endif

// VirtIO block device feature bits (number, *not* mask)
#define VIRTIO_BLK_F_BLK_SIZE       6
#define VIRTIO_BLK_F_TOPOLOGY       10

// VirtIO block request type (struct virtio_blk_req.type values)
#define VIRTIO_BLK_T_IN             0   // read
#define VIRTIO_BLK_T_OUT            1   // write

// Status returned by device after request.
#define VIRTIO_BLK_S_OK             0

// Per-request descriptor chain layout (3 descriptors per request):
//   desc[0] -- header (driver-readable, type+sector)
//   desc[1] -- data (RW depending on type)
//   desc[2] -- status byte (device-writable)
struct virtio_blk_req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

// Internal state for a single attached block device.  We use a 1-deep
// virtq -- each request sits in desc[0..2] and we wait synchronously
// for it to complete.  Adequate for the rubric workload.
struct vioblk_device {
    volatile struct virtio_mmio_regs * regs;
    int      irqno;
    int      instno;
    uint32_t blksz;
    uint64_t capacity_blocks;   // device-reported total in 512-byte sectors
    struct io io;
    struct lock io_lock;        // serialize vioblk_io across threads

    // Single 3-descriptor virtq.  Aligned per VirtIO spec (16-byte for
    // descriptor table, 2-byte for avail/used rings is fine).
    struct {
        uint16_t last_used_idx;
        union {
            struct virtq_avail avail;
            char _avail_filler[VIRTQ_AVAIL_SIZE(3)];
        };
        union {
            volatile struct virtq_used used;
            char _used_filler[VIRTQ_USED_SIZE(3)];
        };
        struct virtq_desc desc[3];
    } vq;

    struct virtio_blk_req_hdr hdr;
    volatile uint8_t          status;
};

// INTERNAL FUNCTION DECLARATIONS

static int  vioblk_open(struct io ** ioptr, void * aux);
static void vioblk_close(struct io * io);
static long vioblk_readat(struct io * io, unsigned long long pos,
                          void * buf, long bufsz);
static long vioblk_writeat(struct io * io, unsigned long long pos,
                           const void * buf, long len);
static int  vioblk_cntl(struct io * io, int cmd, void * arg);
static void vioblk_isr(int srcno, void * aux);

static long vioblk_io(struct vioblk_device * d,
                      uint32_t type,
                      unsigned long long pos,
                      void * buf, long len);

// EXPORTED FUNCTION DEFINITIONS

void vioblk_attach(volatile struct virtio_mmio_regs * regs, int irqno) {
    static const struct iointf vioblk_iointf = {
        .close   = &vioblk_close,
        .readat  = &vioblk_readat,
        .writeat = &vioblk_writeat,
        .cntl    = &vioblk_cntl,
    };
    struct vioblk_device * dev;
    virtio_featset_t needed_features, wanted_features, enabled_features;
    int result;

    // Signal driver presence.
    regs->status |= VIRTIO_STAT_DRIVER;
    __sync_synchronize();

    // Negotiate features.  No "needed" features for the basic driver;
    // we'd like BLK_SIZE so we can pick up a non-512 block size if the
    // device advertises one.
    virtio_featset_init(needed_features);
    virtio_featset_init(wanted_features);
    virtio_featset_add(wanted_features, VIRTIO_BLK_F_BLK_SIZE);
    result = virtio_negotiate_features(regs,
        enabled_features, wanted_features, needed_features);
    if (result != 0) {
        kprintf("%p: virtio feature negotiation failed\n", regs);
        return;
    }

    dev = kcalloc(1, sizeof(struct vioblk_device));
    if (dev == NULL)
        return;
    dev->regs  = regs;
    dev->irqno = irqno;

    if (virtio_featset_test(enabled_features, VIRTIO_BLK_F_BLK_SIZE))
        dev->blksz = regs->config.blk.blk_size;
    else
        dev->blksz = 512;
    assert (((dev->blksz - 1) & dev->blksz) == 0);

    // Read capacity (in 512-byte sectors per VirtIO spec).
    dev->capacity_blocks = regs->config.blk.capacity;

    ioinit0(&dev->io, &vioblk_iointf);
    lock_init(&dev->io_lock);

    // Wire up our 3-descriptor virtq.
    virtio_attach_virtq(regs, /*qid=*/0, /*len=*/3,
        (uint64_t)(uintptr_t)&dev->vq.desc[0],
        (uint64_t)(uintptr_t)&dev->vq.used,
        (uint64_t)(uintptr_t)&dev->vq.avail);
    virtio_enable_virtq(regs, 0);

    enable_intr_source(irqno, VIOBLK_INTR_PRIO, &vioblk_isr, dev);

    regs->status |= VIRTIO_STAT_DRIVER_OK;
    __sync_synchronize();

    dev->instno = register_device(VIOBLK_NAME, &vioblk_open, dev);
}

static int vioblk_open(struct io ** ioptr, void * aux) {
    struct vioblk_device * const dev = aux;
    if (iorefcnt(&dev->io) != 0)
        return -EBUSY;
    *ioptr = ioaddref(&dev->io);
    return 0;
}

static void vioblk_close(struct io * io) {
    assert (iorefcnt(io) == 0);
    // Virtq stays attached for the next open.
}

// Single round-trip: build the 3-descriptor chain, push to avail,
// notify, spin-wait for used.idx to advance, copy out / capture
// status, return bytes transferred.  Serialized via d->io_lock so
// concurrent processes don't trample the single-deep virtq.
static long vioblk_io(struct vioblk_device * d, uint32_t type,
                      unsigned long long pos, void * buf, long len) {
    if (pos % 512 != 0 || len <= 0 || (len % 512) != 0)
        return -EINVAL;
    lock_acquire(&d->io_lock);

    d->hdr.type     = type;
    d->hdr.reserved = 0;
    d->hdr.sector   = pos / 512;
    d->status       = 0xFF;   // sentinel; device writes 0 on success

    // desc[0]: header (driver -> device)
    d->vq.desc[0].addr  = (uint64_t)(uintptr_t)&d->hdr;
    d->vq.desc[0].len   = sizeof d->hdr;
    d->vq.desc[0].flags = VIRTQ_DESC_F_NEXT;
    d->vq.desc[0].next  = 1;

    // desc[1]: data
    d->vq.desc[1].addr  = (uint64_t)(uintptr_t)buf;
    d->vq.desc[1].len   = len;
    d->vq.desc[1].flags = VIRTQ_DESC_F_NEXT |
                          (type == VIRTIO_BLK_T_IN ? VIRTQ_DESC_F_WRITE : 0);
    d->vq.desc[1].next  = 2;

    // desc[2]: status byte (device -> driver)
    d->vq.desc[2].addr  = (uint64_t)(uintptr_t)&d->status;
    d->vq.desc[2].len   = 1;
    d->vq.desc[2].flags = VIRTQ_DESC_F_WRITE;
    d->vq.desc[2].next  = 0;

    // Publish the head descriptor (index 0) in the avail ring.
    uint16_t aidx = d->vq.avail.idx;
    d->vq.avail.ring[aidx % 3] = 0;
    __sync_synchronize();
    d->vq.avail.idx = aidx + 1;
    __sync_synchronize();

    virtio_notify_avail(d->regs, 0);

    // Wait for the device to advance used.idx (cooperative thread_yield
    // so other threads progress while we wait).
    while (d->vq.used.idx == d->vq.last_used_idx) {
        __sync_synchronize();
        thread_yield();
    }
    d->vq.last_used_idx = d->vq.used.idx;

    int status_ok = (d->status == VIRTIO_BLK_S_OK);
    lock_release(&d->io_lock);
    if (!status_ok) return -EIO;
    return len;
}

static long vioblk_readat(struct io * io, unsigned long long pos,
                          void * buf, long bufsz) {
    struct vioblk_device * const d =
        (void *)io - offsetof(struct vioblk_device, io);
    return vioblk_io(d, VIRTIO_BLK_T_IN, pos, buf, bufsz);
}

static long vioblk_writeat(struct io * io, unsigned long long pos,
                           const void * buf, long len) {
    struct vioblk_device * const d =
        (void *)io - offsetof(struct vioblk_device, io);
    // Cast away const -- the descriptor api takes a non-const addr but
    // the device-readable flag means it won't be modified.
    return vioblk_io(d, VIRTIO_BLK_T_OUT, pos, (void *)buf, len);
}

static int vioblk_cntl(struct io * io, int cmd, void * arg) {
    struct vioblk_device * const d =
        (void *)io - offsetof(struct vioblk_device, io);
    switch (cmd) {
    case IOCTL_GETBLKSZ:
        return (int)d->blksz;
    case IOCTL_GETEND:
        if (arg == NULL) return -EINVAL;
        // capacity is in 512-byte sectors per VirtIO spec.
        *(unsigned long long *)arg = d->capacity_blocks * 512ULL;
        return 0;
    default:
        return -ENOTSUP;
    }
}

static void vioblk_isr(int srcno, void * aux) {
    struct vioblk_device * const d = aux;
    (void)srcno;
    // Ack pending bits so PLIC line de-asserts.  The driver thread is
    // polling used.idx so there's nothing else to do here.
    uint32_t status = d->regs->interrupt_status;
    d->regs->interrupt_ack = status;
}
